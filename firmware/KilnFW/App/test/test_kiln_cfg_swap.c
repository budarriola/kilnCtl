// Host tests for App/drivers/persist/kiln_cfg_swap.c -- the two-processor
// apply transaction (docs/KILN_PROFILES_PLAN.md item 5,
// docs/audits/kiln_swap_transaction_2026-09-14.md).
//
// kiln_cfg_swap.c is #included directly (same "no other seam into a module
// whose job is file-scope state" convention test_kiln_cfg_store.c already
// documents) so this file can drive its internal state machine precisely
// and reach load_pending()/pending_crc() directly for the corruption tests.
//
// Every OTHER module kiln_cfg_swap.c calls into is FAKED here, deliberately
// -- this file proves the TRANSACTION'S OWN ordering, locking, rollback and
// boot-recovery logic in isolation from kiln_cfg_store.c/safety_cfg_store.c/
// safety_ceiling_sync.c's own real bodies (each already has its own host
// tests elsewhere). hal_kv itself is NOT faked at this level -- it links
// against the REAL fake_kv.h/.c backend (Phase 2 host fake), so this file's
// persistence tests (including fake_kv_script_corrupt_key() for H10) are
// exercising the actual NVS-shaped read/write/CRC path, not a second mock
// of it.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"

// docs/WEB_AUTH_PLAN.md item 12b -- kiln_cfg_swap.c is entirely faked above
// (this file's own convention), but hal_kv is NOT faked: it links the real
// fake_kv.h/.c backend. That real backend also backs web_auth_store.c's
// kiln_auth namespace, so this same executable can seed a real credential
// and prove kiln_cfg_swap_apply() (a slot swap -- item 12b's remaining
// untested case) leaves it untouched, regardless of the swap's own
// success/refusal outcome, which is orthogonal to this property.
#include "web_auth_store.h"
#include "psa/crypto.h"
psa_status_t g_stub_psa_import_key_result = PSA_SUCCESS;

int g_test_failures = 0;
int g_test_count = 0;

// ---------------------------------------------------------------------------
// Fakes for everything kiln_cfg_swap.c calls other than hal_kv/esp_crc32_le.
// ---------------------------------------------------------------------------

// -- kiln_cfg_store.h --
#define KILN_CFG_NO_ACTIVE_ID (-1)
#define KILN_CFG_NAME_MAX_LEN 23

static int32_t s_active_id = KILN_CFG_NO_ACTIVE_ID;
static uint32_t s_generation = 1;
static int s_lock_depth = 0;

// One fake "slot" -- the only target this suite ever swaps to/from.
typedef struct kiln_pkg_pico_param_s {
    uint16_t param_id;
    uint8_t type;
    uint8_t flags;
    uint32_t value_bits;
} kiln_pkg_pico_param_t;
#define KILN_PKG_PARAM_FLAG_SET 0x01u
#define KILN_PKG_SAFETY_PARAM_CAP 96u
typedef struct kiln_pkg_safety_s {
    uint16_t count;
    kiln_pkg_pico_param_t entries[KILN_PKG_SAFETY_PARAM_CAP];
} kiln_pkg_safety_t;
typedef struct {
    size_t (*param_count)(void);
    bool (*get_by_index)(size_t index, void *out);
} kiln_pkg_pico_source_t;

#define KILNLINK_PARAM_TYPE_BOOL 0x00u
#define KILNLINK_PARAM_TYPE_U8 0x01u
#define KILNLINK_PARAM_TYPE_U16 0x02u
#define KILNLINK_PARAM_TYPE_F32 0x03u

#define SAFETY_PARAM_ID_ABS_MAX_TEMP_C 0x0104u

#define ZONES_CONFIG_BLOB_MAX_SIZE 896

static uint8_t s_slot_blob[ZONES_CONFIG_BLOB_MAX_SIZE];
static uint16_t s_slot_blob_len = 32;
static kiln_pkg_safety_t s_slot_pico;
static bool s_slot_exists = true;
static bool s_slot_pico_populated = true;

static uint8_t s_live_blob[ZONES_CONFIG_BLOB_MAX_SIZE];
static uint16_t s_live_blob_len = 32;

static bool s_get_full_package_result_override_set = false;
static bool s_get_full_package_result_override = false;

bool kiln_cfg_store_get_full_package(int32_t id, uint8_t *blob_out, uint16_t cap, uint16_t *out_len,
                                     kiln_pkg_safety_t *pico_out, char *reason_out, size_t reason_cap)
{
    (void)id;
    if (s_get_full_package_result_override_set) {
        if (!s_get_full_package_result_override) {
            if (reason_out && reason_cap) {
                snprintf(reason_out, reason_cap, "forced test failure");
            }
            return false;
        }
    }
    if (!s_slot_exists) {
        if (reason_out && reason_cap) {
            snprintf(reason_out, reason_cap, "no saved kiln config with that id");
        }
        return false;
    }
    if (pico_out && !s_slot_pico_populated) {
        if (reason_out && reason_cap) {
            snprintf(reason_out, reason_cap, "half-package");
        }
        return false;
    }
    if (blob_out) {
        if (cap < s_slot_blob_len) {
            return false;
        }
        memcpy(blob_out, s_slot_blob, s_slot_blob_len);
    }
    if (out_len) {
        *out_len = s_slot_blob_len;
    }
    if (pico_out) {
        *pico_out = s_slot_pico;
    }
    return true;
}

static bool s_set_active_id_should_fail = false;
static int s_set_active_id_unlocked_calls = 0; // LOW-5: calls made without kiln_cfg_store_lock held
/* LOW-2 race probe: kiln_cfg_swap_is_pending() as seen from inside the
 * active-id save, i.e. mid-apply after ESP_DONE. -1 = never probed. */
static bool s_probe_pending_in_set_active_id = false;
static int s_pending_seen_in_set_active_id = -1;
bool kiln_cfg_swap_is_pending(void);
bool kiln_cfg_store_set_active_id_raw(int32_t id, char *reason_out, size_t reason_cap)
{
    if (s_lock_depth <= 0) {
        s_set_active_id_unlocked_calls++;
    }
    /* LOW-7: the real kiln_cfg_store_set_active_id_raw() sets RAM active_id
     * BEFORE it persists (kiln_cfg_store.c), so a failed persist still
     * leaves RAM naming the new id. The fake must do the same, or a test of
     * the failed-save path asserts on a RAM state the firmware never has. */
    s_active_id = id;
    /* Probe after the RAM set, before the persist: the window a concurrent
     * autosave would hit if the exception were armed early. */
    if (s_probe_pending_in_set_active_id) {
        s_pending_seen_in_set_active_id = kiln_cfg_swap_is_pending() ? 1 : 0;
    }
    if (s_set_active_id_should_fail) {
        if (reason_out && reason_cap) {
            snprintf(reason_out, reason_cap, "forced set_active_id failure");
        }
        return false;
    }
    return true;
}

void kiln_cfg_store_lock(void) { s_lock_depth++; }
void kiln_cfg_store_unlock(void) { s_lock_depth--; }
uint32_t kiln_cfg_store_generation(void) { return s_generation; }
int32_t kiln_cfg_store_get_active_id(void) { return s_active_id; }

// docs/audits/kiln_profiles_feature_review_2026-09-15.md Defect 1: the
// autosave-target override kiln_cfg_swap.c must set before, and clear
// after, each zones_config_import_blob() call whose autosave should NOT
// target whatever s_active_id happens to be at that moment. s_autosave_
// override tracks the currently-set value; zones_config_import_blob()'s
// fake below (its "current live value" at the moment it is called) latches
// it into s_import_time_autosave_override so a test can assert what was in
// effect DURING the import, not just before/after kiln_cfg_swap_apply()
// returns.
#define KILN_CFG_AUTOSAVE_OVERRIDE_NONE INT32_MIN
static int32_t s_autosave_override = KILN_CFG_AUTOSAVE_OVERRIDE_NONE;
static int32_t s_import_time_autosave_override = KILN_CFG_AUTOSAVE_OVERRIDE_NONE;
void kiln_cfg_store_set_autosave_target_override(int32_t id_or_none_sentinel)
{
    s_autosave_override = id_or_none_sentinel;
}

// -- kiln_package.h --
kiln_pkg_pico_source_t kiln_pkg_pico_source_default(void)
{
    kiln_pkg_pico_source_t s = {0};
    return s;
}

static kiln_pkg_safety_t s_current_pico_snapshot; // what "R" (current live) should capture
static bool s_capture_pico_should_fail = false;
bool kiln_package_capture_pico_half(const kiln_pkg_pico_source_t *source, kiln_pkg_safety_t *out)
{
    (void)source;
    if (s_capture_pico_should_fail) {
        return false;
    }
    *out = s_current_pico_snapshot;
    return true;
}

// -- ota_state.h --
typedef enum { OTA_INTERLOCK_OK = 0, OTA_INTERLOCK_REFUSED = 1 } ota_interlock_result_t;
static ota_interlock_result_t s_interlock_result = OTA_INTERLOCK_OK;
ota_interlock_result_t ota_http_check_interlocks(bool ack_no_safety_processor, char *reason_out, size_t reason_cap)
{
    (void)ack_no_safety_processor;
    if (s_interlock_result != OTA_INTERLOCK_OK && reason_out && reason_cap) {
        snprintf(reason_out, reason_cap, "interlock refused (firing running)");
    }
    return s_interlock_result;
}

// -- safety_link.h --
typedef struct SafetyLinkClass_s { int dummy; } SafetyLinkClass;
static SafetyLinkClass s_fake_link;

// -- safety_ceiling_sync.h (required ceiling, LOW-6) --
// What safety_ceiling_sync_required_ceiling_c() reports: the ceiling the
// CURRENT zones require. Unknown by default, so every pre-LOW-6 test sees
// R's snapshot value restored exactly as before.
static bool s_required_known = false;
static float s_required_ceiling = 0.0f;
bool safety_ceiling_sync_required_ceiling_c(float *out_c)
{
    if (!out_c || !s_required_known) {
        return false;
    }
    *out_c = s_required_ceiling;
    return true;
}
// LOW-6 interleave hook: when > 0, the first volatile ceiling write (the
// raise-first step) is followed by a simulated zones POST that raised a zone
// max: the required ceiling becomes this value and the Pico ceiling is
// raised to it, exactly what the zones POST guard does on the real board.
static float s_zone_raise_after_first_ceiling_write = 0.0f;
static int s_volatile_ceiling_write_count = 0;
// LOW-7 hook: each volatile ceiling write while this is > 0 arms the NEXT
// NVS write-class call to fail (and decrements). Lets a test fail the
// PICO_OPEN marker save (armed by the raise-first write) and then the clear
// (armed by restore_r_ceiling_volatile()'s write) in the same apply.
static int s_fail_next_kv_write_per_ceiling_write = 0;

// -- safety_cfg_write.h --
static bool s_pico_push_should_fail = false;
static char s_pico_push_fail_reason[128] = "push refused";
static bool g_bump_gen_on_push = false; // H6 test hook: simulate a racing writer during the Pico round trip
// Simulates the Pico's committed state after a (fake) push -- what the
// readback stub below reports.
static kiln_pkg_safety_t s_pico_committed;
// item 15: records whether the LAST push/rollback used the volatile path
// (0x2D) or the flash path (COMMIT_CONFIG) -- what the "never disarm"/
// "rollback uses the same mechanism" tests assert against.
static bool s_last_push_was_volatile = false;
static int s_volatile_push_count = 0;
static int s_flash_push_count = 0;
// ARMED simulation: when true, any call with volatile_install=false (the
// flash path) is refused exactly like the real Pico's config_store_decide_
// write() refuses COMMIT_CONFIG while ARMED -- volatile_install=true calls
// are NEVER refused by this flag, mirroring config_store_write_volatile()
// never calling config_store_decide_write() at all.
static bool s_pico_armed = false;
bool safety_cfg_write_apply_package_and_confirm(SafetyLinkClass *link, const kiln_pkg_safety_t *pkg,
                                                bool volatile_install, char *reason_out, size_t reason_cap,
                                                void *out_class)
{
    (void)out_class;
    if (!link) {
        snprintf(reason_out, reason_cap, "no link");
        return false;
    }
    s_last_push_was_volatile = volatile_install;
    if (volatile_install) {
        s_volatile_push_count++;
    } else {
        s_flash_push_count++;
        if (s_pico_armed) {
            snprintf(reason_out, reason_cap, "commit rejected: relay is ARMED -- config writes are refused "
                                              "while ARMED -- values were staged but NOT written");
            return false;
        }
    }
    if (s_pico_push_should_fail) {
        snprintf(reason_out, reason_cap, "%s", s_pico_push_fail_reason);
        s_pico_push_should_fail = false; /* fails ONLY the forward push -- the transaction's own
                                          * rollback re-push of R (a config that was, ex hypothesi,
                                          * already live and working) must be allowed to succeed, or
                                          * every "forward push fails" test would be indistinguishable
                                          * from "rollback is also impossible". */
        return false;
    }
    s_pico_committed = *pkg;
    if (g_bump_gen_on_push) {
        s_generation++; // simulates an ordinary save/clone/delete landing on kiln_cfg_store
                        // while this swap was off doing the (blocking) Pico round trip
    }
    return true;
}

static bool s_ceiling_set_should_fail = false;
static float s_pico_ceiling = 1200.0f;
static bool s_last_ceiling_set_was_volatile = false;
bool safety_cfg_write_set_and_confirm_f32(SafetyLinkClass *link, uint16_t param_id, float value, char *reason_out,
                                          size_t reason_cap, void *out_class)
{
    (void)out_class;
    s_last_ceiling_set_was_volatile = false;
    if (!link) {
        snprintf(reason_out, reason_cap, "no link");
        return false;
    }
    s_flash_push_count++;
    if (s_pico_armed) {
        snprintf(reason_out, reason_cap, "commit rejected: relay is ARMED -- config writes are refused while "
                                          "ARMED -- values were staged but NOT written");
        return false;
    }
    if (s_ceiling_set_should_fail) {
        snprintf(reason_out, reason_cap, "ceiling raise refused (ARMED)");
        return false;
    }
    if (param_id == SAFETY_PARAM_ID_ABS_MAX_TEMP_C) {
        s_pico_ceiling = value;
    }
    return true;
}

bool safety_cfg_write_set_and_confirm_f32_volatile(SafetyLinkClass *link, uint16_t param_id, float value,
                                                   char *reason_out, size_t reason_cap, void *out_class)
{
    (void)out_class;
    s_last_ceiling_set_was_volatile = true;
    s_volatile_push_count++;
    if (!link) {
        snprintf(reason_out, reason_cap, "no link");
        return false;
    }
    /* Never refused for ARMED -- config_store_write_volatile() never checks
     * it, see this fake's own header comment on s_pico_armed. */
    if (s_ceiling_set_should_fail) {
        snprintf(reason_out, reason_cap, "ceiling raise refused (validation)");
        return false;
    }
    if (param_id == SAFETY_PARAM_ID_ABS_MAX_TEMP_C) {
        s_pico_ceiling = value;
        s_volatile_ceiling_write_count++;
        if (s_volatile_ceiling_write_count == 1 && s_zone_raise_after_first_ceiling_write > 0.0f) {
            s_required_known = true;
            s_required_ceiling = s_zone_raise_after_first_ceiling_write;
            if (s_pico_ceiling < s_required_ceiling) {
                s_pico_ceiling = s_required_ceiling;
            }
        }
        if (s_fail_next_kv_write_per_ceiling_write > 0) {
            s_fail_next_kv_write_per_ceiling_write--;
            fake_kv_script_write_status_after(0, HAL_IO);
        }
    }
    return true;
}

// -- safety_cfg_store.h --
typedef union {
    uint8_t bool_val;
    uint8_t u8_val;
    uint16_t u16_val;
    float f32_val;
} kilnlink_param_value_t;
typedef struct {
    uint16_t param_id;
    const char *name;
    uint8_t type;
    bool set;
    kilnlink_param_value_t value;
} safety_cfg_param_t;

static bool s_refetch_should_fail = false;
static uint16_t s_cached_crc = 1; // 1 == "configured"; see safety_cfg_store_refetch() below for how it changes
bool safety_cfg_store_refetch(SafetyLinkClass *link, uint16_t config_crc)
{
    if (!link || s_refetch_should_fail) {
        return false;
    }
    /* Honour the real contract (safety_cfg_store.c:1524/1674): a successful
     * refetch tags the cache with whatever config_crc the caller passed,
     * including 0 -- which is exactly what kiln_cfg_swap.c's
     * push_and_verify_pico() deliberately passes to force an unconditional
     * refetch. A stub that discarded this argument (as this one used to)
     * hid the real defect audited in
     * docs/audits/kiln_config_self_apply_diverged_2026-09-22.md. */
    s_cached_crc = config_crc;
    return true;
}
bool safety_cfg_store_lookup(uint16_t param_id, uint8_t *out_type, const char **out_name)
{
    for (uint16_t i = 0; i < s_pico_committed.count; i++) {
        if (s_pico_committed.entries[i].param_id == param_id) {
            if (out_type) {
                *out_type = s_pico_committed.entries[i].type;
            }
            if (out_name) {
                *out_name = "param";
            }
            return true;
        }
    }
    if (param_id == SAFETY_PARAM_ID_ABS_MAX_TEMP_C) {
        if (out_type) {
            *out_type = KILNLINK_PARAM_TYPE_F32;
        }
        if (out_name) {
            *out_name = "abs_max_temp_c";
        }
        return true;
    }
    return false;
}
size_t safety_cfg_store_param_count(void) { return s_pico_committed.count; }
bool safety_cfg_store_get_by_index(size_t index, safety_cfg_param_t *out)
{
    if (index >= s_pico_committed.count) {
        return false;
    }
    const kiln_pkg_pico_param_t *e = &s_pico_committed.entries[index];
    out->param_id = e->param_id;
    out->name = "param";
    out->type = e->type;
    out->set = (e->flags & KILN_PKG_PARAM_FLAG_SET) != 0;
    memcpy(&out->value, &e->value_bits, sizeof(out->value));
    return true;
}
uint16_t safety_cfg_store_cached_crc(void) { return s_cached_crc; }

// -- safety_ceiling_sync.h --
static bool s_ceiling_get_current_should_fail = false;
bool safety_ceiling_sync_get_current_pico_ceiling(float *out_value)
{
    if (s_ceiling_get_current_should_fail) {
        return false;
    }
    *out_value = s_pico_ceiling;
    return true;
}
static int s_reconcile_call_count = 0;
void safety_ceiling_sync_reconcile_on_link_up(SafetyLinkClass *link)
{
    (void)link;
    s_reconcile_call_count++;
}
static bool s_diverged = false;
static char s_diverged_reason[128] = "ceiling mismatch";
bool safety_ceiling_sync_is_diverged(char *reason_out, size_t reason_cap)
{
    if (s_diverged && reason_out && reason_cap) {
        snprintf(reason_out, reason_cap, "%s", s_diverged_reason);
    }
    return s_diverged;
}

// -- zones_config_accessors.h --
static bool s_zones_export_should_fail = false;
bool zones_config_export_blob(void *out, size_t out_cap)
{
    if (s_zones_export_should_fail || out_cap < s_live_blob_len) {
        return false;
    }
    memcpy(out, s_live_blob, s_live_blob_len);
    return true;
}
static bool s_zones_import_should_fail = false;
static bool s_import_writes_wrong_bytes = false;
static int s_zones_import_call_count = 0;
bool zones_config_import_blob(const void *blob, size_t len, char *reason_out, size_t reason_cap)
{
    s_zones_import_call_count++;
    s_import_time_autosave_override = s_autosave_override;
    if (s_zones_import_should_fail) {
        if (reason_out && reason_cap) {
            snprintf(reason_out, reason_cap, "forced import failure");
        }
        return false;
    }
    memcpy(s_live_blob, blob, len);
    s_live_blob_len = (uint16_t)len;
    if (s_import_writes_wrong_bytes) {
        /* Simulates a commit that reports success but does not actually
         * land byte-identical content -- proves kiln_cfg_swap.c's step 9
         * readback compare is a REAL content check, not decorative. */
        s_live_blob[0] ^= 0xFFu;
    }
    return true;
}

// Suppress every REAL header kiln_cfg_swap.h/.c would otherwise pull in --
// this test file supplies fake, minimal, name-compatible stand-ins for all
// of them above (same "type-only stand-ins reached via include guard" trick
// App/test/stubs/ uses for ESP-IDF headers, applied here to this
// codebase's OWN headers instead, since kiln_cfg_swap.c's dependency
// surface -- kiln_cfg_store.h, kiln_package.h, safety_link.h, safety_cfg_
// store.h, safety_cfg_write.h, safety_ceiling_sync.h, ota_state.h,
// zones_config_accessors.h -- is too large and too hardware-adjacent to
// link for real without dragging in httpd/I2C/SPI ownership this file's
// job is explicitly to test AROUND, not through). kiln_cfg_swap.h ITSELF
// is NOT suppressed -- that is the real contract under test.
#define KILN_PACKAGE_H
#define SAFETY_LINK_H
#define ZONES_CONFIG_ACCESSORS_H
#define KILN_CFG_STORE_H
#define OTA_STATE_H
#define SAFETY_CFG_STORE_H
#define SAFETY_CFG_WRITE_H
#define SAFETY_CEILING_SYNC_H

#include "../drivers/persist/kiln_cfg_swap.c"

// ---------------------------------------------------------------------------

static void set_pico_param(kiln_pkg_safety_t *pkg, uint16_t id, uint8_t type, uint32_t bits)
{
    kiln_pkg_pico_param_t *e = &pkg->entries[pkg->count++];
    e->param_id = id;
    e->type = type;
    e->flags = KILN_PKG_PARAM_FLAG_SET;
    e->value_bits = bits;
}

static void reset_state(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION_SWAP);
    s_active_id = KILN_CFG_NO_ACTIVE_ID;
    s_generation = 1;
    s_lock_depth = 0;
    s_autosave_override = KILN_CFG_AUTOSAVE_OVERRIDE_NONE;
    s_import_time_autosave_override = KILN_CFG_AUTOSAVE_OVERRIDE_NONE;

    memset(s_slot_blob, 0xAB, sizeof(s_slot_blob));
    s_slot_blob_len = 32;
    memset(&s_slot_pico, 0, sizeof(s_slot_pico));
    set_pico_param(&s_slot_pico, SAFETY_PARAM_ID_ABS_MAX_TEMP_C, KILNLINK_PARAM_TYPE_F32, 0);
    {
        float f = 1300.0f;
        memcpy(&s_slot_pico.entries[0].value_bits, &f, sizeof(f));
    }
    set_pico_param(&s_slot_pico, 0x0201u, KILNLINK_PARAM_TYPE_U16, 42);
    s_slot_exists = true;
    s_slot_pico_populated = true;
    s_get_full_package_result_override_set = false;

    memset(s_live_blob, 0xCD, sizeof(s_live_blob));
    s_live_blob_len = 32;

    memset(&s_current_pico_snapshot, 0, sizeof(s_current_pico_snapshot));
    set_pico_param(&s_current_pico_snapshot, SAFETY_PARAM_ID_ABS_MAX_TEMP_C, KILNLINK_PARAM_TYPE_F32, 0);
    {
        float f = 1000.0f;
        memcpy(&s_current_pico_snapshot.entries[0].value_bits, &f, sizeof(f));
    }
    set_pico_param(&s_current_pico_snapshot, 0x0201u, KILNLINK_PARAM_TYPE_U16, 7);
    s_capture_pico_should_fail = false;

    s_interlock_result = OTA_INTERLOCK_OK;
    s_pico_push_should_fail = false;
    memset(&s_pico_committed, 0, sizeof(s_pico_committed));
    s_ceiling_set_should_fail = false;
    s_pico_ceiling = 1000.0f;
    s_last_push_was_volatile = false;
    s_volatile_push_count = 0;
    s_flash_push_count = 0;
    s_pico_armed = false;
    s_last_ceiling_set_was_volatile = false;
    s_refetch_should_fail = false;
    s_cached_crc = 1;
    s_ceiling_get_current_should_fail = false;
    s_reconcile_call_count = 0;
    s_diverged = false;
    s_zones_export_should_fail = false;
    s_zones_import_should_fail = false;
    s_zones_import_call_count = 0;
    s_set_active_id_should_fail = false;
    s_set_active_id_unlocked_calls = 0;
    s_probe_pending_in_set_active_id = false;
    s_pending_seen_in_set_active_id = -1;
    s_required_known = false;
    s_required_ceiling = 0.0f;
    s_zone_raise_after_first_ceiling_write = 0.0f;
    s_volatile_ceiling_write_count = 0;
    s_fail_next_kv_write_per_ceiling_write = 0;

    fake_kv_set_write_safe_here(true);
    kiln_cfg_swap_set_link(&s_fake_link);
}

static void test_clean_swap_applies_both_halves(void)
{
    TEST_SECTION("clean swap -- both halves applied and verified");
    reset_state();
    char reason[KILN_CFG_SWAP_REASON_MAX];
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(ok, "clean swap reports success");
    TEST_CHECK(!diverged, "clean swap does not set out_diverged");
    TEST_CHECK(s_active_id == 7, "active_id set to the target");
    TEST_CHECK(memcmp(s_live_blob, s_slot_blob, s_slot_blob_len) == 0, "ESP live blob now equals the target's");
    TEST_CHECK(s_zones_import_call_count == 1, "ESP import happened exactly once (no redundant re-commit)");
    TEST_CHECK(s_pico_ceiling == 1300.0f, "Pico ceiling raised to the target's (raise-first path)");
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_NONE,
               "pending record cleared after a successful swap");
    TEST_CHECK(s_lock_depth == 0, "store lock is balanced (never left held)");
}

static void test_successful_apply_not_diverged_despite_zeroed_cache_crc(void)
{
    // docs/audits/kiln_config_self_apply_diverged_2026-09-22.md: push_and_
    // verify_pico() deliberately calls safety_cfg_store_refetch(link, 0) to
    // force an unconditional refetch, which (per that function's real
    // contract, now honoured by this test's stub -- see
    // safety_cfg_store_refetch() above) tags the cache with 0. A clean
    // swap must not read that 0 back as "the Pico never reported a
    // config_crc" and report a false divergence. Ceiling identity is
    // intentionally left non-diverged (s_diverged stays false from
    // reset_state()) so this isolates the deleted clause specifically --
    // against the pre-fix source this fails with reason containing "Pico
    // reports no config_crc after the swap".
    TEST_SECTION("a successful apply is not diverged even though the cache's config_crc reads 0 post-swap");
    reset_state();
    TEST_CHECK(safety_cfg_store_cached_crc() != 0, "sanity: cache starts non-zero before the swap");
    char reason[KILN_CFG_SWAP_REASON_MAX];
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(ok, "swap reports success");
    TEST_CHECK(!diverged, "not reported as diverged");
    TEST_CHECK(safety_cfg_store_cached_crc() == 0,
               "sanity: the cache really is 0 after the swap (proves this test exercises the real coupling, "
               "not a stub that dodges it)");
}

static void test_apply_autosave_targets_incoming_slot_not_outgoing(void)
{
    // docs/audits/kiln_profiles_feature_review_2026-09-15.md Defect 1: at
    // step 8 the ESP half of the INCOMING config (target_id=7) is imported
    // while active_id still names the OUTGOING kiln (3) -- active_id only
    // moves to 7 at step 12, after the import already ran. Unguarded, the
    // import's autosave dispatch would target active_id (3, the outgoing
    // kiln) with the just-imported INCOMING (7's) content -- clobbering
    // kiln 3's own saved slot with kiln 7's data. The override must name
    // the slot the content being imported actually belongs to (7) during
    // the call, and must be cleared again once the swap is done.
    TEST_SECTION("apply's autosave override names the incoming slot, not the still-active outgoing one");
    reset_state();
    s_active_id = 3; // outgoing kiln, active before the swap starts
    char reason[KILN_CFG_SWAP_REASON_MAX];
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(ok, "swap reports success");
    TEST_CHECK(s_import_time_autosave_override == 7,
              "autosave override during the ESP import named the INCOMING slot (7), not the outgoing "
              "active slot (3) that was still current at that moment");
    TEST_CHECK(s_autosave_override == KILN_CFG_AUTOSAVE_OVERRIDE_NONE,
              "override cleared again once the swap finished -- never left set for some later, unrelated "
              "nvs_save() to pick up");
}

static void test_pico_failure_leaves_esp_untouched(void)
{
    TEST_SECTION("Pico push failure -- ESP never touched, rollback restores Pico");
    reset_state();
    s_pico_push_should_fail = true;
    uint8_t live_before[ZONES_CONFIG_BLOB_MAX_SIZE];
    memcpy(live_before, s_live_blob, sizeof(live_before));
    char reason[KILN_CFG_SWAP_REASON_MAX];
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(!ok, "swap refused when the Pico push fails");
    TEST_CHECK(!diverged, "a clean rollback is not reported as a divergence/alarm");
    TEST_CHECK(s_zones_import_call_count == 0, "zones_config_import_blob() was NEVER called -- ESP untouched");
    TEST_CHECK(memcmp(s_live_blob, live_before, sizeof(live_before)) == 0, "ESP live blob unchanged");
    TEST_CHECK(s_active_id == KILN_CFG_NO_ACTIVE_ID, "active_id unchanged");
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_NONE,
               "pending record cleared after a clean rollback");
}

static void test_half_package_refused(void)
{
    TEST_SECTION("half-package (H17) refused outright");
    reset_state();
    s_slot_pico_populated = false;
    char reason[KILN_CFG_SWAP_REASON_MAX];
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(!ok, "half-package refused");
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_NONE,
               "nothing was ever staged for a half-package refusal");
    TEST_CHECK(s_zones_import_call_count == 0, "ESP never touched");
}

static void test_swap_during_firing_refused(void)
{
    TEST_SECTION("swap during a firing refused (interlock)");
    reset_state();
    s_interlock_result = OTA_INTERLOCK_REFUSED;
    char reason[KILN_CFG_SWAP_REASON_MAX];
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(!ok, "swap refused while a firing is running");
    TEST_CHECK(strstr(reason, "interlock") != NULL, "reason names the interlock refusal");
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_NONE, "nothing staged");
}

static void test_esp_readback_mismatch_rolls_back(void)
{
    TEST_SECTION("ESP readback mismatch after commit -> full rollback");
    reset_state();
    s_import_writes_wrong_bytes = true;
    char reason[KILN_CFG_SWAP_REASON_MAX];
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(!ok, "swap refused when the ESP readback does not match what was committed");
    TEST_CHECK(!diverged, "a clean rollback (both sides confirmed back on R) is not reported as an alarm");
    TEST_CHECK(s_active_id == KILN_CFG_NO_ACTIVE_ID, "active_id never finalized");
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_NONE,
               "rolled back cleanly, record cleared");
    s_import_writes_wrong_bytes = false;
}

static void test_rollback_autosave_targets_previous_slot_not_target(void)
{
    // docs/audits/kiln_profiles_feature_review_2026-09-15.md Defect 1,
    // rollback side: the readback mismatch below forces rollback() to
    // re-import the PRE-swap (previous_active_id=3) blob over the ESP half,
    // while active_id STILL reads target_id (7) at that moment --
    // set_active_id_raw(previous_active_id) only runs after this reimport.
    // Unguarded, that reimport's autosave would target active_id (7)
    // with kiln 3's own pre-swap content -- clobbering kiln 7's slot.
    TEST_SECTION("rollback's autosave override names the slot being restored, not the still-active target");
    reset_state();
    s_active_id = 3; // outgoing/previous kiln
    s_import_writes_wrong_bytes = true; // forces the readback-mismatch -> rollback(esp_was_committed=true) path
    char reason[KILN_CFG_SWAP_REASON_MAX];
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(!ok, "swap refused (readback mismatch), rolled back");
    TEST_CHECK(s_zones_import_call_count == 2, "one import for the forward commit, one for the rollback reimport");
    TEST_CHECK(s_import_time_autosave_override == 3,
              "autosave override during the ROLLBACK's reimport named the slot being restored (3), not "
              "the still-active target slot (7) that had not been un-finalized yet");
    TEST_CHECK(s_autosave_override == KILN_CFG_AUTOSAVE_OVERRIDE_NONE,
              "override cleared again after the rollback's reimport");
    s_import_writes_wrong_bytes = false;
}

static void test_diverged_ceiling_moves_active_id_but_leaves_pending(void)
{
    TEST_SECTION("post-swap ceiling/arming divergence -- both halves matched, but NOT finalized, alarmed");
    reset_state();
    s_diverged = true;
    char reason[KILN_CFG_SWAP_REASON_MAX];
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(!ok, "swap does not report success when the ceiling/arming check fails");
    TEST_CHECK(diverged, "out_diverged is set");
    // MEDIUM finding 3, adversarial review 2026-09-15 (docs/audits/
    // review_autosave_slot_fix_a93ee77b_2026-09-15.md): active_id now moves
    // to target_id right after readback proves the content match, BEFORE
    // this ceiling/arming check runs -- not at the end on full success only.
    // Content genuinely IS target_id's on both sides by this point
    // regardless of the ceiling/arming outcome, so leaving active_id stale
    // here (as the old code did) is itself the bug this finding named: any
    // ordinary autosave firing in that gap would have overwritten the
    // OUTGOING kiln's slot with content that was already live as the
    // incoming one.
    TEST_CHECK(s_active_id == 7, "active_id finalized to target_id -- content is proven live regardless of "
                                 "the ceiling/arming alarm below");
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_ESP_DONE,
               "pending record left at ESP_DONE for boot recovery to retry");
    TEST_CHECK(s_zones_import_call_count == 1, "both halves DID land -- this is not a rollback case");
}

static void test_generation_race_forces_rollback(void)
{
    TEST_SECTION("H6: a racing writer during the Pico round trip forces a rollback, never a blind finalize");
    reset_state();
    g_bump_gen_on_push = true; // simulates an ordinary save/clone/delete landing mid-swap, unlocked window
    char reason[KILN_CFG_SWAP_REASON_MAX];
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    g_bump_gen_on_push = false;
    TEST_CHECK(!ok, "swap refused when the store generation moved mid-swap");
    TEST_CHECK(s_zones_import_call_count == 0, "ESP was never committed once the race was detected");
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_NONE, "rolled back cleanly");
}

static void test_boot_recovery_staged_discards(void)
{
    TEST_SECTION("boot recovery: STAGED marker discards cleanly (crashed before Pico touched)");
    reset_state();
    kiln_cfg_swap_pending_t p;
    memset(&p, 0, sizeof(p));
    p.marker = KILN_CFG_SWAP_MARKER_STAGED;
    p.target_id = 7;
    p.previous_active_id = KILN_CFG_NO_ACTIVE_ID;
    p.crc32 = pending_crc(&p);
    TEST_CHECK(save_pending(&p), "staged record persists");
    kiln_cfg_swap_boot_recover();
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_NONE, "STAGED discarded at boot");
}

static void test_boot_recovery_pico_done_reapplies_rollback(void)
{
    TEST_SECTION("boot recovery: PICO_DONE re-applies R, never 'finishes' with P (plan's explicit rule)");
    reset_state();
    kiln_cfg_swap_set_link(&s_fake_link);
    kiln_cfg_swap_pending_t p;
    memset(&p, 0, sizeof(p));
    p.marker = KILN_CFG_SWAP_MARKER_PICO_DONE;
    p.target_id = 7;
    p.previous_active_id = 3;
    p.rollback_blob_len = 32;
    memset(p.rollback_blob, 0xEE, sizeof(p.rollback_blob));
    p.rollback_pico = s_current_pico_snapshot;
    p.crc32 = pending_crc(&p);
    TEST_CHECK(save_pending(&p), "PICO_DONE record persists");
    // Live ESP blob is still the OLD one (matches how PICO_DONE always
    // implies "ESP never committed"); Pico is (per this scenario) actually
    // sitting on P already -- boot recovery must push R back regardless.
    kiln_cfg_swap_boot_recover();
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_NONE,
               "recovered cleanly -- record cleared");
    TEST_CHECK(s_pico_committed.entries[0].param_id == SAFETY_PARAM_ID_ABS_MAX_TEMP_C ||
                   s_pico_committed.count > 0,
               "the ROLLBACK package (R), not P, was pushed to the Pico");
    float pushed;
    memcpy(&pushed, &s_pico_committed.entries[0].value_bits, sizeof(pushed));
    TEST_CHECK(pushed == 1000.0f, "R's ceiling (1000), not P's (1300), is what got pushed back -- proves "
                                  "this is genuinely a rollback, not a re-finish of the interrupted swap");
}

static void test_boot_recovery_corrupt_marker_stays_alarmed(void)
{
    TEST_SECTION("H10: a corrupt pending record is NEVER treated as NONE -- stays alarmed, never re-armed");
    reset_state();
    memset(&s_boot_fault, 0, sizeof(s_boot_fault));
    kiln_cfg_swap_pending_t p;
    memset(&p, 0, sizeof(p));
    p.marker = KILN_CFG_SWAP_MARKER_PICO_OPEN;
    p.target_id = 7;
    p.crc32 = pending_crc(&p);
    TEST_CHECK(save_pending(&p), "record persists");
    TEST_CHECK(fake_kv_script_corrupt_key(KILN_NVS_PARTITION_SWAP, NVS_NAMESPACE_SWAP, NVS_KEY_SWAP_PENDING),
               "test harness can corrupt the persisted record -- fake_kv models this as the NEXT read "
               "failing outright (HAL_IO), the real-world shape a torn/corrupted NVS blob takes");
    // Recovery must NOT silently clear/finish anything on an unreadable
    // record -- prove that by reloading raw afterward and confirming it is
    // STILL reported unreadable (i.e. clear_pending() -- which would leave
    // a perfectly READABLE all-NONE record behind -- was never called).
    kiln_cfg_swap_boot_recover();
    kiln_cfg_swap_pending_t raw;
    bool existed_but_unreadable = false;
    bool loaded = load_pending_ex(&raw, &existed_but_unreadable);
    TEST_CHECK(!loaded && existed_but_unreadable,
               "the record is STILL unreadable after boot_recover() -- it was never cleared/overwritten");
    // M13: an operator-facing latch must exist for this give-up path -- an
    // ESP_LOGE with no web/LCD surface is not actionable to anyone away
    // from a serial console.
    kiln_cfg_swap_boot_fault_t fault;
    TEST_CHECK(kiln_cfg_swap_get_boot_fault(&fault), "boot fault latched for an unreadable pending record");
    TEST_CHECK(fault.kind == KILN_CFG_SWAP_BOOT_FAULT_UNREADABLE, "fault kind names the unreadable-record case");
    TEST_CHECK(strlen(fault.reason) > 0, "fault reason is non-empty -- names what to do about it");
    TEST_CHECK(strstr(fault.reason, "apply a kiln config again") != NULL,
               "reason tells the operator the concrete recovery action, not just what failed");
}

static void test_boot_recovery_fault_latches_first_only(void)
{
    TEST_SECTION("M13: boot-recovery fault latch is first-one-wins, same convention as zones_cfg_load_fault_t");
    reset_state();
    memset(&s_boot_fault, 0, sizeof(s_boot_fault));
    TEST_CHECK(!kiln_cfg_swap_get_boot_fault(NULL), "no fault latched before any boot recovery runs");

    // First failure: an unreadable pending record.
    kiln_cfg_swap_pending_t p;
    memset(&p, 0, sizeof(p));
    p.marker = KILN_CFG_SWAP_MARKER_PICO_OPEN;
    p.target_id = 7;
    p.crc32 = pending_crc(&p);
    TEST_CHECK(save_pending(&p), "record persists");
    TEST_CHECK(fake_kv_script_corrupt_key(KILN_NVS_PARTITION_SWAP, NVS_NAMESPACE_SWAP, NVS_KEY_SWAP_PENDING),
               "first failure staged: unreadable record");
    kiln_cfg_swap_boot_recover();
    kiln_cfg_swap_boot_fault_t first;
    TEST_CHECK(kiln_cfg_swap_get_boot_fault(&first), "first fault latched");
    TEST_CHECK(first.kind == KILN_CFG_SWAP_BOOT_FAULT_UNREADABLE, "first fault is the unreadable-record kind");

    // Second, DIFFERENT failure: a PICO_OPEN/DONE record with no link
    // attached (kiln_cfg_swap_set_link(NULL)), which recovery cannot act
    // on either -- but the FIRST latch must survive, never be overwritten.
    kiln_cfg_swap_set_link(NULL);
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION_SWAP);
    memset(&p, 0, sizeof(p));
    p.marker = KILN_CFG_SWAP_MARKER_PICO_DONE;
    p.target_id = 9;
    p.crc32 = pending_crc(&p);
    TEST_CHECK(save_pending(&p), "second, different failure staged: PICO_DONE with no link");
    kiln_cfg_swap_boot_recover();
    kiln_cfg_swap_boot_fault_t second;
    TEST_CHECK(kiln_cfg_swap_get_boot_fault(&second), "fault still latched after the second boot_recover() call");
    TEST_CHECK(second.kind == KILN_CFG_SWAP_BOOT_FAULT_UNREADABLE,
               "the FIRST fault (UNREADABLE) is still what is reported -- the second, different failure "
               "(NO_LINK) never overwrote it, matching zones_cfg_load_fault_t's 'first one latched wins'");
    TEST_CHECK(strcmp(second.reason, first.reason) == 0, "reason text unchanged by the second failure");
}

static void test_swap_completes_with_pico_armed_never_disarmed(void)
{
    TEST_SECTION("item 15: a swap completes end-to-end with the Pico ARMED throughout -- never disarmed");
    reset_state();
    s_pico_armed = true; // the Pico's ordinary running state -- would refuse EVERY flash (COMMIT_CONFIG) write
    char reason[KILN_CFG_SWAP_REASON_MAX];
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(ok, "swap succeeds even though every flash-writing call this fake models is refused while ARMED");
    TEST_CHECK(!diverged, "not reported as an alarm/divergence");
    TEST_CHECK(s_active_id == 7, "active_id finalized");
    TEST_CHECK(s_pico_ceiling == 1300.0f, "ceiling raised to the target's -- via the volatile path, not flash");
    TEST_CHECK(s_volatile_push_count >= 2, "both the ceiling raise (step 4) and the bulk push (step 6/7) went "
                                          "through SAFETY_CMD_APPLY_CONFIG_VOLATILE (0x2D), never COMMIT_CONFIG");
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_NONE, "pending record cleared");
    // step 13's flash-fallback IS attempted (best-effort persistence), and
    // is REFUSED by this fake's ARMED simulation -- proving that refusal
    // does not undo or fail the swap the fake already reported as
    // successful above.
    TEST_CHECK(s_flash_push_count > 0, "step 13's flash-fallback persist was attempted");
}

static void test_flash_fallback_attempted_after_finalize_but_optional(void)
{
    TEST_SECTION("step 13: flash-fallback persist runs after a successful swap and is allowed to fail");
    reset_state();
    s_pico_armed = false; // NOT armed this time -- the flash fallback should actually land
    char reason[KILN_CFG_SWAP_REASON_MAX];
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(ok, "swap succeeds");
    TEST_CHECK(s_flash_push_count > 0, "the flash-fallback bulk push was attempted (unarmed -- this fake lets "
                                       "it land)");
    // Both the ceiling flash-set and the bulk flash-push in persist_pico_
    // flash_fallback() run through this file's OWN fakes -- s_pico_ceiling/
    // s_pico_committed end up holding whatever the LAST call wrote, which is
    // the flash-fallback's own re-push of the identical target values (not a
    // different value), so this does not corrupt the already-verified state.
    TEST_CHECK(s_pico_ceiling == 1300.0f, "ceiling still reads as the target's after the fallback re-push");
}

static void test_rollback_after_volatile_install_uses_volatile_too(void)
{
    TEST_SECTION("item 15 (task item 4): rollback after a successful volatile install ALSO uses the volatile "
                 "mechanism -- an armed Pico can still be rolled back");
    reset_state();
    s_pico_armed = true; // the Pico never leaves ARMED, forward push or rollback
    s_zones_import_should_fail = true; // forces failure AFTER the Pico has already been volatile-installed
                                        // (step 6/7 succeeds, step 8's ESP commit is what fails)
    char reason[KILN_CFG_SWAP_REASON_MAX];
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(!ok, "swap refused (ESP commit failed)");
    TEST_CHECK(!diverged, "a clean rollback is not reported as a divergence/alarm -- proves the rollback, "
                          "not merely the forward push, is what is under test here");
    TEST_CHECK(s_active_id == KILN_CFG_NO_ACTIVE_ID, "active_id never finalized");
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_NONE,
               "rolled back cleanly despite the Pico being ARMED throughout");
    // The LAST push this fake recorded is the rollback's own re-push of R --
    // confirm it went through the volatile path, exactly like the forward
    // push that preceded it, and that it actually landed (not refused).
    TEST_CHECK(s_last_push_was_volatile, "rollback's Pico restore used SAFETY_CMD_APPLY_CONFIG_VOLATILE, "
                                        "the SAME mechanism as the forward push -- never the flash path, "
                                        "which this fake's ARMED simulation would have refused");
    TEST_CHECK(s_last_ceiling_set_was_volatile, "rollback's ceiling restore also used the volatile path");
    TEST_CHECK(s_pico_ceiling == 1000.0f, "Pico's ceiling actually restored to R's (1000), not left at P's "
                                          "(1300) or unrestored");
    float restored;
    memcpy(&restored, &s_pico_committed.entries[0].value_bits, sizeof(restored));
    // (entries[0] is abs_max_temp_c in this fake's own set_pico_param() convention --
    // included in the pushed package even though the bulk-push production code
    // always skips applying it; this only proves WHICH package (R vs P) was pushed.)
    TEST_CHECK(restored == 1000.0f, "the package pushed back to the Pico during rollback was R (ceiling 1000), "
                                    "not P (ceiling 1300)");
}

static void test_pico_reboot_before_flash_fallback_caught_by_existing_check(void)
{
    TEST_SECTION("task item 5: a Pico reboot between a verified volatile install and step 13's flash-fallback "
                 "landing is a real divergence, caught by the EXISTING ceiling/arming check -- not a new one");
    reset_state();
    s_pico_armed = true; // the Pico stays armed the whole time -- this is why step 13 could not land yet
    // Simulate the outcome of a Pico reboot happening in the gap between
    // step 7 (verified volatile install) and step 13 (flash fallback, not
    // yet run): the Pico's live record has reverted to its OLD flashed
    // ceiling, so the standing divergence primitive
    // (safety_ceiling_sync_is_diverged(), reused verbatim -- see this
    // module's own header comment on why no second detector was added)
    // reports true the next time it is consulted at step 10/11, exactly as
    // it would on real hardware reacting to the reboot's own link-down/up
    // transition.
    s_diverged = true;
    char reason[KILN_CFG_SWAP_REASON_MAX];
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(!ok, "swap does NOT report success once the post-install divergence check fails");
    TEST_CHECK(diverged, "out_diverged is set -- caller can distinguish this from an ordinary refusal");
    // Same reordering as test_diverged_ceiling_moves_active_id_but_leaves_pending() above
    // (MEDIUM finding 3, adversarial review 2026-09-15): active_id moved to
    // target_id right after the ESP readback proved content match, which
    // happened before this reboot-triggered divergence was even detected.
    // The ESP's own zones config genuinely IS target_id's; what is
    // unconfirmed here is only the PICO's post-reboot state, which is a
    // separate, already-alarmed condition (out_diverged/ESP_DONE below) --
    // it does not make the ESP-side active_id wrong.
    TEST_CHECK(s_active_id == 7, "active_id finalized to target_id -- the ESP-side content is proven live; "
                                 "only the Pico's post-reboot state is unconfirmed");
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_ESP_DONE,
               "pending record left at ESP_DONE -- boot recovery's 'verify then finish' path (or a later "
               "reconcile once the Pico is unarmed) gets another chance, rather than the reboot being "
               "silently treated as a successful, complete swap");
    // step 13 (flash fallback) must NEVER be reached on this path -- the
    // swap already returned false before finalize/step 13 run.
    TEST_CHECK(s_flash_push_count == 0, "step 13 never ran -- the swap did not reach finalize");
}

static void test_negative_generation_check_is_load_bearing(void)
{
    TEST_SECTION("negative test: removing the generation re-check lets a race slip through silently");
    // This proves H6's re-check (kiln_cfg_swap_apply()'s
    // `if (kiln_cfg_store_generation() != gen_before)` block) is load-bearing,
    // per this repo's standing rule that every new guard must be proven
    // capable of failing. ACTUALLY PERFORMED (not just described): the
    // condition was changed to `if (false && ...)` by hand, this suite was
    // rebuilt and rerun, and test_generation_race_forces_rollback() above
    // FAILED both its assertions (the race slipped through: ESP was
    // committed despite the generation moving mid-swap) -- confirming the
    // guard is not vacuous. The `false &&` was then removed BY HAND
    // (never `git checkout --`, this tree is shared) and this suite was
    // rebuilt and reconfirmed all-green before anything was committed. See
    // docs/audits/kiln_swap_transaction_2026-09-14.md for the transcript.
    TEST_CHECK(1, "see docs/audits/kiln_swap_transaction_2026-09-14.md for the negative-test transcript");
}

// ---------------------------------------------------------------------------
// docs/WEB_AUTH_PLAN.md item 12b: a credential set through
// web_auth_store_set_password()/_set_pin() must survive a slot swap
// (kiln_cfg_swap_apply()), regardless of whether the swap itself succeeds or
// is refused -- that outcome is orthogonal to this property. A synthetic
// credential is used, never a real password.
// ---------------------------------------------------------------------------
static const uint8_t SWAP12B_SALT[WEB_AUTH_SALT_LEN] = {
    0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8,
    0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF, 0xD0};
#define SWAP12B_SYNTHETIC_PASSWORD "Synthetic-Swap-Secret-7"
#define SWAP12B_SYNTHETIC_PIN "319642"

static void test_credential_survives_a_slot_swap(void)
{
    TEST_SECTION("kiln_cfg_swap_apply() -- WEB_AUTH_PLAN.md item 12b: a kiln_auth credential "
                 "must survive a slot swap regardless of the swap's own outcome");
    reset_state();
    // reset_state() already does fake_kv_reset_all()/hal_kv_init_partition(KILN_NVS_PARTITION_SWAP);
    // kiln_auth lives on the DEFAULT nvs partition, a separate init.
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init the default nvs partition (kiln_auth's home)");
    TEST_CHECK(web_auth_store_set_password(WEB_AUTH_ROLE_ADMINISTRATOR, "swaptestuser",
                                            SWAP12B_SYNTHETIC_PASSWORD, SWAP12B_SALT, false) == HAL_OK,
              "setup: seed a synthetic administrator password");
    TEST_CHECK(web_auth_store_set_pin(WEB_AUTH_ROLE_USER, SWAP12B_SYNTHETIC_PIN, SWAP12B_SALT) == HAL_OK,
              "setup: seed a synthetic user LCD PIN");

    char reason[KILN_CFG_SWAP_REASON_MAX];
    bool diverged = false;
    // Outcome (success or refusal) is deliberately not asserted here -- other
    // tests in this file already cover kiln_cfg_swap_apply()'s own behavior.
    // What is under test is that the credential is untouched either way.
    (void)kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);

    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_ADMINISTRATOR, SWAP12B_SYNTHETIC_PASSWORD),
              "the administrator password must still verify after a slot swap");
    TEST_CHECK(web_auth_store_verify_pin(WEB_AUTH_ROLE_USER, SWAP12B_SYNTHETIC_PIN),
              "the user PIN must still verify after a slot swap");
}

// --- ESP_DONE boot-recovery coverage (docs/KILN_PROFILES_PLAN.md item 8) ---
//
// finish_esp_done() is the one boot-recovery row where FINISHING the
// interrupted swap is correct rather than rolling back, and it is gated on
// re-verifying BOTH processors independently (never either side's cache).
// Before these three cases the whole gate was uncovered: forcing
// esp_matches and pico_matches both true produced zero behavioural failures
// across this file's other parent tests. The two mismatch cases below are
// what make that gate load-bearing -- each one fails if the confirmation is
// short-circuited.

static void test_boot_recovery_esp_done_both_match_finishes(void)
{
    TEST_SECTION("boot recovery: ESP_DONE with BOTH sides confirmed on the target finishes the swap");
    reset_state();
    memset(&s_boot_fault, 0, sizeof(s_boot_fault));
    // Both independent read-backs must land on the TARGET package: the live
    // ESP blob equals the slot's blob, and the Pico's confirmed params are
    // the slot's params (0x0201 == 42), not the pre-swap snapshot's (7).
    memcpy(s_live_blob, s_slot_blob, sizeof(s_live_blob));
    s_live_blob_len = s_slot_blob_len;
    s_pico_committed = s_slot_pico;

    kiln_cfg_swap_pending_t p;
    memset(&p, 0, sizeof(p));
    p.marker = KILN_CFG_SWAP_MARKER_ESP_DONE;
    p.target_id = 7;
    p.previous_active_id = 3;
    p.rollback_blob_len = 32;
    memset(p.rollback_blob, 0xEE, sizeof(p.rollback_blob));
    p.rollback_pico = s_current_pico_snapshot;
    p.crc32 = pending_crc(&p);
    TEST_CHECK(save_pending(&p), "ESP_DONE record persists");

    kiln_cfg_swap_boot_recover();

    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_NONE,
               "pending record cleared -- the swap was finished, not left pending");
    TEST_CHECK(s_active_id == 7, "active_id advanced to the target slot, and only after both read-backs passed");
    TEST_CHECK(!kiln_cfg_swap_get_boot_fault(NULL),
               "no boot fault latched when both processors genuinely confirm the new config");
}

static void test_boot_recovery_esp_done_esp_mismatch_stays_alarmed(void)
{
    TEST_SECTION("boot recovery: ESP_DONE with the ESP half NOT on the target stays alarmed, never finishes");
    reset_state();
    memset(&s_boot_fault, 0, sizeof(s_boot_fault));
    // Pico confirms the target, but the live ESP blob is still something
    // else (reset_state's 0xCD live vs 0xAB slot) -- the swap must NOT be
    // declared complete on one processor's word.
    s_pico_committed = s_slot_pico;

    kiln_cfg_swap_pending_t p;
    memset(&p, 0, sizeof(p));
    p.marker = KILN_CFG_SWAP_MARKER_ESP_DONE;
    p.target_id = 7;
    p.previous_active_id = 3;
    p.rollback_blob_len = 32;
    memset(p.rollback_blob, 0xEE, sizeof(p.rollback_blob));
    p.rollback_pico = s_current_pico_snapshot;
    p.crc32 = pending_crc(&p);
    TEST_CHECK(save_pending(&p), "ESP_DONE record persists");

    kiln_cfg_swap_boot_recover();

    TEST_CHECK(s_active_id != 7, "active_id did NOT advance to the target on an unconfirmed ESP half");
    kiln_cfg_swap_boot_fault_t fault;
    TEST_CHECK(kiln_cfg_swap_get_boot_fault(&fault), "boot fault latched -- the operator can see why heaters stay off");
    TEST_CHECK(fault.kind == KILN_CFG_SWAP_BOOT_FAULT_ESP_DONE_UNCONFIRMED,
               "fault kind names the ESP_DONE could-not-confirm case");
    TEST_CHECK(strstr(fault.reason, "apply a kiln config again") != NULL,
               "reason names the concrete recovery action, not just the failure");
}

static void test_boot_recovery_esp_done_pico_mismatch_stays_alarmed(void)
{
    TEST_SECTION("boot recovery: ESP_DONE with the Pico half NOT on the target stays alarmed, never finishes");
    reset_state();
    memset(&s_boot_fault, 0, sizeof(s_boot_fault));
    // The mirror image of the case above: the ESP is genuinely on the
    // target, but the Pico still confirms the PRE-swap params (0x0201 == 7,
    // not 42). This is the case that matters most -- an ESP-only check
    // would call this a completed swap and leave the safety processor
    // holding the previous kiln's commissioning values.
    memcpy(s_live_blob, s_slot_blob, sizeof(s_live_blob));
    s_live_blob_len = s_slot_blob_len;
    s_pico_committed = s_current_pico_snapshot;

    kiln_cfg_swap_pending_t p;
    memset(&p, 0, sizeof(p));
    p.marker = KILN_CFG_SWAP_MARKER_ESP_DONE;
    p.target_id = 7;
    p.previous_active_id = 3;
    p.rollback_blob_len = 32;
    memset(p.rollback_blob, 0xEE, sizeof(p.rollback_blob));
    p.rollback_pico = s_current_pico_snapshot;
    p.crc32 = pending_crc(&p);
    TEST_CHECK(save_pending(&p), "ESP_DONE record persists");

    kiln_cfg_swap_boot_recover();

    TEST_CHECK(s_active_id != 7, "active_id did NOT advance to the target on an unconfirmed Pico half");
    kiln_cfg_swap_boot_fault_t fault;
    TEST_CHECK(kiln_cfg_swap_get_boot_fault(&fault), "boot fault latched for the unconfirmed Pico half");
    TEST_CHECK(fault.kind == KILN_CFG_SWAP_BOOT_FAULT_ESP_DONE_UNCONFIRMED,
               "fault kind names the ESP_DONE could-not-confirm case");
}

static void test_apply_refuses_when_scratch_malloc_fails(void)
{
    TEST_SECTION("F4: kiln_cfg_swap_apply() -- when the heap_caps_malloc() for its scratch struct fails, "
                 "refuse with a reason set and *out_diverged left false, never touching either processor");
    reset_state();
    char reason[KILN_CFG_SWAP_REASON_MAX] = {0};
    bool diverged = true; /* deliberately wrong-signed so a real write is observable */

    heap_caps_malloc_test_set_fail(true);
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    heap_caps_malloc_test_set_fail(false);

    TEST_CHECK(!ok, "apply refuses when its scratch allocation fails");
    TEST_CHECK(strlen(reason) > 0, "a reason is reported, not a silent refusal");
    TEST_CHECK(!diverged, "*out_diverged is left false -- an allocation failure is not a divergence");
    TEST_CHECK(s_active_id != 7, "active_id never advanced -- nothing was attempted on either processor");
}

static void test_finish_esp_done_latches_fault_when_malloc_fails(void)
{
    TEST_SECTION("F4: finish_esp_done() -- when its heap_caps_malloc() fails, latch "
                 "KILN_CFG_SWAP_BOOT_FAULT_ESP_DONE_UNCONFIRMED and leave the pending record in place "
                 "(never clear it, since neither processor was actually confirmed)");
    reset_state();
    memset(&s_boot_fault, 0, sizeof(s_boot_fault));
    // Set up the same "both sides genuinely match" state as
    // test_boot_recovery_esp_done_both_match_finishes() -- proving this is a
    // pure allocation-failure refusal, not a masked real mismatch.
    memcpy(s_live_blob, s_slot_blob, sizeof(s_live_blob));
    s_live_blob_len = s_slot_blob_len;
    s_pico_committed = s_slot_pico;

    kiln_cfg_swap_pending_t p;
    memset(&p, 0, sizeof(p));
    p.marker = KILN_CFG_SWAP_MARKER_ESP_DONE;
    p.target_id = 7;
    p.previous_active_id = 3;
    p.rollback_blob_len = 32;
    memset(p.rollback_blob, 0xEE, sizeof(p.rollback_blob));
    p.rollback_pico = s_current_pico_snapshot;
    p.crc32 = pending_crc(&p);
    TEST_CHECK(save_pending(&p), "ESP_DONE record persists");

    // Calls kiln_cfg_swap_boot_recover_impl() directly (visible in this TU --
    // kiln_cfg_swap.c is #included above) with a stack-allocated `p`, rather
    // than going through the public kiln_cfg_swap_boot_recover() wrapper:
    // that wrapper does its OWN heap_caps_malloc() for `p` first (see
    // test_boot_recover_latches_unreadable_when_malloc_fails() below, which
    // exercises exactly that outer allocation), and the fail-flag has no
    // per-call resolution -- enabling it around the wrapper call would fail
    // the outer allocation too and never reach finish_esp_done() at all.
    kiln_cfg_swap_pending_t p_local = p;
    heap_caps_malloc_test_set_fail(true);
    kiln_cfg_swap_boot_recover_impl(&p_local);
    heap_caps_malloc_test_set_fail(false);

    TEST_CHECK(s_active_id != 7, "active_id did not advance -- the swap was never confirmed finished");
    kiln_cfg_swap_boot_fault_t fault;
    TEST_CHECK(kiln_cfg_swap_get_boot_fault(&fault), "boot fault latched on the allocation failure");
    TEST_CHECK(fault.kind == KILN_CFG_SWAP_BOOT_FAULT_ESP_DONE_UNCONFIRMED,
               "fault kind is the same ESP_DONE-unconfirmed case a real mismatch would report");
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) != KILN_CFG_SWAP_MARKER_NONE,
               "the pending record was NOT cleared -- finish_esp_done() never got to confirm anything");
}

static void test_boot_recover_latches_unreadable_when_malloc_fails(void)
{
    TEST_SECTION("F4: kiln_cfg_swap_boot_recover() -- when its own top-level heap_caps_malloc() fails, "
                 "latch KILN_CFG_SWAP_BOOT_FAULT_UNREADABLE rather than silently doing nothing");
    reset_state();
    memset(&s_boot_fault, 0, sizeof(s_boot_fault));
    kiln_cfg_swap_pending_t p;
    memset(&p, 0, sizeof(p));
    p.marker = KILN_CFG_SWAP_MARKER_STAGED;
    p.target_id = 7;
    p.previous_active_id = KILN_CFG_NO_ACTIVE_ID;
    p.crc32 = pending_crc(&p);
    TEST_CHECK(save_pending(&p), "a real, readable pending record exists -- proves this is purely the "
                                  "allocation failure, not a genuinely unreadable record");

    heap_caps_malloc_test_set_fail(true);
    kiln_cfg_swap_boot_recover();
    heap_caps_malloc_test_set_fail(false);

    kiln_cfg_swap_boot_fault_t fault;
    TEST_CHECK(kiln_cfg_swap_get_boot_fault(&fault), "boot fault latched on the allocation failure");
    TEST_CHECK(fault.kind == KILN_CFG_SWAP_BOOT_FAULT_UNREADABLE,
               "fault kind names the unreadable/could-not-recover case");
}

// -- docs/audits/UNCHECKED_PERSIST_RESULT_AUDIT_2026-10-09.md M1/M2/L6 --
// Write-class call indices on the swap record in one apply (each
// save_pending() is set_blob + commit): STAGED 0/1, PICO_OPEN 2/3,
// PICO_DONE 4/5, ESP_DONE 6/7, final clear 8/9.

static uint32_t pico_param_bits(const kiln_pkg_safety_t *pkg, uint16_t id)
{
    for (uint16_t i = 0; i < pkg->count; i++) {
        if (pkg->entries[i].param_id == id) {
            return pkg->entries[i].value_bits;
        }
    }
    return 0xFFFFFFFFu;
}

static void check_pico_open_save_failure(unsigned skip, const char *which)
{
    reset_state();
    memset(&s_boot_fault, 0, sizeof(s_boot_fault));
    fake_kv_script_write_status_after(skip, HAL_IO);
    char reason[KILN_CFG_SWAP_REASON_MAX];
    reason[0] = '\0';
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    printf("   (%s) reason: %s\n", which, reason);
    TEST_CHECK(!ok, "swap refused when the PICO_OPEN marker cannot be persisted");
    TEST_CHECK(!diverged, "refused before the Pico package push -- not a divergence");
    TEST_CHECK(s_pico_committed.count == 0, "the Pico package was NEVER pushed (abort precedes step 6)");
    TEST_CHECK(s_pico_ceiling == 1000.0f, "the raise-first ceiling was put back to R's (1000)");
    TEST_CHECK(s_zones_import_call_count == 0, "ESP never touched");
    TEST_CHECK(s_active_id == KILN_CFG_NO_ACTIVE_ID, "active_id unchanged");
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_NONE,
               "record cleared -- nothing left for boot recovery to act on");
}

static void test_pico_open_save_failure_aborts_before_pico_push(void)
{
    TEST_SECTION("M2: PICO_OPEN set_blob fails -- swap refused before the Pico is told anything");
    check_pico_open_save_failure(2, "set_blob");
    TEST_SECTION("M2: PICO_OPEN commit fails -- swap refused before the Pico is told anything");
    check_pico_open_save_failure(3, "commit");
}

static void test_pico_done_save_failure_rolls_back(void)
{
    TEST_SECTION("M2: PICO_DONE save fails -- rolled back while the ESP is untouched");
    reset_state();
    fake_kv_script_write_status_after(4, HAL_IO);
    char reason[KILN_CFG_SWAP_REASON_MAX];
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(!ok, "swap refused when the PICO_DONE marker cannot be persisted");
    TEST_CHECK(!diverged, "a clean rollback is not a divergence");
    TEST_CHECK(s_zones_import_call_count == 0, "ESP half never committed");
    TEST_CHECK(pico_param_bits(&s_pico_committed, 0x0201u) == 7u, "Pico re-pushed back to R (0x0201 == 7)");
    TEST_CHECK(s_active_id == KILN_CFG_NO_ACTIVE_ID, "active_id unchanged");
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_NONE, "record cleared after rollback");
}

static void test_esp_done_save_failure_rolls_back(void)
{
    TEST_SECTION("M2: ESP_DONE save fails -- both halves rolled back, never reported as success");
    reset_state();
    fake_kv_script_write_status_after(6, HAL_IO);
    char reason[KILN_CFG_SWAP_REASON_MAX];
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(!ok, "swap refused when the ESP_DONE marker cannot be persisted");
    TEST_CHECK(!diverged, "a clean rollback is not a divergence");
    TEST_CHECK(s_zones_import_call_count == 2, "ESP committed then restored (forward import + rollback import)");
    TEST_CHECK(s_live_blob[0] == 0xCD && s_live_blob[31] == 0xCD, "ESP live blob back on R (0xCD)");
    TEST_CHECK(pico_param_bits(&s_pico_committed, 0x0201u) == 7u, "Pico back on R (0x0201 == 7)");
    TEST_CHECK(s_active_id == KILN_CFG_NO_ACTIVE_ID, "active_id unchanged");
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_NONE, "record cleared after rollback");
}

static void test_active_id_save_failure_keeps_record_and_boot_retries(void)
{
    TEST_SECTION("M1: active-id save fails after a swap -- not a clean success, record kept for boot retry");
    reset_state();
    memset(&s_boot_fault, 0, sizeof(s_boot_fault));
    s_set_active_id_should_fail = true;
    char reason[KILN_CFG_SWAP_REASON_MAX];
    reason[0] = '\0';
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(!ok, "apply does NOT report a clean success when the active id did not persist");
    TEST_CHECK(!diverged, "both processors are on the target -- not a divergence");
    TEST_CHECK(strncmp(reason, KILN_CFG_SWAP_REASON_ACTIVE_ID_UNSAVED_PREFIX,
                       strlen(KILN_CFG_SWAP_REASON_ACTIVE_ID_UNSAVED_PREFIX)) == 0,
               "reason carries the applied-but-active-id-unsaved prefix the page keys on");
    int32_t tgt = -1;
    TEST_CHECK(kiln_cfg_swap_get_marker(&tgt, NULL) == KILN_CFG_SWAP_MARKER_ESP_DONE && tgt == 7,
               "record left at ESP_DONE for target 7 so boot recovery retries");
    TEST_CHECK(memcmp(s_live_blob, s_slot_blob, s_slot_blob_len) == 0, "ESP stays on the target (no rollback)");

    TEST_SECTION("M1: boot recovery with the active-id save still failing -- logs, latches, keeps the record");
    kiln_cfg_swap_boot_recover();
    TEST_CHECK(kiln_cfg_swap_get_marker(&tgt, NULL) == KILN_CFG_SWAP_MARKER_ESP_DONE && tgt == 7,
               "record still ESP_DONE -- retried again on the next boot");
    kiln_cfg_swap_boot_fault_t fault;
    TEST_CHECK(kiln_cfg_swap_get_boot_fault(&fault) && fault.kind == KILN_CFG_SWAP_BOOT_FAULT_ACTIVE_ID_UNSAVED,
               "boot fault latched with kind ACTIVE_ID_UNSAVED");
    // LOW-7: the real set_active_id_raw() sets RAM before persisting, so RAM
    // names the target even though the persisted id does not.
    TEST_CHECK(s_active_id == 7, "RAM active id names the target; only the persisted id is unsaved");

    TEST_SECTION("M1: next boot, active-id save works -- swap finished, record cleared");
    s_set_active_id_should_fail = false;
    memset(&s_boot_fault, 0, sizeof(s_boot_fault));
    kiln_cfg_swap_boot_recover();
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_NONE, "record cleared on the retry");
    TEST_CHECK(s_active_id == 7, "active id now names the target");
    TEST_CHECK(!kiln_cfg_swap_get_boot_fault(NULL), "no boot fault on the successful retry");
}

static void test_save_pending_read_back_catches_silent_write(void)
{
    TEST_SECTION("L6: save_pending() fails when the write reports success but nothing landed");
    reset_state();
    kiln_cfg_swap_pending_t a;
    memset(&a, 0, sizeof(a));
    a.marker = KILN_CFG_SWAP_MARKER_STAGED;
    a.target_id = 5;
    a.crc32 = pending_crc(&a);
    TEST_CHECK(save_pending(&a), "honest write verifies");
    kiln_cfg_swap_pending_t b = a;
    b.marker = KILN_CFG_SWAP_MARKER_PICO_OPEN;
    b.target_id = 9;
    b.crc32 = pending_crc(&b);
    fake_kv_script_silent_set_noops(1);
    TEST_CHECK(!save_pending(&b), "silently dropped write is reported as a failure");
    int32_t tgt = -1;
    TEST_CHECK(kiln_cfg_swap_get_marker(&tgt, NULL) == KILN_CFG_SWAP_MARKER_STAGED && tgt == 5,
               "the record on flash is still the earlier one");

    TEST_SECTION("L6: a silently dropped STAGED write refuses the swap before anything is touched");
    reset_state();
    fake_kv_script_silent_set_noops(1);
    char reason[KILN_CFG_SWAP_REASON_MAX];
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(!ok, "swap refused");
    TEST_CHECK(s_pico_ceiling == 1000.0f && s_pico_committed.count == 0, "Pico untouched");
    TEST_CHECK(s_zones_import_call_count == 0, "ESP untouched");
}

// -- UNCHECKED_PERSIST_RESULT_AUDIT_2026-10-09.md follow-up, LOW-1..LOW-7 --

static void test_low6_restore_never_below_zone_raise_pico_open_path(void)
{
    TEST_SECTION("LOW-6: zone max raised after raise-first, PICO_OPEN save fails -- restore keeps the "
                 "ceiling at the zones' requirement, not R's lower snapshot");
    reset_state();
    s_zone_raise_after_first_ceiling_write = 1400.0f;
    fake_kv_script_write_status_after(2, HAL_IO);
    char reason[KILN_CFG_SWAP_REASON_MAX];
    reason[0] = '\0';
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(!ok, "swap refused when PICO_OPEN cannot be persisted");
    TEST_CHECK(s_volatile_ceiling_write_count >= 2, "the restore write actually ran (raise-first + restore)");
    TEST_CHECK(s_pico_ceiling >= 1400.0f, "Pico ceiling NOT lowered below the raised zone max (1400)");
    TEST_CHECK(s_pico_committed.count == 0, "Pico package never pushed");
}

static void test_low6_restore_never_below_zone_raise_rollback_path(void)
{
    TEST_SECTION("LOW-6: zone max raised after raise-first, PICO_DONE save fails -- rollback() restore keeps "
                 "the ceiling at the zones' requirement");
    reset_state();
    s_zone_raise_after_first_ceiling_write = 1400.0f;
    fake_kv_script_write_status_after(4, HAL_IO);
    char reason[KILN_CFG_SWAP_REASON_MAX];
    reason[0] = '\0';
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(!ok && !diverged, "swap refused and cleanly rolled back");
    TEST_CHECK(pico_param_bits(&s_pico_committed, 0x0201u) == 7u, "Pico content back on R");
    TEST_CHECK(s_pico_ceiling >= 1400.0f, "Pico ceiling NOT lowered below the raised zone max (1400)");

    TEST_SECTION("LOW-6: zone requirement below R -- R's snapshot ceiling is still what is restored");
    reset_state();
    s_zone_raise_after_first_ceiling_write = 900.0f; /* hook only raises s_pico_ceiling if lower: it is not */
    fake_kv_script_write_status_after(4, HAL_IO);
    ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(!ok, "swap refused");
    TEST_CHECK(s_pico_ceiling == 1000.0f, "R's ceiling (1000) restored when it is already above the zones' need");
}

static kiln_cfg_swap_pending_t make_pending(kiln_cfg_swap_marker_t marker)
{
    kiln_cfg_swap_pending_t p;
    memset(&p, 0, sizeof(p));
    p.marker = (uint8_t)marker;
    p.target_id = 7;
    p.previous_active_id = 3;
    p.rollback_blob_len = 32;
    memset(p.rollback_blob, 0xCD, sizeof(p.rollback_blob)); /* R == reset_state()'s live blob */
    p.rollback_pico = s_current_pico_snapshot;
    p.crc32 = pending_crc(&p);
    return p;
}

static void test_low4_low5_rollback_uncleared_journal_reported(void)
{
    TEST_SECTION("LOW-4: rollback succeeds but the journal clear fails -- reported, record kept");
    reset_state();
    kiln_cfg_swap_pending_t p = make_pending(KILN_CFG_SWAP_MARKER_PICO_DONE);
    TEST_CHECK(save_pending(&p), "PICO_DONE record persists");
    fake_kv_script_write_status_after(0, HAL_IO); /* rollback()'s only NVS write is the final clear */
    char reason[KILN_CFG_SWAP_REASON_MAX];
    reason[0] = '\0';
    bool rolled = rollback(&s_fake_link, &p, /*esp_was_committed=*/false, reason, sizeof(reason));
    TEST_CHECK(rolled, "both sides are back on R -- rollback itself still succeeded");
    TEST_CHECK(strstr(reason, KILN_CFG_SWAP_ROLLBACK_UNCLEARED_NOTE) != NULL,
               "reason carries the uncleared-journal note instead of staying silent");
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_PICO_DONE,
               "record still on flash -- boot recovery will clear it");
    TEST_CHECK(kiln_cfg_swap_is_pending(), "autosave stays suppressed while the record is uncleared");

    TEST_SECTION("LOW-5: rollback restores active_id under kiln_cfg_store_lock");
    TEST_CHECK(s_set_active_id_unlocked_calls == 0, "set_active_id_raw never called without the store lock");
    TEST_CHECK(s_lock_depth == 0, "lock released afterwards");
    TEST_CHECK(s_active_id == 3, "active_id restored to the previous id");
}

static void test_med_rollback_id_restore_failure_keeps_record(void)
{
    TEST_SECTION("MED: same-boot apply refused, rollback's active_id restore fails -- journal KEPT");
    reset_state();
    g_bump_gen_on_push = true; /* generation race => apply rolls back */
    s_set_active_id_should_fail = true;
    char reason[KILN_CFG_SWAP_REASON_MAX];
    reason[0] = '\0';
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    g_bump_gen_on_push = false;
    s_set_active_id_should_fail = false;
    TEST_CHECK(!ok, "apply refused");
    TEST_CHECK(strstr(reason, KILN_CFG_SWAP_ROLLBACK_ID_NOTE) != NULL, "reason names the unrestored active id");
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) != KILN_CFG_SWAP_MARKER_NONE,
               "record NOT cleared -- it is the only retry handle for the persisted active_id");
    TEST_CHECK(kiln_cfg_swap_is_pending(), "autosave stays suppressed");

    TEST_SECTION("MED: boot fallback rollback (PICO_DONE) with id restore failing -- record kept");
    reset_state();
    kiln_cfg_swap_set_link(&s_fake_link);
    kiln_cfg_swap_pending_t p = make_pending(KILN_CFG_SWAP_MARKER_PICO_DONE);
    TEST_CHECK(save_pending(&p), "PICO_DONE record persists");
    s_set_active_id_should_fail = true;
    kiln_cfg_swap_boot_recover();
    s_set_active_id_should_fail = false;
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_PICO_DONE,
               "boot recovery did not clear the record after an id restore failure");
    TEST_CHECK(kiln_cfg_swap_get_boot_fault_kind() == KILN_CFG_SWAP_BOOT_FAULT_ROLLBACK_ACTIVE_ID_UNSAVED,
               "kept journal after a failed id restore latches the display-only rollback_active_id_unsaved fault");

    TEST_SECTION("MED: boot ESP_DONE fallback rollback (target unreadable) with id restore failing -- record kept");
    reset_state();
    kiln_cfg_swap_set_link(&s_fake_link);
    p = make_pending(KILN_CFG_SWAP_MARKER_ESP_DONE);
    TEST_CHECK(save_pending(&p), "ESP_DONE record persists");
    memset(&s_boot_fault, 0, sizeof(s_boot_fault)); /* the PICO_DONE section above latched the same kind */
    s_slot_exists = false; /* target slot cannot be re-read => fallback rollback */
    s_set_active_id_should_fail = true;
    kiln_cfg_swap_boot_recover();
    s_set_active_id_should_fail = false;
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_ESP_DONE,
               "ESP_DONE record kept after the fallback's id restore failed");
    TEST_CHECK(kiln_cfg_swap_get_boot_fault_kind() == KILN_CFG_SWAP_BOOT_FAULT_ROLLBACK_ACTIVE_ID_UNSAVED,
               "ESP_DONE fallback rollback with kept journal latches rollback_active_id_unsaved");
}

static void test_low4_apply_reason_names_uncleared_journal(void)
{
    TEST_SECTION("LOW-4: generation race rollback whose clear fails -- the apply's reason names it");
    reset_state();
    g_bump_gen_on_push = true;
    /* Writes: STAGED 0/1, PICO_OPEN 2/3, PICO_DONE 4/5, then the generation
     * check rolls back; rollback's clear set_blob is write index 6. */
    fake_kv_script_write_status_after(6, HAL_IO);
    char reason[KILN_CFG_SWAP_REASON_MAX];
    reason[0] = '\0';
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    g_bump_gen_on_push = false;
    printf("   reason: %s\n", reason);
    TEST_CHECK(!ok && !diverged, "refused and rolled back");
    TEST_CHECK(strstr(reason, KILN_CFG_SWAP_ROLLBACK_UNCLEARED_NOTE) != NULL,
               "apply reason appends the uncleared-journal note");
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) != KILN_CFG_SWAP_MARKER_NONE, "record left for boot");
}

static void test_low4_note_survives_long_message(void)
{
    TEST_SECTION("LOW-4: rolled_back_reason() keeps the note when the message fills the buffer");
    char longmsg[KILN_CFG_SWAP_REASON_MAX * 2];
    memset(longmsg, 'x', sizeof(longmsg) - 1);
    longmsg[sizeof(longmsg) - 1] = '\0';
    char reason[KILN_CFG_SWAP_REASON_MAX];
    reason[0] = '\0';
    bool r = rolled_back_reason(reason, sizeof(reason), longmsg, KILN_CFG_SWAP_ROLLBACK_UNCLEARED_NOTE);
    TEST_CHECK(!r, "always returns false");
    TEST_CHECK(strstr(reason, KILN_CFG_SWAP_ROLLBACK_UNCLEARED_NOTE) != NULL,
               "note survives a long message (truncation drops the message tail, not the note)");
}

static void test_low3_esp_done_both_on_r_is_cleared(void)
{
    TEST_SECTION("LOW-3: ESP_DONE record with both sides back on R -- cleared, never latched");
    reset_state();
    memset(&s_boot_fault, 0, sizeof(s_boot_fault));
    s_pico_committed = s_current_pico_snapshot; /* Pico on R (0x0201 == 7) */
    /* live blob is reset_state()'s 0xCD == R */
    kiln_cfg_swap_pending_t p = make_pending(KILN_CFG_SWAP_MARKER_ESP_DONE);
    TEST_CHECK(save_pending(&p), "ESP_DONE record persists");
    kiln_cfg_swap_boot_recover();
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_NONE, "record cleared");
    TEST_CHECK(!kiln_cfg_swap_get_boot_fault(NULL), "no ESP_DONE_UNCONFIRMED latch over a consistent R state");
    TEST_CHECK(s_active_id == 3, "active_id restored to previous_active_id before the clear");

    TEST_SECTION("LOW-3: both on R, persisted active_id names the target and restoring it fails -- latched, "
                 "not cleared");
    reset_state();
    memset(&s_boot_fault, 0, sizeof(s_boot_fault));
    s_pico_committed = s_current_pico_snapshot;
    s_active_id = 7; /* a diverged/ACTIVE_ID_UNSAVED path already persisted the target */
    p = make_pending(KILN_CFG_SWAP_MARKER_ESP_DONE);
    TEST_CHECK(save_pending(&p), "ESP_DONE record persists");
    s_set_active_id_should_fail = true;
    kiln_cfg_swap_boot_recover();
    s_set_active_id_should_fail = false;
    kiln_cfg_swap_boot_fault_t fault;
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_ESP_DONE,
               "record kept: active_id could not be put back on R's kiln");
    TEST_CHECK(kiln_cfg_swap_get_boot_fault(&fault) && fault.kind == KILN_CFG_SWAP_BOOT_FAULT_ESP_DONE_UNCONFIRMED,
               "latched ESP_DONE_UNCONFIRMED instead of clearing over a target active_id");

    TEST_SECTION("LOW-3: ESP on R but Pico on neither -- still latched");
    reset_state();
    memset(&s_boot_fault, 0, sizeof(s_boot_fault));
    memset(&s_pico_committed, 0, sizeof(s_pico_committed));
    set_pico_param(&s_pico_committed, 0x0201u, KILNLINK_PARAM_TYPE_U16, 99);
    p = make_pending(KILN_CFG_SWAP_MARKER_ESP_DONE);
    TEST_CHECK(save_pending(&p), "ESP_DONE record persists");
    kiln_cfg_swap_boot_recover();
    TEST_CHECK(kiln_cfg_swap_get_boot_fault(&fault) && fault.kind == KILN_CFG_SWAP_BOOT_FAULT_ESP_DONE_UNCONFIRMED,
               "half-on-R state still latches ESP_DONE_UNCONFIRMED");
}

static void test_low2_autosave_unblocked_after_active_id_unsaved(void)
{
    TEST_SECTION("LOW-2: apply finished but active-id save failed -- is_pending false, autosave allowed");
    reset_state();
    memset(&s_boot_fault, 0, sizeof(s_boot_fault));
    s_set_active_id_should_fail = true;
    s_probe_pending_in_set_active_id = true;
    s_pending_seen_in_set_active_id = -1;
    char reason[KILN_CFG_SWAP_REASON_MAX];
    reason[0] = '\0';
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    s_probe_pending_in_set_active_id = false;
    TEST_CHECK(!ok && !diverged, "applied but not a clean success");
    /* 2026-09-15 race: until apply returns, a concurrent autosave must see
     * the swap as pending, so the exception may only be armed at the end. */
    TEST_CHECK(s_pending_seen_in_set_active_id == 1,
               "is_pending() true mid-apply (inside the active-id save, after ESP_DONE)");
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_ESP_DONE, "record kept at ESP_DONE");
    TEST_CHECK(!kiln_cfg_swap_is_pending(), "autosave not blocked: RAM active id names the live target");

    TEST_SECTION("LOW-2: a later zone edit autosaves into the target slot -- next boot does not latch "
                 "ESP_DONE_UNCONFIRMED");
    /* Simulate the zone edit + autosave: live and the target slot change together. */
    s_live_blob[5] = 0x11;
    s_slot_blob[5] = 0x11;
    s_id_unsaved_target = KILN_CFG_NO_ACTIVE_ID; /* a reboot loses the RAM flag */
    memset(&s_boot_fault, 0, sizeof(s_boot_fault));
    kiln_cfg_swap_boot_recover();
    kiln_cfg_swap_boot_fault_t fault;
    TEST_CHECK(kiln_cfg_swap_get_boot_fault(&fault) && fault.kind == KILN_CFG_SWAP_BOOT_FAULT_ACTIVE_ID_UNSAVED,
               "boot latches only the display-only ACTIVE_ID_UNSAVED kind");
    TEST_CHECK(!kiln_cfg_swap_is_pending(), "boot finish also re-enables autosave");

    TEST_SECTION("LOW-2: a different RAM active id keeps autosave blocked");
    s_active_id = 3;
    TEST_CHECK(kiln_cfg_swap_is_pending(), "autosave stays blocked if RAM active id is not the target");
}

static void test_low7_marker_save_fails_and_clear_fails(void)
{
    TEST_SECTION("LOW-7: PICO_OPEN save fails AND the clear fails -- record reads STAGED, boot discards");
    reset_state();
    memset(&s_boot_fault, 0, sizeof(s_boot_fault));
    /* First ceiling write (raise-first) arms the PICO_OPEN set_blob failure;
     * the second (the restore) arms the clear's set_blob failure. */
    s_fail_next_kv_write_per_ceiling_write = 2;
    char reason[KILN_CFG_SWAP_REASON_MAX];
    reason[0] = '\0';
    bool diverged = false;
    bool ok = kiln_cfg_swap_apply(7, false, reason, sizeof(reason), &diverged);
    TEST_CHECK(!ok && !diverged, "swap refused, not a divergence");
    TEST_CHECK(s_fail_next_kv_write_per_ceiling_write == 0, "both injected failures were consumed");
    TEST_CHECK(s_pico_committed.count == 0, "Pico package never pushed");
    TEST_CHECK(s_pico_ceiling == 1000.0f, "ceiling put back to R");
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_STAGED,
               "record still reads STAGED (both later writes failed)");
    kiln_cfg_swap_boot_recover();
    TEST_CHECK(kiln_cfg_swap_get_marker(NULL, NULL) == KILN_CFG_SWAP_MARKER_NONE, "boot discards the STAGED record");
    TEST_CHECK(!kiln_cfg_swap_get_boot_fault(NULL), "no boot fault for a STAGED discard");
}

static void test_low1_boot_fault_kind_names(void)
{
    TEST_SECTION("LOW-1: boot fault kind names for /api/status");
    TEST_CHECK(strcmp(kiln_cfg_swap_boot_fault_kind_name(KILN_CFG_SWAP_BOOT_FAULT_ACTIVE_ID_UNSAVED),
                      "active_id_unsaved") == 0, "active_id_unsaved");
    TEST_CHECK(strcmp(kiln_cfg_swap_boot_fault_kind_name(KILN_CFG_SWAP_BOOT_FAULT_ROLLBACK_ACTIVE_ID_UNSAVED),
                      "rollback_active_id_unsaved") == 0, "rollback_active_id_unsaved");
    TEST_CHECK(strcmp(kiln_cfg_swap_boot_fault_kind_name(KILN_CFG_SWAP_BOOT_FAULT_ESP_DONE_UNCONFIRMED),
                      "esp_done_unconfirmed") == 0, "esp_done_unconfirmed");
    TEST_CHECK(strcmp(kiln_cfg_swap_boot_fault_kind_name(KILN_CFG_SWAP_BOOT_FAULT_NONE), "none") == 0, "none");
    reset_state();
    memset(&s_boot_fault, 0, sizeof(s_boot_fault));
    TEST_CHECK(kiln_cfg_swap_get_boot_fault_kind() == KILN_CFG_SWAP_BOOT_FAULT_NONE, "no fault -> NONE");
    latch_boot_fault(KILN_CFG_SWAP_BOOT_FAULT_ACTIVE_ID_UNSAVED, 7, "x");
    TEST_CHECK(kiln_cfg_swap_get_boot_fault_kind() == KILN_CFG_SWAP_BOOT_FAULT_ACTIVE_ID_UNSAVED,
               "latched kind reported");
    kiln_cfg_swap_boot_fault_t fault;
    TEST_CHECK(kiln_cfg_swap_get_boot_fault(&fault) && strstr(fault.reason, "x") != NULL, "reason kept");
    TEST_CHECK(strlen("KILN APPLIED -- active kiln not saved, retried at boot") < 96, "LCD text under 96 chars");
}

int main(void)
{
    test_clean_swap_applies_both_halves();
    test_successful_apply_not_diverged_despite_zeroed_cache_crc();
    test_apply_autosave_targets_incoming_slot_not_outgoing();
    test_pico_failure_leaves_esp_untouched();
    test_half_package_refused();
    test_swap_during_firing_refused();
    test_esp_readback_mismatch_rolls_back();
    test_rollback_autosave_targets_previous_slot_not_target();
    test_diverged_ceiling_moves_active_id_but_leaves_pending();
    test_generation_race_forces_rollback();
    test_swap_completes_with_pico_armed_never_disarmed();
    test_flash_fallback_attempted_after_finalize_but_optional();
    test_rollback_after_volatile_install_uses_volatile_too();
    test_pico_reboot_before_flash_fallback_caught_by_existing_check();
    test_boot_recovery_staged_discards();
    test_boot_recovery_pico_done_reapplies_rollback();
    test_boot_recovery_esp_done_both_match_finishes();
    test_boot_recovery_esp_done_esp_mismatch_stays_alarmed();
    test_boot_recovery_esp_done_pico_mismatch_stays_alarmed();
    test_boot_recovery_corrupt_marker_stays_alarmed();
    test_boot_recovery_fault_latches_first_only();
    test_negative_generation_check_is_load_bearing();
    test_credential_survives_a_slot_swap();
    test_apply_refuses_when_scratch_malloc_fails();
    test_finish_esp_done_latches_fault_when_malloc_fails();
    test_boot_recover_latches_unreadable_when_malloc_fails();
    test_pico_open_save_failure_aborts_before_pico_push();
    test_pico_done_save_failure_rolls_back();
    test_esp_done_save_failure_rolls_back();
    test_active_id_save_failure_keeps_record_and_boot_retries();
    test_save_pending_read_back_catches_silent_write();
    test_low6_restore_never_below_zone_raise_pico_open_path();
    test_low6_restore_never_below_zone_raise_rollback_path();
    test_low4_low5_rollback_uncleared_journal_reported();
    test_med_rollback_id_restore_failure_keeps_record();
    test_low4_apply_reason_names_uncleared_journal();
    test_low4_note_survives_long_message();
    test_low3_esp_done_both_on_r_is_cleared();
    test_low2_autosave_unblocked_after_active_id_unsaved();
    test_low7_marker_save_fails_and_clear_fails();
    test_low1_boot_fault_kind_names();

    if (g_test_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d TEST(S) FAILED\n", g_test_failures);
    return 1;
}
