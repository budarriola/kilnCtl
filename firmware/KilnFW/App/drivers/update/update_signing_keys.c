// update_signing_keys.c -- the compiled-in release public keys (Ed25519, 32 bytes each).
//
// EMPTY until the owner provisions a release signing key: with no key nothing can be verified, so
// update_sig_required() is false and releases stay UNSIGNED (plan D4). Providing a key is an owner
// step, see docs/RELEASING.md "Release signing": generate the keypair OFFLINE with
// `tools/sign_release.py keygen`, keep the private key out of the repo, paste the printed C array
// here (a second entry allows key rotation: a release signed by either verifies), rebuild.
// NEVER put a test key in this list.
#include "update_sign.h"

// static const uint8_t k_release_keys[][UPDATE_SIG_KEY_LEN] = {
//     { 0x00 /* 32 bytes from `sign_release.py keygen` */ },
// };

update_sig_keyset_t update_sig_builtin_keys(void)
{
    update_sig_keyset_t ks = { NULL, 0 };
    return ks;
}
