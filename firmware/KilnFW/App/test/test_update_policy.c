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

// zones == 0: the image's record carries the running board's schema; else its zones_cfg is `zones`.
static update_decision_t up_ex(const update_identity_t *r, const char *ver, const char *commit, uint32_t zones,
                               bool have_id, uint32_t hdr_zones, bool force, bool down, const char *confirm)
{
    update_upload_request_t q;
    memset(&q, 0, sizeof(q));
    q.version = ver;
    q.commit = commit;
    q.have_image_id = have_id;
    if (r != NULL) {
        update_image_id_make(&q.image_id, zones ? zones : r->zones_cfg_version, r->kilnlink_version, r->uart_version, "abc1234");
    }
    q.zones_cfg_version = hdr_zones;
    q.force = force;
    q.allow_downgrade = down;
    q.confirm = confirm;
    return update_policy_decide_upload(r, &q);
}

static update_decision_t up(const update_identity_t *r, const char *ver, const char *commit, uint32_t zones, bool force,
                            bool down, const char *confirm)
{
    return up_ex(r, ver, commit, zones, true, 0, force, down, confirm);
}

static void test_upload_gate(void)
{
    TEST_SECTION("update_policy -- hand upload (docs/GITHUB_RELEASE_UPDATE_PLAN.md section 6)");
    update_identity_t r = ident("v1.2.0", COMMIT_A);
    update_decision_t d = up(&r, "1.3.0", COMMIT_B, 0, false, false, NULL);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_ALLOW_UPGRADE && d.allowed, "newer upload allowed");
    d = up(&r, "1.2.0-rc.1", NULL, 0, false, false, NULL);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE && !d.allowed && d.needs_typed_confirm,
               "prerelease of the running version is older: refused");
    d = up(&r, "1.3.0-rc.1", NULL, 0, false, false, NULL);
    TEST_CHECK(d.allowed, "a newer prerelease is allowed (no channel on a hand upload)");

    d = up(&r, "1.2.0", COMMIT_A, 0, false, false, NULL);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_UP_TO_DATE && !d.allowed, "same version + commit without force refused");
    d = up(&r, "v1.2.0", COMMIT_A, 0, true, false, NULL);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_ALLOW_REINSTALL && d.allowed, "same commit with force allowed");
    d = up(&r, "1.2.0", NULL, 0, false, false, NULL);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_NEEDS_FORCE && !d.allowed, "same version, no commit: needs force");

    d = up(&r, "1.1.0", NULL, 0, false, false, NULL);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE && !d.allowed && strstr(d.reason, "downgrade") != NULL,
               "older version refused, reason names the downgrade");
    d = up(&r, "1.1.0", NULL, 0, true, false, "1.1.0");
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE, "force alone does not override a downgrade");
    d = up(&r, "1.1.0", NULL, 0, false, true, NULL);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE && d.needs_typed_confirm,
               "allow_downgrade without the typed confirm refused");
    d = up(&r, "1.1.0", NULL, 0, false, true, "1.0.9");
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE, "wrong typed confirm refused");
    d = up(&r, "1.1.0", NULL, 0, false, false, "1.1.0");
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE, "typed confirm without allow_downgrade refused");
    d = up(&r, "1.1.0", NULL, 0, false, true, "1.1.0");
    TEST_CHECK(d.verdict == UPDATE_VERDICT_ALLOW_DOWNGRADE && d.allowed && d.needs_typed_confirm,
               "allow_downgrade + typed confirm allowed");
    d = up(&r, "v1.1.0", NULL, 0, false, true, "1.1.0");
    TEST_CHECK(d.verdict == UPDATE_VERDICT_ALLOW_DOWNGRADE, "leading v is ignored in the typed confirm");

    d = up(&r, "1.3.0", NULL, 25, false, false, NULL);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE && d.zones_cfg_lower,
               "lower zones_cfg schema refused even if newer");
    d = up(&r, "1.3.0", NULL, 25, false, true, "1.3.0");
    TEST_CHECK(d.verdict == UPDATE_VERDICT_ALLOW_DOWNGRADE && d.zones_cfg_lower,
               "lower schema overridden with typed confirm, flagged");
    d = up(&r, "1.3.0", NULL, 27, false, false, NULL);
    TEST_CHECK(d.allowed && d.schema_newer, "higher schema is a normal upgrade");

    d = up(&r, "", NULL, 0, true, true, "unversioned");
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_MALFORMED, "no version is malformed (the stager never passes one)");
    d = up(&r, NULL, NULL, 0, true, false, NULL);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_MALFORMED, "NULL version malformed");

    update_identity_t dev = ident("", COMMIT_A);
    d = up(&dev, "1.0.0", NULL, 0, false, false, NULL);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_NEEDS_FORCE, "dev running build: needs force");
    d = up(&dev, "1.0.0", NULL, 0, true, false, NULL);
    TEST_CHECK(!d.allowed && d.needs_typed_confirm, "dev running build: force without typed confirm refused");
    d = up(&dev, "1.0.0", NULL, 0, true, false, "1.0.0");
    TEST_CHECK(d.allowed, "dev running build: force + typed confirm allowed");

    update_identity_t nop = ident("v1.2.0", COMMIT_A);
    nop.partitions_sha256[0] = '\0';
    TEST_CHECK(up(&nop, "1.3.0", NULL, 0, false, false, NULL).allowed, "empty board partition hash does not block an upload");
    TEST_CHECK(up(NULL, "1.3.0", NULL, 0, false, false, NULL).verdict == UPDATE_VERDICT_REFUSE_MALFORMED, "NULL running");
    TEST_CHECK(up(&r, "not-a-version", NULL, 0, false, false, NULL).verdict == UPDATE_VERDICT_REFUSE_MALFORMED,
               "garbage version malformed");
}

static void test_upload_identity_record(void)
{
    TEST_SECTION("update_policy -- embedded identity record (F1), headers advisory, typed-confirm flag (F5), helper (F7)");
    update_identity_t r = ident("v1.2.0", COMMIT_A);
    update_decision_t d;
    // F1: a lower embedded zones_cfg_version is a refused downgrade even though the version is newer and no header was sent.
    d = up_ex(&r, "1.3.0", NULL, 25, true, 0, false, false, NULL);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE && d.zones_cfg_lower && d.needs_typed_confirm,
               "F1: lower embedded schema refused with no header at all");
    d = up_ex(&r, "1.3.0", NULL, 25, true, 0, true, true, "1.3.0");
    TEST_CHECK(d.allowed && d.zones_cfg_lower, "F1: lower embedded schema overridden by allow_downgrade + typed confirm");
    // Missing record: unknown schema needs force AND the typed confirm.
    d = up_ex(&r, "1.3.0", NULL, 0, false, 0, false, false, NULL);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_NEEDS_FORCE && !d.allowed && d.needs_typed_confirm,
               "F1: no record, no overrides: refused, typed confirm flagged");
    d = up_ex(&r, "1.3.0", NULL, 0, false, 0, true, false, NULL);
    TEST_CHECK(!d.allowed && d.needs_typed_confirm, "F1: no record, force only: refused");
    d = up_ex(&r, "1.3.0", NULL, 0, false, 0, false, false, "1.3.0");
    TEST_CHECK(!d.allowed, "F1: no record, confirm only: refused");
    d = up_ex(&r, "1.3.0", NULL, 0, false, 0, true, false, "v1.3.0");
    TEST_CHECK(d.allowed, "F1: no record + force + typed confirm: allowed");
    d = up_ex(&r, "1.1.0", NULL, 0, false, 0, true, false, "1.1.0");
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE, "F1: no record + force + confirm still refuses an older version");
    // Headers are advisory but must agree with the record.
    d = up_ex(&r, "1.3.0", NULL, 0, true, r.zones_cfg_version, false, false, NULL);
    TEST_CHECK(d.allowed, "header equal to the record: fine");
    d = up_ex(&r, "1.3.0", NULL, 0, true, r.zones_cfg_version + 1, false, false, NULL);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_MALFORMED && !d.allowed, "header disagreeing with the record refused");
    d = up_ex(&r, "1.3.0", NULL, 25, true, r.zones_cfg_version, false, false, NULL);
    TEST_CHECK(!d.allowed, "header claiming the board's schema cannot mask a lower embedded one");
    // Record codec.
    update_image_id_t id, got;
    update_image_id_make(&id, 26, 16, 13, "abc1234");
    uint8_t buf[64];
    memset(buf, 0xEE, sizeof(buf));
    memcpy(buf + 8, &id, sizeof(id)); // host is little-endian
    TEST_CHECK(update_image_id_find(buf, sizeof(buf), 0, &got) && got.zones_cfg_version == 26 && got.kilnlink_version == 16 &&
                   got.uart_version == 13,
               "record found at a 4-byte aligned offset");
    TEST_CHECK(strcmp(got.commit, "abc1234") == 0, "M1: the record carries the build commit");
    TEST_CHECK(UPDATE_IMAGE_ID_SIZE == sizeof(update_image_id_t), "M1: UPDATE_IMAGE_ID_SIZE matches the struct");
    TEST_CHECK(!update_image_id_find(buf, sizeof(buf), 12, &got), "scan start past the record finds nothing");
    buf[12] ^= 1;
    TEST_CHECK(!update_image_id_find(buf, sizeof(buf), 0, &got), "a record with a bad check word is ignored");
    buf[12] ^= 1;
    memset(buf + 12, 0, 4);
    update_image_id_make(&id, 0, 16, 13, "abc1234");
    memcpy(buf + 8, &id, sizeof(id));
    TEST_CHECK(!update_image_id_find(buf, sizeof(buf), 0, &got), "a record with a zero version is ignored");

    // F5: needs_typed_confirm wherever force alone will not pass; false where it will.
    update_identity_t dev = ident("", COMMIT_A);
    d = up(&dev, "1.0.0", NULL, 0, false, false, NULL);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_NEEDS_FORCE && d.needs_typed_confirm,
               "F5: unknown running version refused without force still flags the typed confirm");
    d = up(&r, "1.2.0", NULL, 0, false, false, NULL);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_NEEDS_FORCE && !d.needs_typed_confirm,
               "F5: same version, unknown commit: force alone passes, so no typed confirm");
    TEST_CHECK(strstr(d.reason, "commit unknown") != NULL && strstr(d.reason, "different commit") == NULL,
               "F5: unknown commit is not reported as a different commit");
    d = up(&r, "1.2.0", COMMIT_B, 0, false, false, NULL);
    TEST_CHECK(strstr(d.reason, "different commit") != NULL, "F5: a known other commit is reported as different");
    d = up(&dev, "1.0.0", NULL, 0, true, false, NULL);
    TEST_CHECK(strstr(d.reason, "tag") == NULL && strstr(d.reason, "version") != NULL, "F5: upload path says version, not tag");
    d = up(&r, "1.2.0", COMMIT_A, 0, false, false, NULL);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_UP_TO_DATE && !d.needs_typed_confirm, "F5: up-to-date: no typed confirm");

    // F6: strict header number parse.
    uint32_t hv = 99;
    TEST_CHECK(update_policy_parse_hdr_u32("26", &hv) && hv == 26, "hdr u32: plain number");
    TEST_CHECK(!update_policy_parse_hdr_u32("26x", &hv) && hv == 0, "hdr u32: trailing junk rejected");
    TEST_CHECK(!update_policy_parse_hdr_u32("abc", &hv), "hdr u32: letters rejected, not read as 0");
    TEST_CHECK(!update_policy_parse_hdr_u32("-1", &hv), "hdr u32: sign rejected");
    TEST_CHECK(!update_policy_parse_hdr_u32(" 5", &hv), "hdr u32: leading space rejected");
    TEST_CHECK(!update_policy_parse_hdr_u32("", &hv), "hdr u32: empty rejected");
    TEST_CHECK(!update_policy_parse_hdr_u32("0", &hv), "hdr u32: zero rejected");
    TEST_CHECK(!update_policy_parse_hdr_u32("4294967295", &hv), "hdr u32: out of range rejected");
    TEST_CHECK(!update_policy_parse_hdr_u32("99999999999999999999", &hv), "hdr u32: overflow rejected");
    TEST_CHECK(!update_policy_parse_hdr_u32(NULL, &hv), "hdr u32: NULL rejected");

    // F7: one typed-confirm rule.
    TEST_CHECK(update_policy_typed_confirm_ok("1.2.3", "1.2.3"), "typed: equal");
    TEST_CHECK(update_policy_typed_confirm_ok("v1.2.3", "1.2.3"), "typed: leading v on confirm");
    TEST_CHECK(update_policy_typed_confirm_ok("1.2.3", "v1.2.3"), "typed: leading v on version");
    TEST_CHECK(!update_policy_typed_confirm_ok("vv1.2.3", "1.2.3"), "typed: only one v stripped");
    TEST_CHECK(!update_policy_typed_confirm_ok("", ""), "typed: empty never matches");
    TEST_CHECK(!update_policy_typed_confirm_ok("v", "v"), "typed: bare v never matches");
    TEST_CHECK(!update_policy_typed_confirm_ok(NULL, "1.2.3"), "typed: NULL confirm");
    TEST_CHECK(!update_policy_typed_confirm_ok("1.2.4", "1.2.3"), "typed: different");
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
    test_upload_gate();
    test_upload_identity_record();
}
