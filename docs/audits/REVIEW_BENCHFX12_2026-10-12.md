# Review: benchfx12 (06afd7fd9, b3fcdc5d2), 2026-10-12

Adversarial review of the dev commits that fix REVIEW_BENCHFX11 LOW-1, LOW-2, INFO-3 and INFO-4,
all in `firmware/KilnFW/App/test/test_host_refusal_pages.js`:

- LOW-1: the real `blankGuardKeys`/`blankGuardLabels` run in strict mode on an `Object.freeze`d
  array, which is then compared with the original.
- LOW-2: a lexical pin. Each `thermo_mask`/`ct_mask` push line must appear exactly once, be the
  bare statement, and the line before it must end in `;`, `{` or `}`.
- INFO-3: the comment is corrected, and every key parsed out of `ZONE_OPTIONAL_KEY_RE` must be in
  `FW_DROPPABLE` or `MUST_KEEP`.
- INFO-4: the tail `new Function` is wrapped in try/catch, so a failed extraction reports a FAIL
  and the rest of the file still runs.

Reviewed at origin/dev b3fcdc5d2 (worktree minted from origin/dev).

## Verdict

No HIGH or MED findings. Every claimed CAUGHT re-runs CAUGHT, and each log names the intended
node assertion (table A). INFO-3 and INFO-4 hold. LOW-1 holds against any mutation that runs on
every call.

The LOW-2 pin accepts a previous line ending in `{`. That lets the most natural conditional
form, `if (ctMask) {`, through, along with an early `return` and a wrapper function (LOW-1 below).
The LOW-1 frozen-array test runs only the refusal path, with blank masks, so a mutation that runs
only when the save goes ahead still passes (INFO-1). 1 LOW, 4 INFO.

## Must-check answers

1. **LOW-1 (frozen helpers): holds for unconditional mutation.** `splice`, `push`, `sort`,
   index assignment and `length =` all throw on the frozen array (B1, B1b, R8 CAUGHT on
   `blankGuardKeys/blankGuardLabels do not mutate their params argument`). Other routes:
   - A helper cannot reassign the caller's `params`, and it cannot reach the array through a
     closure, because `params` is local to the Save handler.
   - "Return a filtered array that the caller uses" and `params = params.filter(...)` in the
     caller are both call-site edits. The node test does not see them (R9 MISSED). The drift
     test's `params` whitelist does (BENCHFX11 N1). This is the existing INFO-5 split.
   - A mutation that runs only when no guard field is blank and matches a real mask value
     (`z0_ct_mask=0`) is MISSED (R7, INFO-1).
2. **LOW-2 (lexical push pin): partly closed.** Ternary value (R5), `&&` (R6), same-line `if`
   (B5) and a bare-`if` line before the push (B5b) are CAUGHT. `if (ctMask) {` block (R1), an
   early `return` between the pushes (R2), a trailing-comment `if (ctMask) // ;` (R3) and a
   wrapper function with a conditional call (R4) are MISSED. See LOW-1.
3. **Behavioural vs lexical:** a behavioural test is warranted now. `test_unsaved_guard_pages.js`
   already boots `zones_page.html` through `_page_vm.js` and clicks Save (with `zones: []`). So
   most of the DOM-stub cost has already been paid. See LOW-1.
4. **INFO-3 key list vs firmware: correct.** The page regex has 23 keys. Every one is in
   `zones_http_client.ZONE_OMIT_PRESERVED_FLOAT_KEYS`/`_NONFLOAT_KEYS` or `coupling_c\d+`, which
   the drift test pins to `zones_http_post_parse.c`. The firmware-only keys are `xzone`, `tctype`,
   `relaytype`, `zonetype`, `failsafe` and `settings_source`, plus `name` through the third idiom.
   The coverage assert is loud: an unknown key (R10), `relaytype` (R11, which is in neither list)
   and `xzone` (R12, which is in `MUST_KEEP`) all FAIL. A regex rewritten to `(?:...)` also fails,
   because the parser keeps the leading `?:` in the first key. Wording nits are in INFO-3.
5. **INFO-4 extraction failure: still FAILs.** C3 (start marker renamed) and R13 (tail does not
   parse) both print `FAIL: Save handler fetch tail parses: ...`. Then `Save posts to /api/zones`
   and the six POST-body assertions FAIL, because `posted` stays null. The file then runs to the
   end: `step 11 done ...` PASS prints, then `76 passed, 10 failed` / `77 passed, 9 failed`, and
   the exit code is 1. Nothing is skipped silently.
6. **Standing suite: yes.** `run_all_checks.ps1 -ListOnly -AllowFewerChecks` lists
   `tools\check_page_js_tests.ps1` (202 checks). That check runs every
   `firmware/KilnFW/App/test/*.js` except `lint_pages.js`, `_drivers_layout.js` and `_page_vm.js`.

## Findings

### LOW-1: the mask-push pin accepts a `{` line before it, so a block-form conditional push passes

The pin requires `/[;{}]\s*$/` on the line before each push. `{` was allowed so that the first
push after a block opener still passes. But `{` is also how every multi-line conditional opens.
These pass and drop `z<i>_ct_mask` from the real POST for any zone with no CT channel. The firmware
then omit-DEFAULTS the mask, which is the BENCHFX11 LOW-2 regression:

- R1: `if (ctMask) {` / push / `}`. This is as natural as B5, and arguably more so under a
  brace-always style.
- R2: `if (!ctMask) return;` between the thermo and ct pushes. The line before ends in `;`, and
  the `return` leaves the `forEach` callback, so every later field of that zone is dropped too.
- R4: both pushes moved into `function pushMasks() { ... }` and called as `if (ctMask) pushMasks();`.
- R3: `if (ctMask) // ;` on the line before. This one is deliberate.

Fix, recommended: a behavioural test. Reuse the `boot('zones_page.html', ...)` harness from
`test_unsaved_guard_pages.js`. Serve `/api/zones` with one zone, give its `.zone` block a stub
whose `.thermobit`/`.ctbit`/`.relaybit` `querySelectorAll` return `[]` (all masks 0) and whose
other selectors return a default `{value: '0'}` element. Click Save, then assert the POST body has
`z0_thermo_mask=0` and `z0_ct_mask=0`. A mask of 0 is the exact input every conditional form
skips, so this catches R1-R4, B5, B5b and shapes not yet thought of. The lexical alternative is to
track brace depth from `blocks.forEach(function (div) {`, require both pushes at depth 1, and
refuse any `return`/`continue`/`break`/`throw` at depth 1 before them. It closes R1/R2/R4 but
stays a list of known shapes.

### INFO-1: the frozen-array helper test covers only the refusal path

The frozen input (`z0_ct_mask=`, `z0_thermo_mask=`, `z0_driftperiod=`, `z1_xzone=`, `z0_kp=1`)
always holds blank guard fields. With blank guards the Save handler returns before posting. Its
masks are also blank, while real masks are numbers (`=0`). R7 adds
`if (!out.length) { var j = params.indexOf('z0_ct_mask=0'); if (j >= 0) params.splice(j, 1); }`
to `blankGuardLabels`. It never touches the frozen array, passes, and drops the mask on every
successful save. The mutation is deliberate, data-dependent code, so this is INFO. Fix: also run
both helpers on a second frozen array with no blank guard and realistic values
(`z0_ct_mask=0`, `z0_thermo_mask=1`, `z0_driftperiod=30`, `z0_xzone=50`, ...), and assert it is
unchanged and that the labels are `[]`.

### INFO-2: a filter in the caller before `blankGuardLabels` is still node-invisible

R9 adds `params = params.filter(... '_ct_mask=0' ...)` just before `var blankGuards = ...`. It
passes `check_page_js_tests.ps1`, because the node tail starts at `var savedZonesEditSeq`. Only
the pytest drift test's `params` whitelist catches it (BENCHFX11 N1). That test is not in
`run_all_checks.ps1` (BENCHFX11 INFO-5). The behavioural test in LOW-1 would close this as well,
since it runs the whole handler.

### INFO-3: the corrected comment still has two inaccuracies

`test_host_refusal_pages.js:56-58` says `firmware_omit_preserved_keys()` is "in
zones_http_post_parse.c". It is a helper in `tools/PcTools/tests/test_zone_omit_preserved_drift.py`
that parses that file. The firmware-only list also leaves out `name`, which is omit-preserved
through the third idiom (`ZONE_OMIT_PRESERVED_THIRD_IDIOM_KEYS`). Neither nit affects behaviour.
`MUST_KEEP` also lacks `relaytype`. That is harmless, because the coverage assert flags it if
`relaytype` is ever added to the regex (R11).

### INFO-4: the extraction marker rename also fails an unrelated test

C3 and R13 also fail `test_unsaved_guard_pages.js`, because that test executes the page. This is
fine (it is louder), but it means the C3 row's CAUGHT does not rest on this file alone. The
`Save handler fetch tail parses` line in the log confirms that this file's own assertion fired.

## Mutation table

One `tools\negtest.ps1` run: `-Preset check -PresetArg tools/check_page_js_tests.ps1
-ExpectPattern "FAIL: " -Mutations <18 mutations>`, base b3fcdc5d2. Baseline PASS,
`real_tree_unchanged=true`, `copies_removed=true`. All edits are on `zones_page.html`. B1-C3 are
the fixer's own mutations (benchfx12 `m.json`), re-run. For every CAUGHT row, the log shows a node
assertion of `test_host_refusal_pages.js` failing, and the page still parsed except in R13, whose
point is a parse failure.

| Id | Mutation | Verdict | Failing assertion (from the log) |
|----|----------|---------|----------------------------------|
| B1 | `blankGuardLabels` splices `z0_ct_mask=` from its argument | CAUGHT | `blankGuardKeys/blankGuardLabels do not mutate their params argument: Cannot assign to read only property '0'` |
| B1b | `blankGuardKeys` splices the same | CAUGHT | same |
| B5 | `if (ctMask) params.push(...ct_mask...)` | CAUGHT | `ct_mask push is a bare statement, not conditional` |
| B5b | `if (thermoMask)` on its own line before the push | CAUGHT | `thermo_mask push follows a complete statement, not a bare if/else` |
| C3 | rename `savedZonesEditSeq` (start marker gone) | CAUGHT | `found the Save handler fetch tail`, `Save handler fetch tail parses`, POST-body asserts; file completes (`76 passed, 10 failed`) |
| R1 | `if (ctMask) {` / push / `}` | **MISSED** | LOW-1 |
| R2 | `if (!ctMask) return;` between thermo and ct pushes | **MISSED** | LOW-1 |
| R3 | `if (ctMask) // ;` before the push | **MISSED** | LOW-1 (deliberate) |
| R4 | pushes moved into `function pushMasks() {...}`, `if (ctMask) pushMasks();` | **MISSED** | LOW-1 |
| R5 | push value `(ctMask ? ctMask : '')` | CAUGHT | `exactly one push of ct_mask (found 0)` |
| R6 | `ctMask && params.push(...)` | CAUGHT | `ct_mask push is a bare statement, not conditional` |
| R7 | `blankGuardLabels` splices `z0_ct_mask=0` only when `out` is empty | **MISSED** | INFO-1 |
| R8 | `blankGuardKeys` does `params.length = params.length - 1` | CAUGHT | `... do not mutate their params argument: Cannot assign to read only property 'length'` |
| R9 | `params = params.filter(...)` in the caller before `blankGuardLabels` | **MISSED** | INFO-2 (pytest drift test only) |
| R10 | regex gains `foo` | CAUGHT | `ZONE_OPTIONAL_KEY_RE key is in FW_DROPPABLE or MUST_KEEP: foo` |
| R11 | regex gains `relaytype` | CAUGHT | `ZONE_OPTIONAL_KEY_RE key is in FW_DROPPABLE or MUST_KEEP: relaytype` |
| R12 | regex gains `xzone` | CAUGHT | `omit() drops exactly the blank omit-preserved keys, ...` and `xzone is not in the optional-drop regex` |
| R13 | `if (0) { ) }` after `var savedZonesEditSeq = zonesEditSeq;` | CAUGHT | `Save handler fetch tail parses: Unexpected token ')'`, POST-body asserts; file completes (`77 passed, 9 failed`) |

B1, B1b and R8 also fail `test_web_review_fixes.js`. C3 and R13 also fail
`test_unsaved_guard_pages.js`. In every case the `test_host_refusal_pages.js` assertion above
fired as well.
