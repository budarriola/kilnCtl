// Host test for App/drivers/persist/profiles_builtin.c -- specifically the 2026-09-05
// "Unrated" cone sentinel (PROFILES_BUILTIN_CONE_UNRATED, owner decision):
// the 10 catalogue entries whose Digital Fire source page states no cone
// number (FSCG1, FSCGB1, FSCGCL, FSCGWM, FSCRGL, FSHP1, FSHP3, FSNM5, MDDCL,
// QICA -- previously marked UNRESOLVED with an invented placeholder number
// in profiles_builtin_table.inc) now carry that sentinel instead, and must
// print as "Unrated" via profiles_builtin_cone_label(), not a number.
//
// Until 2026-09-21 this file also checked that the sentinel sorted AFTER
// every real cone, via profiles_builtin_cone_sort_key(). That helper was
// removed together with its last production caller, the dead LCD page
// ui_page_profiles_builtin_list.c: nothing in the firmware orders builtins by
// cone today -- the web list sorts by last run start, and the LCD picker
// follows table order. Note for whoever adds cone ordering next: INT8_MIN
// sorts FIRST under a plain signed compare, the opposite of what is wanted,
// so re-add a widened sort key and a test that exercises it.
//
// This file #includes profiles_builtin.c directly (own executable, same
// convention as test_profiles_http.c/test_zones_http.c) so it links the
// REAL, generated profiles_builtin_table.inc rather than a test-local fake
// catalogue -- the defect class this guards against (a sort/label bug) only
// exists against the real table's actual entries.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

// profiles_builtin.c's hidden-mask persistence (profiles_builtin_start()/
// profiles_builtin_set_hidden()) now calls hal_kv_get_u32()/hal_kv_set_u32()
// (HW_ABSTRACTION.md Phase 3 item 3, the nvs.h -> hal_kv.h migration)
// -- none of this file's tests exercise that path, but the symbols must
// still resolve at link time, so this executable links the real fake_kv.c
// backend (build_host_tests.ps1's exe23) rather than hand-rolling
// always-empty/always-OK stand-ins the way the old nvs.h-based version did.
#include "esp_err.h"
#include "fake_kv.h"

#ifdef _WIN32
#include <direct.h>
#define PB_MKDIR(p) _mkdir(p)
#define PB_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define PB_MKDIR(p) mkdir((p), 0755)
#define PB_RMDIR(p) rmdir(p)
#endif

#include "cfg_fs.h"

#include "profiles_builtin.c"

static const char *UNRATED_CODES[] = {
    "FSCG1", "FSCGB1", "FSCGCL", "FSCGWM", "FSCRGL",
    "FSHP1", "FSHP3", "FSNM5", "MDDCL", "QICA",
};
#define UNRATED_CODE_COUNT (sizeof(UNRATED_CODES) / sizeof(UNRATED_CODES[0]))

static bool code_is_expected_unrated(const char *code)
{
    for (size_t i = 0; i < UNRATED_CODE_COUNT; i++) {
        if (strcmp(code, UNRATED_CODES[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void test_cone_label_unrated(void)
{
    TEST_SECTION("profiles_builtin_cone_label -- PROFILES_BUILTIN_CONE_UNRATED prints \"Unrated\", "
                 "not a number");
    char buf[8];
    profiles_builtin_cone_label(PROFILES_BUILTIN_CONE_UNRATED, buf, sizeof(buf));
    TEST_CHECK(strcmp(buf, "Unrated") == 0, "sentinel prints as the literal word \"Unrated\"");
}

static void test_cone_label_real_cone_unaffected(void)
{
    TEST_SECTION("profiles_builtin_cone_label -- a real low-fire cone (-4, i.e. cone04) still prints "
                 "\"04\", unaffected by the sentinel branch");
    char buf[8];
    profiles_builtin_cone_label(-4, buf, sizeof(buf));
    TEST_CHECK(strcmp(buf, "04") == 0, "existing negative-encoding printed form is untouched");
}

static void test_exactly_the_ten_named_entries_are_unrated(void)
{
    TEST_SECTION("g_builtin_profiles -- exactly the 10 named codes carry the Unrated sentinel; every "
                 "other entry keeps a real (non-sentinel) cone");
    size_t found_unrated = 0;
    for (size_t i = 0; i < g_builtin_profile_count; i++) {
        const builtin_profile_t *b = &g_builtin_profiles[i];
        bool expected = code_is_expected_unrated(b->code);
        bool actual = (b->cone == PROFILES_BUILTIN_CONE_UNRATED);
        if (expected) {
            found_unrated++;
        }
        TEST_CHECK(expected == actual,
                   "entry's sentinel state matches the owner's named list (code follows in next check)");
    }
    TEST_CHECK(found_unrated == UNRATED_CODE_COUNT,
               "all 10 named codes were actually present in the table and matched");
}

// SaftyFW's S8 sanity-rate guard ships with a compiled default of
// 33.3 C/min = 2x the fastest RISING ramp_c_per_hr across every built-in
// profile (999.0 C/hr, two tied steps in FSCGB1 -- see
// firmware/SaftyFW/src/config_store.h's CONFIG_STORE_DEFAULT_MAX_RATE_C_PER_MIN
// comment). That "no shipped profile's rising ramp can trip S8" claim is
// only true if every RISING segment actually declares a positive, bounded
// rate -- the table header's convention is that ramp_c_per_hr <= 0 means
// "unlimited rate" (the executor jumps the setpoint immediately), which
// would trip S8 instantly regardless of window sizing. This walks every
// built-in profile and asserts that invariant directly, rather than relying
// on eyeballing the table.
static void test_every_rising_segment_has_bounded_positive_ramp(void)
{
    TEST_SECTION("g_builtin_profiles -- every RISING segment (target above the previous target, or "
                 "above 0 for a profile's first segment) has 0 < ramp_c_per_hr <= 999.0 C/hr, so none "
                 "can produce SaftyFW S8's modeled \"unlimited rate\" (ramp_c_per_hr <= 0) behavior "
                 "and none exceeds the fastest rate the 33.3 C/min default was sized against");
    int checked = 0;
    for (size_t i = 0; i < g_builtin_profile_count; i++) {
        const builtin_profile_t *b = &g_builtin_profiles[i];
        float prev_target = 0.0f;
        for (uint8_t s = 0; s < b->segment_count; s++) {
            const profile_segment_t *seg = &b->segments[s];
            /* target_c/ramp_c_per_hr are only meaningful for
             * PROFILE_SEG_KIND_ZONE_RAMP (profiles_http.h) -- a RELAY_IO
             * segment's target_c/ramp_c_per_hr fields are unrelated unions
             * of io_* state, not a temperature ramp, so they must not be
             * fed into this invariant. */
            if (seg->seg_kind != PROFILE_SEG_KIND_ZONE_RAMP) {
                continue;
            }
            bool rising = seg->target_c > prev_target;
            if (rising) {
                checked++;
                TEST_CHECK(seg->ramp_c_per_hr > 0.0f,
                           "rising segment declares a positive (bounded) ramp, not \"unlimited\"");
                /* 999.0 C/hr mirrors SaftyFW's
                 * CONFIG_STORE_DEFAULT_MAX_RATE_C_PER_MIN (config_store.h,
                 * 33.3 C/min = 2x this) -- the other side of the pair this
                 * test protects. */
                TEST_CHECK(seg->ramp_c_per_hr <= 999.0f,
                           "rising segment's ramp does not exceed the fastest rate the S8 default "
                           "was sized against (999.0 C/hr, FSCGB1)");
            }
            prev_target = seg->target_c;
        }
    }
    /* Coverage floor: 94 is today's count of rising ZONE_RAMP segments
     * across the built-in catalogue (reviewer re-derived). Guards against
     * the loop silently checking near-zero segments if seg_kind filtering
     * or the catalogue itself regresses. */
    TEST_CHECK(checked >= 94, "checked at least 94 rising ZONE_RAMP segments across the catalogue");
}

// ---------------------------------------------------------------------
// cfg_fs dual-write of the hidden-builtin mask (docs/FILESYSTEM_USER_DATA_PLAN.md
// item 6, /cfg/profiles/hidden.json via pref_cfg_fs). Same shape as
// test_unit_pref.c's dual-write cases.
// ---------------------------------------------------------------------
static const char *PB_SCRATCH_BASE = "cfg_fs_test_profiles_builtin";

static void pb_cfg_fs_reset(void)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/.tmp/profiles_hidden.json", PB_SCRATCH_BASE);
    remove(path);
    snprintf(path, sizeof(path), "%s/%s", PB_SCRATCH_BASE, PROFILES_HIDDEN_FILE_PATH);
    remove(path);
    snprintf(path, sizeof(path), "%s/profiles", PB_SCRATCH_BASE);
    PB_RMDIR(path);
    snprintf(path, sizeof(path), "%s/.tmp", PB_SCRATCH_BASE);
    PB_RMDIR(path);
    PB_RMDIR(PB_SCRATCH_BASE);
    PB_MKDIR(PB_SCRATCH_BASE);
    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
}

static void pb_fresh(void)
{
    pb_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(PROFILES_NVS_PARTITION);
}

static void pb_reboot(void)
{
    s_hidden_mask = 0xDEADBEEFu; /* poison: start() must overwrite it */
    s_hidden_rev = 0xFFFFu;
}

static bool pb_nvs_mask(uint32_t *mask, uint32_t *rev)
{
    hal_kv_handle_t h;
    if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, PROFILES_NVS_PARTITION) != HAL_OK) {
        return false;
    }
    bool ok = hal_kv_get_u32(&h, NVS_KEY_HIDDEN, mask) == HAL_OK;
    *rev = 0;
    hal_kv_get_u32(&h, NVS_KEY_HIDDEN_REV, rev);
    hal_kv_close(&h);
    return ok;
}

static void pb_set_nvs(uint32_t mask, uint32_t rev)
{
    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, PROFILES_NVS_PARTITION);
    hal_kv_set_u32(&h, NVS_KEY_HIDDEN, mask);
    hal_kv_set_u32(&h, NVS_KEY_HIDDEN_REV, rev);
    hal_kv_commit(&h);
    hal_kv_close(&h);
}

static void test_hidden_save_writes_file_and_nvs(void)
{
    TEST_SECTION("hidden mask: set_hidden() with cfg_fs mounted writes BOTH the file and NVS at the same rev");
    pb_fresh();
    TEST_CHECK(cfg_fs_init(PB_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts against the scratch dir");
    pb_reboot();
    profiles_builtin_start();
    TEST_CHECK(profiles_builtin_set_hidden(PROFILE_BUILTIN_ID_BASE + 3, true) == ESP_OK, "set_hidden succeeds");

    uint32_t f_mask = 0, f_rev = 0;
    bool f_valid = false;
    pref_cfg_fs_load_raw(PROFILES_HIDDEN_FILE_PATH, sizeof(f_mask), hidden_mask_validate, &f_mask, &f_rev, &f_valid);
    TEST_CHECK(f_valid && f_mask == (1u << 3), "the file decodes to the new mask");
    uint32_t n_mask = 0, n_rev = 0;
    TEST_CHECK(pb_nvs_mask(&n_mask, &n_rev) && n_mask == (1u << 3), "NVS also holds the new mask");
    TEST_CHECK(f_rev == 1 && n_rev == 1 && s_hidden_rev == 1, "file rev, NVS rev and in-RAM rev all agree at 1");
    cfg_fs_deinit();
}

static void test_hidden_boot_resolves_from_file_when_nvs_empty(void)
{
    TEST_SECTION("hidden mask: NVS empty + file present -> start() adopts the file");
    pb_fresh();
    TEST_CHECK(cfg_fs_init(PB_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    uint32_t mask = (1u << 1) | (1u << 5);
    TEST_CHECK(pref_cfg_fs_save(PROFILES_HIDDEN_FILE_PATH, &mask, sizeof(mask), 7) == ESP_OK, "file seeded");
    pb_reboot();
    TEST_CHECK(profiles_builtin_start() == ESP_OK, "start() succeeds");
    TEST_CHECK(s_hidden_mask == mask, "mask came from the file");
    TEST_CHECK(s_hidden_rev == 7, "rev came from the file");
    TEST_CHECK(profiles_builtin_is_hidden(PROFILE_BUILTIN_ID_BASE + 5), "entry 5 reads hidden through the public API");
    cfg_fs_deinit();
}

static void test_hidden_higher_rev_wins_over_file(void)
{
    TEST_SECTION("hidden mask: both valid and different -> higher rev wins, loser resynced; NVS wins at higher rev");
    pb_fresh();
    TEST_CHECK(cfg_fs_init(PB_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    pb_reboot();
    profiles_builtin_start();
    profiles_builtin_set_hidden(PROFILE_BUILTIN_ID_BASE + 2, true); /* file+NVS rev 1, mask bit 2 */
    pb_set_nvs(1u << 9, 5);                                         /* NVS moves ahead */

    pb_reboot();
    TEST_CHECK(profiles_builtin_start() == ESP_OK, "start() succeeds across diverged sides");
    TEST_CHECK(s_hidden_mask == (1u << 9) && s_hidden_rev == 5, "NVS (rev 5) beat the file (rev 1)");
    uint32_t f_mask = 0, f_rev = 0;
    bool f_valid = false;
    pref_cfg_fs_load_raw(PROFILES_HIDDEN_FILE_PATH, sizeof(f_mask), hidden_mask_validate, &f_mask, &f_rev, &f_valid);
    TEST_CHECK(f_valid && f_mask == (1u << 9) && f_rev == 5, "the file was resynced from NVS");

    /* Reverse: file ahead of NVS. */
    uint32_t newer = 1u << 11;
    pref_cfg_fs_save(PROFILES_HIDDEN_FILE_PATH, &newer, sizeof(newer), 9);
    pb_reboot();
    profiles_builtin_start();
    TEST_CHECK(s_hidden_mask == newer && s_hidden_rev == 9, "file (rev 9) beat NVS (rev 5)");
    cfg_fs_deinit();
}

static void test_hidden_unmounted_leaves_nvs_path_working(void)
{
    TEST_SECTION("hidden mask: cfg_fs unmounted -> NVS-only save/boot round trip is unchanged");
    pb_fresh();
    TEST_CHECK(!cfg_fs_is_available(), "precondition: cfg_fs not mounted");
    pb_reboot();
    profiles_builtin_start();
    TEST_CHECK(profiles_builtin_set_hidden(PROFILE_BUILTIN_ID_BASE + 4, true) == ESP_OK,
               "set_hidden succeeds with no filesystem");
    pb_reboot();
    TEST_CHECK(profiles_builtin_start() == ESP_OK, "start() succeeds");
    TEST_CHECK(s_hidden_mask == (1u << 4), "mask survived the simulated reboot via NVS alone");
    TEST_CHECK(profiles_builtin_restore_all() == ESP_OK, "restore_all succeeds");
    pb_reboot();
    profiles_builtin_start();
    TEST_CHECK(s_hidden_mask == 0, "restore_all persisted via NVS alone");

    TEST_CHECK(cfg_fs_init("this_directory_does_not_exist_at_all", NULL) != ESP_OK, "a failed mount is reported");
    pb_reboot();
    profiles_builtin_start();
    TEST_CHECK(s_hidden_mask == 0, "mount-failed boot still resolves from NVS");
    cfg_fs_deinit();
}

static void test_hidden_nvs_migrates_to_file_and_status(void)
{
    TEST_SECTION("hidden mask: NVS-only value migrates to the file when cfg_fs mounts later; status reports it");
    pb_fresh();
    pb_reboot();
    profiles_builtin_start();
    profiles_builtin_set_hidden(PROFILE_BUILTIN_ID_BASE + 6, true); /* NVS only */
    TEST_CHECK(cfg_fs_init(PB_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts on a later boot");
    pb_reboot();
    profiles_builtin_start();
    TEST_CHECK(s_hidden_mask == (1u << 6), "value came from NVS");
    bool exists = false;
    cfg_fs_exists(PROFILES_HIDDEN_FILE_PATH, &exists);
    TEST_CHECK(exists, "the NVS candidate was migrated out to the file");

    bool fv = false, nv = false, dv = true;
    uint32_t fr = 0, nr = 0;
    profiles_builtin_get_dualwrite_status(&fv, &fr, &nv, &nr, &dv);
    TEST_CHECK(fv && nv && !dv, "status: both sides valid, not diverged");
    pb_set_nvs(1u << 2, 4);
    profiles_builtin_get_dualwrite_status(&fv, &fr, &nv, &nr, &dv);
    TEST_CHECK(dv && nr == 4, "status: differing content reports diverged");
    cfg_fs_deinit();
}

void run_test_profiles_builtin(void)
{
    test_cone_label_unrated();
    test_cone_label_real_cone_unaffected();
    test_exactly_the_ten_named_entries_are_unrated();
    test_every_rising_segment_has_bounded_positive_ramp();
    test_hidden_save_writes_file_and_nvs();
    test_hidden_boot_resolves_from_file_when_nvs_empty();
    test_hidden_higher_rev_wins_over_file();
    test_hidden_unmounted_leaves_nvs_path_working();
    test_hidden_nvs_migrates_to_file_and_status();
}

int main(void)
{
    run_test_profiles_builtin();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
