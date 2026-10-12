# Review: benchfx8 (f47e372c1, ae18d5a2e, 475de0972), 2026-10-12

Adversarial review of the dev commits that fix REVIEW_BENCHFX7 LOW-1..4:

- `tools/PcTools/tests/test_zone_omit_preserved_drift.py`: the later-override scan
  (`_classify_full`, lines 75-101) now accepts spaced `z -> f`, `(z)->f`,
  `(*z) .f`, `++`/`--`, and `memset` with or without `&`. The compound-operator class
  is `(?:<<|>>|[-+*/%&|^])?` (line 78). New self-tests S1/S6/S11 and LOW-1 forms
  (lines 205-224). New `test_page_optional_key_regex_pinned_to_preserved_keys`
  (lines 257-268).
- `firmware/KilnFW/App/drivers/http/zones_page.html:2705-2714`: comment now says
  `ZONE_OPTIONAL_KEY_RE` is a subset of the omit-preserved keys.
- `docs/audits/REVIEW_BENCHFX7_2026-10-12.md`: status lines.

Reviewed at origin/dev 475de0972.

## Verdict

No HIGH or MED findings. The fixes do what they claim for the forms named in
BENCHFX7. The page-regex pin is not vacuous. The scan still misses several cheap
syntactic forms of the same kind as BENCHFX7 LOW-1, and the docstring lists fewer
uncovered classes than really exist. Every bypass run against the KilnFW host
tests was caught by a real assertion in `test_zones_http.c`. 2 LOW, 4 INFO.

## Findings

### LOW-1: cheap spacing and parenthesis forms still bypass the scan

**Fixed in 7e9bde161** (the forms in the BENCHFX8 table are scanned and pinned by a self-test; negtested CAUGHT; NOT every form: the uncovered list in the test docstring is authoritative). The SHA was first recorded as the pre-rebase b0b739347. REVIEW_BENCHFX9 LOW-2 lists forms that are still missed.

`test_zone_omit_preserved_drift.py:76` builds the left-hand side as
`(?:\bz\s*->|\(\s*z\s*\)\s*->|\(\*z\)\s*\.)\s*FIELD(?:\[[^\]]*\])?`, and line 99
matches `\bmemset\(\s*&?\s*` + lhs. These forms add no violation (checked by
calling `_classify_full` on the real file with one statement inserted):

| Form | Example |
|------|---------|
| space before the index | `z->coupling_coeff [1] = 0.0f;` (negtest B11b) |
| spaces inside `(*z)` | `( *z ).coil_power_w = 0.0f;` (negtest B10), `(* z).hyst_c = 0.0f;` |
| double parentheses | `((z))->hyst_c = 0.0f;` |
| pointer arithmetic | `(z + 0)->hyst_c = 0.0f;` |
| nested index | `z->coupling_coeff[s[0]] = 0.0f;` (`[^\]]*` stops at the inner `]`) |
| space or cast in memset | `memset (z->coupling_coeff, 0, 4);`, `memset((void *)z->coupling_coeff, 0, 4);` |

The docstring (lines 5-6) says spaced forms are covered, so a reader would expect
these to fail. Failure scenario: a refactor writes `z->coupling_coeff [j] = 0.0f;`
after the coupling loop; the drift test stays green and only the host tests catch
it. B11b and B10 were both CAUGHT by the host tests (table below), so no bad build
gets past the standing suite.

Fix: allow `\s*` before `\[`, use `\(\s*\*\s*z\s*\)` for the deref, and allow an
optional cast and `\s*` in the memset rule, `\bmemset\s*\(\s*(?:\([^)]*\)\s*)?&?\s*`.
Nested index and pointer arithmetic can stay as documented gaps.

### LOW-2: docstring understates the uncovered write classes

**Fixed in 7e9bde161** (docstring lists every uncovered class and names test_zones_http.c as the backstop; z[k].f, memmove and strncpy are now also scanned).

`test_zone_omit_preserved_drift.py:7-9` says the scan does not see "writes through
a macro, memcpy/helper calls, or a conditional default nested inside the omit
`else`", and that `test_zones_http.c` covers those. It also misses, with no
mention:

- a pointer alias: `zone_cfg_t *zp = z; zp->hyst_c = 0.0f;` (negtest B3) or
  `float *hp = &z->hyst_c; *hp = 0.0f;`;
- indexing `z` itself: `z[0].min_off_s = 0u;` (negtest B2);
- a whole-struct write: `*z = tmp;`, `*z = (zone_cfg_t){ ... };`;
- a ternary lvalue: `*(c ? &z->hyst_c : &x) = 0.0f;`;
- other writing helpers: `memmove`, `strncpy`, a parse helper given `&z->hyst_c`
  outside the key's own block.

The BENCHFX7 LOW-1 status line ("macro, memcpy and the conditional default ... are
documented") repeats the short list. Failure scenario: someone trusts the docstring
and treats a green drift test as proof for an alias or struct-copy refactor.

Host-test coverage: B3 CAUGHT (`test_zones_http.c:6670`, `6734`, `6745`), B2 CAUGHT
(`6672`, `6734`). Struct assignment, compound literal, ternary, memmove and
strncpy were not run. They would reset the same fields the per-field
omit-preserve assertions check (`test_zones_http.c` around 1231-1279, 1974-1994,
6644-6745, 6752-6780, 6968-7020, 7057-7091, 7190-7221), so they are expected to
be caught but are not proven. Fix: extend the docstring list. No code change is
needed.

### INFO-1: string literals cause false positives, and comment tokens in strings could hide code

`strip_c_comments` (line 35) does not know about string literals.

- A string containing an assignment, for example `http_log("z->min_on_s >>= 1;");`
  or `log("z->hyst_c = 0;");`, is reported as a violation. This fails loud.
- A string containing `/*` and later `*/`, or `//`, makes the stripper blank real
  code: `http_log("/*"); z->hyst_c = 0.0f; http_log("*/");` and
  `http_log("//"); z->hyst_c = 0.0f;` both hide the write. Today
  `zones_http_post_parse.c` has no string literal containing `/*` or `//`, so this
  cannot happen yet.

Comparisons are no longer false positives: `==`, `!=`, `>=`, `<=`, a spaced
`z -> f == x`, `z->f - -1` and `z->f + +1` all give no violation.

### INFO-2: the page-regex pin is sound but sees only the regex literal

**Addressed in 7e9bde161**: redundant DEFAULTED assert dropped; the pin now also requires a single assignment of the regex and exactly one .test( in omitBlankOptionalParams (negtested CAUGHT). REVIEW_BENCHFX9 LOW-1: the pin does not check which regex the single .test( uses.

`test_page_optional_key_regex_pinned_to_preserved_keys` (lines 257-268) does not
pass vacuously. A missing or reshaped regex fails `assertIsNotNone` (line 263),
and an empty or truncated list fails `assertGreater(len(keys), 15)` (line 265). The
negtest J7 (`|ct_mask|thermo_mask`) failed with an `AssertionError` at line 267.
Alternation tricks also fail loudly: `ct_\w+`, `(?:a|b)` and `min(?:on|off)s` give
non-key strings that are not in the firmware set.

The test does not see:

- a second filter in `omitBlankOptionalParams` (`zones_page.html:2752-2754`), such
  as `&& !/^z\d+_ct_mask=$/.test(p)`;
- a later reassignment `ZONE_OPTIONAL_KEY_RE = /.../`.

The `coupling_c` prefix normalisation (line 264) accepts anything starting with
`coupling_c`, for example `coupling_c.*`. This is harmless: the only firmware keys
with that prefix are `z%u_coupling_c%u`, and `coupling_diag_k_dc` does not match.
The assertion at line 268 (`keys & ZONE_OMIT_DEFAULTED_KEYS`) is implied by line
267, because a defaulted key is never in the preserved set.

### INFO-3: only zones_http_post_parse.c is scanned

Writes in the caller `zones_http_post.c` to `tmp->zones[i]` after
`zones_http_parse_zone_fields()` returns are outside the drift test's scope. The
only such zeroing today is the aux-conversion path
(`zones_http_post.c:117-130`, `zones_http_zone_free_for_aux()`), which is
intended. The POST-handler host tests (`test_zones_http.c` around 1957-2006,
7308) cover the end-to-end path.

### INFO-4: a diagonal coupling write is invisible to the host tests

The first B11 mutant, `z->coupling_coeff [0] = 0.0f;`, was MISSED by the host
tests too. For zone 0, index 0 is the diagonal, which must be 0 anyway, so the
write changes nothing. Rerun on an off-diagonal cell (B11b, `[1]`), it was CAUGHT
at `test_zones_http.c:7018`. This is not a coverage gap. It is recorded so a
later reviewer does not treat the B11 result as one.

## Mutation tables

All runs used `tools\negtest.ps1`. Every run had baseline PASS,
`real_tree_unchanged=true` and `copies_removed=true`. Firmware mutants insert
one statement either after the coilpower block of `zones_http_post_parse.c` or
after its `memcpy(z->coupling_dead_time_s, ...)` line.

Drift test: `-Preset pytest -PresetArg tests/test_zone_omit_preserved_drift.py`.
Every CAUGHT was an `AssertionError` in a run that collected 10 items.

Host tests: `-Command "build_host_tests.ps1 -OutDir {OUT} -Only zones"` with the
kilnfw-host ExpectPattern. Every host CAUGHT compiled and failed a
`FAIL test_zones_http.c:<line>` assertion. None was a build failure.

### Fixer's negtests, re-run

| Id | Mutation | Drift test |
|----|----------|------------|
| Z1 | `z->min_on_s++;` | CAUGHT (`test_firmware_has_no_later_overrides`, "++/-- on z->min_on_s") |
| Z11 | `memset(z->coupling_coeff, 0, sizeof(...))` | CAUGHT (same test, "memset of z->coupling_coeff") |
| J7 | page regex `\|ct_mask\|thermo_mask` | CAUGHT (`test_page_optional_key_regex_pinned_to_preserved_keys`, line 267) |
| S1 | operator class back to `[-+*/\|&]?` | CAUGHT (`test_omit_branch_exact_same_field_only`, line 210, `^=`) |

### New bypass mutants

| Id | Mutation | Drift test | Host tests |
|----|----------|------------|------------|
| B11 | `z->coupling_coeff [0] = 0.0f;` | MISSED | MISSED (diagonal no-op, INFO-4) |
| B11b | `z->coupling_coeff [1] = 0.0f;` | MISSED (LOW-1) | CAUGHT (7018) |
| B10 | `( *z ).coil_power_w = 0.0f;` | MISSED (LOW-1) | CAUGHT (6780, 6788) |
| B3 | `{ zone_cfg_t *zp = z; zp->hyst_c = 0.0f; }` | MISSED (LOW-2) | CAUGHT (6670, 6734, 6745) |
| B2 | `z[0].min_off_s = 0u;` | MISSED (LOW-2) | CAUGHT (6672, 6734) |
