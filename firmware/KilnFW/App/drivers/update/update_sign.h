// update_sign.h -- Ed25519 verification of release.json (docs/GITHUB_RELEASE_UPDATE_PLAN.md
// section 5, WP11). The detached signature is the raw 64-byte RFC 8032 signature over the exact
// bytes of the release.json asset, published as the `release.json.sig` asset.
//
// Enforcement (owner decision 2026-10-07): a release from the DEFAULT repo is refused when it has
// no signature or the signature matches none of the compiled-in public keys. A non-default repo is
// never verified and is always shown UNSIGNED (D5). While the compiled-in key list is EMPTY (no
// release key provisioned yet, see docs/RELEASING.md) nothing can be verified, so enforcement is
// inactive and every release stays UNSIGNED as in v1 (D4).
//
// Pure C, no allocation; host-tested by App/test/test_update_sign.c.
#ifndef KILNCTL_UPDATE_SIGN_H
#define KILNCTL_UPDATE_SIGN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UPDATE_SIG_LEN 64u
#define UPDATE_SIG_KEY_LEN 32u
#define UPDATE_SIG_ASSET_NAME "release.json.sig"

typedef struct {
    const uint8_t (*keys)[UPDATE_SIG_KEY_LEN]; // may be NULL when count == 0
    size_t count;
} update_sig_keyset_t;

typedef enum {
    UPDATE_SIG_VERIFIED = 0,   // signature valid under one of the keys
    UPDATE_SIG_NOT_ENFORCED,   // non-default repo, or no keys compiled in: shown UNSIGNED
    UPDATE_SIG_MISSING,        // enforced and the release has no signature asset
    UPDATE_SIG_INVALID         // enforced and the signature is malformed or matches no key
} update_sig_result_t;

// The compiled-in release public keys (update_signing_keys.c).
update_sig_keyset_t update_sig_builtin_keys(void);

// True when `repo` is the default repo (case-insensitive owner/name compare).
bool update_sig_repo_is_default(const char *repo);

// True when a release from `repo` must carry a valid signature under `ks`.
bool update_sig_required(const char *repo, const update_sig_keyset_t *ks);

// Decides the outcome for a fetched release. `sig` is NULL / sig_len 0 when the release has no
// signature asset; sig_len other than UPDATE_SIG_LEN is INVALID. Tries every key.
update_sig_result_t update_sig_check(const char *repo, const update_sig_keyset_t *ks, const uint8_t *manifest,
                                     size_t manifest_len, const uint8_t *sig, size_t sig_len);

const char *update_sig_result_name(update_sig_result_t r);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_UPDATE_SIGN_H
