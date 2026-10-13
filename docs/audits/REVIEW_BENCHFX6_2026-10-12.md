# Review: benchfx6 (ee5163d66, f0b9b45d7), 2026-10-12

Scope: the two commits on origin/dev that answer REVIEW_BENCHFX5 MED-1 and LOW-1.

- `ee5163d66` adds the host test `test_post_omitting_five_float_fields_preserves_stored_values` in `firmware/KilnFW/App/test/test_zones_http.c`. It also reworks `tools/PcTools/tests/test_zone_omit_preserved_drift.py`, adding the else-if, compound-assignment and cross-block rules and the `_ALLOWED_RHS` exemptions.
- `f0b9b45d7` adds the fix-status table to `docs/audits/REVIEW_BENCHFX5_2026-10-12.md`.

Reviewed at f0b9b45d7, in worktree `C:\wt\rvbfx6_83rxq3`.

## Verdict

There are no HIGH or MED findings. MED-1 and LOW-1 are closed in substance:

- Every omit-preserved field in `zones_http_post_parse.c` now has a host test that fails when the omit path stops preserving it.
- Every escape mutation built for this review was caught by the host tests.

The drift test is a weaker second layer than its commit message claims. It missed all seven escape mutations (Y1-Y7), and three of its own rules have no self-test. That is LOW, because the host tests are the layer that catches these.

## Findings

### HIGH

None.

### MED

None.

### LOW-1: drift test misses seven escape shapes, and its commit message overstates the coverage

The commit message says the drift test "rejects plain assignments outside the field's own else". That is only true for assignments after the field's omit branch and for fields with a simple identifier RHS. Each mutation below passed the drift test (MISSED), and each was CAUGHT by the host tests.

| Id | Mutation | Why the drift test misses it |
|----|----------|------------------------------|
| Y1 | `z->tc_type = preserved_tc_type & 0u;` in the `i >= thermo_count` early return | `_ALLOWED_RHS` is a prefix match on the first RHS identifier only (line 74) |
| Y2 | new `else if (g == SRC_GROUP_GUARDS) { src_raw = ZONE_SETTINGS_SOURCE_CUSTOM; }` before `else if (have_legacy)` | the else-if rule is skipped for the whole field when it is in `_ALLOWED_RHS` (line 64), and the write goes through `src_raw` |
| Y3 | `memset(&z->error_band_c, 0, ...)` after the coupling memcpy | the pattern only matches `z->field =` |
| Y4 | `(*z).coil_power_w = 0.0f;` | the pattern only matches `z->` |
| Y5 | `z->coupling_coeff[(i + 1u) % MAX31856_CHANNEL_COUNT] = 0.0f;` | the index pattern is `\[\w+\]` |
| Y6 | `if (current_z->zone_type == 0u) { z->rate_band_c_per_s = 0.0f; } else {` wrapping the rateband block | a plain assignment *before* the omit branch is flagged only for `_ALLOWED_RHS` fields (lines 78-81) |
| Y7 | `{ float *p_alias = &z->progress_band_c; *p_alias = 0.0f; }` | pointer alias |

Note on Y2: the settings_source exemption is not narrow. It exempts the whole field from the else-if rule, not only the known `else if (have_legacy)` arm. The host test asserts GUARDS==1 (test_zones_http.c:7185/7220), and that is what catches Y2.

A text scan cannot close Y3, Y4, Y5 and Y7 in general, and the host tests already close them. The suggested fix is small:
- Correct the docstring/commit claim.
- Make Y1 and Y6 fail. Require the RHS to be exactly an allowed identifier followed by `;`. Flag a plain before-span assignment for every field unless it is a known initialiser.
- Narrow the else-if exemption to the one literal `else if (have_legacy)` arm.

### LOW-2: three drift-test rules have no self-test

Mutating the drift test itself (`-Preset pytest`, its own file) gave these results:

| Id | Rule disabled | Result |
|----|---------------|--------|
| P1 | compound rule (`if m.group(1)`) | MISSED |
| P2 | else-if rule | CAUGHT |
| P3 | later-assignment rule | CAUGHT |
| P4 | exempt before-span rule | MISSED |
| P5 | whole-file scan narrowed to 400 chars after the span | CAUGHT |
| P6 | allowed-RHS widened to any identifier | MISSED |

- **P1:** the `*=` self-test is also flagged by the later-assignment rule, so the compound rule is never the only thing that fires. A compound assignment placed *before* the omit branch would show the difference.
- **P4:** no self-test places an exempt-field assignment before its span.
- **P6:** the only bad-RHS self-test uses `5u`, which is not an identifier.

Fix: add three synthetic self-test cases: a before-span `*=`, a before-span `z->tc_type = other;`, and a `z->tc_type = other_ident;` after the span.

### LOW-3: zones page sends blank coilpower/hystc/minons/minoffs, which the firmware refuses with a 400

Blank-means-400 is intended and documented:
- `zones_config_json.c:788-805`: "Present-but-unparseable is an error; only a genuinely absent key (-1) is an omission."
- The page comment (zones_page.html around 2705).

`ZONE_OPTIONAL_KEY_RE` (zones_page.html:2748) drops blank `approachratecap`, `errorband`, `rateband` and `progressband`, so for those four fields a cleared input means "keep stored". It does not cover `coilpower` (pushed at 2882) or `hystc`/`minons`/`minoffs` (2853-2855). For those fields a cleared input gets a 400. For coilpower the 400 carries a range message ("coil_power_w out of range (0 = use an equal share...)"), which tells the user to type 0, not that the field was blank. This is a refusal, not a silent reset, so it is safe. It is inconsistent with the other four fields of the same group.

The page comment at 2705-2711 is also stale. It says only the guard thresholds and xzone keep the stored value on omit; many more fields now do.

Fix: either add `coilpower` (and, if wanted, the on/off timing keys) to `ZONE_OPTIONAL_KEY_RE`, or leave the refusal and refresh the comment.

### INFO

- **INFO-1:** test_zone_omit_preserved_drift.py:78 `elif field in _ALLOWED_RHS or m.start() >= 0:` is always true at that point. The inner `if` repeats the membership test, so the branch is dead weight. The comment above line 80 ("only exempt fields have legitimate ones") states the opposite of what the code does. The code flags exempt fields and passes everything else; see LOW-1 Y6.
- **INFO-2:** the new host test checks that a blank `z0_<key>=` gives `!ok`, but it does not pin the err_reason. A refusal for an unrelated reason would also pass.
- **INFO-3:** the `zones_config_json.c` comment attributes "refuse at the door" to PID_EXPANSION_PLAN Phase 4. That phrase is not in `firmware/KilnFW/docs/PID_EXPANSION_PLAN.md` verbatim.
- **INFO-4:** the BENCHFX5 fix-status table's claims check out. X1-X5 are each CAUGHT by both the host tests and the drift test, below.
- **INFO-5 (negtest hygiene):** under `-Parallel 4` the first X2 host run scored CAUGHT only through `BUILD FAILURES`: `fatal error C1083: Cannot open compiler generated file ... cfg_fs_status.obj: Permission denied`. That is a file-lock flake, not a compiled mutant. A serial rerun compiled and failed on real assertions (test_zones_http.c:6777, 6788).
  - The Y2 log also has an unrelated `LNK1561` alongside its two real assertion failures.
  - This is the case the 2026-10-12 NEGTEST NOTE warns about. Every CAUGHT below was checked in its log.

## Verification

All runs used `tools\negtest.ps1`:
- Host tests: `-Command "...build_host_tests.ps1 -OutDir {OUT} -Only zones"` with the kilnfw-host ExpectPattern `(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES`.
- Drift test: `-Preset pytest -PresetArg tests/test_zone_omit_preserved_drift.py`.

The baselines passed in every run. Every run reported `real_tree_unchanged:true` and `copies_removed:true`.

**Host tests, 41 mutations, all CAUGHT.** In each case the mutant compiled and a `  FAIL test_zones_http.c:<line>` assertion fired. X2 was confirmed by the serial rerun.

- X1-X5 (BENCHFX5 definitions; X4 is the X4b `5u` form):
  - X1: line 6776
  - X2: lines 6777 and 6788
  - X3: line 6780
  - X4: line 1279
  - X5: line 7023
- Y1-Y7:
  - Y1: line 1279
  - Y2: lines 7185 and 7220
  - Y3: line 6777
  - Y4: line 6780
  - Y5: line 7018
  - Y6: line 6778
  - Y7: line 6779
- Sweep (29 mutations): for every omit-preserved field the omit branch returned `stored + 1`, and every one was CAUGHT. The fields were tc_type, relay_type, zone_type, failsafe_state, hyst_c, min_on_s, min_off_s, coil_power_w, the 8 guards, cross_zone_max_delta_c, model k/tau/deadtime, fuzzy_strength_pct, coupling_diag_k_dc, ease_off_window_mult, approach_rate_cap_c_per_hr, error_band_c, rate_band_c_per_s, progress_band_c, coupling_coeff[j] and settings_source[g]. No omit-preserved field is uncovered. `name` uses a separate idiom and its existing tests were not re-mutated here.

**Drift test, firmware mutations:**
- X1-X5: CAUGHT, each with its own message from `test_firmware_has_no_later_overrides`.
- Y1-Y7: all MISSED (LOW-1).

**Drift test, self-mutations:** P2, P3 and P5 CAUGHT; P1, P4 and P6 MISSED (LOW-2).

The mutation JSON is kept outside the repo in the reviewer's scratch directory (`rvbfx6/fw.json`, `py_fw.json`, `py_self.json`, `x2.json`).
