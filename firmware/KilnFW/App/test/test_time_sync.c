// Host tests for time_sync_tz.c (the pure, dependency-free TZ
// validation/degrade logic) AND, as of docs/FILESYSTEM_USER_DATA_PLAN.md
// item 14's close-out, time_sync.c itself: an esp_netif_sntp.h host stub
// (test/stubs/esp_netif_sntp.h) now exists, closing the gap time_sync_tz.h's
// header comment used to document ("no host stub written for it in this
// pass"). #includes time_sync.c directly, same convention as
// test_unit_pref.c/test_ramp_assist_cfg.c/test_display_power_cfg.c, to reach
// its file-scope statics (s_tz_rev, TIME_SYNC_TZ_FILE_PATH, tz_file_validate)
// for the dual-write tests below.
//
// OWN, SEPARATE executable (build_host_tests.ps1's own build step), not part
// of the "main" host-test executable this file used to live in: time_sync.c
// defines the REAL time_sync_notify_got_ip(), which collides at link time
// with test_wifi_prov.c's own fake body of the same name (that file needs a
// fake since wifi_prov.c calls it and pulling in the real time_sync.c there
// would drag in esp_netif_sntp.h's whole surface for a test that isn't
// about time sync at all). Two different, non-overlapping fakes/reals of
// the same external symbol cannot share one link, so this module gets its
// own executable instead, same reasoning as test_zone_coupling_solve.c's
// exe20 (its own fake zones_config_get_coupling() would collide with exe4's/
// exe17's).
#include "test_common.h"
#include "../drivers/net/time_sync_tz.h"

#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#ifdef _WIN32
#include <direct.h>
#define TTS_MKDIR(p) _mkdir(p)
#define TTS_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TTS_MKDIR(p) mkdir((p), 0755)
#define TTS_RMDIR(p) rmdir(p)
#endif

#include "esp_err.h"
#include "fake_kv.h"

#include "cfg_fs.h"

#include "../drivers/net/time_sync.c"

static const char *TS_SCRATCH_BASE = "cfg_fs_test_time_sync";

static void ts_cfg_fs_reset(void)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/.tmp/%s", TS_SCRATCH_BASE, TIME_SYNC_TZ_FILE_PATH);
    remove(path);
    snprintf(path, sizeof(path), "%s/%s", TS_SCRATCH_BASE, TIME_SYNC_TZ_FILE_PATH);
    remove(path);
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s/.tmp", TS_SCRATCH_BASE);
    TTS_RMDIR(tmp);
    TTS_RMDIR(TS_SCRATCH_BASE);
    TTS_MKDIR(TS_SCRATCH_BASE);
    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
}

// ---------------------------------------------------------------------
// time_sync.c: NVS load/dual-write/tie-break coverage
// (docs/FILESYSTEM_USER_DATA_PLAN.md item 14 close-out).
// ---------------------------------------------------------------------

static void test_ts_default_is_utc_on_empty_nvs(void)
{
    TEST_SECTION("time_sync: empty NVS -- defaults to UTC0");
    ts_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);

    TEST_CHECK(time_sync_start() == ESP_OK, "time_sync_start() succeeds against an empty stub");
    time_sync_status_t st;
    time_sync_get_status(&st);
    TEST_CHECK(strcmp(st.tz, TIME_SYNC_TZ_DEFAULT) == 0, "empty NVS: TZ defaults to UTC0");
}

static void test_ts_partition_absent_nvs_only_round_trip(void)
{
    TEST_SECTION("time_sync: partition absent (no cfg mount) -- NVS-only set/load round trip");
    ts_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    time_sync_start();

    TEST_CHECK(time_sync_set_tz("PST8PDT,M3.2.0,M11.1.0") == ESP_OK, "set_tz succeeds with no cfg_fs mounted");
    time_sync_status_t st;
    time_sync_get_status(&st);
    TEST_CHECK(strcmp(st.tz, "PST8PDT,M3.2.0,M11.1.0") == 0, "live TZ updates immediately");

    time_sync_start(); // simulated reboot -- resets all in-RAM state, NVS (fake_kv) untouched
    time_sync_get_status(&st);
    TEST_CHECK(strcmp(st.tz, "PST8PDT,M3.2.0,M11.1.0") == 0, "the value survives a simulated reboot via NVS alone");
}

static void test_ts_set_refuses_invalid_tz(void)
{
    TEST_SECTION("time_sync: set_tz refuses an invalid TZ string, in-RAM value untouched");
    ts_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    time_sync_start();
    time_sync_set_tz("EST5EDT");

    esp_err_t err = time_sync_set_tz("America/Chicago"); // IANA name, not POSIX -- Finding 2's case
    TEST_CHECK(err == ESP_ERR_INVALID_ARG, "an IANA zone name is refused");
    time_sync_status_t st;
    time_sync_get_status(&st);
    TEST_CHECK(strcmp(st.tz, "EST5EDT") == 0, "refused set: live TZ unchanged");
}

static void test_ts_dual_write_lands_on_both_file_and_nvs(void)
{
    TEST_SECTION("time_sync: cfg_fs mounted -- set_tz dual-writes file and NVS");
    ts_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(TS_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts against the scratch dir");
    time_sync_start();

    TEST_CHECK(time_sync_set_tz("EST5EDT,M3.2.0,M11.1.0") == ESP_OK, "set_tz succeeds with cfg_fs mounted");

    uint8_t file_item[TZ_ITEM_SIZE];
    uint32_t file_rev = 0;
    bool file_valid = false;
    pref_cfg_fs_load_raw(TIME_SYNC_TZ_FILE_PATH, TZ_ITEM_SIZE, tz_file_validate, file_item, &file_rev, &file_valid);
    TEST_CHECK(file_valid && strcmp((char *)file_item, "EST5EDT,M3.2.0,M11.1.0") == 0,
               "the file decodes to the just-set TZ string");

    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    char nvs_tz[TIME_SYNC_TZ_MAX_LEN + 1] = { 0 };
    size_t len = sizeof(nvs_tz);
    hal_kv_get_str(&h, NVS_KEY_TZ, nvs_tz, &len);
    hal_kv_close(&h);
    TEST_CHECK(strcmp(nvs_tz, "EST5EDT,M3.2.0,M11.1.0") == 0, "NVS also holds the same TZ -- both sides written");
    TEST_CHECK(file_rev == s_tz_rev, "file rev matches the in-RAM rev this save just bumped to");

    cfg_fs_deinit();
}

static void test_ts_nvs_fallback_then_migrates(void)
{
    TEST_SECTION("time_sync: NVS fallback when file absent, then opportunistic migration");
    ts_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    time_sync_start();
    time_sync_set_tz("PST8PDT,M3.2.0,M11.1.0"); // NVS-only, cfg_fs not mounted yet

    TEST_CHECK(cfg_fs_init(TS_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts on a later boot");
    esp_err_t err = time_sync_start();
    TEST_CHECK(err == ESP_OK, "start() succeeds");
    time_sync_status_t st;
    time_sync_get_status(&st);
    TEST_CHECK(strcmp(st.tz, "PST8PDT,M3.2.0,M11.1.0") == 0, "NVS fallback: TZ came from NVS, no file existed yet");

    bool exists = false;
    cfg_fs_exists(TIME_SYNC_TZ_FILE_PATH, &exists);
    TEST_CHECK(exists, "the NVS candidate was opportunistically migrated out to the file on this load");

    cfg_fs_deinit();
}

static void test_ts_divergence_tie_break_higher_rev_wins(void)
{
    TEST_SECTION("time_sync: divergence tie-break -- higher rev wins, both directions");
    ts_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(TS_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    time_sync_start();
    time_sync_set_tz("EST5EDT"); // file+NVS both rev 1

    // NVS advances further (rev 5) with different content -- simulates a
    // later write whose file-side companion failed.
    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    hal_kv_set_str(&h, NVS_KEY_TZ, "PST8PDT,M3.2.0,M11.1.0");
    hal_kv_set_u32(&h, NVS_KEY_TZ_REV, 5);
    hal_kv_commit(&h);
    hal_kv_close(&h);

    esp_err_t err = time_sync_start();
    TEST_CHECK(err == ESP_OK, "start() succeeds across the diverged sides");
    time_sync_status_t st;
    time_sync_get_status(&st);
    TEST_CHECK(strcmp(st.tz, "PST8PDT,M3.2.0,M11.1.0") == 0,
               "higher rev (NVS, rev 5) wins over the lower-rev file (rev 1)");

    uint8_t file_item[TZ_ITEM_SIZE];
    uint32_t file_rev = 0;
    bool file_valid = false;
    pref_cfg_fs_load_raw(TIME_SYNC_TZ_FILE_PATH, TZ_ITEM_SIZE, tz_file_validate, file_item, &file_rev, &file_valid);
    TEST_CHECK(file_valid && strcmp((char *)file_item, "PST8PDT,M3.2.0,M11.1.0") == 0 && file_rev == 5,
               "the file was resynced from the winning NVS side");

    cfg_fs_deinit();
}

static void test_ts_mount_failed_falls_through_to_nvs_only(void)
{
    TEST_SECTION("time_sync: cfg_fs mount FAILED -- falls through to NVS, non-fatal");
    ts_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    time_sync_start();
    time_sync_set_tz("EST5EDT,M3.2.0,M11.1.0");

    esp_err_t mount_err = cfg_fs_init("this_directory_does_not_exist_at_all", NULL);
    TEST_CHECK(mount_err != ESP_OK, "cfg_fs_init() against a nonexistent base dir fails, as documented");
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs reports unavailable after a failed mount");

    esp_err_t err = time_sync_start();
    TEST_CHECK(err == ESP_OK, "start() with a FAILED mount still succeeds (non-fatal, falls back to NVS)");
    time_sync_status_t st;
    time_sync_get_status(&st);
    TEST_CHECK(strcmp(st.tz, "EST5EDT,M3.2.0,M11.1.0") == 0, "mount-failed: the NVS value is still adopted correctly");

    cfg_fs_deinit();
}

// ---------------------------------------------------------------------
// Startup-fault return coverage (a548dfc6): time_sync_start() now returns the
// esp_netif_sntp_init() error instead of ESP_OK so main_boot_early.c can latch
// a startup fault. The stored-TZ load runs BEFORE the sntp init, so a failure
// there must still leave the persisted timezone applied.
// ---------------------------------------------------------------------

static void test_ts_sntp_init_failure_returns_error_and_tz_still_applied(void)
{
    TEST_SECTION("time_sync_start: esp_netif_sntp_init failure returns the error; stored TZ still applied");
    ts_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    time_sync_start();
    TEST_CHECK(time_sync_set_tz("EST5EDT,M3.2.0,M11.1.0") == ESP_OK, "precondition: a TZ is persisted");

    *esp_netif_sntp_stub_init_result() = ESP_FAIL;
    esp_err_t err = time_sync_start(); // simulated reboot against a failing sntp init
    *esp_netif_sntp_stub_init_result() = ESP_OK;

    TEST_CHECK(err == ESP_FAIL, "sntp init failure is returned verbatim (not swallowed as ESP_OK)");
    time_sync_status_t st;
    time_sync_get_status(&st);
    TEST_CHECK(strcmp(st.tz, "EST5EDT,M3.2.0,M11.1.0") == 0,
               "sntp init failure: the stored timezone is still applied");
    TEST_CHECK(!s_sntp_init_ok, "sntp init failure: the module does not claim SNTP is initialised");

    TEST_CHECK(time_sync_start() == ESP_OK, "recovery: with sntp init healthy again start() returns ESP_OK");
    TEST_CHECK(s_sntp_init_ok, "recovery: SNTP is then marked initialised");
}

static void test_ts_sntp_init_failure_on_empty_nvs_defaults_to_utc(void)
{
    TEST_SECTION("time_sync_start: sntp init failure on empty NVS -- non-OK, UTC default applied");
    ts_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);

    *esp_netif_sntp_stub_init_result() = ESP_ERR_NO_MEM;
    esp_err_t err = time_sync_start();
    *esp_netif_sntp_stub_init_result() = ESP_OK;

    TEST_CHECK(err == ESP_ERR_NO_MEM, "the exact sntp error code is propagated");
    time_sync_status_t st;
    time_sync_get_status(&st);
    TEST_CHECK(strcmp(st.tz, TIME_SYNC_TZ_DEFAULT) == 0, "UTC default is applied despite the sntp failure");
}

static void run_test_time_sync(void)
{
    TEST_SECTION("time_sync (TZ validation + degrade-to-UTC)");

    // ---- time_sync_tz_is_valid(): valid strings accepted -------------------
    TEST_CHECK(time_sync_tz_is_valid("UTC0"), "plain UTC accepted");
    TEST_CHECK(time_sync_tz_is_valid("EST5EDT,M3.2.0,M11.1.0"), "US Eastern POSIX rule accepted");
    TEST_CHECK(time_sync_tz_is_valid("ABC1"), "shortest valid form (3-letter name + single-digit offset) accepted");
    {
        // Exactly TIME_SYNC_TZ_MAX_LEN bytes -- the boundary itself must be
        // accepted, not just comfortably-short strings, or the "over-long
        // refused" test below would prove nothing about where the real
        // limit is. Must still be a grammatically valid TZ rule now that
        // time_sync_tz_is_valid() checks grammar, not just charset/length --
        // an unquoted std name has no upper length limit of its own, so
        // TIME_SYNC_TZ_MAX_LEN-1 letters plus a 1-digit offset lands exactly
        // on the boundary.
        char exact[TIME_SYNC_TZ_MAX_LEN + 1];
        memset(exact, 'A', TIME_SYNC_TZ_MAX_LEN - 1);
        exact[TIME_SYNC_TZ_MAX_LEN - 1] = '0';
        exact[TIME_SYNC_TZ_MAX_LEN] = '\0';
        TEST_CHECK(time_sync_tz_is_valid(exact), "exactly TIME_SYNC_TZ_MAX_LEN bytes, valid grammar, accepted");
    }

    // ---- refused: over-long -------------------------------------------------
    {
        // Same shape as the boundary-accepted case above (valid name +
        // offset grammar) but one byte over the length cap -- proves this
        // is refused for LENGTH, not because it happens to also be bad
        // grammar (an all-'A' run with no offset would be refused for
        // grammar first, masking the length check).
        char too_long[TIME_SYNC_TZ_MAX_LEN + 2];
        memset(too_long, 'A', TIME_SYNC_TZ_MAX_LEN);
        too_long[TIME_SYNC_TZ_MAX_LEN] = '0';
        too_long[TIME_SYNC_TZ_MAX_LEN + 1] = '\0';
        TEST_CHECK(!time_sync_tz_is_valid(too_long), "one byte over TIME_SYNC_TZ_MAX_LEN refused");
    }

    // ---- refused: non-printable ---------------------------------------------
    // Each of these is otherwise a plainly valid-length string -- the ONLY
    // thing wrong is the one non-printable byte, so a pass here proves the
    // printable-ASCII check itself fired, not that the string was refused
    // for length or emptiness instead (the task's "make sure the input
    // reaches the logic it names" warning).
    TEST_CHECK(!time_sync_tz_is_valid("UTC\ttab"), "embedded tab refused");
    TEST_CHECK(!time_sync_tz_is_valid("UTC\nline"), "embedded newline refused");
    TEST_CHECK(!time_sync_tz_is_valid("UTC\x01"), "embedded control byte 0x01 refused");
    TEST_CHECK(!time_sync_tz_is_valid("UTC\x7F"), "embedded DEL (0x7F) refused");
    TEST_CHECK(!time_sync_tz_is_valid("UTC\xC3\xA9"), "embedded non-ASCII (UTF-8 e-acute) byte refused");

    // ---- refused: empty / NULL ------------------------------------------
    TEST_CHECK(!time_sync_tz_is_valid(""), "empty string refused");
    TEST_CHECK(!time_sync_tz_is_valid(NULL), "NULL refused");

    // ---- time_sync_tz_effective(): never-set / default state ---------------
    {
        char out[TIME_SYNC_TZ_MAX_LEN + 1];
        time_sync_tz_effective(NULL, out, sizeof(out));
        TEST_CHECK(strcmp(out, TIME_SYNC_TZ_DEFAULT) == 0, "NULL stored -> default UTC string");
        TEST_CHECK(time_sync_tz_is_valid(out), "default UTC string is itself valid");
    }

    // ---- time_sync_tz_effective(): degrade-to-UTC on a corrupt stored TZ ---
    {
        char out[TIME_SYNC_TZ_MAX_LEN + 1];
        // A stored value that fails validation for the SAME reason a
        // corrupted NVS byte would (embedded control byte) -- must degrade
        // to the default, never pass the garbage through.
        time_sync_tz_effective("UTC\x01garbage", out, sizeof(out));
        TEST_CHECK(strcmp(out, TIME_SYNC_TZ_DEFAULT) == 0, "corrupt stored TZ degrades to default UTC");
    }
    {
        char out[TIME_SYNC_TZ_MAX_LEN + 1];
        // Over-long stored value (e.g. a wider future format read by this
        // older build) also degrades rather than being silently truncated.
        char too_long[TIME_SYNC_TZ_MAX_LEN + 2];
        memset(too_long, 'B', TIME_SYNC_TZ_MAX_LEN + 1);
        too_long[TIME_SYNC_TZ_MAX_LEN + 1] = '\0';
        time_sync_tz_effective(too_long, out, sizeof(out));
        TEST_CHECK(strcmp(out, TIME_SYNC_TZ_DEFAULT) == 0, "over-long stored TZ degrades to default UTC");
    }

    // ---- time_sync_tz_effective(): valid stored value passes through -------
    {
        char out[TIME_SYNC_TZ_MAX_LEN + 1];
        time_sync_tz_effective("PST8PDT,M3.2.0,M11.1.0", out, sizeof(out));
        TEST_CHECK(strcmp(out, "PST8PDT,M3.2.0,M11.1.0") == 0, "valid stored TZ passed through unchanged");
    }

    // ---- time_sync_tz_effective(): valid-but-doesn't-fit-out_len degrades --
    // Exercises the `strlen(stored) < out_len` branch specifically: "EST5EDT"
    // is otherwise perfectly valid (passes time_sync_tz_is_valid() on its
    // own, checked below), so a failure here can only be the fits-in-out_len
    // check, not length/grammar rejection upstream of it -- the two other
    // degrade tests above already use inputs time_sync_tz_is_valid() itself
    // refuses, which would mask this branch rather than reach it.
    {
        TEST_CHECK(time_sync_tz_is_valid("EST5EDT"), "EST5EDT is independently valid (sanity check for the next case)");
        char out[5]; // room for 4 chars + NUL -- "EST5EDT" (7 chars) cannot fit
        time_sync_tz_effective("EST5EDT", out, sizeof(out));
        TEST_CHECK(strcmp(out, TIME_SYNC_TZ_DEFAULT) == 0,
                   "valid stored TZ that doesn't fit out_len degrades to default UTC");
    }

    // ---- POSIX TZ grammar: valid rule strings accepted ---------------------
    TEST_CHECK(time_sync_tz_is_valid("UTC0"), "std+offset only");
    TEST_CHECK(time_sync_tz_is_valid("PST8"), "std+offset, no sign on offset");
    TEST_CHECK(time_sync_tz_is_valid("EST5EDT"), "std+offset+dst, no dst offset, no rule");
    TEST_CHECK(time_sync_tz_is_valid("EST5EDT4"), "std+offset+dst+offset, no rule");
    TEST_CHECK(time_sync_tz_is_valid("EST5EDT,M3.2.0,M11.1.0"), "full US Eastern rule with M-form dates");
    TEST_CHECK(time_sync_tz_is_valid("EST5EDT,M3.2.0/2,M11.1.0/2"), "M-form dates with explicit /time");
    TEST_CHECK(time_sync_tz_is_valid("AEST-10AEDT,M10.1.0,M4.1.0/3"), "leading '-' offset (east of UTC)");
    TEST_CHECK(time_sync_tz_is_valid("<+07>-7"), "quoted <...> std name with numeric/sign body");
    TEST_CHECK(time_sync_tz_is_valid("NZST-12:00:00NZDT,M9.5.0,M4.1.0/3"), "hh:mm:ss offset precision");
    TEST_CHECK(time_sync_tz_is_valid("EST5EDT,J1,J365"), "Julian (no-leap-day) date rule");
    TEST_CHECK(time_sync_tz_is_valid("EST5EDT,0,364"), "plain Julian (with-leap-day) date rule");

    // ---- POSIX TZ grammar: refused ------------------------------------------
    // Each input below is short, printable ASCII and would pass the OLD
    // charset/length-only check -- the only thing that can refuse it now is
    // the grammar parser itself, proving Finding 2's actual failure mode
    // (an IANA name reaching setenv()/persist and silently staying UTC) is
    // closed.
    TEST_CHECK(!time_sync_tz_is_valid("America/Chicago"), "IANA zone name refused (the Finding 2 case)");
    TEST_CHECK(!time_sync_tz_is_valid("UTC"), "std name with no offset refused");
    TEST_CHECK(!time_sync_tz_is_valid("ES5EDT"), "std name under 3 letters refused");
    TEST_CHECK(!time_sync_tz_is_valid("EST5EDT,M13.1.0,M11.1.0"), "month 13 out of range refused");
    TEST_CHECK(!time_sync_tz_is_valid("EST5EDT,M3.6.0,M11.1.0"), "week 6 out of range refused");
    TEST_CHECK(!time_sync_tz_is_valid("EST5EDT,M3.2.7,M11.1.0"), "weekday 7 out of range refused");
    TEST_CHECK(!time_sync_tz_is_valid("EST5EDT,M3.2.0"), "only one rule (dst start with no end) refused");
    TEST_CHECK(!time_sync_tz_is_valid("EST25EDT"), "offset hour 25 out of range refused");
    TEST_CHECK(!time_sync_tz_is_valid("EST5EDT trailing junk"), "valid prefix with trailing garbage refused");
    TEST_CHECK(!time_sync_tz_is_valid("<+07-7"), "unterminated <...> quoted name refused");

    // ---- time_sync.c: NVS load / cfg_fs dual-write coverage -----------------
    test_ts_default_is_utc_on_empty_nvs();
    test_ts_partition_absent_nvs_only_round_trip();
    test_ts_set_refuses_invalid_tz();
    test_ts_dual_write_lands_on_both_file_and_nvs();
    test_ts_nvs_fallback_then_migrates();
    test_ts_divergence_tie_break_higher_rev_wins();
    test_ts_mount_failed_falls_through_to_nvs_only();
    test_ts_sntp_init_failure_returns_error_and_tz_still_applied();
    test_ts_sntp_init_failure_on_empty_nvs_defaults_to_utc();

    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
    fake_kv_reset_all(); // leave shared fake state as every other test file in this binary expects
}

int main(void)
{
    run_test_time_sync();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
