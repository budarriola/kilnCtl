# Review: benchfx10 (f829ba4f3), 2026-10-12

Adversarial review of the dev commit that fixes REVIEW_BENCHFX9 LOW-1, LOW-2, INFO-1 and INFO-3:

- `tools/PcTools/tests/test_zone_omit_preserved_drift.py`: brace-balanced `_js_function_body()`
  (lines 54-68). The page pin (lines 303-338) now requires exactly one `.test(` in
  `omitBlankOptionalParams`, and that call must be `ZONE_OPTIONAL_KEY_RE.test(`. It refuses
  `.match/.compile/.exec/.replace/.search`, an inline `/^` regex, a second `.filter`, and
  `ZONE_OPTIONAL_KEY_RE.compile`/`.lastIndex` anywhere on the page. It also pins the call site
  between `params = omitBlankOptionalParams(params);` and `fetch('/api/zones'`. The docstring
  (lines 11-20) lists the C forms the scan does not cover.
- `firmware/KilnFW/App/test/test_host_refusal_pages.js:56-59`: new behavioural assertion that
  blank `z0_ct_mask=` and `z0_thermo_mask=` are KEPT.
- Status lines in REVIEW_BENCHFX8 and REVIEW_BENCHFX9.

Reviewed at origin/dev f829ba4f3 (worktree minted from origin/dev 8381e658a).

## Verdict

No HIGH or MED findings. I re-ran every mutation the fix claims to catch (P3-P7, J2, J3). All
are CAUGHT by a real assertion in the drift test, and J2/J3 are also CAUGHT by the node test.
The node assertion runs the real page function: it extracts the `var ZONE_OPTIONAL_KEY_RE` line
and `function omitBlankOptionalParams` from `zones_page.html` (`test_host_refusal_pages.js:46-49`),
not a copy. J2 against the real page fails it. The pins give no false positive on the current
page.

The pins are still lexical, so some edits get past both tests. Three of them drop a blank
`z0_ct_mask=` from the real POST: a filter placed before the pinned call, one inside the fetch
`body:`, and an aliased `splice`. The node assertion feeds only zone 0. So a function-body drop
that is limited to `z1_`/`z2_` and avoids the regex-call shapes also passes. 2 LOW, 4 INFO.

## Findings

### LOW-1: the call-site pin covers only the span between the call and `fetch(`

**FIXED in benchfx11** (every `params` use in the Save handler must be an allowed form; node test runs the real fetch tail against a mocked fetch and checks the POST body; N1-N3 CAUGHT, see the benchfx11 table in the commit log).

`test_zone_omit_preserved_drift.py:332-338` looks for a second filter only in the text between
`omitBlankOptionalParams(params);` and the first `fetch('/api/zones'`. Within that span it
recognises only `params = params.` and `params.filter/splice/length =`. Each of these drops a
blank `z0_ct_mask=` from the POST body, and the firmware then omit-DEFAULTS that zone's CT mask.
Both the drift test and both node files stay green:

- N1: `params = params.filter(...)` one line BEFORE the pinned call
  (`zones_page.html:3033`);
- N2: `body: params.filter(...).join('&'),` inside the fetch options, after the span ends;
- N3: `var pk = params; pk.splice(...)` after the call (an alias, so the `params.splice` rule
  does not see it).

Other forms that would pass the same way: `params.pop()`/`shift()`, `params.length -= 1`,
`Array.prototype.splice.call(params, ...)`, and a second `fetch('/api/zones', {method:'POST'})`
elsewhere in the page. These are all deliberate code edits, not easy accidents, and the class is
the same as BENCHFX9 P7 (LOW). Fix (cheap and behavioural): extract the save handler's
params-to-body path in a node test, or assert that the fetch's `body:` is exactly
`params.join('&')` and that `params` is assigned only by `var params = [];`, `.push(` and the one
pinned call between the `Saving` line and the fetch.

### LOW-2: the behavioural assertion feeds zone 0 only; non-regex drops are lexically invisible

**FIXED in benchfx11** (node test generates every zone 0..2 x every key x blank/set and compares `omit()` with an independent firmware omit-preserved key list; N4, N6, N8b, N10 CAUGHT by the node test).

`test_host_refusal_pages.js:58` checks only `z0_ct_mask=` and `z0_thermo_mask=`. The drift
test's body checks (lines 322-329) look only for regex-call shapes: `.test(`, `.match(` and the
like, and `/^`. So an extra condition in the filter callback that is not a regex call, and that
is limited to another zone, passes both tests:

- N4: `return !ZONE_OPTIONAL_KEY_RE.test(p) && p !== 'z1_ct_mask='; });`
- N8b: `... && !/z1_ct_mask=/['test'](p);` (bracket call, no `^`)

For zone 0 the same edits (N8a `/ct_mask=/['test'](p)`, N10 `!p.endsWith('ct_mask=')`) are
CAUGHT by the node assertion only. The drift test misses them. Fix: run the node assertion over
every zone index (`z0..z2`) and over every key that is not in `ZONE_OPTIONAL_KEY_RE`. Stronger:
assert that `omit(x)` equals `x.filter(p => !ZONE_OPTIONAL_KEY_RE.test(p))` for a generated list
of every zone/key/blank combination. That is behavioural and makes the lexical body pins mostly
redundant.

### INFO-1: the regex can be replaced through `window[...]`

**FIXED in benchfx11** (the name may appear only in its declaration and `omitBlankOptionalParams`, comments excluded; N5 CAUGHT).

The page's `<script>` declares `var ZONE_OPTIONAL_KEY_RE` at top level (`zones_page.html:2751`),
so it is a `window` property. `window['ZONE_OPTIONAL_KEY_RE'] = /^z\d+_(ct_mask|...)=\s*$/;`
(N5) bypasses the `\bZONE_OPTIONAL_KEY_RE\s*=(?!=)` count, because the name is followed by `']`.
The node test extracts only the original `var` line, so it does not see the replacement either.
This is contrived. Counting `ZONE_OPTIONAL_KEY_RE` occurrences outside the `var` line and the
function (expected: zero) would close it.

### INFO-2: braces inside strings fool `_js_function_body`

**Closed by the behavioural test** (N6 CAUGHT by the node test; the lexical scan is unchanged).

`_js_function_body` (lines 54-68) counts braces without skipping strings, regex literals or
comments, as its docstring says. In N6 the callback contains `p !== '}'`, so the extract ends
at the callback's own `}`. A chained `.filter(function (p) { return !/z1_ct_mask=/.test(p); })`
after that point is then outside the body, and neither the second `.filter` nor the second
`.test(` is counted. To escape the node test as well, this has to be combined with a LOW-2
shape, so it adds little on top of LOW-2. Fix if wanted: skip quoted strings in the scan, or
fail when the extracted body does not end at a line that is only `}`.

### INFO-3: the BENCHFX9 status lines need a SHA, and "see below" points at stale tables

**FIXED in benchfx11** (SHAs added, table pointer fixed, INFO-2 marked Accepted).

`REVIEW_BENCHFX9_2026-10-12.md:28,56,85,94,103` say "Fixed in benchfx10", not `f829ba4f3`.
Line 28 says "P3-P7, J2, J3 re-negtested CAUGHT, see below", but the tables below still show
the pre-fix MISSED rows and contain no re-run. My re-run (table R below) confirms the claim.
Line 94 marks INFO-2 "Fixed" when the text says "left as is". Fix: cite `f829ba4f3`, point at
this document's table R, and say "Accepted (not changed)" for INFO-2.

### INFO-4: the docstring is honest about the C scan but says nothing about the page pin

The corrected docstring (lines 11-20) now names every form BENCHFX9 LOW-2 listed, including
`memset(z, 0, sizeof *z)` and a whole-struct helper clear. For the C scan it is accurate. One
small gap: `memcpy(z->f, ...)` with an array field (decay, no `&`) is covered only by the
phrase "memcpy ... given `&z->f`". The page pin's limits (LOW-1, LOW-2, INFO-1, INFO-2) are not
documented anywhere. A line in `test_page_optional_key_regex_pinned_to_preserved_keys`'s
docstring would make them visible.

## Brittleness

The baseline passes on the current page, so there is no false positive. A legitimate refactor
fails loudly with a message that says which pin broke. That holds for converting to an arrow
function or `const` ("not found"), using double quotes in `fetch("/api/zones"` ("Save handler
call site not found"), using `p.trim()`/`p.replace(...)` or `?.test(` in the filter ("must not
use another regex or call" / "the single test must be ..."), and using
`params = params.concat(...)` after the call. None of these messages is misleading. The node
extraction (`\r?\n\}\r?\n`, column-0 brace) and the Python extraction (brace-balanced) differ,
but a mismatch fails the node "found omitBlankOptionalParams" assertion loudly.

## Mutation tables

All runs used `tools\negtest.ps1` at base f829ba4f3. Every run had baseline PASS,
`real_tree_unchanged=true` and `copies_removed=true`. The drift test was run with
`-Preset pytest -PresetArg tests/test_zone_omit_preserved_drift.py`. Every CAUGHT run collected
10 items and failed `test_page_optional_key_regex_pinned_to_preserved_keys` with an
`AssertionError` (message quoted). The node runs used `-Command` (both
`test_host_refusal_pages.js` and `test_web_review_fixes.js`) with `-RequireAssertion`. Every
CAUGHT run printed a `FAIL:` assertion. All mutations are on `zones_page.html`.

### New bypasses

| Id | Mutation | Drift test | Node tests |
|----|----------|------------|------------|
| N1 | `params = params.filter(p => p !== 'z0_ct_mask=')` before the pinned call | MISSED (LOW-1) | MISSED |
| N2 | `body: params.filter(... 'z0_ct_mask=' ...).join('&')` | MISSED (LOW-1) | MISSED |
| N3 | `var pk = params; pk.splice(pk.indexOf('z0_ct_mask=') >>> 0, 1);` after the call | MISSED (LOW-1) | MISSED |
| N4 | filter adds `&& p !== 'z1_ct_mask='` | MISSED (LOW-2) | MISSED |
| N5 | `window['ZONE_OPTIONAL_KEY_RE'] = /^z\d+_(ct_mask\|thermo_mask\|k)=\s*$/;` | MISSED (INFO-1) | MISSED |
| N6 | `p !== '}'` in the callback, then a chained `.filter(... /z1_ct_mask=/.test(p) ...)` | MISSED (INFO-2) | MISSED |
| N7 | `.reduce` with `a.push` in place of `.filter` | CAUGHT ("exactly one .filter in the function") | MISSED |
| N8a | filter adds `&& !/ct_mask=/['test'](p)` | MISSED | CAUGHT ("blank ct_mask/thermo_mask entries are kept") |
| N8b | filter adds `&& !/z1_ct_mask=/['test'](p)` | MISSED (LOW-2) | MISSED |
| N9 | `var R = ZONE_OPTIONAL_KEY_RE;` and the filter uses `R.test(p)` | CAUGHT ("the single test must be ZONE_OPTIONAL_KEY_RE.test(") | not counted (ReferenceError, not an assertion) |
| N10 | filter adds `&& !p.endsWith('ct_mask=')` | MISSED | CAUGHT (same as N8a) |

### R: re-run of the BENCHFX9 mutations the fix claims

| Id | Mutation | Drift test | Node tests |
|----|----------|------------|------------|
| P3 | filter uses `/^z\d+_(ct_mask\|thermo_mask\|hystc)=\s*$/` only | CAUGHT ("the single test must be ZONE_OPTIONAL_KEY_RE.test(") | CAUGHT (4 `FAIL:`) |
| P4 | second chained filter written `.test (p)` | CAUGHT ("exactly one regex test", 2 != 1) | CAUGHT |
| P5/J3 | second chained filter `!p.match(/^z\d+_ct_mask=\s*$/)` | CAUGHT ("must not use another regex or call") | CAUGHT |
| P6 | `ZONE_OPTIONAL_KEY_RE.compile(...)` before the function | CAUGHT ("must not be rewritten in place") | MISSED |
| P7 | `params = params.filter(...)` after the call | CAUGHT ("no second filter on params at the call site") | MISSED |
| J2 | inline copy of the full list plus `ct_mask\|thermo_mask` | CAUGHT ("the single test must be ...") | CAUGHT ("blank ct_mask/thermo_mask entries are kept") |
