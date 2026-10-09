// update_policy.h -- the pure install/refuse decision for a candidate release
// (docs/GITHUB_RELEASE_UPDATE_PLAN.md sections 5-7, owner decision D8:
// downgrade refused by default, ADMIN override with a typed confirm).
//
// Inputs are the RUNNING image's identity (build_info/esp_app_desc plus the
// board's schema versions) and the CANDIDATE's manifest `compat` block
// (release.json, section 7) or stage header. No ESP-IDF calls: callers fill
// update_identity_t from their own sources. The result is a verdict plus
// flags; it never performs anything, and HTTP/UI layers decide how to word it.
//
// Rule order (first match wins, hard refusals before soft ones):
//   1. candidate identity unusable (semver unparsable, partitions_sha256
//      missing/malformed)                          -> REFUSE_MALFORMED
//   2. candidate built from a dirty tree           -> REFUSE_DIRTY
//   3. partitions_sha256 differs from the board's  -> REFUSE_PARTITIONS
//      (never overridable: the image assumes a different flash map)
//   4. candidate is a prerelease and the channel does not allow them
//                                                  -> REFUSE_PRERELEASE
//   5. running version < manifest min_updatable_from
//                                                  -> REFUSE_TOO_OLD
//   6. any schema version (zones_cfg, kilnlink, uart) LOWER than the board's,
//      or candidate semver < running                -> DOWNGRADE
//      (REFUSE_DOWNGRADE unless allow_downgrade, then ALLOW_DOWNGRADE)
//   7. same semver (build metadata ignored) and same commit -> UP_TO_DATE
//      (refused unless force, then ALLOW_REINSTALL); same semver, different
//      commit -> REFUSE_NEEDS_FORCE unless force (then ALLOW_REINSTALL)
//   8. running version unparsable (dev build): comparison impossible ->
//      REFUSE_NEEDS_FORCE unless force (then ALLOW_REINSTALL); the schema and
//      partition rules above still apply
//   9. otherwise                                   -> ALLOW_UPGRADE
//
// A schema version HIGHER than the board's is normal for an upgrade (the new
// firmware migrates forward) and is only reported via schema_newer.
//
// Pure C; host-tested by App/test/test_update_policy.c.
#ifndef KILNCTL_UPDATE_POLICY_H
#define KILNCTL_UPDATE_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UPDATE_VERSION_STR_MAX 64   // incl. NUL; semver text as published
#define UPDATE_COMMIT_HEX_LEN 40
#define UPDATE_PARTITIONS_SHA_HEX_LEN 64

typedef struct {
    char version[UPDATE_VERSION_STR_MAX]; // "v1.0.0", "1.0.0-rc.1", "" if unknown
    char commit[UPDATE_COMMIT_HEX_LEN + 1]; // 40 hex, or "" if unknown
    char partitions_sha256[UPDATE_PARTITIONS_SHA_HEX_LEN + 1]; // 64 hex, lowercase
    uint32_t zones_cfg_version;
    uint32_t kilnlink_version;
    uint32_t uart_version;
    bool dirty;                           // candidate only: built from a dirty tree
    char min_updatable_from[UPDATE_VERSION_STR_MAX]; // candidate only; "" = no floor
} update_identity_t;

typedef struct {
    bool allow_prerelease; // D7: default false
    bool allow_downgrade;  // D8 override; caller has already checked the typed confirm
    bool force;            // reinstall of the same / unversioned image
} update_policy_flags_t;

typedef enum {
    UPDATE_VERDICT_ALLOW_UPGRADE = 0,
    UPDATE_VERDICT_ALLOW_REINSTALL,   // needed force
    UPDATE_VERDICT_ALLOW_DOWNGRADE,   // needed allow_downgrade
    UPDATE_VERDICT_UP_TO_DATE,        // same version+commit, no force
    UPDATE_VERDICT_REFUSE_DOWNGRADE,  // older or schema lower, no override
    UPDATE_VERDICT_REFUSE_PARTITIONS,
    UPDATE_VERDICT_REFUSE_TOO_OLD,
    UPDATE_VERDICT_REFUSE_DIRTY,
    UPDATE_VERDICT_REFUSE_PRERELEASE,
    UPDATE_VERDICT_REFUSE_MALFORMED,
    UPDATE_VERDICT_REFUSE_NEEDS_FORCE, // same version/different commit, or running version unknown
} update_verdict_t;

typedef struct {
    update_verdict_t verdict;
    bool allowed;           // true only for the ALLOW_* verdicts
    bool needs_confirm;     // UI/route must obtain a confirm (every ALLOW_*)
    bool needs_typed_confirm; // ALLOW_DOWNGRADE: typed confirm + rollback-hazard notice
    bool zones_cfg_lower;   // candidate zones_cfg_version < board's (hazard class, section 6)
    bool schema_newer;      // some schema version higher than the board's
    int semver_cmp;         // candidate vs running: -1/0/+1; 2 if not comparable
    const char *reason;     // static string, safe to show/log
} update_decision_t;

#define UPDATE_SEMVER_CMP_UNKNOWN 2

update_decision_t update_policy_decide(const update_identity_t *running, const update_identity_t *candidate,
                                       const update_policy_flags_t *flags);

// update_policy_decide() plus the fetch-path rule for an unknown running version: force=1 only
// yields ALLOW_REINSTALL there when typed_confirm_ok (confirm_downgrade == the candidate tag).
// Otherwise REFUSE_NEEDS_FORCE with needs_typed_confirm set. Known running version: identical to
// update_policy_decide().
update_decision_t update_policy_decide_typed(const update_identity_t *running, const update_identity_t *candidate,
                                             const update_policy_flags_t *flags, bool typed_confirm_ok);

// Hand-upload (POST /api/update/stage) front door onto the same policy. The raw image carries no
// manifest, so the caller supplies what the uploader declared: X-Stage-Version / X-Stage-Commit and
// optional schema versions (0 = not declared, taken as equal to the board's, i.e. unchecked), plus the
// override flags. The partition-table hash cannot be checked for an upload (not in the image) and
// prerelease versions are allowed (no channel), and the dirty/floor rules do not apply. The typed
// confirm is `confirm` equal to `version` exactly; allow_downgrade only counts with it. A missing
// version is treated as unknown and needs force plus confirm == "unversioned". Signing plays no part.
// Identity record the firmware embeds in its own image (a const in .rodata_custom_desc, the section the
// linker places directly after esp_app_desc_t), so a hand upload's schema versions come from the image
// itself rather than from headers the uploader may omit. check = magic ^ the three versions ^ 0xA5A5A5A5.
#define UPDATE_IMAGE_ID_MAGIC 0x4B494449u
#define UPDATE_IMAGE_ID_SIZE 20u
typedef struct {
    uint32_t magic;
    uint32_t zones_cfg_version;
    uint32_t kilnlink_version;
    uint32_t uart_version;
    uint32_t check;
} update_image_id_t;

uint32_t update_image_id_check(const update_image_id_t *id);
void update_image_id_make(update_image_id_t *id, uint32_t zones, uint32_t kl, uint32_t uart);
// Scans head[from .. len) at 4-byte steps for a valid record (little-endian words). false = none.
bool update_image_id_find(const uint8_t *head, size_t len, size_t from, update_image_id_t *out);

// One typed-confirm rule for every install path: `confirm` equals `version`, ignoring one leading v/V
// on either side; empty never matches.
bool update_policy_typed_confirm_ok(const char *confirm, const char *version);

// Strict header number: plain decimal digits only (no sign, space or trailing junk), 1..0xFFFFFFFE.
// true and *out set when valid; false (and *out 0) for anything else, including "" and "0".
bool update_policy_parse_hdr_u32(const char *text, uint32_t *out);

typedef struct {
    const char *version;   // the resolved version (declared, which the stager has checked equals the image's)
    const char *commit;    // as declared; NULL/"" = none (non-40-hex is ignored)
    bool have_image_id;    // the image carries an identity record
    update_image_id_t image_id;
    uint32_t zones_cfg_version; // header-declared, advisory: 0 = not declared; must equal the record when both
    uint32_t kilnlink_version;
    uint32_t uart_version;
    bool force;
    bool allow_downgrade;
    const char *confirm;   // typed confirm text, NULL/"" = none
} update_upload_request_t;

// An image without the record has an unknown schema: it needs force AND the typed confirm, on top of
// whatever the version comparison demands. A missing version is MALFORMED (the stager never passes one).
update_decision_t update_policy_decide_upload(const update_identity_t *running, const update_upload_request_t *req);

const char *update_verdict_name(update_verdict_t v);

// True for a 40-char lowercase-hex commit id.
bool update_policy_commit_valid(const char *s);
// True for a 64-char lowercase-hex sha256.
bool update_policy_sha256_hex_valid(const char *s);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_UPDATE_POLICY_H
