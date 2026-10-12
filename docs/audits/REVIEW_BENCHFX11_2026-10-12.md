# Review: benchfx11 (2492fb3c4, 0e260bd02), 2026-10-12

Adversarial review of the dev commits that fix REVIEW_BENCHFX10 LOW-1/2 and INFO-1/2/3:

- `tools/PcTools/tests/test_zone_omit_preserved_drift.py`: lexical whitelist of every `params`
  use in the Save handler (declaration, `.push(`, the `blankGuardLabels` call, the one
  `omitBlankOptionalParams` call, `body: params.join('&'),`); exactly one `fetch('/api/zones',
  {method: 'POST'`; the helpers are not reassigned; `ZONE_OPTIONAL_KEY_RE` appears only in its
  declaration and `omitBlankOptionalParams`, comments excluded.
- `firmware/KilnFW/App/test/test_host_refusal_pages.js`: generated `omit()` check over zones 0..2,
  every key, blank and set values, against a hand list (`FW_DROPPABLE` / `MUST_KEEP`); and a
  behavioural test that runs the real Save-handler tail (from `  var savedZonesEditSeq` up to
  `}).then(function (r) {`) against a mocked `fetch` and checks the POST body.
- Status lines in REVIEW_BENCHFX9 and REVIEW_BENCHFX10.

Reviewed at origin/dev 0e260bd02 (worktree minted from origin/dev).

## Verdict

No HIGH or MED findings. The fix does what it claims. I re-ran N1, N2, N3 and N5 through the
drift test and confirmed the assertion message in each failing log (table A). The node test is
in the standing suite, and a mutant in the Save tail fails `check_page_js_tests.ps1` itself, not
just `node` (table C). If the tail extraction breaks, the test fails loudly. It does not pass
vacuously (C3, C4).

Six edits still drop a mask entry from the real POST and pass both tests (table B). Two are
plausible-looking edits (LOW-1, LOW-2). The rest are deliberate `window[...]`/redeclaration/XHR
tricks of the same class as BENCHFX10 INFO-1 (INFO-1, INFO-2). 2 LOW, 5 INFO.

## Must-check answers

1. **Node test in the standing suite: yes.** `tools/check_page_js_tests.ps1` runs every
   `firmware/KilnFW/App/test/*.js` except three named helpers. `run_all_checks.ps1 -ListOnly`
   lists it (202 checks discovered). Mutants C1-C4 via `-Preset check -PresetArg
   tools/check_page_js_tests.ps1` are CAUGHT on the check's own `FAIL: test_host_refusal_pages.js`
   line, and each log shows the node assertion that failed.
2. **Tail extraction is robust (fails loud).** If the start marker is renamed (C3), the run prints
   `FAIL: found the Save handler fetch tail`, then `new Function` throws a SyntaxError and the
   run exits 1 (see INFO-4). If the omit call moves before the marker (C4), the body keeps the
   blank optional fields and three `POST body drops blank optional` assertions fail. If the end
   marker matches an earlier `}).then(function (r) {`, the extracted code either does not parse
   or never calls `fetch`, and `posted` stays null, so `Save posts to /api/zones` fails. I found
   no refactor shape that passes vacuously.
3. **The hand-copied key list is not pinned (INFO-3).** `FW_DROPPABLE` is a copy of the page's
   `ZONE_OPTIONAL_KEY_RE` set, not of the firmware omit-preserved set, despite its comment. Drift
   is safe in one direction and silent in the other. The firmware pin is still the drift test.
4. **Bypasses passing both tests: six found** (table B; LOW-1, LOW-2, INFO-1, INFO-2).
5. **Message text of the pytest CAUGHT rows: confirmed** (table A). Each run collected 10 items,
   failed only `test_page_optional_key_regex_pinned_to_preserved_keys` (9 passed), and the
   `AssertionError` is the intended one.

## Findings

### LOW-1: `blankGuardLabels` gets the live `params` array, and neither test watches it

The Save handler calls `blankGuardLabels(params, guardFieldHidden)` before the omit call
(`zones_page.html:3024`). The whitelist accepts that call as one allowed form and does not look
inside the helper. The node tail starts after it. The node check of `blankGuardKeys` compares
only the return value. B1 adds one line to `blankGuardLabels` that splices `z0_ct_mask=` out of
its argument. Both tests stay green, and the real POST loses the entry. The same holds for
`blankGuardKeys`, which receives the same array.

Fix: in the node test, extract `blankGuardLabels`/`blankGuardKeys` and call them on an
`Object.freeze`d array (a mutation then throws in strict mode, or assert a copy is unchanged).
Alternatively, start the tail at `  var blankGuards = blankGuardLabels(` and pass the real helpers.

### LOW-2: how `params` is built is unconstrained, and a conditional push drops a mask

The whitelist accepts every `params.push(` unconditionally. B5 changes the zone loop's
`params.push('z' + i + '_ct_mask=' + ctMask);` (`zones_page.html:2848`) to
`if (ctMask) params.push(...)`. A zone with no CT channel then sends no `ct_mask`, and the
firmware omit-DEFAULTS it. Both tests pass. This is the most natural-looking edit of the set.
Today the masks are computed from checkboxes (`z.ct_mask || 0`, line 1311), so a blank mask
cannot reach the filter at all. An omitted mask is therefore the realistic regression, more
than a filtered one.

Fix: a lexical pin that the `_thermo_mask=` and `_ct_mask=` pushes are unconditional. For
example, require the exact two lines and no `if (`/`?`/`&&` on them. A DOM-free behavioural test
of the zone loop would be costly.

### INFO-1: the helper can be replaced through `window[...]`, a redeclaration, or a page fetch wrapper

These pass both tests (same class as BENCHFX10 INFO-1, all deliberate):

- B2: `window['omitBlankOptionalParams'] = ...` wrapping the original. The `\bomitBlankOptionalParams\s*=`
  check does not see `']`.
- B3: a second `function omitBlankOptionalParams(ps) {...}` later in the page. Both extractors
  take the first declaration, and the later one wins at run time.
- B4: a page-level `window.fetch = function (u, o) {...}` that edits `o.body`. The node test
  passes its own `fetch`.

Fix if wanted: exactly one `function omitBlankOptionalParams`, no `window\s*\[\s*['"](omitBlankOptionalParams|blankGuardLabels|fetch)`,
and no `\bfetch\s*=(?!=)` or `window\.fetch\s*=` in the page.

### INFO-2: "exactly one POST to /api/zones" counts only `fetch(` literals

B6 adds an `XMLHttpRequest` POST to `/api/zones` (an unused helper). It passes. The pin cannot
see XHR, `navigator.sendBeacon`, or a URL that is built from parts. Counting `'/api/zones'`
occurrences, and refusing `XMLHttpRequest`/`sendBeacon` in this page, would close it.

### INFO-3: `FW_DROPPABLE` says it mirrors the firmware, but it mirrors the page regex

The comment at `test_host_refusal_pages.js:56-58` says the firmware omit-PRESERVES "exactly
these keys". It does not. `firmware_omit_preserved_keys()` on `zones_http_post_parse.c` also
returns `xzone`, `tctype`, `zonetype`, `failsafe`, `settings_source` and `relaytype`. Five of
those are in the JS `MUST_KEEP` list, which is right for the page's policy but wrong for the
comment. The list is pinned to nothing:

- A key removed from the page regex but left in `FW_DROPPABLE` fails loudly.
- A key added to the regex and in `MUST_KEEP` fails loudly.
- A key added to the regex and to neither list is silently not covered.

None of these can drop a mask, because the drift test still pins the regex to the firmware set.
Fix: correct the comment. Optionally, have the drift test parse `FW_DROPPABLE` and assert that it
equals the regex key set.

### INFO-4: an extraction failure aborts the rest of the node file

In C3 the `found the Save handler fetch tail` FAIL prints, then `new Function` throws an uncaught
SyntaxError. The remaining assertions (setup wizard section) do not run, and the summary line
does not print. It still exits 1 with a clear first FAIL, so this is not vacuous. Guarding the
tail block with `if (sIdx > 0 && eIdx > sIdx)` would keep the rest of the file reporting.

### INFO-5: the drift test runs only in the PcTools pytest suite

`test_zone_omit_preserved_drift.py` is reached by pytest discovery (`run_pctools_tests`).
`run_all_checks.ps1` does not run it. So the lexical whitelist half of this fix (which alone
catches N1, the filter before the call) is enforced only when that suite runs. This is the repo's
existing split, not something these commits introduced.

## Mutation tables

All runs used `tools\negtest.ps1` at base 0e260bd02. Every run had baseline PASS,
`real_tree_unchanged=true` and `copies_removed=true`. All mutations are on `zones_page.html`.
Drift test runs: `-Preset pytest -PresetArg tests/test_zone_omit_preserved_drift.py`. Check runs:
`-Preset check -PresetArg tools/check_page_js_tests.ps1`. Set C ran with
`-ExpectPattern 'FAIL: test_host_refusal_pages\.js'` (the check's own per-file verdict). Set B
ran with the default nonzero-exit verdict.

### A: the claimed CAUGHT rows, re-run through the drift test, with the message confirmed

| Id | Mutation | Verdict | Assertion message (from the log) |
|----|----------|---------|-------------------------------------|
| N1 | `params = params.filter(p => p !== 'z0_ct_mask=')` before the omit call | CAUGHT | `Save handler uses \`params\` other than push / declaration / ...` |
| N2 | `body: params.filter(... 'z0_ct_mask=' ...).join('&'),` | CAUGHT | `0 != 1 : Save handler: expected 1 x body: params\.join\('&'\),` |
| N3 | `var pk = params; pk.splice(pk.indexOf('z0_ct_mask=') >>> 0, 1);` after the call | CAUGHT | `Save handler uses \`params\` other than push / ...` |
| N5 | `window['ZONE_OPTIONAL_KEY_RE'] = /^z\d+_(ct_mask\|thermo_mask\|k)=\s*$/;` | CAUGHT | `2 != 1 : ZONE_OPTIONAL_KEY_RE must appear only in its declaration and omitBlankOptionalParams` |

Each run: 10 collected, 1 failed (`test_page_optional_key_regex_pinned_to_preserved_keys`), 9 passed.

### B: candidate bypasses (each drops `z0_ct_mask=` from the real POST)

| Id | Mutation | Drift test | check_page_js_tests.ps1 |
|----|----------|------------|-------------------------|
| B1 | `blankGuardLabels` splices `'z0_ct_mask='` out of its `params` argument | MISSED (LOW-1) | MISSED |
| B2 | `window['omitBlankOptionalParams'] = ` wrapper that also drops `'z0_ct_mask='` | MISSED (INFO-1) | MISSED |
| B3 | second `function omitBlankOptionalParams(ps)` declared later in the page | MISSED (INFO-1) | MISSED |
| B4 | page-level `window.fetch = ...` that filters `o.body` | MISSED (INFO-1) | MISSED |
| B5 | `if (ctMask) params.push('z' + i + '_ct_mask=' + ctMask);` | MISSED (LOW-2) | MISSED |
| B6 | unused `XMLHttpRequest` POST to `/api/zones` helper | MISSED (INFO-2) | MISSED |

### C: the standing check catches tail mutants, and extraction failures are loud

| Id | Mutation | Verdict | Failing node assertion (from the log) |
|----|----------|---------|----------------------------------------|
| C1 | N2 (`body:` filter) | CAUGHT | `FAIL: POST body keeps blank ct_mask/thermo_mask for zone 0` (50 passed, 1 failed) |
| C2 | N3 (alias splice after the call) | CAUGHT | same as C1 |
| C3 | rename `savedZonesEditSeq` to `savedSeq` (start marker gone) | CAUGHT | `FAIL: found the Save handler fetch tail`, then SyntaxError (INFO-4) |
| C4 | move `params = omitBlankOptionalParams(params);` above `msg.textContent = 'Saving…'` | CAUGHT | `POST body drops blank optional, keeps set value` for zones 0-2, plus two lexical asserts; `test_web_review_fixes.js` also fails |
