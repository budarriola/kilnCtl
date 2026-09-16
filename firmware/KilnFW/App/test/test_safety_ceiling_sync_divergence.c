// Host tests for safety_ceiling_sync.c's DIVERGENCE ENFORCEMENT path --
// the 2026-09-14 owner decision, verbatim: "if a config doesn't land and
// match on both sides then alarm and dissable heaters." Named by
// safety_ceiling_sync.h's own doc comment on
// safety_ceiling_sync_set_disable_heat_hooks() since that feature landed,
// but this file did not exist until the 2026-09-14 opus review (defect 3,
// docs/audits/pico_ceiling_mirror_and_rate_guard_2026-09-14.md) found the
// citation dangling -- the enforcement path (hooks firing, the latch, the
// target_known exclusion, and the uninstalled-hook case) had NO test at
// all. This file closes that gap using the REAL production functions
// (safety_ceiling_sync_reconcile_on_link_up(), safety_ceiling_sync_set_
// disable_heat_hooks(), safety_ceiling_sync_is_diverged()) -- not a mirror
// or a reimplementation.
//
// This is its own, separate executable (see build_host_tests.ps1): it
// fakes zones_config_is_valid()/zones_config_get_temp_limits(),
// safety_cfg_store_param_count()/_get_by_index() and safety_cfg_http_
// set_and_confirm_f32() itself, own bodies -- linking the REAL
// zones_config_*/safety_cfg_store.c/safety_cfg_http.c would pull in the
// whole NVS/httpd ecosystem those files own, which this file has no
// business depending on just to drive safety_ceiling_sync.c's divergence
// enforcement. Same "fake the seam, link the real file under test"
// convention as test_safety_ceiling_policy.c/test_zones_http.c.
//
// WHAT THIS FILE MUST PROVE (2026-09-14 opus review, defect 3):
//  1. A real divergence (ESP has a target, Pico's confirmed ceiling
//     disagrees or is unconfirmed) fires BOTH installed hooks
//     (all-relays-off, halt-run) and safety_ceiling_sync_is_diverged()
//     reports true with a non-empty reason.
//  2. LATCH BEHAVIOUR: the hooks fire on every tick a divergence persists
//     (not just the first), and once the sides agree again, is_diverged()
//     clears (reason resets to empty) and the hooks stop being called.
//  3. TARGET_KNOWN EXCLUSION: a fresh/all-zero ESP config (no positive
//     max_temp_c anywhere -- target_known == false) is NEVER reported as a
//     divergence, regardless of what the Pico side holds -- a
//     never-configured board must not alarm on every tick before anyone
//     has set a zone ceiling.
//  4. THE UNINSTALLED-HOOK CASE (2026-09-14 opus review defect 2's actual
//     bug): with NO hooks installed (the default -- every existing host
//     test, and the real ESP during the boot window before main_control_
//     bringup.c installs them), a real divergence still sets is_diverged()
//     true (the verdict readiness_gate.c/readiness_http.c actually read is
//     never wrong) but calls nothing -- proving the NULL-hook path is a
//     safe no-op, not a crash, which is what makes defect 2's "log an
//     action not taken" a truthfulness bug rather than a null-pointer bug.
//
// NEGATIVE TEST (2026-09-14, run and recorded below in this header
// comment's own commit history -- see docs/audits/divergence_message_and_
// enforcement_test_2026-09-14.md for the actual RED/restore transcript):
// commenting out the `if (s_disable_all_relays_off) { s_disable_all_relays_
// off(); }` call in enforce_ceiling_divergence() (safety_ceiling_sync.c)
// fails test_divergence_fires_both_hooks() below (relays_off_calls stays 0).
// Restore BY HAND (re-add the two lines exactly), delete App/test/build,
// and rebuild from scratch before trusting a green result again -- this
// repo has shipped vacuous checks and a test that never reached the code
// it named; this file exists specifically to stop being the next one.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

#include "MAX31856.h"
#include "config_divergence.h"
#include "safety_cfg_write.h"
#include "safety_cfg_store.h"
#include "safety_ceiling_sync.h"

// ---------------------------------------------------------------------
// Fakes: zones_config_*() -- the ESP's own live zone-ceiling source.
// Empty/zero by default (every zone's max_temp_c == 0.0f, i.e.
// target_known == false), same "off by default, a test that wants a real
// target sets it explicitly" convention as test_zones_http.c's s_cfg_rows.
// ---------------------------------------------------------------------
static bool s_zones_valid = true;
static float s_zone_max_temp_c[MAX31856_CHANNEL_COUNT];

static void fake_zones_reset(void)
{
    s_zones_valid = true;
    memset(s_zone_max_temp_c, 0, sizeof(s_zone_max_temp_c));
}

bool zones_config_is_valid(void)
{
    return s_zones_valid;
}

bool zones_config_get_temp_limits(uint8_t zone_index, float *out_max_temp_c, float *out_min_temp_c)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    if (out_max_temp_c) {
        *out_max_temp_c = s_zone_max_temp_c[zone_index];
    }
    if (out_min_temp_c) {
        *out_min_temp_c = 0.0f;
    }
    return true;
}

// ---------------------------------------------------------------------
// Fakes: safety_cfg_store_param_count()/_get_by_index() -- the Pico's
// cached abs_max_temp_c, as safety_ceiling_sync_get_current_pico_ceiling()
// reads it. Empty by default (Pico ceiling unknown/unconfirmed), same
// convention as test_zones_http.c's s_cfg_rows.
// ---------------------------------------------------------------------
static bool s_pico_ceiling_set = false;
static float s_pico_ceiling_c = 0.0f;

// 2026-09-15 audit fix (Defect 2): a second, independent fake row for the
// "extra" (non-ceiling) broadened field -- id chosen arbitrarily, distinct
// from SAFETY_PARAM_ID_ABS_MAX_TEMP_C.
#define FAKE_EXTRA_PARAM_ID 0x0201u
static bool s_pico_extra_set = false;
static float s_pico_extra_c = 0.0f;
// 2026-09-15 review (F6): the extra row's param_id/type used to be
// hardcoded to FAKE_EXTRA_PARAM_ID/F32, which is why the first cut of
// test_tc_type_revert_divergence_detected() below had to overwrite its
// expectation's param_id to FAKE_EXTRA_PARAM_ID right before the assertion
// to make the fake and the expectation agree on an id -- a test that
// would pass identically even if tc_type were never captured at all.
// Parametrizing these two lets a test drive the REAL tc_type id (0x0105,
// U8) through safety_cfg_store_get_by_index() and have the production
// comparator (safety_ceiling_sync.c's type-aware switch) actually decode
// it as a U8, not merely echo an F32 under a borrowed id.
static uint16_t s_pico_extra_param_id = FAKE_EXTRA_PARAM_ID;
static uint8_t s_pico_extra_type = KILNLINK_PARAM_TYPE_F32;

// 2026-09-15 review (review_divergence_check_561efa3b_2026-09-15.md,
// MEDIUM 5): controllable fake for the stale-cache accessor. Defaults false
// (fresh cache, matches every existing test in this file unless it opts in).
static bool s_cache_stale = false;

static void fake_pico_ceiling_reset(void)
{
    s_pico_ceiling_set = false;
    s_pico_ceiling_c = 0.0f;
    s_pico_extra_set = false;
    s_pico_extra_c = 0.0f;
    s_pico_extra_param_id = FAKE_EXTRA_PARAM_ID;
    s_pico_extra_type = KILNLINK_PARAM_TYPE_F32;
    s_cache_stale = false;
}

static void fake_pico_ceiling_set(float value)
{
    s_pico_ceiling_set = true;
    s_pico_ceiling_c = value;
}

// Default-id/type extra-field setter, unchanged behaviour for the existing
// test_broadened_field_divergence_detected() below.
static void fake_pico_extra_set(float value)
{
    s_pico_extra_set = true;
    s_pico_extra_c = value;
}

// Extra-field setter that also picks which param_id/type the fake reports
// it under -- see the comment on s_pico_extra_param_id above.
static void fake_pico_extra_set_ex(uint16_t param_id, uint8_t type, float value)
{
    s_pico_extra_param_id = param_id;
    s_pico_extra_type = type;
    s_pico_extra_set = true;
    s_pico_extra_c = value;
}

size_t safety_cfg_store_param_count(void)
{
    size_t n = s_pico_ceiling_set ? 1u : 0u;
    n += s_pico_extra_set ? 1u : 0u;
    return n;
}

bool safety_cfg_store_cache_is_stale(void)
{
    return s_cache_stale;
}

// 2026-09-15 review (review_divergence_fixes_b2e7017f_2026-09-15.md, MEDIUM
// 3): enforce_ceiling_divergence() now latches the cache generation at entry
// (cache_gen_at_entry) so kiln_cfg_store.c's autosave gate can tell whether a
// refetch raced in between. This file does not link the real safety_cfg_
// store.c, so a controllable stub is needed here too, same convention as
// test_kiln_cfg_store.c's own copy. Fixed (not moving) is fine -- none of
// this file's tests exercise the race itself, only that enforce_ceiling_
// divergence() calls this and does not crash.
uint32_t safety_cfg_store_cache_generation(void)
{
    return 0;
}

// Same review, MEDIUM 5 follow-up: enforce_ceiling_divergence() reads this to
// decide whether to append the "an operator refused an unsafe change" hint
// to the standing-warning reason (HIGH 2's REJECT_ARMED path). No armed
// refusal by default -- the one test that needs the hint text sets this via
// s_stub_recent_armed_refusal below.
static bool s_stub_recent_armed_refusal = false;
bool safety_cfg_store_recent_armed_refusal(void)
{
    return s_stub_recent_armed_refusal;
}

bool safety_cfg_store_get_by_index(size_t index, safety_cfg_param_t *out)
{
    // Index 0 is the ceiling row (if set), index 1 (or 0 if the ceiling is
    // unset) is the extra row (if set) -- mirrors this fake's own
    // param_count() above; order does not matter to the production code
    // under test, which scans by param_id, not by index.
    size_t i = 0;
    if (s_pico_ceiling_set) {
        if (index == i) {
            if (out) {
                memset(out, 0, sizeof(*out));
                out->param_id = SAFETY_PARAM_ID_ABS_MAX_TEMP_C;
                out->type = KILNLINK_PARAM_TYPE_F32;
                out->value.f32_val = s_pico_ceiling_c;
                out->set = true;
            }
            return true;
        }
        i++;
    }
    if (s_pico_extra_set) {
        if (index == i) {
            if (out) {
                memset(out, 0, sizeof(*out));
                out->param_id = s_pico_extra_param_id;
                out->type = s_pico_extra_type;
                // s_pico_extra_c is always stored as a float regardless of
                // the reported type -- for KILNLINK_PARAM_TYPE_U8 (tc_type's
                // real wire type) it is decoded into u8_val the same way
                // safety_cfg_store.c's real mirror table would report an
                // actual byte value, so the production comparator's
                // type-aware switch (safety_ceiling_sync.c) sees a real U8
                // row, not an F32 row wearing tc_type's param_id.
                if (s_pico_extra_type == KILNLINK_PARAM_TYPE_U8) {
                    out->value.u8_val = (uint8_t)s_pico_extra_c;
                } else {
                    out->value.f32_val = s_pico_extra_c;
                }
                out->set = true;
            }
            return true;
        }
    }
    return false;
}

// 2026-09-15 audit fix (Defect 2): safety_ceiling_sync.c's broadened
// enforce_ceiling_divergence() calls safety_cfg_store_lookup() to name any
// EXTRA (non-ceiling) field in the human-readable reason string. Trivial
// fake -- this file's own extra-field tests below supply the name they
// expect directly and don't depend on a real mirror table.
bool safety_cfg_store_lookup(uint16_t param_id, uint8_t *out_type, const char **out_name)
{
    (void)param_id;
    if (out_type) {
        *out_type = KILNLINK_PARAM_TYPE_F32;
    }
    if (out_name) {
        *out_name = "extra_field";
    }
    return true;
}

// ---------------------------------------------------------------------
// Fake: safety_cfg_write_set_and_confirm_f32() -- the Pico-write half of a
// RAISE. This file is not exercising the raise/backoff machinery
// (test_safety_ceiling_policy.c already owns that, at the pure-logic
// layer) -- it only needs the reconcile call this test drives to complete
// without touching real ESP-httpd code, so this always succeeds and
// mirrors the write into the same fake Pico-ceiling cache above, exactly
// the way a real confirmed write would update safety_cfg_store's cache.
// ---------------------------------------------------------------------
bool safety_cfg_write_set_and_confirm_f32(SafetyLinkClass *link, uint16_t param_id, float value, char *reason_out,
                                          size_t reason_cap, safety_ceiling_refusal_class_t *out_class)
{
    (void)link;
    (void)param_id;
    if (reason_out && reason_cap > 0) {
        reason_out[0] = '\0';
    }
    if (out_class) {
        *out_class = SAFETY_CEILING_REFUSAL_NONE;
    }
    fake_pico_ceiling_set(value);
    return true;
}

// ---------------------------------------------------------------------
// Hook counters -- what test_uninstalled_hooks_are_a_safe_noop() and
// test_divergence_fires_both_hooks() both observe.
// ---------------------------------------------------------------------
static int s_relays_off_calls = 0;
static int s_halt_run_calls = 0;

static void fake_all_relays_off(void)
{
    s_relays_off_calls++;
}

static void fake_halt_run(void)
{
    s_halt_run_calls++;
}

static void hook_counters_reset(void)
{
    s_relays_off_calls = 0;
    s_halt_run_calls = 0;
}

// A non-NULL SafetyLinkClass* is all safety_ceiling_sync_reconcile_on_
// link_up() needs to treat the link as "up" -- it is never dereferenced by
// this file's fakes (safety_cfg_write_set_and_confirm_f32() above ignores
// its `link` argument, same as test_zones_http.c's identical fake), so an
// uninitialized-but-non-NULL pointer is sufficient and standard practice
// in this test suite (test_zones_http.c's `(SafetyLinkClass *)1`).
static SafetyLinkClass *const FAKE_LINK = (SafetyLinkClass *)1;

static void test_reset_all(void)
{
    fake_zones_reset();
    fake_pico_ceiling_reset();
    hook_counters_reset();
}

// ---------------------------------------------------------------------
// 1. Uninstalled hooks: a real divergence sets the verdict but calls
//    nothing. This is the exact NULL-hook state main_control_bringup.c's
//    boot window (before this 2026-09-14 fix, the whole stretch between
//    safety_link_start() and the hook install) left enforcement in --
//    defect 2's bug was logging "heaters disabled" here, not this
//    behaviour itself, which is documented as the correct, safe default
//    for a test binary (or a not-yet-bootstrapped board) with no hooks
//    installed.
// ---------------------------------------------------------------------
static void test_uninstalled_hooks_are_a_safe_noop(void)
{
    TEST_SECTION("no hooks installed: divergence verdict is set, nothing is called");
    test_reset_all();
    // Deliberately do NOT call safety_ceiling_sync_set_disable_heat_hooks()
    // -- this proves the module's own default state, not a state this test
    // constructed. NOTE: safety_ceiling_sync.c holds its hook pointers in
    // file-scope statics with no reset-to-NULL entry point, so if an
    // EARLIER test in this same process installed hooks, that would leak
    // into this one silently. This test runs FIRST for exactly that
    // reason -- see main() below.
    s_zone_max_temp_c[0] = 80.0f; // target_known == true
    fake_pico_ceiling_reset();    // Pico side unconfirmed -- guaranteed divergence

    safety_ceiling_sync_reconcile_on_link_up(FAKE_LINK);

    char reason[CONFIG_DIVERGENCE_REASON_MAX];
    bool diverged = safety_ceiling_sync_is_diverged(reason, sizeof(reason));
    TEST_CHECK(diverged, "verdict is diverged even with no hooks installed");
    TEST_CHECK(reason[0] != '\0', "reason is non-empty even with no hooks installed");
    TEST_CHECK(s_relays_off_calls == 0, "all-relays-off hook not called: it was never installed");
    TEST_CHECK(s_halt_run_calls == 0, "halt-run hook not called: it was never installed");
}

// ---------------------------------------------------------------------
// 2. Hooks fire on divergence, once installed.
// ---------------------------------------------------------------------
static void test_divergence_fires_both_hooks(void)
{
    TEST_SECTION("installed hooks: a real divergence calls both");
    test_reset_all();
    safety_ceiling_sync_set_disable_heat_hooks(fake_all_relays_off, fake_halt_run);

    // Pico WIDER than the ESP's target (100 > 80), not narrower: reconcile_
    // on_link_up() calls enforce_ceiling_divergence() UNCONDITIONALLY, then
    // (separately) attempts a RAISE via safety_ceiling_sync_guard_raise() --
    // a narrower Pico would let that raise succeed against this file's own
    // always-succeeds writer fake and self-heal the mismatch inside this
    // same call, which is real behaviour but would make this test about the
    // raise path, not the enforcement path it exists to prove. A Pico
    // that is already wider than the target gives guard_raise nothing to
    // do (safety_ceiling_policy_guard_raise() no-ops once "already wide
    // enough") while the identity comparator still calls it a genuine
    // divergence (it is not equal, in EITHER direction) -- see
    // test_latch_persists_while_diverged() below for the multi-tick version
    // of this same setup.
    s_zone_max_temp_c[0] = 80.0f;  // ESP target known
    fake_pico_ceiling_set(100.0f); // Pico disagrees -- real divergence, and not "too narrow"

    safety_ceiling_sync_reconcile_on_link_up(FAKE_LINK);

    char reason[CONFIG_DIVERGENCE_REASON_MAX];
    bool diverged = safety_ceiling_sync_is_diverged(reason, sizeof(reason));
    TEST_CHECK(diverged, "verdict is diverged on a real value mismatch");
    TEST_CHECK(strstr(reason, "abs_max_temp_c") != NULL, "reason names the diverged field");
    TEST_CHECK(s_relays_off_calls == 1, "all-relays-off hook fired once");
    TEST_CHECK(s_halt_run_calls == 1, "halt-run hook fired once");
}

// ---------------------------------------------------------------------
// 2b. LATCH BEHAVIOUR, persistence: a divergence that guard_raise() cannot
//     resolve (Pico already wider than target -- nothing to raise) keeps
//     firing the hooks on every subsequent tick, not just the first. This
//     is not an edge-triggered "alarm once" mechanism -- safety_ceiling_
//     sync.c's own header comment: "There is no separate 'clear the alarm'
//     action... simply stops calling the disable path... the moment
//     config_divergence_check() reports no divergence."
// ---------------------------------------------------------------------
static void test_latch_persists_while_diverged(void)
{
    TEST_SECTION("persistent divergence: hooks fire again on every later tick");
    test_reset_all();
    safety_ceiling_sync_set_disable_heat_hooks(fake_all_relays_off, fake_halt_run);

    s_zone_max_temp_c[0] = 80.0f;
    fake_pico_ceiling_set(100.0f); // wider than target -- guard_raise has nothing to do, mismatch persists

    safety_ceiling_sync_reconcile_on_link_up(FAKE_LINK);
    TEST_CHECK(s_relays_off_calls == 1 && s_halt_run_calls == 1, "hooks fire on the first diverged tick");
    TEST_CHECK(safety_ceiling_sync_is_diverged(NULL, 0), "still diverged after the first tick");

    safety_ceiling_sync_reconcile_on_link_up(FAKE_LINK);
    TEST_CHECK(s_relays_off_calls == 2, "all-relays-off hook fires again on a second diverged tick");
    TEST_CHECK(s_halt_run_calls == 2, "halt-run hook fires again on a second diverged tick");

    safety_ceiling_sync_reconcile_on_link_up(FAKE_LINK);
    TEST_CHECK(s_relays_off_calls == 3, "all-relays-off hook fires a third time on a third diverged tick");
    TEST_CHECK(s_halt_run_calls == 3, "halt-run hook fires a third time on a third diverged tick");
    TEST_CHECK(safety_ceiling_sync_is_diverged(NULL, 0), "still diverged after three ticks -- never self-clears");
}

// ---------------------------------------------------------------------
// 3. Latch clears once the sides genuinely agree, and the hooks stop
//    being called from that tick on.
// ---------------------------------------------------------------------
static void test_latch_clears_when_sides_agree(void)
{
    TEST_SECTION("latch clears once ESP and Pico agree; hooks stop firing");
    test_reset_all();
    safety_ceiling_sync_set_disable_heat_hooks(fake_all_relays_off, fake_halt_run);

    s_zone_max_temp_c[0] = 80.0f;
    fake_pico_ceiling_set(60.0f); // diverged
    safety_ceiling_sync_reconcile_on_link_up(FAKE_LINK);
    TEST_CHECK(safety_ceiling_sync_is_diverged(NULL, 0), "diverged after the first mismatched tick");
    TEST_CHECK(s_relays_off_calls == 1 && s_halt_run_calls == 1, "hooks fired once while diverged");

    // The Pico's cache now reports the SAME value the ESP holds (e.g. a
    // reconcile write landed and was confirmed by a real read-back, or an
    // operator manually re-armed the Pico to match) -- the very next tick
    // must clear the latch, not merely stop re-asserting it.
    fake_pico_ceiling_set(80.0f);
    safety_ceiling_sync_reconcile_on_link_up(FAKE_LINK);

    char reason[CONFIG_DIVERGENCE_REASON_MAX];
    bool diverged = safety_ceiling_sync_is_diverged(reason, sizeof(reason));
    TEST_CHECK(!diverged, "latch clears once both sides agree");
    TEST_CHECK(reason[0] == '\0', "reason resets to empty once cleared");
    TEST_CHECK(s_relays_off_calls == 1, "all-relays-off hook NOT called again once agreement is reached");
    TEST_CHECK(s_halt_run_calls == 1, "halt-run hook NOT called again once agreement is reached");
}

// ---------------------------------------------------------------------
// 4. target_known exclusion: a fresh/all-zero ESP config never diverges,
//    no matter what the Pico side holds.
// ---------------------------------------------------------------------
static void test_target_known_exclusion(void)
{
    TEST_SECTION("all-zero ESP config (target_known == false) is never a divergence");
    test_reset_all();
    safety_ceiling_sync_set_disable_heat_hooks(fake_all_relays_off, fake_halt_run);

    // Every zone's max_temp_c stays 0.0f (fake_zones_reset()'s default) --
    // target_known == false via safety_ceiling_policy_target_c() -- while
    // the Pico side is left BOTH unconfirmed and, separately, holding a
    // real confirmed value, to prove neither shape of "Pico side" trips
    // this exclusion.
    fake_pico_ceiling_reset();
    safety_ceiling_sync_reconcile_on_link_up(FAKE_LINK);
    TEST_CHECK(!safety_ceiling_sync_is_diverged(NULL, 0),
               "no target yet + unconfirmed Pico: not a divergence (nothing to compare)");
    TEST_CHECK(s_relays_off_calls == 0 && s_halt_run_calls == 0, "hooks not called: no target yet");

    fake_pico_ceiling_set(45.0f);
    safety_ceiling_sync_reconcile_on_link_up(FAKE_LINK);
    TEST_CHECK(!safety_ceiling_sync_is_diverged(NULL, 0),
               "no target yet + a real Pico value: still not a divergence");
    TEST_CHECK(s_relays_off_calls == 0 && s_halt_run_calls == 0, "hooks still not called: no target yet");
}

// ---------------------------------------------------------------------
// 5. 2026-09-15 audit fix (Defect 2): the broadened seam. With the
//    expected-pico-fields source installed and an EXTRA (non-ceiling)
//    field mismatched while abs_max_temp_c itself agrees, this must still
//    be reported and enforced as a divergence -- proving the fix for the
//    exact defect: before this change, a mismatch confined to any field
//    other than abs_max_temp_c was invisible to this check.
// ---------------------------------------------------------------------
static safety_ceiling_expected_param_t s_expected_fields[1];
static size_t s_expected_field_count = 0;

static size_t fake_expected_pico_fields_source(safety_ceiling_expected_param_t *out_fields, size_t cap)
{
    size_t n = s_expected_field_count < cap ? s_expected_field_count : cap;
    for (size_t i = 0; i < n; i++) {
        out_fields[i] = s_expected_fields[i];
    }
    return n;
}

static void test_broadened_field_divergence_detected(void)
{
    TEST_SECTION("2026-09-15 fix, HIGH 2 interim (review_divergence_check_561efa3b_2026-09-15.md): "
                 "an extra (non-ceiling) field mismatch is detected and reported as a WARNING, "
                 "never as heat-off (kiln_cfg_store_apply() does not yet push/confirm this field "
                 "set to the Pico, so treating it as heat-disabling would false-trip on an "
                 "ordinary apply -- see safety_ceiling_sync.h's own doc comment on "
                 "safety_ceiling_sync_is_standing_diverged())");
    test_reset_all();
    safety_ceiling_sync_set_disable_heat_hooks(fake_all_relays_off, fake_halt_run);
    safety_ceiling_sync_set_expected_pico_fields_source(fake_expected_pico_fields_source);

    // abs_max_temp_c AGREES on both sides -- the pre-fix check would see no
    // divergence at all here.
    s_zone_max_temp_c[0] = 80.0f;
    fake_pico_ceiling_set(80.0f);

    // The extra field DISAGREES: the active kiln-config slot expects 42.0,
    // but the Pico's live cache (e.g. reverted to a flash-persisted value
    // after a reboot) reports 7.0.
    s_expected_fields[0].param_id = FAKE_EXTRA_PARAM_ID;
    s_expected_fields[0].value = 42.0f;
    s_expected_field_count = 1;
    fake_pico_extra_set(7.0f);

    safety_ceiling_sync_reconcile_on_link_up(FAKE_LINK);

    char reason[CONFIG_DIVERGENCE_REASON_MAX];
    bool warned = safety_ceiling_sync_is_standing_diverged(reason, sizeof(reason));
    TEST_CHECK(warned, "a mismatch confined to a non-ceiling field is reported as a standing warning");
    TEST_CHECK(!safety_ceiling_sync_is_diverged(NULL, 0),
               "a non-ceiling-only mismatch does NOT set the heat-disabling verdict (HIGH 2 interim)");
    TEST_CHECK(s_relays_off_calls == 0, "all-relays-off hook does NOT fire on a non-ceiling-only divergence");
    TEST_CHECK(s_halt_run_calls == 0, "halt-run hook does NOT fire on a non-ceiling-only divergence");

    // Now the extra field also agrees -- the warning must clear.
    fake_pico_extra_set(42.0f);
    safety_ceiling_sync_reconcile_on_link_up(FAKE_LINK);
    TEST_CHECK(!safety_ceiling_sync_is_standing_diverged(NULL, 0), "warning clears once the extra field also agrees");

    // Cleanup: leave the seam installed but pointed at zero fields, so it
    // cannot leak a stale expectation into any test added after this one in
    // the same process (safety_ceiling_sync.c's seam pointer, like its hook
    // pointers, has no unset-to-NULL entry point by design).
    s_expected_field_count = 0;
}

// 2026-09-15 review (review_divergence_check_561efa3b_2026-09-15.md,
// MEDIUM 5): a stale cache (config_crc known to disagree with the live
// Pico's, e.g. right after a Pico reboot) must not hide a real revert on the
// broadened/extra field set. This test proves the fix by constructing the
// exact hazard the audit named: the extra field's cache VALUE still equals
// the expected value (a stale, pre-reboot snapshot that happens to look
// right), but the cache is marked stale -- with the old code that would
// have read as "still agrees"; the fix must report it as unknown/diverged
// instead, since a stale cache proves nothing about what the Pico currently
// holds.
static void test_stale_cache_does_not_hide_a_revert(void)
{
    TEST_SECTION("2026-09-15 fix, MEDIUM 5 (review_divergence_check_561efa3b_2026-09-15.md): a stale "
                 "cache (config_crc known to disagree with the live Pico's, refetch not yet caught up) "
                 "must not be read as still agreeing on the broadened/extra field set -- an unrefreshed "
                 "field is UNKNOWN, not a match, so a real revert during the stale window is never hidden");
    test_reset_all();
    safety_ceiling_sync_set_disable_heat_hooks(fake_all_relays_off, fake_halt_run);
    safety_ceiling_sync_set_expected_pico_fields_source(fake_expected_pico_fields_source);

    // Ceiling agrees throughout -- this fix is scoped to the broadened
    // fields only, never the ceiling field (which keeps its own pre-existing,
    // separately-tracked staleness weakness per the audit's own note).
    s_zone_max_temp_c[0] = 80.0f;
    fake_pico_ceiling_set(80.0f);

    s_expected_fields[0].param_id = FAKE_EXTRA_PARAM_ID;
    s_expected_fields[0].value = 42.0f;
    s_expected_field_count = 1;
    // The cache's VALUE agrees byte-for-byte with what is expected...
    fake_pico_extra_set(42.0f);
    // ...but it is a stale snapshot: safety_cfg_store_maybe_refetch() has
    // seen the live config_crc disagree and has not yet caught up.
    s_cache_stale = true;

    safety_ceiling_sync_reconcile_on_link_up(FAKE_LINK);

    char reason[CONFIG_DIVERGENCE_REASON_MAX];
    bool warned = safety_ceiling_sync_is_standing_diverged(reason, sizeof(reason));
    TEST_CHECK(warned,
               "a value that only APPEARS to agree, read off a cache known to be stale, is reported as "
               "diverged (unknown), not silently accepted as a match -- the pre-fix code would have "
               "reported no warning at all here");
    TEST_CHECK(!safety_ceiling_sync_is_diverged(NULL, 0),
               "still never promoted to the heat-disabling verdict -- HIGH 2 interim scope is unchanged "
               "by this fix");

    // Once the cache is no longer stale (the refetch caught up), the SAME
    // agreeing value clears the warning -- proving the warning above was
    // really about staleness, not some other side effect of this test setup.
    s_cache_stale = false;
    safety_ceiling_sync_reconcile_on_link_up(FAKE_LINK);
    TEST_CHECK(!safety_ceiling_sync_is_standing_diverged(NULL, 0),
               "the identical agreeing value clears the warning once the cache is fresh again");

    s_expected_field_count = 0;
    s_cache_stale = false;
}

// Safety-processor tc_type made settable (2026-09-15). Same generic
// enforce_ceiling_divergence() extra-field path test_broadened_field_
// divergence_detected() above already exercises with an arbitrary param id
// -- this test uses the REAL tc_type param id (0x0105, U8,
// safety_cfg_store.c's CONFIG_PARAM_TABLE row) THROUGH THE ASSERTION and
// names the concrete hazard directly: a Pico that silently reverts to a
// different tc_type than the ESP's saved kiln config expects (e.g. its own
// flash-fallback persist landed a stale value, or a reboot raced a live
// SET_CONFIG before it was durably committed) must not go unnoticed just
// because abs_max_temp_c still agrees.
//
// 2026-09-15 review (F6): the first cut of this test built its fake row
// under FAKE_EXTRA_PARAM_ID (0x0201, F32) and then overwrote its own
// expectation's param_id to match right before the assertion -- so it ran
// through the exact same code path, with the exact same ids, as
// test_broadened_field_divergence_detected() above, and would have passed
// identically even if tc_type (0x0105) were never captured into the
// record at all. fake_pico_extra_set_ex() (added for this fix) lets this
// test report its fake row under the REAL 0x0105/U8 pair, so the
// production comparator's type-aware switch actually decodes a U8 row
// under tc_type's own id -- this test now fails if 0x0105 is ever excluded
// or mis-typed, which is the coverage the review asked for.
#define TC_TYPE_PARAM_ID 0x0105u
static void test_tc_type_revert_divergence_detected(void)
{
    TEST_SECTION("tc_type made settable (2026-09-15): a Pico-side tc_type revert is caught by the standing divergence check");
    test_reset_all();
    safety_ceiling_sync_set_disable_heat_hooks(fake_all_relays_off, fake_halt_run);
    safety_ceiling_sync_set_expected_pico_fields_source(fake_expected_pico_fields_source);

    // abs_max_temp_c agrees -- isolating the assertion to the tc_type field.
    s_zone_max_temp_c[0] = 80.0f;
    fake_pico_ceiling_set(80.0f);

    // The saved kiln config's Pico half expects tc_type == K (3), but the
    // Pico's live GET_CONFIG_PAGE cache reports it reverted to B (0) --
    // exactly the "changed type without telling anyone" scenario a wrong
    // CR1 byte can otherwise leave silently unnoticed downstream of
    // max31856_tc_type_verified() (that flag only catches a failed CR1
    // *write*, not a config the Pico never received in the first place).
    // Both sides now agree on the SAME id (0x0105) and the SAME type (U8).
    s_expected_fields[0].param_id = TC_TYPE_PARAM_ID;
    s_expected_fields[0].value = 3.0f; // MAX31856 type K
    s_expected_field_count = 1;
    fake_pico_extra_set_ex(TC_TYPE_PARAM_ID, KILNLINK_PARAM_TYPE_U8, 0.0f); // MAX31856 type B

    safety_ceiling_sync_reconcile_on_link_up(FAKE_LINK);

    // 2026-09-15 review HIGH 2 interim (review_divergence_check_561efa3b_
    // 2026-09-15.md, landed after this test): tc_type is one of the ~60
    // broadened (non-ceiling) fields, and kiln_cfg_store_apply() does not
    // yet push/confirm that field set to the Pico -- so, same as every
    // other non-ceiling field, a mismatch here is reported as a WARNING,
    // never as heat-off, until a confirmed push path exists. Still fully
    // detected and named -- nothing about this scenario goes unnoticed.
    char reason[CONFIG_DIVERGENCE_REASON_MAX];
    bool warned = safety_ceiling_sync_is_standing_diverged(reason, sizeof(reason));
    TEST_CHECK(warned, "a Pico tc_type that reverted away from the saved kiln config's expected value is reported as a standing warning");
    TEST_CHECK(!safety_ceiling_sync_is_diverged(NULL, 0),
               "a reverted tc_type alone does NOT set the heat-disabling verdict (HIGH 2 interim)");
    TEST_CHECK(s_relays_off_calls == 0, "all-relays-off hook does NOT fire on a reverted tc_type alone");
    TEST_CHECK(s_halt_run_calls == 0, "halt-run hook does NOT fire on a reverted tc_type alone");

    // Once the Pico is reconfigured back to the expected type, the warning clears.
    fake_pico_extra_set_ex(TC_TYPE_PARAM_ID, KILNLINK_PARAM_TYPE_U8, 3.0f); // MAX31856 type K -- now matches
    safety_ceiling_sync_reconcile_on_link_up(FAKE_LINK);
    TEST_CHECK(!safety_ceiling_sync_is_standing_diverged(NULL, 0), "warning clears once the Pico's tc_type matches the saved kiln config again");

    s_expected_field_count = 0;
}

int main(void)
{
    // test_uninstalled_hooks_are_a_safe_noop() MUST run first in this
    // process: safety_ceiling_sync_set_disable_heat_hooks() has no
    // unset/NULL-again entry point (by design -- see its own header
    // comment), so once any other test in this file installs hooks, the
    // "nothing installed yet" state this test verifies can never be
    // observed again in the same run.
    test_uninstalled_hooks_are_a_safe_noop();
    test_divergence_fires_both_hooks();
    test_latch_persists_while_diverged();
    test_latch_clears_when_sides_agree();
    test_target_known_exclusion();
    test_broadened_field_divergence_detected();
    test_tc_type_revert_divergence_detected();
    test_stale_cache_does_not_hide_a_revert();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
