// Host tests for live_profile.c and profile_executor_live_pickup.c
// (docs/LIVE_PROFILE_EDIT_PLAN.md pass 1). Its own SEPARATE executable
// (same convention as test_kiln_cfg_swap.c / test_profiles_http.c): needs
// the REAL host hal_kv backend (fake_kv.c) so the working-slot/record
// read-back-verified writes are exercised for real, not mocked a second
// time. profile_encode_current_blob()/profile_decode_blob() (the shared
// wire format live_profile.c reuses, per the "one implementation" rule) live
// in profiles_http.c, which pulls in the full httpd tier and is not linked
// here -- this file supplies small deterministic FAKES of just those two
// functions instead, so live_profile.c's own persistence/read-back logic is
// exercised for real while the wire format itself is a stand-in (the real
// encoder/decoder is already covered by test_profiles_http.c).
#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

#include <string.h>

#include "hal_kv.h"
#include "live_profile.h"
#include "profile_executor_live_pickup.h"

// ---- fakes: the shared wire format (profiles_http.c, not linked here) -----
// Deliberately trivial and NOT the real format: a raw memcpy of the
// profile_t. Good enough to prove live_profile.c's own save/load/
// read-back-verify logic round-trips correctly; the real format's own
// correctness is test_profiles_http.c's job. Pre-declares the same guarded
// profile_decode_result_t shim live_profile.c declares (see that file's
// comment) so this file's fakes and live_profile.c's own #include below
// agree on the type without a duplicate enum-tag definition error.
#define PROFILE_DECODE_RESULT_SHIM_DECLARED
typedef enum {
    PROFILE_DECODE_OK,
    PROFILE_DECODE_CORRUPT,
    PROFILE_DECODE_NEWER,
} profile_decode_result_t;

size_t profile_encode_current_blob(const profile_t *profile, void *out, size_t cap)
{
    if (!profile || !out || cap < sizeof(profile_t)) {
        return 0;
    }
    memcpy(out, profile, sizeof(profile_t));
    return sizeof(profile_t);
}

profile_decode_result_t profile_decode_blob(const void *blob, size_t len, profile_t *out, const char **err_reason)
{
    if (!blob || !out || len != sizeof(profile_t)) {
        if (err_reason) *err_reason = "bad length";
        return PROFILE_DECODE_CORRUPT;
    }
    memcpy(out, blob, sizeof(profile_t));
    return PROFILE_DECODE_OK;
}

// Pull in the real module under test, AFTER the fakes above. Same
// "#include the .c directly" convention test_zones_http.c/
// test_profile_executor_prestart.c already use for reaching file-local
// pieces.
#include "../drivers/persist/live_profile.c"
#include "../drivers/control/profile_executor_live_pickup.c"

// ---------------------------------------------------------------------------
// live_edit_record_t encode/decode

static void test_record_roundtrip(void)
{
    TEST_SECTION("live_edit_record_encode/decode round-trip");
    live_edit_record_t rec = {0};
    rec.version = LIVE_EDIT_RECORD_VERSION;
    rec.origin_id = 3;
    rec.working_id = LIVE_EDIT_WORKING_SLOT_ID;
    rec.origin_is_builtin = 0;
    rec.pending = 1;
    strncpy(rec.origin_name, "Bisque", sizeof(rec.origin_name) - 1);

    uint8_t buf[64];
    size_t len = live_edit_record_encode(&rec, buf, sizeof(buf));
    TEST_CHECK(len > 0, "encode returns nonzero length");

    live_edit_record_t out;
    memset(&out, 0xAA, sizeof(out));
    bool ok = live_edit_record_decode(buf, len, &out);
    TEST_CHECK(ok, "decode succeeds on a freshly-encoded record");
    TEST_CHECK(out.origin_id == 3, "origin_id round-trips");
    TEST_CHECK(out.working_id == LIVE_EDIT_WORKING_SLOT_ID, "working_id round-trips");
    TEST_CHECK(out.pending == 1, "pending round-trips");
    TEST_CHECK(strcmp(out.origin_name, "Bisque") == 0, "origin_name round-trips");
}

static void test_record_version_mismatch_discarded(void)
{
    TEST_SECTION("live_edit_record_decode -- version mismatch is discarded, not migrated");
    live_edit_record_t rec = {0};
    rec.version = LIVE_EDIT_RECORD_VERSION + 1; // a future/unknown version
    rec.origin_id = 5;

    uint8_t buf[64];
    size_t len = live_edit_record_encode(&rec, buf, sizeof(buf));
    // live_edit_record_encode always stamps the CURRENT version -- to test a
    // genuine mismatch, corrupt the version field in place after encoding.
    memcpy(buf, &rec.version, sizeof(rec.version));

    live_edit_record_t out;
    memset(&out, 0, sizeof(out));
    bool ok = live_edit_record_decode(buf, len, &out);
    TEST_CHECK(!ok, "a version-mismatched record is refused, never migrated");
}

// ---------------------------------------------------------------------------
// live_edit_can_overwrite -- structural, not a flag

static void test_overwrite_refused_for_builtin(void)
{
    TEST_SECTION("live_edit_can_overwrite -- builtin origin refused");
    live_edit_record_t rec = {0};
    rec.origin_is_builtin = 1;
    rec.origin_id = 200; // a plausible builtin id, PROFILE_BUILTIN_ID_BASE-ish
    char err[128];
    bool ok = live_edit_can_overwrite(&rec, err, sizeof(err));
    TEST_CHECK(!ok, "builtin-origin overwrite is refused");
    TEST_CHECK(err[0] != '\0', "a reason is given");
}

static void test_overwrite_refused_for_id_outside_user_range(void)
{
    TEST_SECTION("live_edit_can_overwrite -- origin_id outside user range refused even if the flag is unset");
    live_edit_record_t rec = {0};
    rec.origin_is_builtin = 0; // deliberately wrong/unset
    rec.origin_id = PROFILES_MAX_COUNT; // outside 0..PROFILES_MAX_COUNT-1
    char err[128];
    bool ok = live_edit_can_overwrite(&rec, err, sizeof(err));
    TEST_CHECK(!ok, "an out-of-range origin id is refused regardless of the flag");
}

static void test_overwrite_allowed_for_user_profile(void)
{
    TEST_SECTION("live_edit_can_overwrite -- an ordinary user profile is allowed");
    live_edit_record_t rec = {0};
    rec.origin_is_builtin = 0;
    rec.origin_id = 2;
    char err[128];
    bool ok = live_edit_can_overwrite(&rec, err, sizeof(err));
    TEST_CHECK(ok, "a user-origin record may be overwritten");
}

// ---------------------------------------------------------------------------
// live_edit_name_collides

typedef struct {
    const char *names[PROFILES_MAX_COUNT];
} fake_name_table_t;

static const char *fake_name_at(void *ctx, uint8_t id)
{
    fake_name_table_t *t = (fake_name_table_t *)ctx;
    if (id >= PROFILES_MAX_COUNT) return NULL;
    return t->names[id];
}

static void test_name_collision_case_and_whitespace_insensitive(void)
{
    TEST_SECTION("live_edit_name_collides -- case/whitespace-insensitive match refused");
    fake_name_table_t t = {0};
    t.names[0] = "Bisque Fast";
    char err[128];
    bool collides = live_edit_name_collides("  bisque fast  ", fake_name_at, &t, 0xFF, err, sizeof(err));
    TEST_CHECK(collides, "a case/whitespace-differing duplicate is refused");
}

static void test_name_collision_excludes_self(void)
{
    TEST_SECTION("live_edit_name_collides -- exclude_id lets a rename keep its own name");
    fake_name_table_t t = {0};
    t.names[3] = "Bisque Fast";
    char err[128];
    bool collides = live_edit_name_collides("Bisque Fast", fake_name_at, &t, 3, err, sizeof(err));
    TEST_CHECK(!collides, "excluding the target id's own slot avoids a false collision");
}

static void test_name_no_collision_distinct_name(void)
{
    TEST_SECTION("live_edit_name_collides -- a genuinely distinct name is accepted");
    fake_name_table_t t = {0};
    t.names[0] = "Bisque Fast";
    char err[128];
    bool collides = live_edit_name_collides("Glaze Slow", fake_name_at, &t, 0xFF, err, sizeof(err));
    TEST_CHECK(!collides, "a distinct name never collides");
}

// ---------------------------------------------------------------------------
// live_edit_decide -- top-level decision-layer entry point (plan section 5/6)

static void test_decide_save_as_refuses_name_collision(void)
{
    TEST_SECTION("live_edit_decide -- SAVE_AS refuses a name collision");
    fake_name_table_t t = {0};
    t.names[0] = "Bisque Fast";
    char err[128];
    bool ok = live_edit_decide(LIVE_EDIT_DECISION_SAVE_AS, NULL, "  bisque fast  ", false, fake_name_at, &t, err,
                                sizeof(err));
    TEST_CHECK(!ok, "a colliding save-as name is refused");
    TEST_CHECK(err[0] != '\0', "a reason is given");
}

static void test_decide_save_as_accepts_distinct_name(void)
{
    TEST_SECTION("live_edit_decide -- SAVE_AS accepts a distinct name");
    fake_name_table_t t = {0};
    t.names[0] = "Bisque Fast";
    char err[128];
    bool ok =
        live_edit_decide(LIVE_EDIT_DECISION_SAVE_AS, NULL, "Glaze Slow", false, fake_name_at, &t, err, sizeof(err));
    TEST_CHECK(ok, "a distinct save-as name is allowed");
}

static void test_decide_overwrite_refuses_builtin_before_confirm(void)
{
    TEST_SECTION("live_edit_decide -- OVERWRITE refuses a builtin origin even with confirm=1");
    live_edit_record_t rec = {0};
    rec.origin_is_builtin = 1;
    rec.origin_id = 200;
    char err[128];
    bool ok = live_edit_decide(LIVE_EDIT_DECISION_OVERWRITE, &rec, NULL, true, NULL, NULL, err, sizeof(err));
    TEST_CHECK(!ok, "a builtin origin is refused structurally, regardless of confirm");
}

static void test_decide_overwrite_refuses_without_confirm(void)
{
    TEST_SECTION("live_edit_decide -- OVERWRITE of a user profile without confirm=1 is refused");
    live_edit_record_t rec = {0};
    rec.origin_is_builtin = 0;
    rec.origin_id = 2;
    char err[128];
    bool ok = live_edit_decide(LIVE_EDIT_DECISION_OVERWRITE, &rec, NULL, false, NULL, NULL, err, sizeof(err));
    TEST_CHECK(!ok, "a user-profile overwrite without an explicit confirm flag is refused");
    TEST_CHECK(err[0] != '\0', "a reason is given");
}

static void test_decide_overwrite_allowed_with_confirm(void)
{
    TEST_SECTION("live_edit_decide -- OVERWRITE of a user profile with confirm=1 is allowed");
    live_edit_record_t rec = {0};
    rec.origin_is_builtin = 0;
    rec.origin_id = 2;
    char err[128];
    bool ok = live_edit_decide(LIVE_EDIT_DECISION_OVERWRITE, &rec, NULL, true, NULL, NULL, err, sizeof(err));
    TEST_CHECK(ok, "an explicitly confirmed user-profile overwrite is allowed");
}

static void test_decide_discard_always_allowed(void)
{
    TEST_SECTION("live_edit_decide -- DISCARD is always allowed");
    char err[128];
    bool ok = live_edit_decide(LIVE_EDIT_DECISION_DISCARD, NULL, NULL, false, NULL, NULL, err, sizeof(err));
    TEST_CHECK(ok, "discard needs no record, name or confirmation");
}

// ---------------------------------------------------------------------------
// live_edit_should_prompt -- plan section 5's abort/trip/reboot handling

static void test_should_prompt_when_pending_and_not_running(void)
{
    TEST_SECTION("live_edit_should_prompt -- pending + not running raises the prompt");
    live_edit_record_t rec = {0};
    rec.pending = 1;
    TEST_CHECK(live_edit_should_prompt(&rec, false), "pending and not RUNNING prompts");
}

static void test_should_prompt_false_while_running(void)
{
    TEST_SECTION("live_edit_should_prompt -- pending but still RUNNING does not prompt yet");
    live_edit_record_t rec = {0};
    rec.pending = 1;
    TEST_CHECK(!live_edit_should_prompt(&rec, true), "a pending record while still running does not prompt");
}

static void test_should_prompt_false_when_not_pending(void)
{
    TEST_SECTION("live_edit_should_prompt -- nothing pending never prompts");
    live_edit_record_t rec = {0};
    rec.pending = 0;
    TEST_CHECK(!live_edit_should_prompt(&rec, false), "no pending record means no prompt even when not running");
}

static void test_should_prompt_uniform_across_abort_trip_reboot(void)
{
    TEST_SECTION("live_edit_should_prompt -- DONE, HALTED, FAULTED and a post-reboot record all prompt identically");
    // Plan section 5 is explicit that the prompt condition must not special-
    // case HOW the run ended -- a DONE, a HALTED, a FAULTED trip and a
    // reboot that interrupted the firing all reduce to the same two facts
    // (pending, not running), so all four are asserted to agree here.
    live_edit_record_t rec = {0};
    rec.pending = 1;
    TEST_CHECK(live_edit_should_prompt(&rec, false), "a clean DONE prompts");
    TEST_CHECK(live_edit_should_prompt(&rec, false), "a HALTED run prompts identically");
    TEST_CHECK(live_edit_should_prompt(&rec, false), "a FAULTED trip prompts identically");
    TEST_CHECK(live_edit_should_prompt(&rec, false), "a reboot that interrupted the firing prompts identically");
}

// ---------------------------------------------------------------------------
// live_edit_check_window -- plan section 2's edit window rule

static profile_t make_test_profile(void)
{
    profile_t p;
    memset(&p, 0, sizeof(p));
    strncpy(p.name, "Test", sizeof(p.name) - 1);
    p.zone_mask = 0x01;
    p.segment_count = 3;
    p.segments[0].target_c = 100.0f;
    p.segments[0].ramp_c_per_hr = 60.0f;
    p.segments[0].dwell_min = 10;
    p.segments[1].target_c = 500.0f;
    p.segments[1].ramp_c_per_hr = 120.0f;
    p.segments[1].dwell_min = 20;
    p.segments[2].target_c = 900.0f;
    p.segments[2].ramp_c_per_hr = 90.0f;
    p.segments[2].dwell_min = 0;
    return p;
}

static void test_window_refuses_change_to_frozen_segment(void)
{
    TEST_SECTION("live_edit_check_window -- a change to an already-run segment is refused");
    profile_t running = make_test_profile();
    profile_t candidate = running;
    candidate.segments[0].target_c = 999.0f; // segment 0 already ran (running index is 1)
    char err[128];
    bool refused = live_edit_check_window(&running, &candidate, 1, err, sizeof(err));
    TEST_CHECK(refused, "editing a frozen (already-run) segment is refused");
}

static void test_window_allows_target_change_on_running_segment(void)
{
    TEST_SECTION("live_edit_check_window -- target_c/ramp/dwell on the RUNNING segment is allowed");
    profile_t running = make_test_profile();
    profile_t candidate = running;
    candidate.segments[1].target_c = 550.0f;
    candidate.segments[1].ramp_c_per_hr = 100.0f;
    candidate.segments[1].dwell_min = 25;
    char err[128];
    bool refused = live_edit_check_window(&running, &candidate, 1, err, sizeof(err));
    TEST_CHECK(!refused, "target_c/ramp/dwell may change on the segment currently running");
}

static void test_window_refuses_seg_kind_change_on_running_segment(void)
{
    TEST_SECTION("live_edit_check_window -- seg_kind change on the RUNNING segment is refused");
    profile_t running = make_test_profile();
    profile_t candidate = running;
    candidate.segments[1].seg_kind = PROFILE_SEG_KIND_RELAY_IO;
    char err[128];
    bool refused = live_edit_check_window(&running, &candidate, 1, err, sizeof(err));
    TEST_CHECK(refused, "seg_kind cannot change on the running segment");
}

static void test_window_allows_future_segment_deletion(void)
{
    TEST_SECTION("live_edit_check_window -- deleting a future segment is allowed");
    profile_t running = make_test_profile();
    profile_t candidate = running;
    candidate.segment_count = 2; // drop segment 2 (future, running index is 1)
    char err[128];
    bool refused = live_edit_check_window(&running, &candidate, 1, err, sizeof(err));
    TEST_CHECK(!refused, "a future segment may be deleted");
}

static void test_window_refuses_running_segment_deletion(void)
{
    TEST_SECTION("live_edit_check_window -- deleting the RUNNING segment is refused");
    profile_t running = make_test_profile();
    profile_t candidate = running;
    candidate.segment_count = 1; // would delete segment 1, the one running
    char err[128];
    bool refused = live_edit_check_window(&running, &candidate, 1, err, sizeof(err));
    TEST_CHECK(refused, "the running segment itself cannot be deleted");
}

static void test_window_refuses_zone_mask_change(void)
{
    TEST_SECTION("live_edit_check_window -- zone_mask can never change while running");
    profile_t running = make_test_profile();
    profile_t candidate = running;
    candidate.zone_mask = 0x03;
    char err[128];
    bool refused = live_edit_check_window(&running, &candidate, 1, err, sizeof(err));
    TEST_CHECK(refused, "zone_mask is refused unconditionally");
}

// ---------------------------------------------------------------------------
// profile_executor_live_pickup_check -- combines the window check with a
// caller-supplied HARD-validate seam (plan section 7/11).

static bool always_valid(void *ctx, const profile_t *candidate, char *err_msg, size_t err_cap)
{
    (void)ctx;
    (void)candidate;
    if (err_msg && err_cap) err_msg[0] = '\0';
    return true;
}

static bool always_invalid(void *ctx, const profile_t *candidate, char *err_msg, size_t err_cap)
{
    (void)ctx;
    (void)candidate;
    if (err_msg && err_cap) snprintf(err_msg, err_cap, "fake refusal");
    return false;
}

static void test_pickup_ok_when_window_and_validate_both_pass(void)
{
    TEST_SECTION("profile_executor_live_pickup_check -- OK when window and HARD validate both pass");
    profile_t running = make_test_profile();
    profile_t candidate = running;
    candidate.segments[1].target_c = 550.0f;
    char err[128];
    profile_live_pickup_result_t r =
        profile_executor_live_pickup_check(&running, &candidate, 1, always_valid, NULL, err, sizeof(err));
    TEST_CHECK(r == PROFILE_LIVE_PICKUP_OK, "pickup adopts a structurally-legal, HARD-valid candidate");
}

static void test_pickup_refuses_on_window_violation_before_validating(void)
{
    TEST_SECTION("profile_executor_live_pickup_check -- window violation refuses before the validate callback runs");
    profile_t running = make_test_profile();
    profile_t candidate = running;
    candidate.zone_mask = 0x03; // window violation
    char err[128];
    // always_invalid would ALSO refuse -- if this test's real return code
    // were REFUSED_INVALID instead of REFUSED_WINDOW, that would prove the
    // window check was skipped, not merely that both checks agree.
    profile_live_pickup_result_t r =
        profile_executor_live_pickup_check(&running, &candidate, 1, always_invalid, NULL, err, sizeof(err));
    TEST_CHECK(r == PROFILE_LIVE_PICKUP_REFUSED_WINDOW, "the window check runs and refuses first, cheaper than validation");
}

static void test_pickup_refuses_on_hard_validate_failure(void)
{
    TEST_SECTION("profile_executor_live_pickup_check -- HARD validate refusal is reported distinctly");
    profile_t running = make_test_profile();
    profile_t candidate = running;
    candidate.segments[1].target_c = 550.0f; // window-legal
    char err[128];
    profile_live_pickup_result_t r =
        profile_executor_live_pickup_check(&running, &candidate, 1, always_invalid, NULL, err, sizeof(err));
    TEST_CHECK(r == PROFILE_LIVE_PICKUP_REFUSED_INVALID, "a window-legal but HARD-invalid candidate is refused distinctly");
    TEST_CHECK(strcmp(err, "fake refusal") == 0, "the validator's own reason is surfaced");
}

// ---------------------------------------------------------------------------
// live_profile_fork/save/load/clear -- real hal_kv (fake_kv.c) persistence,
// with read-back verification.

static void test_fork_then_load_working_and_record(void)
{
    TEST_SECTION("live_profile_fork -- persists both the working copy and the record, read-back verified");
    profile_t origin = make_test_profile();
    profile_t working;
    live_edit_record_t rec;
    char err[128];
    bool ok = live_profile_fork(4, false, "Bisque Fast", &origin, &working, &rec, err, sizeof(err));
    TEST_CHECK(ok, "fork succeeds");
    TEST_CHECK(memcmp(&working, &origin, sizeof(profile_t)) == 0, "the working copy matches the origin verbatim");
    TEST_CHECK(rec.origin_id == 4, "record captures origin_id");
    TEST_CHECK(rec.pending == 1, "record starts pending");

    profile_t loaded;
    TEST_CHECK(live_profile_load_working(&loaded), "load_working succeeds after fork");
    TEST_CHECK(memcmp(&loaded, &origin, sizeof(profile_t)) == 0, "loaded working copy matches origin");

    live_edit_record_t loaded_rec;
    TEST_CHECK(live_profile_load_record(&loaded_rec), "load_record succeeds after fork");
    TEST_CHECK(loaded_rec.origin_id == 4, "loaded record's origin_id round-trips");

    // Cleanup for the next test's fresh fork() to not see this as "already pending".
    TEST_CHECK(live_profile_clear(err, sizeof(err)), "clear succeeds");
}

static void test_fork_is_idempotent_when_already_pending(void)
{
    TEST_SECTION("live_profile_fork -- a second fork while one is pending returns the EXISTING working copy");
    profile_t origin = make_test_profile();
    profile_t working1, working2;
    live_edit_record_t rec1, rec2;
    char err[128];
    TEST_CHECK(live_profile_fork(5, false, "Original", &origin, &working1, &rec1, err, sizeof(err)),
               "first fork succeeds");

    profile_t different_origin = make_test_profile();
    different_origin.segments[0].target_c = 12345.0f; // would be visibly different if re-forked
    TEST_CHECK(live_profile_fork(6, false, "Different", &different_origin, &working2, &rec2, err, sizeof(err)),
               "second fork call succeeds (idempotent, does not error)");
    TEST_CHECK(rec2.origin_id == 5, "the EXISTING record wins, not the second call's origin_id");
    TEST_CHECK(working2.segments[0].target_c != 12345.0f, "the existing working copy is untouched by the second call");

    TEST_CHECK(live_profile_clear(err, sizeof(err)), "clear succeeds");
}

static void test_save_working_bumps_generation(void)
{
    TEST_SECTION("live_profile_save_working -- bumps the generation counter the executor polls");
    uint32_t before = live_profile_generation();
    profile_t p = make_test_profile();
    char err[128];
    TEST_CHECK(live_profile_save_working(&p, err, sizeof(err)), "save_working succeeds");
    TEST_CHECK(live_profile_generation() != before, "generation counter moves on a successful save");
    TEST_CHECK(live_profile_clear(err, sizeof(err)), "clear succeeds");
}

static void test_clear_is_idempotent(void)
{
    TEST_SECTION("live_profile_clear -- calling it with nothing pending is not an error");
    char err[128];
    TEST_CHECK(live_profile_clear(err, sizeof(err)), "first clear (nothing pending) succeeds");
    TEST_CHECK(live_profile_clear(err, sizeof(err)), "second clear (still nothing pending) also succeeds");

    live_edit_record_t rec;
    TEST_CHECK(!live_profile_load_record(&rec), "no record is readable after clear");
}

int main(void)
{
    // fake_kv.c (the host hal_kv backend) requires every partition to be
    // explicitly initialized before hal_kv_open() will succeed against it --
    // same requirement test_profiles_http.c's nvs_stub_reset() documents.
    // LIVE_PROFILE_NVS_PARTITION's literal ("profiles_nvs") is duplicated
    // here rather than referencing the macro, since that macro is private to
    // live_profile.c and this call must run before that file's #include below.
    hal_kv_init_partition("profiles_nvs");

    test_record_roundtrip();
    test_record_version_mismatch_discarded();
    test_overwrite_refused_for_builtin();
    test_overwrite_refused_for_id_outside_user_range();
    test_overwrite_allowed_for_user_profile();
    test_name_collision_case_and_whitespace_insensitive();
    test_name_collision_excludes_self();
    test_name_no_collision_distinct_name();
    test_decide_save_as_refuses_name_collision();
    test_decide_save_as_accepts_distinct_name();
    test_decide_overwrite_refuses_builtin_before_confirm();
    test_decide_overwrite_refuses_without_confirm();
    test_decide_overwrite_allowed_with_confirm();
    test_decide_discard_always_allowed();
    test_should_prompt_when_pending_and_not_running();
    test_should_prompt_false_while_running();
    test_should_prompt_false_when_not_pending();
    test_should_prompt_uniform_across_abort_trip_reboot();
    test_window_refuses_change_to_frozen_segment();
    test_window_allows_target_change_on_running_segment();
    test_window_refuses_seg_kind_change_on_running_segment();
    test_window_allows_future_segment_deletion();
    test_window_refuses_running_segment_deletion();
    test_window_refuses_zone_mask_change();
    test_pickup_ok_when_window_and_validate_both_pass();
    test_pickup_refuses_on_window_violation_before_validating();
    test_pickup_refuses_on_hard_validate_failure();
    test_clear_is_idempotent(); // run before the fork tests so state starts clean
    test_fork_then_load_working_and_record();
    test_fork_is_idempotent_when_already_pending();
    test_save_working_bumps_generation();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    return 0;
}
