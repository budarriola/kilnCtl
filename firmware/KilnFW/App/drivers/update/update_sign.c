// update_sign.c -- see update_sign.h.
#include "update_sign.h"

#include <string.h>

#include "update_settings.h"
#include "third_party/monocypher/monocypher-ed25519.h"

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool update_sig_repo_is_default(const char *repo)
{
    if (repo == NULL) {
        return false;
    }
    const char *d = UPDATE_SETTINGS_DEFAULT_REPO;
    size_t i = 0;
    for (; d[i] != '\0'; i++) {
        if (repo[i] == '\0' || lower(repo[i]) != lower(d[i])) {
            return false;
        }
    }
    return repo[i] == '\0';
}

bool update_sig_required(const char *repo, const update_sig_keyset_t *ks)
{
    return ks != NULL && ks->count > 0 && ks->keys != NULL && update_sig_repo_is_default(repo);
}

update_sig_result_t update_sig_check(const char *repo, const update_sig_keyset_t *ks, const uint8_t *manifest,
                                     size_t manifest_len, const uint8_t *sig, size_t sig_len)
{
    if (!update_sig_required(repo, ks)) {
        return UPDATE_SIG_NOT_ENFORCED;
    }
    if (sig == NULL || sig_len == 0) {
        return UPDATE_SIG_MISSING;
    }
    if (sig_len != UPDATE_SIG_LEN || manifest == NULL || manifest_len == 0) {
        return UPDATE_SIG_INVALID;
    }
    int ok = 0;
    for (size_t i = 0; i < ks->count; i++) {
        // crypto_ed25519_check returns 0 when the signature is valid.
        if (crypto_ed25519_check(sig, ks->keys[i], manifest, manifest_len) == 0) {
            ok = 1;
        }
    }
    return ok ? UPDATE_SIG_VERIFIED : UPDATE_SIG_INVALID;
}

const char *update_sig_result_name(update_sig_result_t r)
{
    switch (r) {
    case UPDATE_SIG_VERIFIED: return "verified";
    case UPDATE_SIG_NOT_ENFORCED: return "not_enforced";
    case UPDATE_SIG_MISSING: return "release_unsigned";
    case UPDATE_SIG_INVALID: return "signature_invalid";
    }
    return "signature_invalid";
}
