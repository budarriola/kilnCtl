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
#include "../drivers/safety/safety_pico_relay_mask.h"

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

/* Fresh fake NVS plus a freshly mounted cfg scratch: saves are cfg-file-only
 * since the dual-write close, so a mounted partition is the normal case. */
static void fresh_board(void)
{
    ao_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(AO_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts against the scratch dir");
    simulate_reboot();
}

/* Same, with NO cfg partition mounted. */
static void fresh_board_unmounted(void)
{
    ao_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
}

static aux_outputs_blob_t make_blob_one_enabled(unsigned idx)
{
    aux_outputs_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = AUX_OUTPUTS_CFG_VERSION;
    blob.entries[idx].enabled = 1;
    blob.crc32 = blob_checksum(&blob);
    return blob;
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

static void test_oversize_blob_quarantined(void)
{
    TEST_SECTION("start(): a blob larger than 256 B (wider newer schema) is quarantined, never defaulted over");
    fresh_board();
    aux_outputs_cfg_start(0);
    uint8_t huge[400];
    memset(huge, 0xCD, sizeof(huge));
    huge[0] = AUX_OUTPUTS_CFG_VERSION + 1;
    stash_blob(huge, sizeof(huge), 9);

    simulate_reboot();
    aux_outputs_cfg_start(0);
    TEST_CHECK(aux_outputs_cfg_quarantined(), "oversize blob quarantines");
    aux_output_entry_t e = on_entry();
    TEST_CHECK(aux_outputs_cfg_set(1, &e, 0) == ESP_ERR_INVALID_STATE, "set refuses to overwrite the oversize blob");
    hal_kv_handle_t h;
    uint8_t back[400];
    size_t len = sizeof(back);
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK, "open");
    TEST_CHECK(hal_kv_get_blob(&h, NVS_KEY_AUX_OUT, back, &len) == HAL_OK && len == sizeof(huge) &&
                   memcmp(back, huge, len) == 0,
               "oversize blob intact");
    hal_kv_close(&h);
}

static void test_checksum_known_answer(void)
{
    TEST_SECTION("blob checksum: known-answer vector pins the on-flash CRC32/IEEE (zlib) format");
    /* Expected value computed independently with Python zlib.crc32 over the same 52 bytes
     * (bytes (i*7+3)&255, i = 0..51 == offsetof(aux_outputs_blob_t, crc32)). */
    uint8_t raw[sizeof(aux_outputs_blob_t)];
    memset(raw, 0, sizeof(raw));
    for (size_t i = 0; i < offsetof(aux_outputs_blob_t, crc32); i++) {
        raw[i] = (uint8_t)((i * 7u + 3u) & 0xFFu);
    }
    aux_outputs_blob_t blob;
    memcpy(&blob, raw, sizeof(blob));
    TEST_CHECK(offsetof(aux_outputs_blob_t, crc32) == 52, "checksummed span is 52 bytes");
    TEST_CHECK(blob_checksum(&blob) == 0x95bead8au, "checksum equals the independent zlib CRC-32 of the same bytes");
}

static void test_corrupt_blob_defaults(void)
{
    TEST_SECTION("start(): corrupt blob (bad CRC / wrong size) -> all-disabled defaults, may be rewritten");
    fresh_board();
    aux_outputs_cfg_start(0);
    aux_output_entry_t e = on_entry();
    TEST_CHECK(aux_outputs_cfg_set(3, &e, 0) == ESP_OK, "persist a good blob");

    /* Overwrite the cfg file with a blob whose CRC no longer matches. */
    aux_outputs_blob_t blob = make_blob_one_enabled(2);
    blob.entries[0].hyst_c = 3.0f; /* payload changed after the CRC was computed */
    TEST_CHECK(pref_cfg_fs_save(AUX_OUTPUTS_FILE_PATH, &blob, sizeof(blob), 9) == ESP_OK, "stage a bad-CRC file");

    simulate_reboot();
    aux_outputs_cfg_start(0);
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0, "bad CRC: fall back to all disabled (never trusted)");
    TEST_CHECK(!aux_outputs_cfg_quarantined(), "a corrupt blob is not a quarantine");
    TEST_CHECK(aux_outputs_cfg_set(3, &e, 0) == ESP_OK, "and it can be rewritten");

    /* A legacy NVS blob of the wrong size, no cfg file. */
    fresh_board();
    uint8_t two[2] = { 1, 2 };
    stash_blob(two, sizeof(two), 1);
    simulate_reboot();
    aux_outputs_cfg_start(0);
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0 && !aux_outputs_cfg_quarantined(), "wrong size: defaults");
}

static void test_set_without_cfg_partition_fails_loud(void)
{
    TEST_SECTION("aux_outputs_cfg_set: no cfg partition mounted -- fails loud, nothing falls back to NVS");
    fresh_board_unmounted();
    aux_outputs_cfg_start(0);
    aux_output_entry_t e = on_entry();
    TEST_CHECK(aux_outputs_cfg_set(1, &e, 0) != ESP_OK, "set() reports the failed cfg write");
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_NOT_FOUND,
               "no NVS write was made as a fallback");
}

static void test_dual_write_and_file_tiebreak(void)
{
    TEST_SECTION("cfg-only save: file written, NVS untouched, higher rev wins, equal-rev legacy NVS wins");
    fresh_board();
    aux_outputs_cfg_start(0);
    aux_output_entry_t e = on_entry();
    TEST_CHECK(aux_outputs_cfg_set(1, &e, 0) == ESP_OK, "set lands");

    bool fv = false, nv = false, div = true;
    uint32_t fr = 0, nr = 0;
    aux_outputs_cfg_get_dualwrite_status(&fv, &fr, &nv, &nr, &div);
    TEST_CHECK(fv && !nv && fr == 1 && !div, "file valid at rev 1, no NVS copy written, not diverged");

    /* File ahead of NVS: a later write that only reached the file. */
    aux_outputs_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = AUX_OUTPUTS_CFG_VERSION;
    blob.entries[2].enabled = 1;
    blob.crc32 = blob_checksum(&blob);
    TEST_CHECK(pref_cfg_fs_save(AUX_OUTPUTS_FILE_PATH, &blob, sizeof(blob), 5) == ESP_OK, "file-only newer write");
    simulate_reboot();
    aux_outputs_cfg_start(0);
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0x04, "higher-rev file wins over NVS");

    /* Zone conflict applies regardless of which side supplied the value. */
    simulate_reboot();
    aux_outputs_cfg_start(0x04);
    TEST_CHECK(aux_outputs_cfg_conflict_mask() == 0x04 && aux_outputs_cfg_enabled_mask() == 0,
               "file-sourced entry is reconciled against the zones too");

    /* A legacy/rollback writer left an NVS copy at the SAME rev with different
     * content: NVS wins the equal-rev tie (the dangerous case stays visible). */
    aux_outputs_blob_t legacy = make_blob_one_enabled(0);
    stash_blob(&legacy, sizeof(legacy), 5);
    simulate_reboot();
    aux_outputs_cfg_start(0);
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0x01, "equal rev, differing bytes: NVS wins");

    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
}

/* WP-9: the helper the ESP uses to strip aux-bound relays from every mask it sends the Pico. */
static void test_pico_mask_strips_aux(void)
{
    TEST_SECTION("safety_pico_relay_mask: aux bits stripped, non-aux bits untouched, tracks live config");
    fresh_board();
    aux_outputs_cfg_start(0x03); /* zones own relays 1,2 */

    TEST_CHECK(safety_pico_relay_mask(0x0F) == 0x0F, "no aux enabled: mask passes through unchanged");

    aux_output_entry_t e = on_entry();
    TEST_CHECK(aux_outputs_cfg_set(4, &e, 0x03) == ESP_OK, "enable aux on relay 4");
    TEST_CHECK(safety_pico_relay_mask(0x0F) == 0x07, "aux relay 4 stripped, heater relays 1-3 kept");
    TEST_CHECK(safety_pico_relay_mask(0x08) == 0x00, "aux-only shadow reads as nothing commanded");
    TEST_CHECK(safety_pico_relay_mask(0x03) == 0x03, "non-aux bits unchanged when aux is off");

    /* Live tracking: a second aux, then disabling the first, no restart in between. */
    TEST_CHECK(aux_outputs_cfg_set(3, &e, 0x03) == ESP_OK, "enable aux on relay 3 as well");
    TEST_CHECK(safety_pico_relay_mask(0x0F) == 0x03, "relays 3 and 4 both stripped after the change");
    e.enabled = 0;
    TEST_CHECK(aux_outputs_cfg_set(4, &e, 0x03) == ESP_OK, "disable aux on relay 4");
    TEST_CHECK(safety_pico_relay_mask(0x0F) == 0x0B, "relay 4 reported again at once; relay 3 still stripped");

    /* Bound-but-disabled is not stripped; a conflict-forced-off aux is not stripped either. */
    TEST_CHECK(safety_pico_relay_mask(0x08) == 0x08, "disabled aux relay 4 is reported to the Pico");
    simulate_reboot();
    aux_outputs_cfg_start(0x04); /* a zone now claims relay 3 -> aux 3 forced off in RAM */
    TEST_CHECK(safety_pico_relay_mask(0x04) == 0x04, "conflict-forced-off aux never hides a zone's relay");

    TEST_CHECK(safety_pico_relay_mask_strip(0xFF, 0x0A) == 0xF5, "pure core: only the named bits are removed");
}

/* The call site cannot be linked on the host (FreeRTOS), so pin its shape in source:
 * the strip must feed relay_now_mask BEFORE the recent-mask bookkeeping. */
static void test_pico_mask_call_site_shape(void)
{
    TEST_SECTION("safety_link_frames.c routes the Pico relay mask through safety_pico_relay_mask() first");
    char path[600];
    snprintf(path, sizeof(path), "%s", __FILE__);
    char *slash = strrchr(path, '/');
    char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) {
        slash = bslash;
    }
    if (!slash) {
        TEST_CHECK(false, "cannot derive the test directory from __FILE__");
        return;
    }
    snprintf(slash + 1, sizeof(path) - (size_t)(slash + 1 - path), "%s", "../drivers/safety/safety_link_frames.c");
    FILE *f = fopen(path, "rb");
    TEST_CHECK(f != NULL, "safety_link_frames.c readable from the test directory");
    if (!f) {
        return;
    }
    static char src[400000];
    size_t n = fread(src, 1, sizeof(src) - 1, f);
    fclose(f);
    src[n] = 0;
    const char *strip = strstr(src, "relay_now_mask = io ? safety_pico_relay_mask(kiln_io_get_relay_shadow(io))");
    const char *recent = strstr(src, "relay_recent_mask = safety_context_update_relay_recent(link, relay_now_mask)");
    TEST_CHECK(strip != NULL, "relay_now_mask is built through safety_pico_relay_mask()");
    TEST_CHECK(recent != NULL, "relay_recent_mask is derived from that same relay_now_mask");
    TEST_CHECK(strip && recent && strip < recent, "strip happens before the recent-mask update");
    {
        /* Exactly one call site: catches the ternary-revert form and any new raw use. */
        int uses = 0;
        for (const char *p = src; (p = strstr(p, "kiln_io_get_relay_shadow(")) != NULL; p += 1) {
            uses++;
        }
        TEST_CHECK(uses == 1, "kiln_io_get_relay_shadow( appears exactly once in safety_link_frames.c");
    }
}

static void test_raw_verify_and_journal(void)
{
    TEST_SECTION("get_raw / verify_persisted / conversion journal");
    fresh_board();
    aux_outputs_cfg_start(0x03);
    aux_output_entry_t e = on_entry();
    e.hyst_c = 0.0f; /* zero stays zero in the RAW view, defaults only at get() */
    TEST_CHECK(aux_outputs_cfg_set(4, &e, 0x03) == ESP_OK, "enable relay 4");
    aux_output_entry_t raw;
    memset(&raw, 0xEE, sizeof(raw));
    TEST_CHECK(aux_outputs_cfg_get_raw(4, &raw) && raw.enabled == 1 && raw.hyst_c == 0.0f && raw.min_on_s == 0,
               "get_raw returns the stored entry without defaults");
    TEST_CHECK(!aux_outputs_cfg_get_raw(0, &raw) && !aux_outputs_cfg_get_raw(5, &raw), "get_raw refuses a bad relay");
    TEST_CHECK(aux_outputs_cfg_verify_persisted(), "verify_persisted true right after a good save");

    fake_kv_script_next_write_status(HAL_IO);
    aux_output_entry_t e2 = on_entry();
    e2.min_on_s = 30;
    TEST_CHECK(aux_outputs_cfg_set(3, &e2, 0x03) != ESP_OK, "save failure is returned");
    TEST_CHECK(aux_outputs_cfg_get_raw(3, &raw) && raw.enabled == 1, "the RAM value stands after the failed save");
    TEST_CHECK(!aux_outputs_cfg_verify_persisted(), "verify_persisted false: RAM and NVS differ");

    aux_convert_journal_t j;
    memset(&j, 0, sizeof(j));
    TEST_CHECK(!aux_convert_journal_read(&j), "no marker on a fresh board");
    TEST_CHECK(aux_convert_journal_clear(), "clear with no marker is fine");
    aux_convert_journal_t w = {.zone = 1, .relay = 4, .stage = 2, .has_tc = 1, .hyst_c = 3.5f, .min_on_s = 60, .min_off_s = 90};
    TEST_CHECK(aux_convert_journal_write(&w), "write marker");
    memset(&j, 0, sizeof(j));
    TEST_CHECK(aux_convert_journal_read(&j) && j.zone == 1 && j.relay == 4 && j.stage == 2 && j.has_tc == 1 &&
                   j.hyst_c == 3.5f && j.min_on_s == 60 && j.min_off_s == 90,
               "marker round trips");
    TEST_CHECK(aux_convert_journal_read(NULL), "read tolerates a NULL out");
    w.stage = 3;
    TEST_CHECK(aux_convert_journal_write(&w) && aux_convert_journal_read(&j) && j.stage == 3, "marker updated in place");
    fake_kv_script_next_write_status(HAL_IO);
    w.stage = 4;
    TEST_CHECK(!aux_convert_journal_write(&w), "a failed marker write is reported");
    TEST_CHECK(aux_convert_journal_clear() && !aux_convert_journal_read(&j), "clear removes the marker");
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
    test_oversize_blob_quarantined();
    test_checksum_known_answer();
    test_corrupt_blob_defaults();
    test_set_without_cfg_partition_fails_loud();
    test_dual_write_and_file_tiebreak();
    test_pico_mask_strips_aux();
    test_pico_mask_call_site_shape();
    test_raw_verify_and_journal();

    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
    fake_kv_reset_all();
}
