# Vendored Monocypher (Ed25519 verify for release.json.sig)

Used only by `../../update_sign.c` (docs/GITHUB_RELEASE_UPDATE_PLAN.md WP11). ESP-IDF v6.0.2's mbedTLS/PSA
defines the Ed25519 constants but ships no implementation, so a small public-domain verifier is vendored.

- Upstream: https://github.com/LoupVaillant/Monocypher, release tag 4.0.3
- Licence: dual BSD-2-Clause / CC0-1.0 (choose either); full text in `LICENCE.md`.
- Files are byte-identical to upstream (do not edit; re-vendor to update):

| file here | upstream path | sha256 |
|---|---|---|
| monocypher.c | src/monocypher.c | f1f838cdd483bdebe0df0ff5c5ed60535e496f769c6a2f933ac4c0b114207123 |
| monocypher.h | src/monocypher.h | fcaf6ed771358bb4f40fba016f6518ae86ec02b1b877d2cc35ad92d3a26fd7b3 |
| monocypher-ed25519.c | src/optional/monocypher-ed25519.c | ce0d2f8e32ca8f66398ba5b3456cc74327c3eff14e7b950ce7d57be9025cc453 |
| monocypher-ed25519.h | src/optional/monocypher-ed25519.h | 3a3035181f991a158d0e1c7567258f0bae8ba0f1f23c5512b4a1db1b3c9730ce |
| LICENCE.md | LICENCE.md | a5781770269d2516e52ba4863f790c10a16da4089a1e81823aee19ff1e9026b0 |

Only these four sources are vendored: `monocypher-ed25519.c` supplies the SHA-512 variant
(`crypto_ed25519_check`, RFC 8032 Ed25519), and it needs `monocypher.c` for the curve arithmetic.
Unused functions in `monocypher.c` (ChaCha20, Poly1305, Argon2, ...) are removed by the linker's
`--gc-sections`. Only `crypto_ed25519_check` is ever called; no signing or secret-key code runs on the board.
`tools/check_monocypher_vendored.ps1` re-verifies the hashes above.
