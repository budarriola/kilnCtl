// test_tc_type_reapply_policy.c -- runtime host test for the REAL
// tc_type_reapply_policy_should_reapply() (tasks/tc_type_reapply_policy.c),
// added per the 2026-09-15 Opus review (F5) of the tc_type-settable feature:
// the original commit's only test exercised the stack-budget script, and
// thermo_task.c/link_task.c (where the reapply actually happens) are not
// linked into any host test, so a negative test against them could never
// fail. This links the real production tc_type_reapply_policy.c -- the
// single function link_task.c's three config-write handlers (SET_CONFIG,
// COMMIT_CONFIG, APPLY_CONFIG_VOLATILE) all call to decide whether to
// request a live reapply -- not a test-local reimplementation.
//
// NEGATIVE TEST (2026-09-15, run in a clean C:\wt\ worktree, forced full
// rebuild after restore per this repo's own negative-test discipline):
// changed tc_type_reapply_policy_should_reapply()'s body to
// `(void)prev_tc_type; (void)new_tc_type; return false;` unconditionally
// (the `(void)` casts were required only to keep MSVC's unreferenced-
// parameter warning, which this build treats as an error, from failing the
// build for the wrong reason -- a compile error instead of a runtime test
// failure). Result: the build linked and ran, and three checks below FAILED
// at runtime ("a real tc_type change (K -> S) requests a reapply", "a
// tc_type change spanning the full MAX31856_TC_TYPE_* range requests a
// reapply", "a tc_type change is detected regardless of direction"),
// dropping the suite to 2517/2520 -- proving this is a real runtime
// assertion against production logic, not a compile-only guard. Restored
// by hand (never `git checkout --`), confirmed an empty `git diff` against
// the main tree's copy of this file, then rebuilt from a completely fresh
// output directory (2520/2520, all passed) before trusting the green
// result again.
#include <stdio.h>

#include "test_common.h"
#include "tasks/tc_type_reapply_policy.h"

void run_test_tc_type_reapply_policy(void)
{
    TEST_SECTION("tc_type_reapply_policy_should_reapply(): the real decision link_task.c's "
                 "three config-write handlers (SET_CONFIG/COMMIT_CONFIG/APPLY_CONFIG_VOLATILE) all gate on");

    TEST_CHECK(!tc_type_reapply_policy_should_reapply(3u, 3u),
               "an unchanged tc_type (K -> K) does not request a reapply");
    TEST_CHECK(!tc_type_reapply_policy_should_reapply(0u, 0u),
               "an unchanged tc_type at the low end of the range (B -> B) does not request a reapply");

    TEST_CHECK(tc_type_reapply_policy_should_reapply(3u, 5u),
               "a real tc_type change (K -> S) requests a reapply");
    TEST_CHECK(tc_type_reapply_policy_should_reapply(0u, 7u),
               "a tc_type change spanning the full MAX31856_TC_TYPE_* range requests a reapply");
    TEST_CHECK(tc_type_reapply_policy_should_reapply(7u, 0u),
               "a tc_type change is detected regardless of direction (loosening or not is not this "
               "function's job -- config_store.c's own ARMED carve-out handles that separately)");
}
