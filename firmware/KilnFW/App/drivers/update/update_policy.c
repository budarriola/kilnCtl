// update_policy.c -- see update_policy.h for the rule order.
#include "update_policy.h"

#include <string.h>

#include "update_semver.h"

static bool is_lower_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

static bool hex_valid(const char *s, size_t want)
{
    if (s == NULL) {
        return false;
    }
    size_t n = 0;
    while (n <= want && s[n] != '\0') {
        if (!is_lower_hex(s[n])) {
            return false;
        }
        n++;
    }
    return n == want;
}

bool update_policy_commit_valid(const char *s) { return hex_valid(s, UPDATE_COMMIT_HEX_LEN); }
bool update_policy_sha256_hex_valid(const char *s) { return hex_valid(s, UPDATE_PARTITIONS_SHA_HEX_LEN); }

// Bounded strlen over a fixed-size field; returns max if no NUL found.
static size_t flen(const char *f, size_t max)
{
    size_t n = 0;
    while (n < max && f[n] != '\0') {
        n++;
    }
    return n;
}

// Parse a version field; *present is false for an empty field.
static bool parse_field(const char *f, update_semver_t *out, bool *present)
{
    size_t n = flen(f, UPDATE_VERSION_STR_MAX);
    *present = n != 0;
    if (n == 0 || n >= UPDATE_VERSION_STR_MAX) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return update_semver_parse_n(f, n, out);
}

static update_decision_t make(update_verdict_t v, bool allowed, const char *reason)
{
    update_decision_t d;
    memset(&d, 0, sizeof(d));
    d.verdict = v;
    d.allowed = allowed;
    d.needs_confirm = allowed;
    d.semver_cmp = UPDATE_SEMVER_CMP_UNKNOWN;
    d.reason = reason;
    return d;
}

update_decision_t update_policy_decide(const update_identity_t *running, const update_identity_t *candidate,
                                       const update_policy_flags_t *flags)
{
    update_policy_flags_t none;
    memset(&none, 0, sizeof(none));
    if (flags == NULL) {
        flags = &none;
    }
    if (running == NULL || candidate == NULL) {
        return make(UPDATE_VERDICT_REFUSE_MALFORMED, false, "missing version information");
    }

    // 1. candidate identity must be complete.
    update_semver_t cand;
    bool cand_present;
    if (!parse_field(candidate->version, &cand, &cand_present)) {
        return make(UPDATE_VERDICT_REFUSE_MALFORMED, false, "candidate version is not a valid semver");
    }
    if (!update_policy_sha256_hex_valid(candidate->partitions_sha256)) {
        return make(UPDATE_VERDICT_REFUSE_MALFORMED, false, "candidate partitions_sha256 missing or malformed");
    }
    if (candidate->commit[0] != '\0' && !update_policy_commit_valid(candidate->commit)) {
        return make(UPDATE_VERDICT_REFUSE_MALFORMED, false, "candidate commit is not 40 lowercase hex");
    }
    if (candidate->zones_cfg_version == 0 || candidate->kilnlink_version == 0 || candidate->uart_version == 0 ||
        running->zones_cfg_version == 0 || running->kilnlink_version == 0 || running->uart_version == 0) {
        return make(UPDATE_VERDICT_REFUSE_MALFORMED, false, "schema versions missing");
    }
    update_semver_t floor_v;
    bool floor_present;
    bool floor_ok = parse_field(candidate->min_updatable_from, &floor_v, &floor_present);
    if (floor_present && !floor_ok) {
        return make(UPDATE_VERDICT_REFUSE_MALFORMED, false, "candidate min_updatable_from is not a valid semver");
    }

    // 2. never install a dirty-tree build.
    if (candidate->dirty) {
        return make(UPDATE_VERDICT_REFUSE_DIRTY, false, "candidate was built from a dirty tree");
    }

    // 3. flash-map compatibility; not overridable.
    if (!update_policy_sha256_hex_valid(running->partitions_sha256)) {
        return make(UPDATE_VERDICT_REFUSE_PARTITIONS, false, "board partition table hash unknown");
    }
    if (strcmp(running->partitions_sha256, candidate->partitions_sha256) != 0) {
        return make(UPDATE_VERDICT_REFUSE_PARTITIONS, false,
                    "candidate expects a different partition table; reflash over JTAG");
    }

    // 4. prerelease channel.
    if (cand.has_pre && !flags->allow_prerelease) {
        return make(UPDATE_VERDICT_REFUSE_PRERELEASE, false, "candidate is a prerelease and prereleases are off");
    }

    // Running version (may be unknown on a dev build).
    update_semver_t run;
    bool run_present;
    bool run_ok = parse_field(running->version, &run, &run_present);

    // 5. updatable-from floor.
    if (floor_present) {
        if (!run_ok) {
            if (!flags->force) {
                return make(UPDATE_VERDICT_REFUSE_TOO_OLD, false,
                            "running version unknown; cannot check min_updatable_from");
            }
        } else if (update_semver_compare(&run, &floor_v) < 0) {
            return make(UPDATE_VERDICT_REFUSE_TOO_OLD, false,
                        "running version is older than min_updatable_from; reflash over JTAG");
        }
    }

    int cmp = run_ok ? update_semver_compare(&cand, &run) : UPDATE_SEMVER_CMP_UNKNOWN;
    bool zones_lower = candidate->zones_cfg_version < running->zones_cfg_version;
    bool schema_lower = zones_lower || candidate->kilnlink_version < running->kilnlink_version ||
                        candidate->uart_version < running->uart_version;
    bool schema_newer = candidate->zones_cfg_version > running->zones_cfg_version ||
                        candidate->kilnlink_version > running->kilnlink_version ||
                        candidate->uart_version > running->uart_version;

    update_decision_t d;
    // 6. downgrade.
    if (schema_lower || (run_ok && cmp < 0)) {
        if (flags->allow_downgrade) {
            d = make(UPDATE_VERDICT_ALLOW_DOWNGRADE, true,
                     "downgrade allowed by override; read back zones before heating");
            d.needs_typed_confirm = true;
        } else {
            d = make(UPDATE_VERDICT_REFUSE_DOWNGRADE, false,
                     schema_lower ? "candidate has a lower schema version than the board; downgrade refused"
                                  : "candidate is older than the running version; downgrade refused");
        }
        d.zones_cfg_lower = zones_lower;
        d.schema_newer = schema_newer;
        d.semver_cmp = cmp;
        return d;
    }

    // 7./8. same version, or version unknown.
    bool same_commit = candidate->commit[0] != '\0' && strcmp(candidate->commit, running->commit) == 0;
    if (run_ok && cmp == 0) {
        if (same_commit) {
            d = flags->force ? make(UPDATE_VERDICT_ALLOW_REINSTALL, true, "reinstalling the running image (forced)")
                             : make(UPDATE_VERDICT_UP_TO_DATE, false, "already up to date");
        } else {
            d = flags->force ? make(UPDATE_VERDICT_ALLOW_REINSTALL, true, "same version, different commit (forced)")
                             : make(UPDATE_VERDICT_REFUSE_NEEDS_FORCE, false,
                                    "same version, different commit; force required");
        }
    } else if (!run_ok) {
        d = flags->force ? make(UPDATE_VERDICT_ALLOW_REINSTALL, true, "running version unknown (forced)")
                         : make(UPDATE_VERDICT_REFUSE_NEEDS_FORCE, false, "running version unknown; force required");
    } else {
        d = make(UPDATE_VERDICT_ALLOW_UPGRADE, true, "newer version available");
    }
    d.zones_cfg_lower = false;
    d.schema_newer = schema_newer;
    d.semver_cmp = cmp;
    return d;
}

// Fetch-path rule on top of update_policy_decide(): when the running version is unknown (a dev
// build, FW_RELEASE_VERSION ""), force=1 alone must not silently install an arbitrary release,
// which may be a downgrade the policy cannot see. It also needs the typed confirm (the caller's
// confirm_downgrade equal to the release tag), exactly as an explicit downgrade does.
update_decision_t update_policy_decide_typed(const update_identity_t *running, const update_identity_t *candidate,
                                             const update_policy_flags_t *flags, bool typed_confirm_ok)
{
    update_decision_t d = update_policy_decide(running, candidate, flags);
    if (running == NULL || flags == NULL || !flags->force || typed_confirm_ok || !d.allowed ||
        d.verdict != UPDATE_VERDICT_ALLOW_REINSTALL) {
        return d;
    }
    update_semver_t run;
    bool present;
    if (parse_field(running->version, &run, &present)) {
        return d; // version known: force keeps its ordinary same-version meaning
    }
    update_decision_t r = make(UPDATE_VERDICT_REFUSE_NEEDS_FORCE, false,
                               "running version unknown; force also needs confirm_downgrade equal to the tag");
    r.needs_typed_confirm = true;
    r.semver_cmp = d.semver_cmp;
    r.schema_newer = d.schema_newer;
    return r;
}

const char *update_verdict_name(update_verdict_t v)
{
    switch (v) {
    case UPDATE_VERDICT_ALLOW_UPGRADE: return "allow_upgrade";
    case UPDATE_VERDICT_ALLOW_REINSTALL: return "allow_reinstall";
    case UPDATE_VERDICT_ALLOW_DOWNGRADE: return "allow_downgrade";
    case UPDATE_VERDICT_UP_TO_DATE: return "up_to_date";
    case UPDATE_VERDICT_REFUSE_DOWNGRADE: return "refuse_downgrade";
    case UPDATE_VERDICT_REFUSE_PARTITIONS: return "refuse_partitions";
    case UPDATE_VERDICT_REFUSE_TOO_OLD: return "refuse_too_old";
    case UPDATE_VERDICT_REFUSE_DIRTY: return "refuse_dirty";
    case UPDATE_VERDICT_REFUSE_PRERELEASE: return "refuse_prerelease";
    case UPDATE_VERDICT_REFUSE_MALFORMED: return "refuse_malformed";
    case UPDATE_VERDICT_REFUSE_NEEDS_FORCE: return "refuse_needs_force";
    }
    return "unknown";
}
