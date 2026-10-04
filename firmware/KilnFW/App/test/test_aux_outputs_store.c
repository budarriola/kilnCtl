// Host tests for App/drivers/persist/aux_outputs_cfg.c and
// aux_outputs_conflict.h -- WP-1 of docs/SPARE_RELAY_ONOFF_PLAN.md.
// #includes aux_outputs_cfg.c directly (same convention as
// test_display_power_cfg.c) to reach its file-scope state for the simulated
// reboot.
//
// LOAD-BEARING PROPERTIES:
//   1. "One relay, one owner": the pure predicate and the setter refuse an aux
//      enable on a zone-claimed relay.
//   2. A persisted conflict (corrupt pair / a missed writer) at start() forces
//      the AUX off in RAM, sets the sticky conflict flag, and never touches the
//      persisted blob (the zone is never stripped).
//   3. A newer-version blob is quarantined (not defaulted over) and set()
//      refuses to overwrite it; a corrupt blob falls back to all-disabled.
//   4. Fresh/default state is all-disabled, fail-safe fixed OFF.
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TAO_MKDIR(p) _mkdir(p)
#define TAO_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TAO_MKDIR(p) mkdir((p), 0755)
#define TAO_RMDIR(p) rmdir(p)
#endif

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"

#include "cfg_fs.h"

#include "../drivers/persist/aux_outputs_cfg.c"

static const char *AO_SCRATCH_BASE = "cfg_fs_test_aux_outputs";

static void ao_cfg_fs_reset(void)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/.tmp/%s", AO_SCRATCH_BASE, AUX_OUTPUTS_FILE_PATH);
    remove(path);
    snprintf(path, sizeof(path), "%s/%s", AO_SCRATCH_BASE, AUX_OUTPUTS_FILE_PATH);
    remove(path);
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s/.tmp", AO_SCRATCH_BASE);
    TAO_RMDIR(tmp);
    TAO_RMDIR(AO_SCRATCH_BASE);
    TAO_MKDIR(AO_SCRATCH_BASE);
    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
}

/* Wipes the RAM state to deliberately WRONG values so start() is proven to
 * overwrite them, leaving the fake NVS (flash stand-in) untouched. */
static void simulate_reboot(void)
{
    memset(s_entries, 0x5A, sizeof(s_entries));
    s_enabled_mask = 0x0F;
    s_conflict_mask = 0x0F;
    s_quarantined = true;
    s_rev = 77;
}

static void fresh_board(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
}

static aux_output_entry_t on_entry(void)
{
    aux_output_entry_t e;
    memset(&e, 0, sizeof(e));
    e.enabled = 1;
    return e;
}

/* Stashes an arbitrary blob straight into the fake NVS, bypassing set(). */
static void stash_blob(const void *bytes, size_t len, uint32_t rev)
{
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "test setup: open for stash");
    TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_AUX_OUT, bytes, len) == HAL_OK, "test setup: stash blob");
    hal_kv_set_u32(&h, NVS_KEY_AUX_OUT_REV, rev);
    hal_kv_commit(&h);
    hal_kv_close(&h);
}

static void test_predicate(void)
{
    TEST_SECTION("aux_outputs_relay_conflict: pure predicate, bit i = relay i+1");
    TEST_CHECK(!aux_outputs_relay_conflict(0x00, 0x00), "nothing claimed: no conflict");
    TEST_CHECK(!aux_outputs_relay_conflict(0x03, 0x0C), "disjoint zone/aux sets: no conflict");
    TEST_CHECK(aux_outputs_relay_conflict(0x03, 0x02), "relay 2 claimed by both: conflict");
    TEST_CHECK(aux_outputs_relay_conflict_mask(0x0F, 0x0A) == 0x0A, "mask names every doubly-claimed relay");
    TEST_CHECK(!aux_outputs_relay_conflict(0xFF, 0x00), "no enabled aux: never a conflict");
}

static void test_defaults_all_disabled(void)
{
    TEST_SECTION("aux_outputs_cfg_start on empty NVS: every aux disabled, no conflict, not quarantined");
    fresh_board();
    TEST_CHECK(aux_outputs_cfg_start(0x03) == ESP_OK, "start succeeds on an empty store");
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0, "no aux enabled");
    TEST_CHECK(!aux_outputs_cfg_conflict(), "no conflict flag");
    TEST_CHECK(!aux_outputs_cfg_quarantined(), "not quarantined");
    aux_output_t o;
    TEST_CHECK(aux_outputs_cfg_get(3, &o) && !o.enabled, "relay 3 reads disabled");
    TEST_CHECK(o.hyst_c == AUX_HYST_C_DEFAULT && o.min_on_s == AUX_MIN_ON_OFF_S_DEFAULT &&
                   o.min_off_s == AUX_MIN_ON_OFF_S_DEFAULT && o.tc_zone == AUX_TC_ZONE_NONE,
               "zero fields normalise to the documented defaults at read; tc_zone_plus1 0 reads as none");
    TEST_CHECK(!aux_outputs_cfg_get(0, &o) && !aux_outputs_cfg_get(5, &o), "relay outside 1..4 is refused");
}

static void test_set_round_trip(void)
{
    TEST_SECTION("aux_outputs_cfg_set: validates, persists, survives a reboot, tc_zone encoded plus1");
    fresh_board();
    aux_outputs_cfg_start(0x03);
    aux_output_entry_t e = on_entry();
    e.tc_zone_plus1 = 2; /* zone index 1 */
    e.hyst_c = 5.0f;
    e.min_on_s = 60;
    e.min_off_s = 90;
    TEST_CHECK(aux_outputs_cfg_set(4, &e, 0x03) == ESP_OK, "enable relay 4 (zones own 1,2)");
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0x08, "RAM enabled mask is bit 3");

    simulate_reboot();
    TEST_CHECK(aux_outputs_cfg_start(0x03) == ESP_OK, "restart ok");
    aux_output_t o;
    TEST_CHECK(aux_outputs_cfg_get(4, &o), "get relay 4");
    TEST_CHECK(o.enabled && !o.conflicted, "relay 4 enabled after reload");
    TEST_CHECK(o.tc_zone == 1 && o.hyst_c == 5.0f && o.min_on_s == 60 && o.min_off_s == 90, "fields round trip");
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0x08, "mask survives");
    TEST_CHECK(aux_outputs_cfg_get(1, &o) && !o.enabled, "other relays stay disabled");
}

static void test_set_refuses_invalid(void)
{
    TEST_SECTION("aux_outputs_cfg_set: invalid fields refused, nothing changed");
    fresh_board();
    aux_outputs_cfg_start(0);
    aux_output_entry_t e = on_entry();
    TEST_CHECK(aux_outputs_cfg_set(0, &e, 0) == ESP_ERR_INVALID_ARG, "relay 0 refused");
    TEST_CHECK(aux_outputs_cfg_set(5, &e, 0) == ESP_ERR_INVALID_ARG, "relay 5 refused");
    TEST_CHECK(aux_outputs_cfg_set(1, NULL, 0) == ESP_ERR_INVALID_ARG, "NULL entry refused");
    e.hyst_c = 0.2f;
    TEST_CHECK(aux_outputs_cfg_set(1, &e, 0) == ESP_ERR_INVALID_ARG, "hyst_c below the 0.5 sliver refused");
    e = on_entry();
    e.hyst_c = 26.0f;
    TEST_CHECK(aux_outputs_cfg_set(1, &e, 0) == ESP_ERR_INVALID_ARG, "hyst_c above 25 refused");
    e = on_entry();
    e.min_on_s = 3601;
    TEST_CHECK(aux_outputs_cfg_set(1, &e, 0) == ESP_ERR_INVALID_ARG, "min_on_s above 3600 refused");
    e = on_entry();
    e.tc_zone_plus1 = (uint8_t)(MAX31856_CHANNEL_COUNT + 1);
    TEST_CHECK(aux_outputs_cfg_set(1, &e, 0) == ESP_ERR_INVALID_ARG, "tc_zone beyond the channel count refused");
    e = on_entry();
    e.reserved = 1;
    TEST_CHECK(aux_outputs_cfg_set(1, &e, 0) == ESP_ERR_INVALID_ARG, "non-zero reserved refused");
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0, "refused sets left RAM untouched");
}

static void test_set_refuses_zone_claimed_relay(void)
{
    TEST_SECTION("aux_outputs_cfg_set: enabling a zone-claimed relay is refused (one relay, one owner)");
    fresh_board();
    aux_outputs_cfg_start(0x03);
    aux_output_entry_t e = on_entry();
    TEST_CHECK(aux_outputs_cfg_set(2, &e, 0x03) == ESP_ERR_INVALID_STATE, "relay 2 is zone-owned: refused");
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0, "refused: RAM untouched");
    simulate_reboot();
    aux_outputs_cfg_start(0x03);
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0, "refused: nothing persisted");
    e.enabled = 0;
    TEST_CHECK(aux_outputs_cfg_set(2, &e, 0x03) == ESP_OK, "DISABLING an aux on a zone-claimed relay is always allowed");
}

static void test_boot_conflict_forces_aux_off_in_ram_only(void)
{
    TEST_SECTION("start(): persisted aux/zone conflict -> aux forced off in RAM, sticky flag, blob untouched");
    fresh_board();
    aux_outputs_cfg_start(0);
    aux_output_entry_t e = on_entry();
    TEST_CHECK(aux_outputs_cfg_set(2, &e, 0) == ESP_OK, "persist aux on relay 2 and 4");
    TEST_CHECK(aux_outputs_cfg_set(4, &e, 0) == ESP_OK, "persist aux on relay 4");

    /* A zones write that bypassed the check now claims relay 2. */
    simulate_reboot();
    TEST_CHECK(aux_outputs_cfg_start(0x03) == ESP_OK, "start tolerates the conflict (non-fatal)");
    TEST_CHECK(aux_outputs_cfg_conflict(), "sticky aux_conflict flag set");
    TEST_CHECK(aux_outputs_cfg_conflict_mask() == 0x02, "flag names relay 2 only");
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0x08, "relay 2 aux forced off; unconflicted relay 4 still on");
    aux_output_t o;
    TEST_CHECK(aux_outputs_cfg_get(2, &o) && !o.enabled && o.conflicted, "relay 2 reads disabled + conflicted");

    /* Persisted blob untouched: with the zone claim gone the aux comes back. */
    simulate_reboot();
    aux_outputs_cfg_start(0);
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0x0A, "no silent rewrite: both aux return once the conflict is gone");
    TEST_CHECK(!aux_outputs_cfg_conflict(), "flag clears when the cause is gone");

    /* Operator action clears the flag for that relay. */
    simulate_reboot();
    aux_outputs_cfg_start(0x03);
    e.enabled = 0;
    TEST_CHECK(aux_outputs_cfg_set(2, &e, 0x03) == ESP_OK, "operator disables the conflicted aux");
    TEST_CHECK(!aux_outputs_cfg_conflict(), "sticky flag cleared by the operator's set");
}

static void test_newer_version_quarantined(void)
{
    TEST_SECTION("start(): newer-version blob is quarantined, not defaulted; set() refuses to overwrite it");
    fresh_board();
    aux_outputs_cfg_start(0);
    uint8_t wide[100];
    memset(wide, 0xAB, sizeof(wide));
    wide[0] = AUX_OUTPUTS_CFG_VERSION + 1;
    stash_blob(wide, sizeof(wide), 9);

    simulate_reboot();
    TEST_CHECK(aux_outputs_cfg_start(0) == ESP_OK, "start non-fatal");
    TEST_CHECK(aux_outputs_cfg_quarantined(), "quarantined flag set");
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0, "all aux disabled while quarantined");
    aux_output_entry_t e = on_entry();
    TEST_CHECK(aux_outputs_cfg_set(1, &e, 0) == ESP_ERR_INVALID_STATE, "set refuses while quarantined");

    hal_kv_handle_t h;
    uint8_t back[100];
    size_t len = sizeof(back);
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK, "open");
    TEST_CHECK(hal_kv_get_blob(&h, NVS_KEY_AUX_OUT, back, &len) == HAL_OK && len == sizeof(wide) &&
                   memcmp(back, wide, len) == 0,
               "the newer firmware's blob is byte-for-byte intact");
    hal_kv_close(&h);
}

static void test_corrupt_blob_defaults(void)
{
    TEST_SECTION("start(): corrupt blob (bad CRC / wrong size) -> all-disabled defaults, may be rewritten");
    fresh_board();
    aux_outputs_cfg_start(0);
    aux_output_entry_t e = on_entry();
    TEST_CHECK(aux_outputs_cfg_set(3, &e, 0) == ESP_OK, "persist a good blob");

    /* Flip one payload byte inside the stored blob: CRC no longer matches. */
    hal_kv_handle_t h;
    aux_outputs_blob_t blob;
    size_t len = sizeof(blob);
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK, "open");
    TEST_CHECK(hal_kv_get_blob(&h, NVS_KEY_AUX_OUT, &blob, &len) == HAL_OK && len == sizeof(blob), "read back");
    blob.entries[0].hyst_c = 3.0f;
    hal_kv_set_blob(&h, NVS_KEY_AUX_OUT, &blob, sizeof(blob));
    hal_kv_commit(&h);
    hal_kv_close(&h);

    simulate_reboot();
    aux_outputs_cfg_start(0);
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0, "bad CRC: fall back to all disabled (never trusted)");
    TEST_CHECK(!aux_outputs_cfg_quarantined(), "a corrupt blob is not a quarantine");
    TEST_CHECK(aux_outputs_cfg_set(3, &e, 0) == ESP_OK, "and it can be rewritten");

    uint8_t two[2] = { 1, 2 };
    stash_blob(two, sizeof(two), 1);
    simulate_reboot();
    aux_outputs_cfg_start(0);
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0 && !aux_outputs_cfg_quarantined(), "wrong size: defaults");
}

static void test_dual_write_and_file_tiebreak(void)
{
    TEST_SECTION("cfg_fs dual-write: file mirrors NVS, higher rev wins on divergence");
    fresh_board();
    ao_cfg_fs_reset();
    TEST_CHECK(cfg_fs_init(AO_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    aux_outputs_cfg_start(0);
    aux_output_entry_t e = on_entry();
    TEST_CHECK(aux_outputs_cfg_set(1, &e, 0) == ESP_OK, "set lands");

    bool fv = false, nv = false, div = true;
    uint32_t fr = 0, nr = 0;
    aux_outputs_cfg_get_dualwrite_status(&fv, &fr, &nv, &nr, &div);
    TEST_CHECK(fv && nv && fr == nr && fr == 1 && !div, "file and NVS both valid, same rev, not diverged");

    /* File ahead of NVS: a later write that only reached the file. */
    aux_outputs_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = AUX_OUTPUTS_CFG_VERSION;
    blob.entries[2].enabled = 1;
    blob.crc32 = blob_crc(&blob);
    TEST_CHECK(pref_cfg_fs_save(AUX_OUTPUTS_FILE_PATH, &blob, sizeof(blob), 5) == ESP_OK, "file-only newer write");
    simulate_reboot();
    aux_outputs_cfg_start(0);
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0x04, "higher-rev file wins over NVS");

    /* Zone conflict applies regardless of which side supplied the value. */
    simulate_reboot();
    aux_outputs_cfg_start(0x04);
    TEST_CHECK(aux_outputs_cfg_conflict_mask() == 0x04 && aux_outputs_cfg_enabled_mask() == 0,
               "file-sourced entry is reconciled against the zones too");

    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
}

void run_test_aux_outputs_store(void)
{
    test_predicate();
    test_defaults_all_disabled();
    test_set_round_trip();
    test_set_refuses_invalid();
    test_set_refuses_zone_claimed_relay();
    test_boot_conflict_forces_aux_off_in_ram_only();
    test_newer_version_quarantined();
    test_corrupt_blob_defaults();
    test_dual_write_and_file_tiebreak();

    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
    fake_kv_reset_all();
}
