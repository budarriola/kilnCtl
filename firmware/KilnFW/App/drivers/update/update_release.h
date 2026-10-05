// update_release.h -- bounded parsers for the two JSON documents the GitHub
// release fetch consumes (docs/GITHUB_RELEASE_UPDATE_PLAN.md sections 7, 9, WP8):
//
//   1. the GitHub API "latest release" reply (tag_name, draft, prerelease,
//      assets[] of {name, size, browser_download_url}), reduced to the two
//      assets this firmware cares about, and
//   2. release.json (schema 1, written by tools/release_manifest.py), reduced
//      to the candidate update_identity_t plus the app image's size and sha256.
//
// Both parsers work on a caller-owned buffer with an explicit length (the
// document is read into PSRAM first), never allocate, never recurse deeper
// than UPDATE_JSON_MAX_DEPTH, and treat anything unexpected as a refusal. The
// values they hand back are bounded copies; every URL is pinned to the
// configured repo, the tag and the asset name by update_asset_url_matches().
//
// Pure C; host-tested by App/test/test_update_release.c.
#ifndef KILNCTL_UPDATE_RELEASE_H
#define KILNCTL_UPDATE_RELEASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "update_policy.h"
#include "update_url.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UPDATE_JSON_MAX_DEPTH 8
#define UPDATE_TAG_MAX 33 // incl. NUL (tag <= 32 chars)
#define UPDATE_SHA256_HEX_LEN 64

typedef enum {
    UPDATE_REL_OK = 0,
    UPDATE_REL_E_ARGS,
    UPDATE_REL_E_JSON,           // not valid / too deeply nested / truncated JSON
    UPDATE_REL_E_NO_TAG,         // tag_name missing
    UPDATE_REL_E_BAD_TAG,        // tag_name not vMAJOR.MINOR.PATCH[-pre]
    UPDATE_REL_E_DRAFT,          // draft release
    UPDATE_REL_E_NO_APP,         // no KilnCtrl-<tag>.bin asset
    UPDATE_REL_E_NO_MANIFEST,    // no release.json asset
    UPDATE_REL_E_DUP_ASSET,      // the same wanted asset listed twice
    UPDATE_REL_E_BAD_URL,        // asset url not pinned to repo/tag/name
    UPDATE_REL_E_BAD_SIZE,       // asset size missing, zero or above the cap
    UPDATE_REL_E_SCHEMA,         // manifest schema != 1
    UPDATE_REL_E_TAG_MISMATCH,   // manifest tag != release tag
    UPDATE_REL_E_REPO_MISMATCH,  // manifest repo present and != configured repo
    UPDATE_REL_E_BAD_COMMIT,     // manifest commit not 40 lowercase hex
    UPDATE_REL_E_DIRTY,          // manifest dirty is not false
    UPDATE_REL_E_BAD_COMPAT,     // compat block missing / malformed
    UPDATE_REL_E_NO_APP_IMAGE,   // images[] has no usable "app" entry
    UPDATE_REL_E_SIZE_MISMATCH   // manifest app size != API asset size
} update_rel_err_t;

typedef struct {
    char tag[UPDATE_TAG_MAX];
    bool prerelease;
    char app_name[UPDATE_TAG_MAX + 16];
    char app_url[UPDATE_URL_MAX];
    uint32_t app_size;
    char manifest_url[UPDATE_URL_MAX];
    uint32_t manifest_size;
} update_release_info_t;

typedef struct {
    update_identity_t identity;                 // version = tag; the policy's candidate
    uint32_t app_size;
    char app_sha256[UPDATE_SHA256_HEX_LEN + 1]; // lowercase hex
} update_manifest_t;

// Parses the API reply. `repo` is the configured owner/name; `max_app_size` is
// the largest app image the stage partition can take. Zeroes *out first.
update_rel_err_t update_release_parse_api(const char *json, size_t len, const char *repo, uint32_t max_app_size,
                                          update_release_info_t *out);

// Parses release.json. `tag` and `app_size` come from the API reply and must
// agree with the manifest; `repo` is checked when the manifest names one.
update_rel_err_t update_release_parse_manifest(const char *json, size_t len, const char *repo, const char *tag,
                                               uint32_t api_app_size, update_manifest_t *out);

const char *update_rel_err_name(update_rel_err_t e);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_UPDATE_RELEASE_H
