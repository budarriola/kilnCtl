# Review: benchfx13 (9475f0188, 994c7f549, 8cdaa9b75, 9cd3f47dd), 2026-10-12

Adversarial review of the dev commits that fix REVIEW_BENCHFX12 LOW-1 and INFO-1..3:

- `firmware/KilnFW/App/test/test_zones_save_masks.js` (new): boots `zones_page.html`'s inline
  script through `_page_vm.js`, clicks Save with one all-unchecked zone and asserts the POST
  carries `z0_thermo_mask=0`, `z0_ct_mask=0` and `z0_relay_mask=0`.
- `test_host_refusal_pages.js`: the blank-guard helpers also run on a frozen, no-blank array.
- `tools/check_zone_omit_preserved_drift.ps1` (new): runs
  `tools/PcTools/tests/test_zone_omit_preserved_drift.py` in the standing suite.
- `docs/audits/GATE_NEGATIVE_TEST_EVIDENCE.md`: a row for the new check.

Reviewed at origin/dev 9cd3f47dd in a worktree minted from origin/dev.

## Verdict

No HIGH or MED findings. 1 LOW, 5 INFO.

The JS test drives the page's real Save handler (J1-J4, J7 CAUGHT, each by a node `FAIL:`
assertion). The new check is registered and never SKIPs. The evidence row matches a re-run (D2).
The drift check does not pin which omit-preserved keys are stripped. Moving `tau` or `hystc` out
of the stripped float list passes the standing suite. The only tests that catch it are pytest
files that `run_all_checks.ps1` does not run (LOW-1).

## Must-check answers

**(a) The JS test drives the page's own code.** It reads `zones_page.html` from the drivers
directory and takes the last `<script>` block, which is the page's inline script (line 591). It
runs that block in `runPageScript` and fires the real `saveBtn` click handler
(`test_zones_save_masks.js:31-60`). Nothing is copied. The stubs only supply DOM nodes, and the
mask checkbox lists are empty. A conditional push of any of the three masks (J1-J3), or an early
return when all masks are 0 (J4), fails the matching `POST body carries ...` assertion. One limit:
the masks are only ever 0, so the bit computation from checked boxes is never exercised (INFO-2).

**(b) The check fails on real drift, with gaps.**
- The fixer recorded that emptying `ZONE_OMIT_PRESERVED_THIRD_IDIOM_KEYS` was MISSED. That is
  confirmed (D1), and so is renaming it to `("bogus",)` (D7). The tuple is checked against
  nothing in the firmware. `firmware_omit_preserved_keys()` never classifies `name`: its
  `http_form_find_field` idiom does not match `_PRESERVE_RE` (`test_zone_omit_preserved_drift.py:30`).
  The tuple's only use is one loop iteration at line 174. That is not a behavioural gap, because
  line 300 separately pins that `ZONE_OMIT_PRESERVED_KEY_RE` never matches `z1_name`. So emptying
  the tuple cannot make any writer strip `name` (INFO-1).
- If the firmware blanks `name` on omit (D6), this check misses it. The backstop is the KilnFW
  host test at `test_zones_http.c:1278` ("name preserved when the submission omits z1_name").
  That backstop was not negtested in this review.
- **The float/non-float partition is not pinned.** `python_omit_preserved_keys()` takes the
  union of the two lists (`test_zone_omit_preserved_drift.py:153-155`). Moving a key from
  `ZONE_OMIT_PRESERVED_FLOAT_KEYS` to `ZONE_OMIT_PRESERVED_NONFLOAT_KEYS` therefore leaves the
  union unchanged. `test_regex_matches_exactly_the_float_keys` follows the lists, so it does not
  fail either. See LOW-1 and INFO-3.

**(c) Registration.** `run_all_checks.ps1 -ListOnly` lists
`tools\check_zone_omit_preserved_drift.ps1`. The script has no SKIP path. With no interpreter it
prints FAIL and exits 1. A failed import, an error or zero tests run (Python 3.14 exits 5) all
give a nonzero exit, so the check FAILs. The interpreter is resolved through
`lib_pctools_python.ps1`, and `PYTHONPATH` points at the tree's own `src`, so a worktree tests its
own `zones_http_client.py`. On this worktree the check runs and PASSes. `check_gate_negative_test_table.ps1`
also passes: 203 discovered checks, 204 rows.

**(d) The evidence row is accurate.** Row `GATE_NEGATIVE_TEST_EVIDENCE.md:184` says dropping
`"debounce"` from `ZONE_OMIT_PRESERVED_FLOAT_KEYS` gives RED. Re-run as D2, it is CAUGHT by two
assertions:
- an `AssertionError` in `test_guard_keys_stripped_from_body`
  (`'z0_debounce=3&z0_kp=1&z0_zonetype=1' != 'z0_kp=1&z0_zonetype=1'`);
- the set-difference assertion in `test_python_lists_match_firmware`, which names the key.

The counts (192 NEGATIVE-TESTED of 204) agree with the table check. The row does not record the
MISSED tuple mutation (D1), which is the weakness found while writing it. Other rows in the table
record such weaknesses (INFO-5).

## Findings

### LOW-1: the drift check does not pin which omit-preserved keys are stripped

`tools/PcTools/tests/test_zone_omit_preserved_drift.py:153-155`,
`tools/PcTools/src/kilnctrl/zones_http_client.py:1387-1396`.

`ZONE_OMIT_PRESERVED_KEY_RE` is built from `ZONE_OMIT_PRESERVED_FLOAT_KEYS` alone. It decides what
`strip_omit_preserved()` and `mcp_server_control._strip_omit_preserved_zone_fields()` drop from
narrow GET-merge-POST writes. Moving a key into the non-float list (D3: `tau`, D4: `hystc`) leaves
the drift check green. After that move, narrow writers such as `control_set_zone_coupling`
re-post `tau` from GET's `%.1f` print (`zones_http_get.c:468`), and the stored value is
rounded. That rounding is exactly what the strip exists to prevent.

`test_mcp_server_control_zone_coupling.py`, `test_mcp_server_control_relay_type.py` and
`test_pctools_write_review_2026_10_09.py` do catch D3 and D4. None of them runs in
`run_all_checks.ps1`, so the standing suite misses both moves.

Fix, either of:
- have `check_zone_omit_preserved_drift.ps1` also run those three files;
- pin the float set in the drift test, either as a frozen literal or derived from the
  `%.<n>f` printf formats in `zones_http_get.c`.

### INFO-1: `ZONE_OMIT_PRESERVED_THIRD_IDIOM_KEYS` is hand-kept and unpinned

`zones_http_client.py:1404`, `test_zone_omit_preserved_drift.py:174`. Emptying or renaming the
tuple passes (D1, D7). The behaviour it documents ("never strip name") is pinned elsewhere, at
line 300. A new third-idiom key added to the firmware would also go unnoticed. Low value to fix.
Either drop the tuple, or parse `http_form_find_field(body, key` keys out of
`zones_http_post_parse.c` and compare them with the tuple.

### INFO-2: the Save test never exercises nonzero masks

`zones_page.html:2815`. Making `ctMask` ignore `cb.checked` (J6) passes the whole page-js
suite, because the stub zone has no checkboxes. Replacing `ctMask` with a literal `0` (J5) is
caught only by the lexical pin in `test_host_refusal_pages.js`, not by the new behavioural test.
BENCHFX12 LOW-1 asked only for the mask-0 path. A second stub zone with one checked `.ctbit`
(`dataset.bit = '1'`), asserting `z1_ct_mask=2`, would close this.

### INFO-3: a non-float key moved into the float list goes unnoticed

D5 moves `failsafe` into `ZONE_OMIT_PRESERVED_FLOAT_KEYS`. It is MISSED by this check and by the
three pytest files above. The effect is benign: the firmware omit-preserves `failsafe`, so
stripping it from a narrow write keeps the stored value. The LOW-1 fix (pinning the float set)
covers this too.

### INFO-4: two inaccurate references

- `test_host_refusal_pages.js:57-59` says `name` is "parsed out of zones_http_post_parse.c by
  firmware_omit_preserved_keys()". That helper does not classify `name` (see (b)). `name` is
  omit-preserved through `http_form_find_field` and is listed only by hand.
- `REVIEW_BENCHFX12_2026-10-12.md` "Fixed (benchfx13, ef9e52628)" names a pre-rebase SHA that
  no remote branch contains. The landed commit is 9475f0188.

### INFO-5: the evidence row omits the MISSED mutation

`GATE_NEGATIVE_TEST_EVIDENCE.md:184` records only the CAUGHT mutation. The first version of the
row (8cdaa9b75) claimed the `name` tuple mutation was RED. 9cd3f47dd replaced it rather than
recording it as a MISSED, unpinned weakness, which is how other rows record one. Consider
appending "THIRD_IDIOM_KEYS emptied: PASSED (unpinned by design, see REVIEW_BENCHFX13 INFO-1);
FLOAT->NONFLOAT move: PASSED (LOW-1)".

## Negtest table

Every run: `tools\negtest.ps1`, baseline PASS, `real_tree_unchanged: true`, copies removed.
Every CAUGHT below comes from a failing assertion: a Python `AssertionError`, a pytest
`FAILED <test>` or a node `FAIL: <assertion>`. None of them is a crash or a compile error.

**Drift check** (`-Preset check -PresetArg tools/check_zone_omit_preserved_drift.ps1`,
`-ExpectPattern AssertionError`):

| ID | Mutation | Verdict | Failing assertion |
|---|---|---|---|
| D1 | `ZONE_OMIT_PRESERVED_THIRD_IDIOM_KEYS = ("name",)` -> `()` | MISSED | none (INFO-1) |
| D2 | `ZONE_OMIT_PRESERVED_FLOAT_KEYS`: drop `"debounce"` (the evidence row's mutation) | CAUGHT | `test_guard_keys_stripped_from_body`; set diff in `test_python_lists_match_firmware` |
| D3 | move `tau` from FLOAT to NONFLOAT | MISSED | none (LOW-1) |
| D4 | move `hystc` from FLOAT to NONFLOAT | MISSED | none (LOW-1) |
| D5 | move `failsafe` from NONFLOAT to FLOAT | MISSED | none (INFO-3) |
| D6 | `zones_http_post_parse.c`: name omit branch `strncpy(z->name, current_z->name, ...)` -> `z->name[0] = '\0';` | MISSED | none; backstop `test_zones_http.c:1278`, not run here |
| D7 | `THIRD_IDIOM_KEYS = ("bogus",)` | MISSED | none (INFO-1) |

**Related pytest files** (`-Preset pytest`, the files named in LOW-1; these are not in the
standing suite):

| ID | Verdict | Failing tests |
|---|---|---|
| D3 | CAUGHT | `test_mcp_server_control_zone_coupling.py::EndToEndRealBodyTest::test_tool_posts_only_the_target_cell_in_bench_orientation`, `test_mcp_server_control_relay_type.py::HappyPathTest::test_post_carries_exactly_one_relay_key_and_strips_omit_preserved` |
| D4 | CAUGHT | `test_pctools_write_review_2026_10_09.py::ZoneStripTests::test_regex_covers_hystc_and_coilpower`, `::test_strip_keeps_named_keys_only` |
| D5 | MISSED | none |

**Page-js check** (`-Preset check -PresetArg tools/check_page_js_tests.ps1`, per-mutation
expect patterns):

| ID | Mutation (`zones_page.html`) | Verdict | Failing assertion |
|---|---|---|---|
| J1 | `if (ctMask) params.push(..._ct_mask...)` | CAUGHT | `FAIL: POST body carries z0_ct_mask=0` |
| J2 | `if (thermoMask) params.push(..._thermo_mask...)` | CAUGHT | `FAIL: POST body carries z0_thermo_mask=0` |
| J3 | `if (mask) params.push(..._relay_mask...)` | CAUGHT | `FAIL: POST body carries z0_relay_mask=0` |
| J4 | `if (!mask && !thermoMask && !ctMask) return;` before the name push | CAUGHT | all three `POST body carries` assertions |
| J5 | `'_ct_mask=' + ctMask` -> `'_ct_mask=' + 0` | CAUGHT | `FAIL: exactly one push of ct_mask (found 0)` (lexical pin only; INFO-2) |
| J6 | `if (cb.checked) ctMask \|=` -> `ctMask \|=` | MISSED | none (INFO-2) |
| J7 | `'_ct_mask=' + (ctMask \|\| '')` | CAUGHT | lexical pin, plus `FAIL: POST body carries z0_ct_mask=0` |

Direct runs: `check_zone_omit_preserved_drift.ps1` PASS; `node test_zones_save_masks.js` 4/4;
`node test_host_refusal_pages.js` 88/88; `check_gate_negative_test_table.ps1` OK.
