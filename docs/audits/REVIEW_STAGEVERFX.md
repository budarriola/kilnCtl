# Review: STAGEVERFX (fixes for REVIEW_STAGEVER)

Reviewed: d80b0b179 (code) and 551722a76 (Fix status in `REVIEW_STAGEVER.md`) on origin/dev, 2026-10-13.
Reviewer: Opus, adversarial. Scope: device behaviour of the stage/policy path and the release-version
build plumbing. No board access.

Summary: 0 HIGH, 1 MED, 2 LOW, 5 INFO.

The MED-1 fix itself holds. The undeclared branch normalises the descriptor the same way the declared
branch does, and the dirty bit reaches `update_policy_decide_upload`, where the dirty rule refuses it.
I tried these descriptor spellings on the undeclared path:

| Descriptor | Result |
|---|---|
| `V...-dirty` | stripped, marked dirty |
| `...-DIRTY`, `...-Dirty` | BAD_VERSION |
| `...-dirty-dirty` (one strip leaves `-dirty`) | BAD_VERSION |
| bare `-dirty`, `v-dirty` | BAD_VERSION |
| trailing whitespace, any non-semver byte | BAD_VERSION (dry-run header parse) |
| embedded NUL | truncates at the NUL, then parsed as usual |
| over-long (no NUL in the 32-byte field) | BAD_VERSION (length check against `st->semver`) |
| `+build` metadata | accepted as valid semver; ranks by its core and prerelease only, so it cannot outrank anything |

The GitHub fetch path is unchanged. `update_stage_manifest_gate` ignores `dirty` by design, and the
declared compare is normalised. No regression is visible there.

Two findings remain:

- **MED-1.** The LOW-1 fix turned a silent stale-cache mismatch into a hard build failure. Nothing
  reconfigures the persistent check build directory, so a release build after any dev build fails, and so
  does a dev build after a release.
- **LOW-1.** The declared upload path still never sets `dirty`, so the two upload paths reach different
  policy decisions for the same image.

## Findings

### MED-1: a KILNCTL_RELEASE_VERSION change never reconfigures the reused check build dir, so make_release fails after any dev build (and the next dev build fails after a release)

Where:

- `firmware/KilnFW/CMakeLists.txt:45` caches `KILN_CONFIGURED_RELEASE_VERSION` at configure time only.
- `firmware/KilnFW/App/drivers/gen_build_info.cmake:89-93` runs on every build and stops with FATAL when
  the env differs from the cached value.
- `firmware/KilnFW/App/test/check_00_kilnfw_target_build.ps1:949-970` reconfigures only on an sdkconfig
  hash change.
- `tools/make_release.ps1:267-273` sets the env var and runs that check script.

ESP-IDF's `idf.py` reruns cmake only when CMakeCache is missing or new `-D` entries appear
(`tools/idf_py_actions/tools.py` `ensure_build_directory`). A changed environment variable does neither.
check_00 builds in a persistent `C:\wt\checkbuild` / `checkbuild_<hex>` directory, and its only reconfigure
trigger is `build\.sdkconfig_built.sha256`.

Failure scenario:

1. The tree has been built by the standing suite with the env unset, so the cached value is `''`.
2. `make_release.ps1 -Tag v1.0.0-pre.5` exports the tag and runs check_00 on the same checkbuild dir.
3. The sdkconfig is unchanged, so there is no reconfigure. The `drivers_build_info` target runs with
   `-DCONFIGURED_RELEASE_VERSION=` against env `v1.0.0-pre.5`.
4. The build stops with `FATAL ... does not match the configured ... with ''; re-run cmake/idf.py reconfigure`.
   The release cannot be built until someone runs `idf.py reconfigure` by hand inside the checkbuild dir,
   which no tool does.
5. The reverse also happens. If the release dir was reconfigured by hand, it now caches the tag. The next
   dev `check_00_kilnfw_target_build` (env unset) fails the same way, so every `run_all_checks` on that
   tree reports a target-build FAIL until another manual reconfigure.

Both directions were reproduced in CMake script mode (`cmake -P gen_build_info.cmake`, CMake 4.0.3, no
target build):

| Cached value | Env | Result |
|---|---|---|
| `''` | `v1.0.0-pre.5` | FATAL |
| `v1.0.0-pre.4` | unset | FATAL |
| equal | equal | `FW_RELEASE_VERSION` written correctly |
| `''` | unset | `""` written correctly |

The fix's evidence covered only the unset/unset case ("check_00_kilnfw_target_build PASS with the var
unset").

The build fails closed: no image with a wrong version is ever produced, so this is not a device-safety
defect. It is still MED, because it breaks the release flow the fix was written for, and it can break the
standing target-build check for a whole tree.

Suggested fix: in check_00 (and the KilnFW part of make_release), read `KILN_CONFIGURED_RELEASE_VERSION`
from `build\CMakeCache.txt` and run `idf.py reconfigure` when it differs from `$env:KILNCTL_RELEASE_VERSION`.
Alternatively, fold the env value into the stale-config marker next to the sdkconfig hash. Passing the env
as `-DKILNCTL_RELEASE_VERSION=` on the idf.py line would also make idf.py see a changed cache entry.

### LOW-1: declared hand upload passes dirty=false, so the same -dirty image is refused undeclared and accepted declared; a verbatim "-DIRTY" declaration still outranks the next prerelease

Where: `firmware/KilnFW/App/drivers/update/update_stage.c:287-299` (declared branch: `dirty` stays false)
vs `:300-321` (undeclared branch), and `update_policy.h:109`, `:155`.

The dirty rule now depends on whether the uploader sent `X-Stage-Version`:

- **Lowercase `-dirty`.** An image whose descriptor reads `v1.0.0-pre.3-dirty` (every published pre.3
  asset does) behaves differently on the two paths:
  - Uploaded undeclared, which is the MCP `update_stage_upload` default, it is refused `refuse_dirty`.
    Force cannot override that, and the message, "candidate was built from a dirty tree", does not mention
    that declaring the version is the way through.
  - Declared as `1.0.0-pre.3`, it is accepted with `dirty=false`.

  `update_policy.h:109` says the dirty rule applies "for an image whose own descriptor says -dirty". That is
  true on only one of the two paths.
- **Uppercase `-DIRTY`.** A descriptor `1.0.0-pre.3-DIRTY` declared verbatim as `1.0.0-pre.3-DIRTY` passes:
  - `desc_version_normalise` does not strip the uppercase suffix, the strings are equal, and there is no
    `ci_dirty_suffix` check on the declared branch.
  - The image is staged as prerelease `pre.3-DIRTY`. That is an alphanumeric identifier, so it ranks above
    `pre.4`, which is the original MED-1 shape.
  - Undeclared, the same image gets BAD_VERSION.

Mitigations: the uploader is an ADMIN; git never produces `-DIRTY`; and anyone who can build a custom image
can set any `PROJECT_VER` they like. That is why this is LOW. The real cost is the inconsistency, and the
fact that the published release asset cannot be hand-staged with the default tool call.

Suggested fix: pick one rule for both branches.

- Either compute `dirty` on the declared branch too and pass it on, so the declared path cannot launder a
  dirty descriptor; the GitHub path already ignores it through `update_stage_manifest_gate`.
- Or, if declaring is meant to vouch for the bytes, say so in `update_policy.h` and make the `refuse_dirty`
  reply text name the declared-version retry.
- In either case, run the `ci_dirty_suffix` refusal on the declared descriptor as well.

### LOW-2: the update_http.c dirty wiring has no host test

Where: `firmware/KilnFW/App/drivers/update/update_http.c:272` (`s_gate.ur.dirty = dirty`).

The stage test records `dirty` at the gate, and the policy test sets `req.dirty` directly. Nothing checks the
handoff in `policy_gate` between the two. Negtest M3 (see below) replaces it with `false`, which is the
exact pre-fix behaviour for a dirty undeclared upload, and the KilnFW host suite does not catch it.
`test_update_http_refusals.c` drives the HTTP handlers and is the natural place for a case asserting that an
undeclared `-dirty` body reaches a `refuse_dirty` reply.

### INFO

- **INFO-1. Configure accepts tags that the build-time check refuses.** The configure regex
  (`CMakeLists.txt:33`) is looser than `_RV_TAG_REGEX` (`gen_build_info.cmake:67`): it allows a capital
  `V` and leading zeros. Such a tag passes configure, then fails at build time, so the build still fails
  closed.
- **INFO-2. A 32-character tag cannot be built.** The length caps differ: configure allows at most 31
  characters (`CMakeLists.txt:29`), `gen_build_info` at most 32 (`:71`), and `update_url.c` accepts tags up
  to its own bound. A 32-character tag that the board would accept as a release can no longer be built.
  That is harmless today, because tags are about 15 characters.
- **INFO-3. A tag ending in `-dirty` passes both regexes.** The `-[0-9A-Za-z.-]+` allows a release tag
  such as `v1.0.0-pre.5-dirty`, or `-DIRTY`. The image then reads `-dirty`, so an undeclared upload refuses
  it and the GitHub path stages it under the manifest tag. This fails closed, but `release_gates` would be a
  cheap place to refuse such a tag.
- **INFO-4. The build-time check also fires on a bare `-P` call.** `gen_build_info.cmake:89` also fires
  for a caller that omits `-DCONFIGURED_RELEASE_VERSION` while the env is set (the
  `NOT release_version STREQUAL ""` arm). No such caller exists today. This is the intended fail-closed
  direction.
- **INFO-5. Values that parse as semver but are not git-describe output still rank as prereleases.** On the
  undeclared path, a descriptor such as `1.0.0-pre.3-dirtyx` or `1.0.0-pre.3.dirty` is not treated as dirty:
  - `1.0.0-pre.3-dirtyx` ranks above `pre.4`.
  - `1.0.0-pre.3.dirty` ranks below `pre.4`, by its numeric 3.

  Neither form is produced by `git describe`. A crafted descriptor can claim any version anyway, so the
  dirty rule only targets accidental dev builds. This is noted so nobody reads the rule as a defence
  against crafted images.

## Negtest

`tools\negtest.ps1 -Preset kilnfw-host -Mutations <muts.json> -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`,
run in C:\wt\rvstagever_2mmwhh at 551722a76. For every CAUGHT mutant, the log was checked to show that it
compiled and that a test assertion failed.

Baseline passed (75.6 s). The run did not report a real-tree change. Result: 6 CAUGHT, 1 MISSED.

| # | Mutation | Verdict | Evidence |
|---|---|---|---|
| M1 | `ci_dirty_suffix` refusal disabled (`if (0 && ...)`) | CAUGHT | FAIL test_update_stage.c:953 |
| M2 | `cand_s.dirty = req->dirty` -> `false` (update_policy.c) | CAUGHT | FAIL test_update_policy.c:315, :322 |
| M3 | `s_gate.ur.dirty = dirty` -> `false` (update_http.c:272) | MISSED | Suite passes. Becomes LOW-2. |
| M4 | gate call passes `false` instead of `dirty` | CAUGHT | FAIL test_update_stage.c:938, :944 |
| M5 | `dirty = strlen(dn) < ...` -> `dirty = false` | CAUGHT | FAIL test_update_stage.c:938, :944 |
| M6 | dirty rule removed (`if (0 && candidate->dirty)`) | CAUGHT | FAIL test_update_policy.c:153, :315, :322 |
| M7 | `desc_version_normalise` never strips `-dirty` | CAUGHT | FAIL test_update_stage.c:936-938, :944, :1177 |
