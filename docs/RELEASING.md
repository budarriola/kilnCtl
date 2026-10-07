# Releasing kilnCtl

How to cut a GitHub release of github.com/budarriola/kilnCtl. Design and rationale:
the GitHub-release update plan (sections 6-8). The gh CLI is not used; publishing is
plain REST from `tools/make_release.ps1`.

Versions are semver tags `vMAJOR.MINOR.PATCH` (optionally `-pre.N`). The first release is
`v1.0.0`, cut only after every gate below passes. Tags are created by GitHub from the
release's `target_commitish`; never `git push --force` and never push a tag by hand.

## Files

| File | Role |
|------|------|
| `tools/make_release.ps1` | gates, build, collect, manifest, optional publish (dry run by default) |
| `tools/release_gates.py` | validates and checks the gates record; builds the default release-notes body |
| `docs/release_gates.json` | tracked gates record: every stable gate, `open` or `pass`, with evidence |
| `tools/release_manifest.py` | stdlib-only `generate` / `validate` of `release.json` and `SHA256SUMS` |
| `tools/check_release_manifest.ps1` | standing check: unit tests, bad-tag refusal, mocked publish flow |
| `tools/PcTools/tests/test_release_manifest.py`, `test_release_gates.py` | the unit tests |

## Release assets

Written to `logs/release/<tag>/` and uploaded:

- `KilnCtrl-<tag>.bin` application image (embeds both SaftyFW slot images)
- `KilnRecovery-<tag>.bin` recovery image (informational, JTAG `flash_recovery` only)
- `KilnCtrl-<tag>.elf.zip` ELF for crash symbolization
- `release.json` manifest, schema 1: tag, channel, published, repo, commit, `dirty:false`,
  build_date, `compat` (`zones_cfg_version`, `kilnlink_version`, `uart_version`,
  `partitions_sha256` of `firmware/KilnFW/partitions.csv`), `images[]` with size and sha256
- `SHA256SUMS`
- `release.json.sig` optional detached Ed25519 signature (64 bytes) over the exact bytes of `release.json`; see "Release signing"

`zones_cfg_version`, `kilnlink_version` and `uart_version` are parsed from
`zones_config_json.h`, `kilnlink_version.h` and `uart_task_ids.h` at release time (all three are mandatory and must be positive: the generator refuses if one cannot be read, since the update policy treats zero as malformed), never
typed by hand. A release is refused if `KilnCtrl.bin` exceeds 0x400000 (4 MB). That
gate is deliberately the planned post-split app size (docs/GITHUB_RELEASE_UPDATE_PLAN.md
WP2), which is now also the actual `app` partition size in `partitions.csv`.

## Cutting a release

1. Pass the stable gates below.
2. Mint a clean worktree at origin/main: `tools\worktree_mint.ps1`. Never release from the
   shared tree.
3. Dry run (default). From that worktree, in PowerShell:

       powershell -ExecutionPolicy Bypass -File tools\make_release.ps1 -Tag v1.0.0

   This refuses unless: the tag is semver, the tree is clean, HEAD equals origin/main, the tag
   is absent locally and on origin (`git ls-remote`). It then runs the two standing
   target-build checks (`check_00_kilnfw_target_build.ps1`,
   `check_00_kilnfw_recovery_target_build.ps1`, which build in isolated `C:\wt\checkbuild_*`
   trees under the repo's build gate), zips the ELF, writes and validates the manifest, and
   prints what `-Publish` would send. Nothing leaves the machine.
   `-SkipBuild [-BuildDir <dir>] [-RecoveryBin <file>]` reuses existing artifacts (dry run
   only: see the provenance rule under step 5);
   `-NotesFile <path>` makes that file's content the release body verbatim; without it the
   body is generated (see "Release notes" below). `-GatesFile <path>` points at another gates
   record (tests only).
   `-DevDryRun` downgrades the git gates to warnings (refused with `-Publish`) for
   exercising the packaging path from a scratch worktree.
4. Review `logs/release/<tag>/release.json` (commit, compat, sizes).
5. Publish: add `-Publish`. A stable tag is refused while any gate in
   `docs/release_gates.json` is open (see "Gates record"); `-AllowOpenGates` overrides that and
   prints the open gates loudly. Flow: POST a draft release with `target_commitish`, upload each
   asset to uploads.github.com, re-download each asset and compare sha256, then PATCH
   `draft=false`. A mismatch leaves the draft unpublished (delete it on GitHub, fix, retry
   with a fresh `logs/release/<tag>` directory). `-WhatIf` prints the REST plan and calls
   nothing.
   Provenance rule: `-Publish` is refused together with `-SkipBuild`, `-BuildDir` or
   `-RecoveryBin`. The manifest and release are stamped with HEAD's commit, so the shipped
   binaries must be the ones this script just built from it. (Chosen over verifying the
   `esp_app_desc_t` in `KilnCtrl.bin`: its version string is `git describe`-derived and
   only proves a build time, not that the binary came from HEAD, and the recovery image
   has no such check at all.)
   The re-download hop is `Get-AssetToFile`, written with `System.Net.Http.HttpClient`
   (`AllowAutoRedirect=false`): the token goes only on the first api.github.com request,
   the `Location` is followed without it, and the body is streamed to a file. It is tested
   by `tools/check_release_manifest.ps1` against a local 127.0.0.1 redirect server (token
   present on hop 1, absent on hop 2, bytes match, a 404 on hop 2 throws). It is still
   UNVERIFIED against live GitHub (signed blob URLs, real 302 headers); the first real
   release is that test.
6. Record the release in ROADMAP.md and the bench log.

## Stable gates (all required)

1. `tools\run_all_checks.ps1 -ExecutionPolicy Bypass` full (not `-Fast`, not `-Only`), all
   PASS at the release commit, zero unexplained SKIPs.
2. Release commit equals origin/main HEAD in a clean worktree; artifacts built from it.
3. ROADMAP.md has no unchecked release-blocking item; `docs/RELEASE_HARDENING_PLAN.md`
   section 2 items closed or waived by the owner in that file.
4. No open regression rows in the status/bench logs; latest bench pass (suites `ota`, `lcd`,
   safety link) at most 7 days old and on the exact image released (`fw_build` matches).
5. `get_heap_status`: heap_internal min_free >= 8192 B over a 24 h soak on the release image,
   no crash report, `capability_preflight` clean, all required tasks alive.
6. `ota_matrix_run` passes on the image (ESP via recovery, Pico auto-update, rollback).
7. Readiness all ok on the bench board after flashing the release image (hardware-gated
   items listed in the notes).
8. `KilnCtrl.bin` <= 0x400000 and `.dram0.bss` <= 101000 B.
9. Release notes list schema versions and any rollback hazard versus the previous release.

`make_release.ps1` enforces mechanically what it can prove (clean tree, origin/main, free
tag, size gate, manifest consistency) and, for gates 1 and 3-9 plus the update-feature gates
from the plan, checks that the gates record says `pass` (below). The tool cannot verify the
evidence; it only refuses to publish a stable tag while the record says otherwise.

## Gates record

`docs/release_gates.json` (schema 1, validated by `tools/release_gates.py`):

    {"schema": 1, "gates": [
      {"id": "soak-24h", "title": "...", "status": "open", "evidence": "", "source": "..."}]}

- `status` is exactly `open` or `pass`. A `pass` needs non-empty `evidence` (log path, commit,
  date and what was observed). There is no `waived`: an owner waiver is recorded by setting
  the gate to `pass` with the waiver as its evidence, so git history shows who decided and when.
- Seeded 2026-10-06 from the readiness audit; everything not yet demonstrated is `open`
  (only `clean-origin-main` is `pass`, because `make_release.ps1` enforces it itself).
- Every `make_release.ps1` run (including the dry run) prints each gate and a summary. A dry
  run never fails on open gates; a missing, unreadable or malformed gates file (bad JSON,
  wrong schema, duplicate or bad id, unknown status, `pass` without evidence) refuses every
  run, dry or not.
- `-Publish` of a stable tag (no `-suffix`) exits 1 while any gate is `open`, before any git
  or build step. `-AllowOpenGates` lets it through and prints the open gates between `!!!`
  lines.
- A pre-release tag (`v1.0.0-pre.1`) may publish with open gates but still prints them.
  Pre-release shapes are accepted end to end: `update_semver.c` parses `-prerelease`
  (sorts below the same release), the manifest generator accepts the tag shape, and the
  `-Channel pre` switch marks the GitHub release as a prerelease. The tag suffix, not
  `-Channel`, decides whether gates may be open. Use a pre-release first to exercise the
  still-unverified live GitHub hop.
- A GitHub release flagged pre-release (the "Set as a pre-release" box, or `-Channel pre`)
  is invisible to `GET /repos/<repo>/releases/latest`, so the board reads
  `releases?per_page=5` instead whenever pre-releases are allowed (tick "Include
  pre-releases" on the OTA page, or `update_check(allow_prerelease=True)` / the
  `allow_prerelease=1` query on `POST /api/update/check` and `/download`) and takes the
  highest-semver non-draft release, `-pre.10` above `-pre.9`. With pre-releases off the
  board keeps using `/releases/latest`. Only the five newest releases are looked at, and a
  draft is never offered. A pre-release is still refused by policy unless the download is
  also made with `allow_prerelease=1`.
- `tools/check_release_manifest.ps1` (standing check) validates the tracked file, runs
  `test_release_gates.py`, and asserts the stable-tag refusal.

## Release notes

`-NotesFile <path>` (must exist and be non-empty) becomes the GitHub release body verbatim.
Without it the body is `git log --oneline <previous semver tag merged into HEAD>..HEAD`
(tags matching `vX.Y.Z[-pre]` only, highest wins, a prerelease below its release), or the
last 50 commits when there is no previous semver tag, followed by the commit and
`zones_cfg_version`. The body is resolved before the build starts and written to
`logs/release/<tag>.notes.md` for review (outside the asset directory, so it is not uploaded).
Gate 9 (schema versions and rollback hazards versus the previous release) stays a human
review: pass a hand-written notes file for a real release.

## Release signing

Plan: `docs/GITHUB_RELEASE_UPDATE_PLAN.md` WP11. `tools/sign_release.py` signs `release.json`
(Ed25519, RFC 8032); the board verifies it against public keys compiled into the firmware
(`firmware/KilnFW/App/drivers/update/update_signing_keys.c`, verifier: vendored Monocypher, see
`drivers/update/third_party/monocypher/README.md`).

**What the board does.** For the default repo (`budarriola/kilnCtl`) and only when the firmware has at
least one key built in, a release with no `release.json.sig` is refused with `release_unsigned` and one
whose signature fails every key with `signature_invalid`; nothing is staged. Any other repo is unchanged
and is always shown UNSIGNED (D5). **The shipped key list is empty**, so until the owner provisions a key
nothing can be verified and enforcement is inactive: every release, signed or not, is shown UNSIGNED (D4).

**Pre-WP11 releases.** `v1.0.0-pre.1` (and any release cut before signing) has no `release.json.sig`. Firmware
without a key (today's) stages it as before. Firmware built with a release key refuses it as `release_unsigned`:
to move such a board forward, cut a new signed release (or stage by upload, which is not signature-checked).
The unsigned release cannot be retro-signed by anyone but the key holder; run `sign_release.py sign` on its
downloaded `release.json` and upload `release.json.sig` to that release if it must stay installable.

**Key custody (owner step, not done by tooling).**

1. On an offline machine: `python tools/sign_release.py keygen --out <path outside the repo>\kilnctl_release.pem`.
   It refuses to overwrite and prints the public key as a C array. The PEM has no passphrase: keep it on
   offline or encrypted storage with a backup. Never commit it; never put a test key in the firmware.
2. Paste the printed entry into `k_release_keys` in `update_signing_keys.c` (uncomment the array and make
   `update_sig_builtin_keys()` return it), rebuild and flash. Only then is enforcement active.
3. Releasing: set `KILNCTL_RELEASE_SIGNING_KEY` to the PEM path (User scope or session) and run
   `make_release.ps1` as usual; it writes `release.json.sig` into `logs\release\<tag>\` and uploads it.
   Without the variable a default-repo `-Publish` prints a warning and ships UNSIGNED.
4. Rotation: add the new public key as a second entry, ship that firmware, then sign with the new key; a
   release signed by either key verifies. Drop the old entry in a later firmware.
5. Loss or leak of the private key: no remote revocation exists. Build and JTAG-flash firmware with a new key
   list; boards already running the old list keep trusting the old key until reflashed.

Check a signature by hand: `python tools/sign_release.py verify --manifest release.json --sig release.json.sig --pub <hex>`.
`tools/check_monocypher_vendored.ps1` keeps the vendored verifier byte-identical to upstream 4.0.3.

## Token setup

Publishing reads the environment variable `KILNCTL_GITHUB_TOKEN`. It is never printed,
logged, or passed on a command line.

1. GitHub, Settings, Developer settings, Fine-grained personal access tokens, Generate.
2. Resource owner: your account. Repository access: Only select repositories, kilnCtl.
3. Permissions: Repository, Contents: Read and write. Nothing else.
4. Short expiry (30-90 days).
5. Store it at User scope so it is not in the repo or shell history:

       [Environment]::SetEnvironmentVariable("KILNCTL_GITHUB_TOKEN", "<paste token>", "User")

   Open a new terminal afterwards.
6. Revoke it on GitHub when you are done releasing.

## Troubleshooting

- "tag already exists": a tag cannot be reused; bump the version.
- "HEAD is not origin/main": merge/push first, then mint a fresh worktree.
- Size gate refusal: the image exceeds the 0x400000 planned post-split app size; that is a
  partition-table decision, not something to bypass.
- Build SKIP (exit 3): the ESP-IDF toolchain is missing; a release needs real binaries.
