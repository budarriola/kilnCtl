# Review: STAGEVERFX2 (review of the REVIEW_STAGEVERFX fixes)

Date: 2026-10-13. Reviewer: Claude Opus 5.5 (adversarial review, no code changes).

Commits reviewed on origin/dev:

- d5814f5b3: the code fix (check_00 release-version reconfigure, declared-path dirty derivation, tests, CMakeLists regex).
- 49c723ce8: the Fix status section of `docs/audits/REVIEW_STAGEVERFX.md`.

Claims under review:

| Finding | Claimed fix |
|---|---|
| MED-1 | `check_00_kilnfw_target_build.ps1` reconfigures when the cached `KILN_CONFIGURED_RELEASE_VERSION` differs from `KILNCTL_RELEASE_VERSION` |
| LOW-1 | the declared upload path derives `dirty` from the descriptor and refuses a non-lowercase `-DIRTY` spelling |
| LOW-2 | `update_http.c`'s `s_gate.ur.dirty = dirty` is covered by a test |
| INFO | the `CMakeLists.txt` release regex is aligned with `gen_build_info.cmake` |

## Summary

| Severity | Count |
|---|---|
| HIGH | 0 |
| MED | 1 |
| LOW | 1 |
| INFO | 7 |

The four fixes are correct for what they claim. MED-1 below is not a regression from these commits. It is a gap in the dirty rule they extend: an ordinary development build's descriptor no longer ends in `-dirty` once ESP-IDF cuts it to 31 characters, so the image is accepted as a clean prerelease. LOW-1 is a missing boundary test on the new declared-branch refusal (negtest N1 MISSED).

## Verification of each claimed fix

### MED-1 (check_00 stale release version): correct

`firmware/KilnFW/App/test/check_00_kilnfw_target_build.ps1:956-967` reads `KILN_CONFIGURED_RELEASE_VERSION` from `CMakeCache.txt` and forces `$sdkconfigStale = $true` when it differs from the env. Cases checked by reading:

| Case | Cached | Env | Result |
|---|---|---|---|
| env unset | `""` (or missing entry) | `""` | no reconfigure (correct) |
| env empty string | any `""` | `""` (`if ($env:...)` is false for empty) | no reconfigure (correct; CMakeLists treats empty as unset too, `CMakeLists.txt:25`) |
| env set, build dir from before the fix (no cache entry) | `""` | `v1.2.3` | reconfigure (correct) |
| env changed between runs | `v1.2.3` | `v1.2.4` | reconfigure (correct) |
| env removed after a release build | `v1.2.3` | `""` | reconfigure (correct; gen_build_info would otherwise be FATAL) |
| no `CMakeCache.txt` | n/a | any | first configure runs anyway (correct) |

The marker write (`check_00_kilnfw_target_build.ps1:1248`) stores only the sdkconfig hash. That is fine, because the next run re-reads the cache entry rather than the marker. No stale cache survives a reconfigure: the reconfigure reruns `CMakeLists.txt:22-45`, which sets the cache entry with `FORCE`.

The fixer's "proven with real builds" claim was not re-run (full target builds are out of scope for this review).

### LOW-1 (declared path dirty): correct, one boundary untested

`firmware/KilnFW/App/drivers/update/update_stage.c:296-308` normalises the descriptor, sets `dirty = strlen(dn) < strlen(strip_v(v))` (line 299), and refuses a case-variant `-DIRTY` that normalisation did not strip (line 302) with `UPDATE_STAGE_ERR_BAD_VERSION`, before the version-mismatch check and before the gate. The undeclared branch (lines 309-327) is unchanged in shape.

Regression checks:

- **Release image refused as dirty:** no. A clean descriptor (`v1.0.0-pre.3`) gives `dn` equal to `strip_v(v)`, so `dirty` is false (test `test_update_stage.c:973`, and negtest N3 proves the `strip_v` is load-bearing).
- **Published pre.3 image (`v1.0.0-pre.3-dirty`) fetched from GitHub:** still fetchable. The manifest gate ignores `dirty` (`update_stage.c:433-435`), so the declared-path dirty flag only matters for hand uploads that go through `update_http.c`'s `policy_gate`. This is unchanged and intended (the manifest sha256 binds the bytes).
- **Dirty image accepted when declared:** closed for a descriptor that actually ends in `-dirty` (`test_update_stage.c:961-968`, `test_update_http_refusals.c:326`). Not closed for a truncated descriptor; see MED-1 below.
- **A GitHub release whose descriptor ends in `-DIRTY` (upper case):** now refused as BAD_VERSION on the fetch path too. This fails closed, and no such release exists.

### LOW-2 (update_http dirty assignment tested): correct

`firmware/KilnFW/App/test/test_update_http_refusals.c:308-329` drives a `v1.2.3-dirty` image through the HTTP stage route three ways (undeclared, forced, declared `1.2.3`) and expects 409 `refuse_dirty` and nothing staged. Negtest N5 (force overriding dirty inside `update_policy.c`) is caught by `test_update_http_refusals.c:322` and `test_update_policy.c:315`, `:322`.

### INFO (CMakeLists regex aligned): overclaimed, fails closed

See INFO-1.

## Findings

### MED-1: a real development build's `-dirty` suffix is cut off by ESP-IDF, so the dirty rule never sees it

Pre-existing, not introduced by d5814f5b3, but it answers the review question "can a dirty image be accepted".

ESP-IDF v6.0.2 cuts `PROJECT_VER` to 31 characters before writing `esp_app_desc_t.version` (`components/esp_app_format/CMakeLists.txt:28-31`, `string(SUBSTRING "${project_ver}" 0 31 PROJECT_VER_CUT)`). Without `KILNCTL_RELEASE_VERSION`, `PROJECT_VER` is `git describe --always --tags --dirty`. On current dev that is `v1.0.0-pre.1-1661-g49c723ce8` (28 characters), so a dirty tree gives `v1.0.0-pre.1-1661-g49c723ce8-dirty` (34 characters), and the descriptor holds `v1.0.0-pre.1-1661-g49c723ce8-di`.

That descriptor:

- does not end in `-dirty`, so `desc_version_normalise()` strips nothing and `dirty` stays false (`update_stage.c:299`, `:314`);
- does not end in any case of `-dirty`, so the `ci_dirty_suffix()` refusal does not fire (`update_stage.c:302`, `:321`);
- parses as a valid semver prerelease (`1.0.0-pre.1-1661-g49c723ce8-di`). Its second prerelease identifier is alphanumeric, so it sorts above any numeric `pre.N`, and the policy sees an upgrade.

So an undeclared hand upload of any development build from a dirty tree is staged as a clean, newer prerelease. Any describe string of 26 characters or more loses its suffix this way, and that is the normal case once the commit count since the last tag reaches four digits. A clean development build (`...-g49c723ce8`) is also accepted, which is the existing intended behaviour for a non-release image. Only the dirty rule is defeated.

Recommendation: either stamp a short non-release `PROJECT_VER` that keeps the `-dirty` suffix within 31 characters (for example, drop the tag prefix and use `<short-sha>-dirty`), or treat a 31-character descriptor whose shape is a git-describe output (`-<n>-g<hex>`) as unknown provenance and refuse it as dirty unless declared. Add a host test using a real 31-character truncated descriptor.

### LOW-1: no test pins the declared-branch `dl >= 6u` boundary

`update_stage.c:302` refuses when `dl >= 6u && ci_dirty_suffix(dn + dl - 6u)`. Every declared-branch test uses a descriptor longer than 6 characters (`test_update_stage.c:961-985`). The undeclared refusal list (`test_update_stage.c:946`) has `"-dirty"` and `"v-dirty"`, but the declared branch has no `"-DIRTY"` or `"v-DIRTY"` case.

Negtest N1 (`dl >= 6u` changed to `dl > 6u` in the declared branch) was MISSED: the mutant compiled and every host test passed. With that mutant, a descriptor of exactly `-DIRTY` declared as some version would skip the refusal. It is caught later only because the version-mismatch check then refuses it, so the practical impact is small. The point is that the declared refusal as written has no boundary pin. Add declared cases for `-DIRTY`, `v-DIRTY`, `-Dirty` and `x-dirty-DIRTY`.

### INFO-1: the CMakeLists regex does not fully "match" gen_build_info.cmake

`firmware/KilnFW/CMakeLists.txt:33` accepts `^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$` with a 31-character cap. It does not apply gen_build_info.cmake's reject list or its 9-digit core cap. Configure therefore accepts `v1.2.3-01`, `v1.2.3-rc..1`, `v1.2.3-.`, `v1.2.3-rc.01` and `v1234567890.0.0`, all of which gen_build_info then refuses at build time. This fails closed (the build still stops), so it is not a defect, but the Fix status's "matches" is an overclaim. `tools/check_release_version_regex.ps1` pins only gen_build_info.cmake against the `update_tag_valid()` host test (22 tags agree), not CMakeLists.txt.

### INFO-2: the reconfigure messages still talk only about the sdkconfig hash

When only the release version changed, `check_00_kilnfw_target_build.ps1:970` prints that the build directory "was last reconfigured against sdkconfig hash X; the sdkconfig just copied in hashes X", and the failure text at line 976 names the sdkconfig hashes too. The preceding release-version line (`:963`) does say what really changed, but the follow-on text reads as if the hashes differ. Word the block to name the actual reason.

### INFO-3: stale `update_policy.h` comment

`firmware/KilnFW/App/drivers/update/update_policy.h:155` still says `dirty` is set for an "(undeclared upload)". Since d5814f5b3 the declared path sets it too.

### INFO-4: mislabelled finding IDs in the new tests

`test_update_stage.c:961` says "STAGEVERFX2 LOW-1" and `test_update_http_refusals.c:308` says "STAGEVERFX2 LOW-2". The findings are REVIEW_STAGEVERFX LOW-1 and LOW-2.

### INFO-5: REVIEW_STAGEVERFX.md Fix status has a pre-rebase SHA and mangled text

- `docs/audits/REVIEW_STAGEVERFX.md:180` and `:187` cite 03490dd26, the pre-rebase commit. The landed code commit is d5814f5b3.
- Lines 191-193 contain a form-feed byte where `\f` was typed: "-> \x0Calse" should read "-> false". `check_source_bytes.ps1` does not flag it in a `.md` file.

### INFO-6: build_kilnfw MCP tool has no release-version reconfigure

The `build_kilnfw` MCP tool builds in the shared `firmware/KilnFW/build` directory and does not compare the cached `KILN_CONFIGURED_RELEASE_VERSION` with the env. A mismatch there still fails the build loudly in gen_build_info.cmake, so this fails closed. Release builds go through `tools/make_release.ps1`, so the risk is low.

### INFO-7: landing without -CheckLog hid nothing found here

The fixer landed without a full `run_all_checks` log. Re-run at 49c723ce8 for this review, the checks most likely to be affected all pass: `check_release_version_regex.ps1` (gen_build_info.cmake is unchanged), `check_doc_hash_citations.ps1` (the pre-rebase SHA in INFO-5 still resolves), `check_doc_citations.ps1` and `check_source_bytes.ps1`. The kilnfw-host baseline in the negtest below also passed. Nothing in this review depended on a check that only the full suite runs, though the full suite was not run.

## Negtest

The command was `tools\negtest.ps1 -Preset kilnfw-host -Mutations <json> -Parallel 2 -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`, run on base 49c723ce8. The baseline passed, the real tree was unchanged and the copies were removed.

Every CAUGHT row below compiled (its log has no `BUILD FAILURES`) and failed on a test assertion.

| ID | Mutation | Verdict | Evidence |
|---|---|---|---|
| N1 | `update_stage.c` declared branch `dl >= 6u` -> `dl > 6u` | MISSED | exit 0, all host tests pass (LOW-1) |
| N2 | `ci_dirty_suffix` case fold `c = (char)(c - 'A' + 'a');` -> `c = c;` | CAUGHT | FAIL `test_update_stage.c:953`, `:979` |
| N3 | declared branch `strlen(strip_v(v))` -> `strlen(v)` | CAUGHT | FAIL `test_update_stage.c:973` (clean declared image read as dirty) |
| N5 | `update_policy.c` `cand_s.dirty = req->dirty;` -> `req->dirty && !req->force` | CAUGHT | FAIL `test_update_policy.c:315`, `:322`; `test_update_http_refusals.c:322`, `:327` |

There is no N4: it was dropped while the mutation set was drafted.

## Fix status

Fixed in 48010bf4a (rebased onto origin/dev). Targeted host tests pass.

| Finding | Status |
|---|---|
| MED-1 | fixed: `firmware/KilnFW/CMakeLists.txt` composes a dev PROJECT_VER at configure time so the "-dirty" form fits in 31 chars, and fails the configure if it cannot |
| LOW-1 | fixed: tests for the `dl >= 6u` boundary and the truncated-dirty case |
| INFO (stale comment, labels, form feed, pre-rebase SHA) | fixed in `update_policy.h`, the tests and `REVIEW_STAGEVERFX.md` |

Negtest: shortening `ci_dirty_suffix` to 5 chars is CAUGHT (`test_update_stage.c:1229`). The `dl >= 6u` boundary was not negtested separately (both sites share identical text, so a unique find string was not available); open.
