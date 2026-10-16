# Review: stage descriptor-version normalisation (492ae25cb, c549a144c)

Date: 2026-10-12. Reviewer: Opus, adversarial, no board access. Base: origin/dev c549a144c.

Scope:
- `firmware/KilnFW/App/drivers/update/update_stage.c`: `desc_version_normalise()` drops one leading
  v/V and one trailing `-dirty` from the image's app_desc version before `flush_head` compares it to
  the declared (manifest or X-Stage-Version) semver.
- `firmware/KilnFW/App/test/test_update_stage.c`: pre.3 `-dirty` accept/refuse cases.
- `firmware/KilnFW/CMakeLists.txt`: `PROJECT_VER` comes from `ENV{KILNCTL_RELEASE_VERSION}` before
  `project.cmake` when the variable is set and non-empty.

Verdict: 0 HIGH, 1 MED (pre-existing, adjacent path), 2 LOW, 6 INFO. The fix does what it says. The
normaliser cannot make the manifest compare accept a different version.

## Findings

### MED-1 (pre-existing, not introduced here): undeclared hand upload takes the raw `-dirty` descriptor as the candidate version, which misjudges newer vs downgrade

`flush_head`'s `else` branch, used when a hand upload sends no X-Stage-Version, sets
`st->semver = strip_v(v)` with no `-dirty` strip. That value goes to `st->gate` (`update_http.c`
`policy_gate`, `s_gate.ur.version = semver`), into `update_policy_decide_upload`, and into the stage
header and status.

Semver precedence ranks an alphanumeric prerelease identifier above a numeric one, so `3-dirty` beats
`4`, and `1.0.0-pre.3-dirty` > `1.0.0-pre.4`. Results:
- A hand upload of the published pre.3 image (descriptor `v1.0.0-pre.3-dirty`) onto a board running
  pre.4 or later gets `ALLOW_UPGRADE "newer version available"`. That skips the downgrade refusal and
  its typed confirm.
- On a board running pre.3 it reads as an upgrade, not same version / needs force.
- The policy's dirty refusal (`update_policy.c:98`, `candidate->dirty`) does not fire: the upload path
  builds the candidate with `dirty=false`, and `update_image_id_t` carries no dirty flag.
- The status page and stage header show `1.0.0-pre.3-dirty`.

Dev images (`T-N-g<hash>-dirty`) have the same shape.

The commits did not cause this, but they reviewed the same `-dirty` string and fixed only the declared
branch.

Fix: in the `else` branch, either refuse a descriptor that ends in `-dirty` (fail closed, matching the
policy's intent), or normalise it and set the candidate's dirty flag so the policy refuses it. Add a
test: an undeclared hand upload of a `-dirty` descriptor must not judge as an upgrade over a newer
running version.

### LOW-1: env-driven PROJECT_VER does not force a reconfigure, so incremental build dirs can carry a stale descriptor version

IDF calls `git_describe` (which `configure_file`s `.git/HEAD` and so reconfigures on a new commit) only
`if(NOT DEFINED PROJECT_VER)`. The value reaches `esp_app_desc.c` as a compile definition set at
configure time.
- **Release after dev, same HEAD.** A `make_release.ps1` build in a persistent checkbuild dir already
  configured at the same HEAD does not reconfigure, so the descriptor keeps the old git-describe value,
  possibly `-dirty`. The new normaliser hides the `-dirty` case. A stale `T-N-g<hash>` would still be
  refused (fails closed).
- **Dev after release.** Later dev builds in a dir configured with the env set never reconfigure on a
  HEAD change (no HEAD dependency was registered) and never notice the env var is gone. The descriptor
  keeps claiming tag T. A hand upload then reads "same version T, different commit". The GitHub fetch
  path is protected by the M1 commit compare.

Recommendation: `gen_build_info` (runs every build) compares the configured `PROJECT_VER` with
`KILNCTL_RELEASE_VERSION` and FATAL_ERRORs on a mismatch, or `make_release.ps1` forces a reconfigure
(touch CMakeLists or a fresh build dir).

### LOW-2: the CMake hook does not validate the tag

`set(PROJECT_VER "$ENV{KILNCTL_RELEASE_VERSION}")` takes any string. `gen_build_info` already
FATAL_ERRORs on an invalid tag, so a malformed value cannot ship. A 32-character tag passes
`gen_build_info` but is cut to 31 by `esp_app_format` (`string(SUBSTRING ... 0 31)`). The manifest
compare then refuses it, which fails closed but would brick a release's self-update path.

Recommendation: CMake checks length <= 31 and the semver regex next to the `set`.

### INFO

- **I-1: Normaliser edge cases all fail closed or are unchanged.**
  - Leading V: `strip_v` handles v and V, the same as the declared side.
  - Case: `-DIRTY` is not stripped (case-sensitive `strcmp`), so it is refused.
  - Only one `-dirty` is stripped. `vv` keeps one v and does not parse.
  - An empty descriptor, or `-dirty` alone (n == sl is not stripped), does not parse. The declared
    version stands, as before; the commit check and sha256 still bind the bytes.
  - A NUL inside the 32-byte field ends `strlen`, and `v` is always NUL-terminated at
    `STAGE_SEMVER_FIELD_LEN`.
  - Truncation: `out` has `STAGE_SEMVER_FIELD_LEN + 1` bytes and `n` <= `strlen(v)`. A 31-character cut
    can only yield T from a descriptor that began with T, and only by dropping the tail. `T-dirty`
    longer than 31 characters cuts mid-suffix, is not stripped, and is refused.
  - Manifest with or without v: the declared value is `strip_v`'d before it is stored.
- **I-2: `T-dirty` is accepted as T.** This admits an image built from a dirty tree. That is acceptable
  because the manifest sha256 binds the exact bytes and the M1 commit compare binds the commit.
- **I-3: A tag that itself ends in `-dirty`.** For example `v1.0.0-dirty`: the descriptor normalises to
  `1.0.0` and mismatches the declared `1.0.0-dirty`. This is a false refusal, and it fails closed.
- **I-4: GitHub check/fetch running version.** `update_fetch.c` `running_identity()` uses
  `FW_RELEASE_VERSION` (from `gen_build_info`), not the descriptor. A pre.3 board does not see
  "update available" forever because of `-dirty`.
- **I-5: Recovery image.** `KilnFW_recovery` is its own project, and its CMakeLists only includes
  `project.cmake`, so the env hook does not apply. `recovery_http.c` does no version comparison: it
  reads the descriptor and prints `hdr.semver` in its status. The recovery apply path is unaffected.
- **I-6: Dev build.** With the env var unset or empty, `PROJECT_VER` is undefined and IDF's
  git-describe path is unchanged (subject to LOW-1's stale-dir caveat).

## Are the tests real?

Yes. The `test_update_stage.c` cases (around lines 1134-1160) drive the real `upload_gated` path with
`update_stage_manifest_gate`.
- Accepted: pre.3 `-dirty` and pre.3 clean.
- Refused, each asserting VERSION_MISMATCH and nothing staged: `pre.4-dirty`, `pre.4`, `v1.0.0-dirty`,
  `pre.3-dirtyx`, `-dirt`, `-dirtz`, `-xdirty`, `v1.0.1-pre.3-dirty`.

Gaps, none of which changes a verdict:
- No uppercase `V`/`-DIRTY` case.
- No undeclared hand upload with a `-dirty` descriptor (MED-1).
- No `n == sl` boundary case (see M2 below).

## Negative tests

`tools\negtest.ps1 -Preset kilnfw-host -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES" -Parallel 4`
at c549a144c (run exit 1 only because M2 is MISSED; no REAL TREE CHANGED). The baseline passed (449.9 s). Every mutation edits `update_stage.c`. For each CAUGHT row,
the log shows the mutant compiled and `test_update_stage.c` assertions failed; none was a build failure.

| # | Mutation | Verdict | Evidence |
|---|---|---|---|
| M1 | `n -= sl;` to `n -= 0;` (no `-dirty` strip) | CAUGHT | FAIL test_update_stage.c:1141 (pre.3 `-dirty` accepted) |
| M2 | `n > sl` to `n >= sl` | MISSED (equivalent) | `-dirty` alone becomes empty, which does not parse either way; no observable change |
| M3 | suffix `strcmp` to `strncmp(..., 5)` | CAUGHT | FAIL :1157 (`-dirtz` read as `-dirty` and accepted) |
| M4 | normalise without `strip_v` | CAUGHT | FAIL :1107, :1141, :1146, :1193 and others |
| M5 | `strcmp(dn, semver)` to prefix `strncmp` | CAUGHT | FAIL :1157 (genuinely different version refused) |
| M6 | version gate disabled (`false &&`) | CAUGHT | FAIL :1133, :1157, :969/:970 |
| M7 | `desc_version_normalise` to `strcpy(dn, v)` | CAUGHT | FAIL :1107, :1114, :1141, :1146, :1193, :1195, :1203, :932 (every v-prefixed descriptor now mismatches) |

## Fix status

Fixed in the STAGEVERFX commit (see git log for the landed SHA).

| Finding | Status | Evidence |
|---|---|---|
| MED-1 | FIXED | `flush_head` undeclared branch strips v/V and one `-dirty` via `desc_version_normalise`, passes `dirty` to the gate; `policy_gate` sets `update_upload_request_t.dirty`; `update_policy_decide_upload` sets the candidate dirty so the dirty rule refuses. A differently-cased `-DIRTY`, `-dirty` alone and `v-dirty` are refused (BAD_VERSION). Tests in `test_update_stage.c` (undeclared -dirty, uppercase V, odd spellings) and `test_update_policy.c` (dirty upload refused on newer and same running version). |
| LOW-1 | FIXED | `CMakeLists.txt` stores the configure-time value in cache (`KILN_CONFIGURED_RELEASE_VERSION`); `gen_build_info.cmake` (every build) fails on env/configured mismatch in either direction. Unset env builds pass (check_00_kilnfw_target_build PASS with the var unset). |
| LOW-2 | FIXED | Configure fails on a tag over 31 characters or not semver. |

Negtest (kilnfw-host, ExpectPattern; both mutants compiled, test assertions failed):

| # | Mutation | Verdict | Evidence |
|---|---|---|---|
| M1 | `dirty = strlen(dn) < ...` to `dirty = false` | CAUGHT | FAIL test_update_stage.c:938, :944 |
| M2 | undeclared branch uses `strip_v(v)` instead of normalise | CAUGHT | FAIL test_update_stage.c:936-938, :944 |
