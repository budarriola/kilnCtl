#include "boot_guard.h"

#include <stddef.h>
#include <string.h>

#include "esp_attr.h"
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
/* CURRENT namespace. Was its own "boot_guard" namespace until 2026-09-08 --
 * see NVS_KEY_REC below for why the record moved into the namespace every
 * other small kiln_nvs value already lives in. */
#define NVS_NAMESPACE "kiln_cfg"
#define NVS_NAMESPACE_LEGACY "boot_guard"

/* CURRENT record key. Was "count" until 2026-09-08; see NVS_KEY_REC_LEGACY.
 *
 * WHY THE KEY MOVED (docs/audits/boot_guard_recovery_loop_2026-09-08.md):
 * on this bench board the item stored under "count" stopped changing in
 * flash. Every in-boot call reported success -- hal_kv_open(), set_blob()
 * and commit() all returned HAL_OK, and a reopened READ_ONLY handle read
 * the new value straight back with a valid CRC -- yet every subsequent boot
 * loaded the same stale 3, for days, through two separate attempted fixes.
 * Freshly created probe keys written to the SAME partition on the SAME
 * boots incremented correctly across every one of those reboots WHEN THEY
 * WERE IN THE "kiln_cfg" NAMESPACE, so NVS itself, the partition (used 314
 * of 2016 entries), the key lengths and the write path were all healthy.
 * A first attempt at a cure moved only the KEY ("count" -> "bootcnt2")
 * inside the same "boot_guard" namespace: it was flashed and made no
 * difference at all -- the next boot still found no record under the new
 * key and fell back to the stale legacy 3. So the stuck unit is the
 * NAMESPACE, not the key: nothing written under "boot_guard" survives a
 * reboot, while "kiln_cfg" writes on the very same boots do. The record
 * therefore lives in "kiln_cfg" now, alongside unit_pref/ramp_assist/
 * relay_cycles/kiln_cfg_store, under its own key. */
#define NVS_KEY_REC "bootguard"

/* The pre-2026-09-08 location, "boot_guard"/"count". Still READ (as a
 * fallback) so a board upgrading from an older firmware does not silently
 * forget how many unconfirmed boots it has had, and erased on a best-effort
 * basis whenever the new record is written. */
#define NVS_KEY_REC_LEGACY "count"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_REC);
NVS_KEY_LEN_CHECK(NVS_KEY_REC_LEGACY);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE_LEGACY);

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

/* ---------------------------------------------------------------------
 * STUCK-COUNTER ESCAPE (docs/audits/boot_guard_recovery_loop_2026-09-08.md)
 *
 * Observed on this board, 2026-09-08, with per-step instrumentation over
 * JTAG/serial: boot_guard_mark_healthy() wrote 0, and hal_kv_open()/
 * set_blob()/commit() ALL returned HAL_OK, and a reopened READ_ONLY handle
 * read the record straight back as boot_count=0 with a valid CRC -- and the
 * very next boot's load_count() read boot_count=3 again. Freshly created
 * probe keys written in the same partition on the same boots (both a u32
 * and a blob, in both the "boot_guard" and "kiln_cfg" namespaces)
 * incremented correctly across every one of those reboots, so NVS itself,
 * the partition, the key lengths and the write path are all fine: it is
 * this ONE pre-existing item, kiln_nvs/boot_guard/count, that never changes
 * in flash.
 *
 * Why the read-back "verification" added in 0b6e82b7 cannot see that:
 * nvs_open() does NOT re-read flash. NVS builds one in-RAM index per
 * partition at nvs_flash_init_partition() time and every handle on that
 * partition -- new handle, READ_ONLY handle, different namespace -- is
 * served from it. So a read-back after a write reports the value that was
 * just written whether or not it ever reached flash. Any verification that
 * lives inside the same boot as the write is structurally incapable of
 * detecting this failure; only the NEXT boot can.
 *
 * So carry exactly that across the reboot, in storage that is not NVS:
 * RTC slow memory, which survives a software reset/panic/watchdog reset and
 * is only lost on a power cycle. If a boot marked itself healthy (which
 * means NVS, the web server and the OTA routes were all up) and the next
 * boot still loads a non-zero count, the persisted counter is stuck and
 * must not be allowed to hold the board in recovery mode.
 *
 * This cannot let a genuinely reset-looping board out of recovery: the flag
 * is only ever set by a boot that actually reached boot_confirm_is_healthy()
 * and cleared its counter -- exactly the boots whose counter today's code
 * already resets to 0. The change is only in whether that reset is
 * BELIEVED when flash quietly refuses it.
 * --------------------------------------------------------------------- */
#define BOOT_GUARD_RTC_MAGIC 0x42474432u /* "BGD2" */

typedef struct {
    uint32_t magic;
    uint32_t marked_healthy; /* 1 if the PREVIOUS boot's mark_healthy() verified its clear */
} boot_guard_rtc_t;

/* RTC_NOINIT_ATTR: deliberately NOT zeroed by the startup code, so it
 * carries across a software reset. Garbage after a power-on is rejected by
 * the magic check below. */
RTC_NOINIT_ATTR static boot_guard_rtc_t s_bg_rtc;

/* True if the previous boot verified a clear (and therefore the counter
 * this boot just loaded should have been 0). Pure predicate over the two
 * inputs so test_boot_guard.c can exercise every combination directly. */
bool boot_guard_counter_is_stuck(uint32_t rtc_magic, uint32_t rtc_marked_healthy,
                                 uint32_t loaded_count)
{
    return rtc_magic == BOOT_GUARD_RTC_MAGIC && rtc_marked_healthy != 0u && loaded_count != 0u;
}

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
        /* Best effort: retire the stuck legacy item so it can never be read
         * again and stops occupying entries. HAL_NOT_FOUND is the ordinary
         * steady state (it is gone after the first successful write) and is
         * silent; a real failure is logged but never allowed to fail the
         * write -- on the board this was found on the legacy item resisted
         * erasure too, and that must not stop the new key from being
         * written. Not gated behind a once-per-boot flag on purpose: this
         * function runs a handful of times per boot at most, and a flag
         * would make the retirement depend on which call happened to run
         * first. */
        hal_kv_handle_t lh;
        if (hal_kv_open(&lh, NVS_NAMESPACE_LEGACY, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION)
            == HAL_OK) {
            hal_status_t lerr = hal_kv_erase_key(&lh, NVS_KEY_REC_LEGACY);
            if (lerr == HAL_OK) {
                (void)hal_kv_commit(&lh);
            } else if (lerr != HAL_NOT_FOUND) {
                ESP_LOGW(TAG, "could not erase the legacy boot-guard record '%s'/'%s': %s -- "
                              "harmless, nothing reads it once '%s'/'%s' exists",
                         NVS_NAMESPACE_LEGACY, NVS_KEY_REC_LEGACY, hal_status_to_name(lerr),
                         NVS_NAMESPACE, NVS_KEY_REC);
            }
            hal_kv_close(&lh);
        }
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
        /* Nothing at the current location yet -- either a board that has
         * never run this firmware, or the first boot after the 2026-09-08
         * move. Fall back to the old namespace once so an upgrade does not
         * reset the counter's history. See NVS_KEY_REC. */
        hal_kv_handle_t lh;
        hal_status_t lopen = hal_kv_open(&lh, NVS_NAMESPACE_LEGACY, HAL_KV_MODE_READ_ONLY,
                                          KILN_NVS_PARTITION);
        if (lopen != HAL_OK) {
            return 0;
        }
        len = sizeof(rec);
        err = hal_kv_get_blob(&lh, NVS_KEY_REC_LEGACY, &rec, &len);
        hal_kv_close(&lh);
        if (err == HAL_OK && len == sizeof(rec)) {
            ESP_LOGW(TAG, "boot-guard record read from the LEGACY location '%s'/'%s' -- it will be "
                          "rewritten under '%s'/'%s' (see boot_guard.c's comment on NVS_KEY_REC)",
                     NVS_NAMESPACE_LEGACY, NVS_KEY_REC_LEGACY, NVS_NAMESPACE, NVS_KEY_REC);
        }
    }
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

/* Strict read for boot_guard_get_persisted_count(): unlike load_count()
 * (which deliberately collapses every read failure -- open failure, get
 * error, wrong length, bad CRC/version -- down to a safe boot-time default
 * of 0), this distinguishes "genuinely never written" from "could not be
 * read". Conflating them let boot_guard_get_persisted_count() report
 * "persisted_count":0 while NVS was actually unreadable, which on the reset
 * route reads as a fabricated "cleared" sitting next to that same call's own
 * ok:false.
 *
 * Returns true with *out_count = 0 only when the record is genuinely absent
 * from BOTH the current and legacy namespaces (HAL_NOT_FOUND on the get, or
 * the legacy namespace failing to open at all) -- the same "never written"
 * case load_count()'s legacy fallback exists for. Returns false, leaving
 * *out_count untouched, on any other failure: an open error other than "the
 * namespace doesn't exist yet", a get error other than HAL_NOT_FOUND, a
 * length mismatch, or a failed record_is_valid() (bad CRC/version) in either
 * namespace.
 *
 * Logs at most once, at ESP_LOGW, and only for the CRC/version-fail path --
 * GET /api/boot_guard is unauthenticated and may be polled, so this must not
 * become a per-request log line the way load_count()'s own warning would if
 * reused here unchanged. */
static bool load_count_strict(uint32_t *out_count)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return false;
    }
    boot_guard_record_t rec;
    size_t len = sizeof(rec);
    err = hal_kv_get_blob(&h, NVS_KEY_REC, &rec, &len);
    hal_kv_close(&h);
    if (err == HAL_OK) {
        if (len != sizeof(rec)) {
            return false;
        }
        if (!record_is_valid(&rec)) {
            ESP_LOGW(TAG, "boot_guard_get_persisted_count: record failed its version/CRC check -- "
                          "reporting unreadable rather than a fabricated 0");
            return false;
        }
        *out_count = rec.boot_count;
        return true;
    }
    if (err != HAL_NOT_FOUND) {
        return false;
    }

    /* Not found at the current location -- check the legacy one, same as
     * load_count(). A missing legacy namespace is also a genuine "never
     * written" case, not an error. */
    hal_kv_handle_t lh;
    hal_status_t lopen = hal_kv_open(&lh, NVS_NAMESPACE_LEGACY, HAL_KV_MODE_READ_ONLY,
                                      KILN_NVS_PARTITION);
    if (lopen != HAL_OK) {
        *out_count = 0;
        return true;
    }
    len = sizeof(rec);
    err = hal_kv_get_blob(&lh, NVS_KEY_REC_LEGACY, &rec, &len);
    hal_kv_close(&lh);
    if (err == HAL_OK) {
        if (len != sizeof(rec)) {
            return false;
        }
        if (!record_is_valid(&rec)) {
            ESP_LOGW(TAG, "boot_guard_get_persisted_count: legacy record failed its version/CRC "
                          "check -- reporting unreadable rather than a fabricated 0");
            return false;
        }
        *out_count = rec.boot_count;
        return true;
    }
    if (err != HAL_NOT_FOUND) {
        return false;
    }
    *out_count = 0;
    return true;
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

    /* See the STUCK-COUNTER ESCAPE comment above. Read the RTC marker
     * BEFORE it is overwritten for this boot, and drop the loaded count to
     * 0 if the previous boot already proved the counter cannot be cleared
     * in flash -- that keeps the whole downstream decision (recovery_mode,
     * the new count, the banner) working off an honest number rather than
     * needing a second, parallel path. */
    bool stuck = boot_guard_counter_is_stuck(s_bg_rtc.magic, s_bg_rtc.marked_healthy, loaded);
    if (stuck) {
        ESP_LOGE(TAG, "boot-guard counter is STUCK: the previous boot verified a clear to 0 and "
                      "this boot still loaded %lu -- the persisted record is not actually being "
                      "updated in flash (see docs/audits/boot_guard_recovery_loop_2026-09-08.md). "
                      "Treating this boot as confirmed healthy rather than letting a counter that "
                      "cannot be cleared hold the board in recovery mode forever.",
                 (unsigned long)loaded);
        loaded = 0;
    }
    s_bg_rtc.magic = BOOT_GUARD_RTC_MAGIC;
    s_bg_rtc.marked_healthy = 0u; /* this boot has not marked itself healthy yet */

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

/* Shared by boot_guard_mark_healthy() and boot_guard_reset_counter(): does
 * the actual NVS work (write, verify, one bounded erase-then-retry) and
 * returns whether the clear is CONFIRMED in flash. Caller holds s_bg.lock
 * and is responsible for the s_bg/s_bg_rtc bookkeeping and logging that
 * differs between the two call sites (see each function's own comment for
 * why they differ). Pulled out 2026-09-08 (post-flash-recovery-foot-gun
 * audit) rather than duplicated, so the verify-then-retry sequence -- the
 * actual fix for the write-lies bug this file's big comment block
 * documents -- has exactly one implementation to keep correct. */
static bool clear_persisted_counter_verified_locked(void)
{
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
    if (!verified) {
        if (err != HAL_OK) {
            ESP_LOGW(TAG, "could not clear boot-guard counter: %s -- will retry next call",
                     hal_status_to_name(err));
        } else {
            /* The write call itself reported success, but
             * verify_persisted_count() did NOT confirm boot_count==0
             * immediately afterward -- exactly the failure mode that
             * bricked this board into a permanent recovery loop (that
             * function already logs the specific reason: unreadable,
             * invalid, or a different nonzero value). */
            ESP_LOGE(TAG, "boot-guard counter WRITE REPORTED SUCCESS BUT DID NOT VERIFY -- will "
                          "retry next call rather than trusting the write's own return code");
        }
    }
    return verified;
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
    bool verified = clear_persisted_counter_verified_locked();
    if (verified) {
        s_bg.healthy_marked = true;
        /* See the STUCK-COUNTER ESCAPE comment at the top of this file: this
         * is the half of the pair that the NEXT boot reads. Set only on a
         * verified clear, so a boot that never got healthy never arms it. */
        s_bg_rtc.magic = BOOT_GUARD_RTC_MAGIC;
        s_bg_rtc.marked_healthy = 1u;
    }
    xSemaphoreGive(s_bg.lock);

    if (verified) {
        ESP_LOGI(TAG, "boot-guard counter cleared and VERIFIED (read back as 0) -- this boot is "
                      "confirmed healthy");
    }
    return verified;
}

bool boot_guard_get_persisted_count(uint32_t *out_count)
{
    /* Read-only: only ever calls load_count_strict(), never persist_count()/
     * erase_then_persist_count() -- but a flash read still requires an
     * internal-RAM task stack, same as this module's write paths (flash
     * reads disable the cache too; see boot_guard.h's doc comment on this
     * function). Requires boot_guard_init() to have already run this boot --
     * s_bg.lock does not exist before that, and there is no NVS partition
     * handle to read from either.
     *
     * Deliberately NOT load_count(): that helper collapses every read
     * failure to a safe default of 0, which is correct for an ordinary
     * boot-time load but would fabricate a "persisted_count":0 here whenever
     * NVS is genuinely unreadable -- see load_count_strict()'s own comment. */
    if (!out_count) {
        return false;
    }
    if (!s_bg.initialized || !s_bg.lock) {
        return false;
    }
    xSemaphoreTake(s_bg.lock, portMAX_DELAY);
    uint32_t count = 0;
    bool ok = load_count_strict(&count);
    xSemaphoreGive(s_bg.lock);
    if (!ok) {
        return false;
    }
    *out_count = count;
    return true;
}

bool boot_guard_reset_counter(void)
{
    /* See docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md:
     * boot_confirm_is_healthy() requires nvs_report_capture()'s ONE-SHOT,
     * never-retried snapshot of ALL THREE NVS partitions (wifi_nvs/kiln_nvs/
     * profiles_nvs) to be mounted, sampled once early in
     * main_network_http_bringup(). A board that is otherwise completely
     * fine can sample that snapshot during a genuinely transient window --
     * most plausibly right after flash_firmware() resets the chip, before
     * every partition has finished mounting -- and if it does, that boot's
     * nvs_ok is wrong for the rest of the boot (nothing re-samples it), so
     * boot_guard_mark_healthy() is never even attempted and the counter
     * that boot climbs by one for a reason that has nothing to do with
     * whether the FIRMWARE can boot. A developer flashing several times in
     * a row during ordinary iteration can walk an entirely healthy board
     * into RECOVERY_MODE_BOOT_THRESHOLD this way.
     *
     * This function is the deliberate-flash escape hatch: unlike
     * boot_guard_mark_healthy(), it does NOT require s_bg.initialized (a
     * tool driving this from outside the board, e.g. over a future
     * authenticated HTTP route called from flash_firmware()'s verify step,
     * is asserting "I just flashed this board on purpose" independent of
     * whatever boot_confirm_is_healthy() would eventually decide) and does
     * NOT check s_bg.healthy_marked (a deliberate reset is idempotent to
     * call again, same as mark_healthy, but is not "the same event" as an
     * automatic health confirmation, so it does not short-circuit on that
     * flag). It DOES arm the same RTC stuck-counter marker mark_healthy()
     * does -- see boot_guard_counter_is_stuck()'s comment: that escape only
     * exists to believe a verified clear over a counter that flash refuses
     * to actually update, and a deliberate reset is exactly as much "a
     * verified clear" as an automatic one is. Sharing that arming is what
     * keeps this change from being able to combine with 0b5d9dad's marker
     * to produce a board where NEITHER path clears the stuck counter: both
     * paths funnel through the same clear_persisted_counter_verified_locked()
     * and the same RTC-arm, so there is only ever one "did the last clear
     * verify" fact for the next boot to trust, not two independently
     * maintained ones.
     *
     * Does NOT touch s_bg.recovery_mode: same as boot_guard_mark_healthy(),
     * this can only affect the NEXT boot's decision, never retroactively
     * un-decide the one currently running (boot_guard_is_recovery_mode()'s
     * own doc comment). A board already running in recovery mode this boot
     * stays in recovery mode this boot even after a successful reset; it
     * simply will not still be in recovery mode on the boot after. */
    if (!ensure_lock()) {
        return false;
    }
    xSemaphoreTake(s_bg.lock, portMAX_DELAY);
    bool verified = clear_persisted_counter_verified_locked();
    if (verified) {
        s_bg.healthy_marked = true;
        s_bg_rtc.magic = BOOT_GUARD_RTC_MAGIC;
        s_bg_rtc.marked_healthy = 1u;
    }
    xSemaphoreGive(s_bg.lock);

    if (verified) {
        ESP_LOGI(TAG, "boot-guard counter explicitly reset and VERIFIED (read back as 0) -- "
                      "recorded as a deliberate flash, not an automatic health confirmation");
    }
    return verified;
}
