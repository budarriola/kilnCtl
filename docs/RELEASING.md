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
| `tools/release_manifest.py` | stdlib-only `generate` / `validate` of `release.json` and `SHA256SUMS` |
| `tools/check_release_manifest.ps1` | standing check: unit tests, bad-tag refusal, mocked publish flow |
| `tools/PcTools/tests/test_release_manifest.py` | the unit tests |

## Release assets

Written to `logs/release/<tag>/` and uploaded:

- `KilnCtrl-<tag>.bin` application image (embeds both SaftyFW slot images)
- `KilnRecovery-<tag>.bin` recovery image (informational, JTAG `flash_recovery` only)
- `KilnCtrl-<tag>.elf.zip` ELF for crash symbolization
- `release.json` manifest, schema 1: tag, channel, published, repo, commit, `dirty:false`,
  build_date, `compat` (`zones_cfg_version`, `kilnlink_version`, `uart_version`,
  `partitions_sha256` of `firmware/KilnFW/partitions.csv`), `images[]` with size and sha256
- `SHA256SUMS`

`zones_cfg_version`, `kilnlink_version` and `uart_version` are parsed from
`zones_config_json.h`, `kilnlink_version.h` and `uart_task_ids.h` at release time, never
typed by hand. A release is refused if `KilnCtrl.bin` exceeds 4 MB (0x400000, the `app`
partition; staging lives in its own `stage` partition).

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
   `-SkipBuild [-BuildDir <dir>] [-RecoveryBin <file>]` reuses existing artifacts;
   `-DevDryRun` downgrades the git gates to warnings (refused with `-Publish`) for
   exercising the packaging path from a scratch worktree.
4. Review `logs/release/<tag>/release.json` (commit, compat, sizes).
5. Publish: add `-Publish`. Flow: POST a draft release with `target_commitish`, upload each
   asset to uploads.github.com, re-download each asset and compare sha256, then PATCH
   `draft=false`. A mismatch leaves the draft unpublished (delete it on GitHub, fix, retry
   with a fresh `logs/release/<tag>` directory). `-WhatIf` prints the REST plan and calls
   nothing. The re-download hop (API asset URL with the token, then the signed blob URL
   without it) is UNVERIFIED against live GitHub; the first real release is its test.
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
8. `KilnCtrl.bin` <= 4 MB and `.dram0.bss` <= 101000 B.
9. Release notes list schema versions and any rollback hazard versus the previous release.

`make_release.ps1` enforces only what it can prove mechanically (clean tree, origin/main,
free tag, size gate, manifest consistency); gates 1 and 3-9 are the releaser's checklist.

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
- Size gate refusal: the image no longer fits the 4 MB `app` partition; that is a
  partition-table decision, not something to bypass.
- Build SKIP (exit 3): the ESP-IDF toolchain is missing; a release needs real binaries.
