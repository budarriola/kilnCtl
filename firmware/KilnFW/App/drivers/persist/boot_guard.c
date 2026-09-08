#include "boot_guard.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal_kv.h"
#include "nvs_key_check.h"

static const char *TAG = "boot_guard";

/* Same partition as run_state.c/relay_cycles.c/zones_http.c (TODO.md 8.1's
 * split), own namespace+key so a corrupt/rejected boot-guard record can
 * never take any of those down with it and vice versa. */
#define KILN_NVS_PARTITION "kiln_nvs"
#define NVS_NAMESPACE "boot_guard"
#define NVS_KEY_REC "count"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_REC);

/* Bumped whenever boot_guard_record_t's layout changes. Same "discard rather
 * than migrate" convention as run_state.h -- a lost boot-guard record across
 * a firmware update costs at most one boot's worth of recovery-tracking
 * accuracy, never a reason to mis-parse an old layout. */
#define BOOT_GUARD_RECORD_VERSION 1

/* Persisted verbatim as one fixed-size NVS blob. crc32 covers every byte of
 * the struct up to (not including) itself -- see run_state.c for why a
 * pinned, explicit layout matters here, and this header's own doc comment
 * for why a CRC is worth the extra 4 bytes on top of run_state.c's
 * version+size bar: a corrupted-to-zero boot counter on a genuinely
 * reset-looping board would silently suppress the one feature this module
 * exists to provide. */
typedef struct {
    uint8_t  version;
    uint8_t  reserved[3]; /* explicit, so boot_count below is 4-aligned */
    uint32_t boot_count;
    uint32_t crc32;
} boot_guard_record_t;

/* Plain array-size trick instead of _Static_assert: this file is compiled
 * both by the ESP-IDF (C11-capable) toolchain and, unmodified, by MSVC's
 * default C compiler for the host tests (build_host_tests.ps1), which
 * rejects a bare _Static_assert() at file scope outside of /std:c11. */
typedef char boot_guard_record_size_check[(sizeof(boot_guard_record_t) == 12) ? 1 : -1];

typedef struct {
    SemaphoreHandle_t lock;
    bool     initialized;
    uint32_t count;          /* this boot's count, AFTER the boot_guard_init() increment */
    bool     recovery_mode;  /* decided once, at boot_guard_init(), from the count BEFORE the increment */
    bool     healthy_marked; /* boot_guard_mark_healthy() already cleared the counter this boot */
} boot_guard_ctx_t;

static boot_guard_ctx_t s_bg;

/* Table-less CRC32 (IEEE 802.3/zlib polynomial, same algorithm
 * esp_rom_crc32_le() implements) reimplemented locally rather than pulling
 * in esp_rom_crc.h: this module's own host tests (App/test/test_boot_guard.c)
 * build and run entirely off-target with no ESP-IDF ROM available, and a
 * dozen lines of portable C is cheaper than adding a new ROM-CRC stub header
 * just for this one caller. Verified against the standard CRC32 test vector
 * ("123456789" -> 0xCBF43926) in test_boot_guard.c. */
static uint32_t crc32_compute(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

/* CRC covers every field except crc32 itself, i.e. offsetof(boot_guard_record_t, crc32) bytes. */
static uint32_t record_crc(const boot_guard_record_t *rec)
{
    return crc32_compute(rec, offsetof(boot_guard_record_t, crc32));
}

/* Pure predicate, no I/O -- exercised directly by test_boot_guard.c. A
 * record fails this check (and is treated as "no record", i.e. count 0) if
 * its version doesn't match this build's layout OR its CRC doesn't match its
 * own contents. Either failure is indistinguishable from "never written" or
 * "corrupted in flash" from the caller's point of view, which is exactly the
 * right thing to do with both: see run_state.c's identical load-tolerant
 * reasoning. */
static bool record_is_valid(const boot_guard_record_t *rec)
{
    return rec->version == BOOT_GUARD_RECORD_VERSION && rec->crc32 == record_crc(rec);
}

/* Pure logic, no I/O -- exercised directly by test_boot_guard.c. Computes
 * this boot's recovery_mode decision and the new persisted count from
 * whatever was loaded (or "nothing valid was loaded", loaded_count = 0). */
static uint32_t next_boot_count(uint32_t loaded_count, bool *out_recovery_mode)
{
#if RECOVERY_MODE_ENABLED
    *out_recovery_mode = (loaded_count >= RECOVERY_MODE_BOOT_THRESHOLD);
#else
    /* See RECOVERY_MODE_ENABLED in boot_guard.h. Counting and reporting stay
     * live; only ENTERING the mode is disabled, because entering it is what
     * is currently broken. */
    (void)loaded_count;
    *out_recovery_mode = false;
#endif
    return loaded_count + 1u;
}

static hal_status_t nvs_partition_init(const char *partition)
{
    return hal_kv_init_partition(partition);
}

static hal_status_t persist_count(uint32_t count)
{
    boot_guard_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.version = BOOT_GUARD_RECORD_VERSION;
    rec.boot_count = count;
    rec.crc32 = record_crc(&rec);

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return err;
    }
    err = hal_kv_set_blob(&h, NVS_KEY_REC, &rec, sizeof(rec));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return err;
}

/* Loads the persisted count, or 0 if there is nothing valid to load --
 * missing (first boot), wrong size (a build with a different layout), wrong
 * version, or a bad CRC all collapse to the same "count 0" answer. */
static uint32_t load_count(void)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return 0;
    }
    boot_guard_record_t rec;
    size_t len = sizeof(rec);
    err = hal_kv_get_blob(&h, NVS_KEY_REC, &rec, &len);
    hal_kv_close(&h);
    if (err != HAL_OK || len != sizeof(rec)) {
        return 0;
    }
    if (!record_is_valid(&rec)) {
        ESP_LOGW(TAG, "boot-guard record failed its version/CRC check -- treating as count 0 "
                      "(this either loses one boot's worth of recovery-tracking accuracy, or is "
                      "flash corruption worth knowing about)");
        return 0;
    }
    return rec.boot_count;
}

/* Strict read-back for boot_guard_mark_healthy()'s verification: unlike
 * load_count() (which deliberately collapses "missing/corrupt/unreadable"
 * to a safe default of 0, appropriate for an ordinary boot-time load), a
 * verify step must NOT treat "could not read it back at all" as "confirmed
 * zero" -- those are opposite conclusions here. Returns true only if the
 * record was read back successfully, is version/CRC-valid, AND its
 * boot_count is exactly `expected`. Any read/validity failure returns
 * false, distinctly from "read back some other value" -- both are logged
 * differently by the caller, but both are "not verified". */
static bool verify_persisted_count(uint32_t expected)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "verify_persisted_count: could not reopen '%s'/'%s' to read back: %s",
                 KILN_NVS_PARTITION, NVS_NAMESPACE, hal_status_to_name(err));
        return false;
    }
    boot_guard_record_t rec;
    size_t len = sizeof(rec);
    err = hal_kv_get_blob(&h, NVS_KEY_REC, &rec, &len);
    hal_kv_close(&h);
    if (err != HAL_OK || len != sizeof(rec)) {
        ESP_LOGE(TAG, "verify_persisted_count: read-back failed: %s (len=%u, want %u)",
                 hal_status_to_name(err), (unsigned)len, (unsigned)sizeof(rec));
        return false;
    }
    if (!record_is_valid(&rec)) {
        ESP_LOGE(TAG, "verify_persisted_count: record failed version/CRC check immediately after "
                      "writing it -- the write plainly did not take");
        return false;
    }
    return rec.boot_count == expected;
}

/* Best-effort mitigation, tried once by boot_guard_mark_healthy() before it
 * gives up for this call: explicitly erase the key first, then write+commit
 * a fresh record. An ordinary nvs_set_blob() overwrite-in-place is what
 * persist_count() already does and is what was observed (2026-09-08 audit)
 * to sometimes report HAL_OK without the read-back changing; erasing first
 * removes any possibility of an in-place-update quirk being the cause,
 * without requiring a diagnosis of exactly which NVS-internal condition
 * produced the original symptom. Returns the write's own status (NOT
 * whether it verified -- the caller still verifies separately). */
static hal_status_t erase_then_persist_count(uint32_t count)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return err;
    }
    hal_status_t erase_err = hal_kv_erase_key(&h, NVS_KEY_REC);
    if (erase_err != HAL_OK && erase_err != HAL_NOT_FOUND) {
        /* Not fatal by itself -- fall through and try the write anyway,
         * same as persist_count()'s own tolerance -- but worth knowing. */
        ESP_LOGW(TAG, "erase_then_persist_count: hal_kv_erase_key failed: %s -- writing anyway",
                 hal_status_to_name(erase_err));
    }
    hal_kv_close(&h);
    return persist_count(count);
}

static bool ensure_lock(void)
{
    if (!s_bg.lock) {
        s_bg.lock = xSemaphoreCreateMutex();
        if (!s_bg.lock) {
            ESP_LOGE(TAG, "xSemaphoreCreateMutex failed -- boot-guard cannot track boots this boot "
                          "(defaulting to NOT recovery mode, so a real lockup would go uncaught -- "
                          "this is the one failure path in this module worth flagging loudly)");
            return false;
        }
    }
    return true;
}

esp_err_t boot_guard_init(void)
{
    if (s_bg.initialized) {
        /* Already ran this boot -- see the header's "safe to call more than
         * once" note. Returning ESP_OK here rather than re-incrementing is
         * what makes that safe: a second call must not count as a second
         * boot. */
        return ESP_OK;
    }
    if (!ensure_lock()) {
        s_bg.initialized = true;
        s_bg.count = 1;
        s_bg.recovery_mode = false;
        return ESP_ERR_NO_MEM;
    }

    hal_status_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGE(TAG, "NVS partition '%s' init failed: %s -- boot-guard counter will not persist "
                      "this boot (defaulting to NOT recovery mode)",
                 KILN_NVS_PARTITION, hal_status_to_name(part_err));
    }

    xSemaphoreTake(s_bg.lock, portMAX_DELAY);

    uint32_t loaded = (part_err == HAL_OK) ? load_count() : 0;
    bool recovery_mode;
    uint32_t new_count = next_boot_count(loaded, &recovery_mode);

    hal_status_t write_err = HAL_OK;
    if (part_err == HAL_OK) {
        write_err = persist_count(new_count);
    }

    s_bg.count = new_count;
    s_bg.recovery_mode = recovery_mode;
    s_bg.healthy_marked = false;
    s_bg.initialized = true;
    xSemaphoreGive(s_bg.lock);

    if (write_err != HAL_OK) {
        /* Loud on purpose -- see boot_guard.h's doc comment on boot_guard_init():
         * a boot-guard that silently stops counting is exactly the failure
         * mode that would leave a genuinely reset-looping board never
         * entering recovery. */
        ESP_LOGE(TAG, "could not persist boot-guard count %lu: %s -- next boot will not see this "
                      "one counted",
                 (unsigned long)new_count, hal_status_to_name(write_err));
    }

    if (recovery_mode) {
        ESP_LOGE(TAG, "**********************************************************");
        ESP_LOGE(TAG, "RECOVERY MODE: %lu consecutive boots were never confirmed healthy "
                      "(threshold %d).", (unsigned long)loaded, RECOVERY_MODE_BOOT_THRESHOLD);
        ESP_LOGE(TAG, "Coming up with Wi-Fi + OTA HTTP routes ONLY -- no profile executor, no "
                      "autotune, no rules task, no PID this boot.");
        ESP_LOGE(TAG, "**********************************************************");
    } else {
        ESP_LOGI(TAG, "boot-guard: %lu unconfirmed boot(s) so far (threshold %d)",
                 (unsigned long)loaded, RECOVERY_MODE_BOOT_THRESHOLD);
    }

    return ESP_OK;
}

bool boot_guard_is_recovery_mode(void)
{
    return s_bg.recovery_mode;
}

uint32_t boot_guard_get_boot_count(void)
{
    return s_bg.count;
}

bool boot_confirm_is_healthy(bool nvs_ok, bool web_ok, bool ota_routes_ok)
{
    /* Pure logic, no I/O, no safety-link term -- see boot_guard.h's doc
     * comment on this function for the full history of why. */
    return nvs_ok && web_ok && ota_routes_ok;
}

boot_confirm_action_t boot_confirm_decide(bool is_factory_partition, bool nvs_ok, bool web_ok,
                                           bool ota_routes_ok)
{
    /* Pure logic, no I/O -- exercised directly by test_boot_guard.c. See
     * boot_guard.h's doc comment on this function for what each outcome
     * means and why the factory-vs-OTA-slot branch exists at all. */
    if (!boot_confirm_is_healthy(nvs_ok, web_ok, ota_routes_ok)) {
        return BOOT_CONFIRM_SKIP_NOT_HEALTHY;
    }
    return is_factory_partition ? BOOT_CONFIRM_SKIP_FACTORY : BOOT_CONFIRM_CONFIRM_OTA_SLOT;
}

bool boot_guard_mark_healthy(void)
{
    if (!s_bg.initialized) {
        return false;
    }
    if (s_bg.healthy_marked) {
        return true; /* already verified cleared earlier this boot */
    }
    if (!ensure_lock()) {
        return false;
    }
    xSemaphoreTake(s_bg.lock, portMAX_DELAY);
    hal_status_t err = persist_count(0);
    /* See boot_guard.h's doc comment on this function: a HAL_OK write result
     * was observed on real hardware NOT to guarantee the persisted value
     * actually changed (docs/audits/boot_guard_recovery_loop_2026-09-08.md).
     * Read it back with verify_persisted_count() (NOT load_count() -- that
     * function's own "unreadable collapses to 0" default is exactly wrong
     * for a verification step, which must tell a genuine confirmed-zero
     * apart from "could not read it back at all") before ever trusting the
     * clear -- this is the fix, not the write's own return code. */
    bool verified = (err == HAL_OK) && verify_persisted_count(0);
    if (!verified) {
        /* One bounded retry, erasing the key first -- see
         * erase_then_persist_count()'s own comment. Cheap (this call only
         * runs a handful of times total per boot, from a low-priority
         * background task or an explicit operator action, never a hot
         * path), and it measurably improves the odds of actually clearing
         * on real hardware where a plain overwrite was observed not to
         * stick. Logged distinctly so a retry that was needed is visible,
         * not just a retry that succeeded. */
        ESP_LOGW(TAG, "boot-guard clear did not verify on the first attempt -- retrying once with "
                      "an explicit erase-then-write");
        err = erase_then_persist_count(0);
        verified = (err == HAL_OK) && verify_persisted_count(0);
    }
    if (verified) {
        s_bg.healthy_marked = true;
    }
    xSemaphoreGive(s_bg.lock);

    if (verified) {
        ESP_LOGI(TAG, "boot-guard counter cleared and VERIFIED (read back as 0) -- this boot is "
                      "confirmed healthy");
    } else if (err != HAL_OK) {
        ESP_LOGW(TAG, "could not clear boot-guard counter: %s -- will retry next call",
                 hal_status_to_name(err));
    } else {
        /* The write call itself reported success, but verify_persisted_count()
         * did NOT confirm boot_count==0 immediately afterward -- exactly the
         * failure mode that bricked this board into a permanent recovery
         * loop (that function already logs the specific reason: unreadable,
         * invalid, or a different nonzero value). */
        ESP_LOGE(TAG, "boot-guard counter WRITE REPORTED SUCCESS BUT DID NOT VERIFY -- NOT marking "
                      "this boot healthy; will retry next call rather than trusting the write's own "
                      "return code");
    }
    return verified;
}
