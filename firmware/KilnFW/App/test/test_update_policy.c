// Host tests for App/drivers/update/update_policy.c
// (docs/GITHUB_RELEASE_UPDATE_PLAN.md sections 5-7, owner decision D8).
#include <string.h>

#include "test_common.h"

#include "../drivers/update/update_policy.h"

#define PSHA "1111111111111111111111111111111111111111111111111111111111111111"
#define PSHA2 "2222222222222222222222222222222222222222222222222222222222222222"
#define COMMIT_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define COMMIT_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static update_identity_t ident(const char *ver, const char *commit)
{
    update_identity_t i;
    memset(&i, 0, sizeof(i));
    strcpy(i.version, ver);
    strcpy(i.commit, commit);
    strcpy(i.partitions_sha256, PSHA);
    i.zones_cfg_version = 26;
    i.kilnlink_version = 16;
    i.uart_version = 13;
    return i;
}

static update_decision_t decide(const update_identity_t *r, const update_identity_t *c, bool pre, bool down, bool force)
{
    update_policy_flags_t f = { pre, down, force };
    return update_policy_decide(r, c, &f);
}

static void test_upgrade(void)
{
    TEST_SECTION("update_policy -- ordinary upgrade");
    update_identity_t r = ident("v1.0.0", COMMIT_A), c = ident("v1.0.1", COMMIT_B);
    update_decision_t d = decide(&r, &c, false, false, false);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_ALLOW_UPGRADE && d.allowed && d.needs_confirm, "patch upgrade allowed with confirm");
    TEST_CHECK(!d.needs_typed_confirm && d.semver_cmp == 1 && !d.zones_cfg_lower, "no typed confirm, cmp +1");
    TEST_CHECK(d.reason != NULL && d.reason[0] != '\0', "reason always set");

    c = ident("v2.0.0", COMMIT_B);
    c.zones_cfg_version = 27;
    c.kilnlink_version = 17;
    d = decide(&r, &c, false, false, false);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_ALLOW_UPGRADE && d.schema_newer, "higher schema is a normal upgrade, flagged schema_newer");

    r = ident("v1.0.0-rc.1", COMMIT_A);
    c = ident("v1.0.0", COMMIT_B);
    d = decide(&r, &c, false, false, false);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_ALLOW_UPGRADE, "rc -> release is an upgrade");

    r = ident("1.0.0", COMMIT_A);
    c = ident("v1.0.1", COMMIT_B);
    TEST_CHECK(decide(&r, &c, false, false, false).allowed, "leading v on one side only is fine");
    TEST_CHECK(update_policy_decide(&r, &c, NULL).verdict == UPDATE_VERDICT_ALLOW_UPGRADE, "NULL flags == all false");
}

static void test_downgrade(void)
{
    TEST_SECTION("update_policy -- downgrade refused, override needs typed confirm (D8)");
    update_identity_t r = ident("v1.1.0", COMMIT_A), c = ident("v1.0.0", COMMIT_B);
    update_decision_t d = decide(&r, &c, false, false, false);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE && !d.allowed && !d.needs_confirm, "older semver refused");
    TEST_CHECK(d.semver_cmp == -1, "cmp -1");

    d = decide(&r, &c, false, false, true);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE, "force does NOT override a downgrade");

    d = decide(&r, &c, false, true, false);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_ALLOW_DOWNGRADE && d.allowed && d.needs_confirm && d.needs_typed_confirm,
               "allow_downgrade yields ALLOW_DOWNGRADE with typed confirm");

    // Newer semver but LOWER zones_cfg_version: still a downgrade (rollback hazard).
    r = ident("v1.0.0", COMMIT_A);
    c = ident("v2.0.0", COMMIT_B);
    c.zones_cfg_version = 25;
    d = decide(&r, &c, false, false, false);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE && d.zones_cfg_lower, "lower zones_cfg_version refused even with newer semver");
    d = decide(&r, &c, false, true, false);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_ALLOW_DOWNGRADE && d.zones_cfg_lower && d.needs_typed_confirm,
               "override reports zones_cfg_lower for the rollback notice");

    // Lower kilnlink or uart alone is also a downgrade, but not a zones hazard.
    c = ident("v2.0.0", COMMIT_B);
    c.kilnlink_version = 15;
    d = decide(&r, &c, false, false, false);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE && !d.zones_cfg_lower, "lower kilnlink refused");
    c = ident("v2.0.0", COMMIT_B);
    c.uart_version = 12;
    TEST_CHECK(decide(&r, &c, false, false, false).verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE, "lower uart refused");

    // Mixed: one schema up, one down -> downgrade.
    c = ident("v2.0.0", COMMIT_B);
    c.zones_cfg_version = 27;
    c.uart_version = 12;
    d = decide(&r, &c, false, false, false);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE && d.schema_newer, "mixed schema direction counts as downgrade");

    // Same semver, lower schema: still a downgrade, not 'up to date'.
    c = ident("v1.0.0", COMMIT_A);
    c.zones_cfg_version = 25;
    TEST_CHECK(decide(&r, &c, false, false, true).verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE,
               "same version with lower schema is refused, force or not");

    // Release -> its own prerelease is a downgrade.
    r = ident("v1.0.0", COMMIT_A);
    c = ident("v1.0.0-rc.1", COMMIT_B);
    TEST_CHECK(decide(&r, &c, true, false, false).verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE, "release -> own prerelease refused");
}

static void test_same_version(void)
{
    TEST_SECTION("update_policy -- same version / reinstall / unknown running version");
    update_identity_t r = ident("v1.0.0", COMMIT_A), c = ident("v1.0.0", COMMIT_A);
    update_decision_t d = decide(&r, &c, false, false, false);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_UP_TO_DATE && !d.allowed && d.semver_cmp == 0, "same version+commit is up to date");
    d = decide(&r, &c, false, false, true);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_ALLOW_REINSTALL && d.allowed && d.needs_confirm && !d.needs_typed_confirm,
               "force allows reinstall");

    c = ident("v1.0.0+build7", COMMIT_B);
    d = decide(&r, &c, false, false, false);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_NEEDS_FORCE && !d.allowed, "same version, different commit needs force");
    TEST_CHECK(decide(&r, &c, false, false, true).verdict == UPDATE_VERDICT_ALLOW_REINSTALL, "...and force allows it");

    c = ident("v1.0.0", "");
    TEST_CHECK(decide(&r, &c, false, false, false).verdict == UPDATE_VERDICT_REFUSE_NEEDS_FORCE,
               "candidate with no commit cannot prove it is the same image");

    // Running version unknown (dev build).
    r = ident("", COMMIT_A);
    c = ident("v1.0.0", COMMIT_B);
    d = decide(&r, &c, false, false, false);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_NEEDS_FORCE && d.semver_cmp == UPDATE_SEMVER_CMP_UNKNOWN,
               "unversioned running image needs force");
    TEST_CHECK(decide(&r, &c, false, false, true).verdict == UPDATE_VERDICT_ALLOW_REINSTALL, "force ok on unversioned board");
    r = ident("dev-3a243fea", COMMIT_A);
    TEST_CHECK(decide(&r, &c, false, false, false).verdict == UPDATE_VERDICT_REFUSE_NEEDS_FORCE, "non-semver running version same as unknown");
    c.zones_cfg_version = 25;
    TEST_CHECK(decide(&r, &c, false, false, true).verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE,
               "schema rules still apply to an unversioned board even with force");
}

static void test_hard_refusals(void)
{
    TEST_SECTION("update_policy -- non-overridable refusals");
    update_identity_t r = ident("v1.0.0", COMMIT_A), c = ident("v1.1.0", COMMIT_B);
    update_decision_t d;

    c.dirty = true;
    d = decide(&r, &c, true, true, true);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_DIRTY && !d.allowed, "dirty candidate refused even with every override");

    c = ident("v1.1.0", COMMIT_B);
    strcpy(c.partitions_sha256, PSHA2);
    d = decide(&r, &c, true, true, true);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_PARTITIONS && !d.allowed, "partition hash mismatch refused even with every override");

    c = ident("v1.1.0", COMMIT_B);
    r.partitions_sha256[0] = '\0';
    TEST_CHECK(decide(&r, &c, true, true, true).verdict == UPDATE_VERDICT_REFUSE_PARTITIONS, "board hash unknown refused");
    r = ident("v1.0.0", COMMIT_A);

    c = ident("v1.1.0-rc.1", COMMIT_B);
    TEST_CHECK(decide(&r, &c, false, false, false).verdict == UPDATE_VERDICT_REFUSE_PRERELEASE, "prerelease refused when channel off");
    d = decide(&r, &c, true, false, false);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_ALLOW_UPGRADE, "prerelease allowed when channel on");

    c = ident("v1.1.0", COMMIT_B);
    strcpy(c.min_updatable_from, "v1.0.5");
    d = decide(&r, &c, false, true, true);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_TOO_OLD && !d.allowed, "below min_updatable_from refused even with overrides");
    strcpy(c.min_updatable_from, "v1.0.0");
    TEST_CHECK(decide(&r, &c, false, false, false).verdict == UPDATE_VERDICT_ALLOW_UPGRADE, "running == floor is allowed");
    strcpy(c.min_updatable_from, "v0.9.0");
    TEST_CHECK(decide(&r, &c, false, false, false).allowed, "running above floor allowed");
    strcpy(c.min_updatable_from, "v1.0.0-rc.1");
    TEST_CHECK(decide(&r, &c, false, false, false).allowed, "floor prerelease below running release allowed");
    strcpy(c.min_updatable_from, "v1.0.5");
    r = ident("", COMMIT_A);
    TEST_CHECK(decide(&r, &c, false, false, false).verdict == UPDATE_VERDICT_REFUSE_TOO_OLD, "unknown running version + floor refused without force");
    TEST_CHECK(decide(&r, &c, false, false, true).verdict == UPDATE_VERDICT_ALLOW_REINSTALL, "...allowed with force");
}

static void test_malformed(void)
{
    TEST_SECTION("update_policy -- malformed candidate / missing data");
    update_identity_t r = ident("v1.0.0", COMMIT_A), c;
    update_policy_flags_t f = { true, true, true };

    TEST_CHECK(update_policy_decide(NULL, &r, &f).verdict == UPDATE_VERDICT_REFUSE_MALFORMED, "NULL running");
    TEST_CHECK(update_policy_decide(&r, NULL, &f).verdict == UPDATE_VERDICT_REFUSE_MALFORMED, "NULL candidate");

#define MAL(MUTATE, MSG)                                                                                               \
    do {                                                                                                               \
        c = ident("v1.1.0", COMMIT_B);                                                                                 \
        MUTATE;                                                                                                        \
        TEST_CHECK(update_policy_decide(&r, &c, &f).verdict == UPDATE_VERDICT_REFUSE_MALFORMED, MSG);                  \
    } while (0)
    MAL(c.version[0] = '\0', "empty candidate version");
    MAL(strcpy(c.version, "latest"), "non-semver candidate version");
    MAL(strcpy(c.version, "1.0"), "two-part candidate version");
    MAL(c.partitions_sha256[0] = '\0', "missing partitions_sha256");
    MAL(c.partitions_sha256[10] = 'G', "non-hex partitions_sha256");
    MAL(c.partitions_sha256[0] = 'A', "uppercase partitions_sha256");
    MAL(c.partitions_sha256[63] = '\0', "short partitions_sha256");
    MAL(strcpy(c.commit, "abc123"), "short commit");
    MAL(c.commit[3] = 'Q', "non-hex commit");
    MAL(c.zones_cfg_version = 0, "zones_cfg_version 0");
    MAL(c.kilnlink_version = 0, "kilnlink_version 0");
    MAL(c.uart_version = 0, "uart_version 0");
    MAL(strcpy(c.min_updatable_from, "oldest"), "garbage min_updatable_from");
#undef MAL
    c = ident("v1.1.0", COMMIT_B);
    r.zones_cfg_version = 0;
    TEST_CHECK(update_policy_decide(&r, &c, &f).verdict == UPDATE_VERDICT_REFUSE_MALFORMED, "running schema missing");

    // A candidate version field with no NUL inside the buffer must not overrun.
    r = ident("v1.0.0", COMMIT_A);
    c = ident("v1.1.0", COMMIT_B);
    memset(c.version, '1', sizeof(c.version));
    TEST_CHECK(update_policy_decide(&r, &c, &f).verdict == UPDATE_VERDICT_REFUSE_MALFORMED, "unterminated version field");
}

static void test_helpers(void)
{
    TEST_SECTION("update_policy -- helpers");
    TEST_CHECK(update_policy_commit_valid(COMMIT_A) && !update_policy_commit_valid("") && !update_policy_commit_valid(NULL) &&
                   !update_policy_commit_valid(COMMIT_A "a"),
               "commit validator");
    TEST_CHECK(update_policy_sha256_hex_valid(PSHA) && !update_policy_sha256_hex_valid(COMMIT_A), "sha256 validator");
    TEST_CHECK(strcmp(update_verdict_name(UPDATE_VERDICT_REFUSE_DOWNGRADE), "refuse_downgrade") == 0, "verdict name");
    TEST_CHECK(strcmp(update_verdict_name(UPDATE_VERDICT_REFUSE_NEEDS_FORCE), "refuse_needs_force") == 0, "needs_force name");

    // allowed <=> needs_confirm, and every ALLOW_* verdict is allowed, no REFUSE is.
    for (int v = 0; v <= UPDATE_VERDICT_REFUSE_NEEDS_FORCE; v++) {
        TEST_CHECK(strcmp(update_verdict_name((update_verdict_t)v), "unknown") != 0, "every verdict has a name");
    }
}

static void test_unknown_running_force_needs_typed(void)
{
    TEST_SECTION("update_policy -- unknown running version: force alone is not enough");
    update_identity_t r = ident("", COMMIT_A), c = ident("v1.0.0", COMMIT_B);
    update_policy_flags_t f = { false, false, true };
    update_decision_t d = update_policy_decide_typed(&r, &c, &f, false);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_NEEDS_FORCE && !d.allowed && d.needs_typed_confirm,
               "unknown running + force without typed confirm refused");
    d = update_policy_decide_typed(&r, &c, &f, true);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_ALLOW_REINSTALL && d.allowed, "force + typed confirm allowed");
    f.force = false;
    d = update_policy_decide_typed(&r, &c, &f, true);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_NEEDS_FORCE && !d.allowed, "typed confirm without force still needs force");
    r = ident("dev-3a243fea", COMMIT_A);
    f.force = true;
    TEST_CHECK(!update_policy_decide_typed(&r, &c, &f, false).allowed, "non-semver running treated as unknown");
    r = ident("v1.0.0", COMMIT_A);
    c = ident("v1.0.0", COMMIT_B);
    TEST_CHECK(update_policy_decide_typed(&r, &c, &f, false).verdict == UPDATE_VERDICT_ALLOW_REINSTALL,
               "known running version: force keeps its ordinary meaning");
    r = ident("", COMMIT_A);
    c = ident("v1.0.0", COMMIT_B);
    c.zones_cfg_version = 25;
    f.allow_downgrade = true;
    TEST_CHECK(update_policy_decide_typed(&r, &c, &f, true).verdict == UPDATE_VERDICT_ALLOW_DOWNGRADE,
               "explicit typed downgrade unchanged");
    TEST_CHECK(update_policy_decide_typed(NULL, &c, &f, false).verdict == UPDATE_VERDICT_REFUSE_MALFORMED, "NULL running");
}

void run_test_update_policy(void)
{
    test_upgrade();
    test_downgrade();
    test_same_version();
    test_hard_refusals();
    test_malformed();
    test_helpers();
    test_unknown_running_force_needs_typed();
}
