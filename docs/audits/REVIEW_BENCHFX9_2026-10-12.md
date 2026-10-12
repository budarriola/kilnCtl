# Review: benchfx9 (7e9bde161, 633ced138), 2026-10-12

Adversarial review of the dev commits that fix REVIEW_BENCHFX8 LOW-1, LOW-2 and INFO-2:

- `tools/PcTools/tests/test_zone_omit_preserved_drift.py`: new `_Z_DEREF`/`_IDX` left-hand
  side (lines 43-48) used by the later-override scan (line 92). The memset rule also takes
  memmove/strncpy, a space before the paren and one cast (line 115). The docstring (lines 5-17)
  lists the classes that stay uncovered. There are new self-tests (lines 241-250). The page pin
  now requires one assignment of `ZONE_OPTIONAL_KEY_RE` and one `.test(` in
  `omitBlankOptionalParams` (lines 296-302).
- `docs/audits/REVIEW_BENCHFX8_2026-10-12.md`: status lines.

Reviewed at origin/dev 633ced138.

## Verdict

No HIGH or MED findings. Every form listed in BENCHFX8 LOW-1 and LOW-2 is now scanned. Each one
was negtested against the real `zones_http_post_parse.c` and CAUGHT by a real assertion. The new
regexes give no false positive on the real firmware file. Reads, comparisons and
`memmove(buf, z->f, n)` (z as the source) also give none. The page pin still does not check
which regex its single `.test(` uses. The scan also misses more left-hand-side forms than the
docstring lists. Both are backstopped only in part. 2 LOW, 4 INFO.

## Findings

### LOW-1: the page pin counts `.test(` but not which regex it calls

**Fixed in benchfx10** (pin requires the single call to be `ZONE_OPTIONAL_KEY_RE.test(`, refuses `.match(`/`.compile(`/other regex or a second filter in the function or at the call site; node test asserts blank `z0_ct_mask=`/`z0_thermo_mask=` are KEPT. P3-P7, J2, J3 re-negtested CAUGHT, see below.)

`test_zone_omit_preserved_drift.py:299-302` extracts `omitBlankOptionalParams` and requires
exactly one `\.test\(`. It does not require that call to be `ZONE_OPTIONAL_KEY_RE.test(`. Line
288 pins only the `var ZONE_OPTIONAL_KEY_RE = /.../` literal, so the filter can stop using it
while the pinned variable stays correct and unused.

Failure scenario: an edit makes the filter
`return !/^z\d+_(...the same list...|ct_mask|thermo_mask)=\s*$/.test(p);` (negtest J2). Then a
blank `ct_mask`/`thermo_mask` field is dropped from the POST. The firmware omit-DEFAULTS those
keys, so the zone's CT/thermo mask is reset to its default, not kept. The drift test stays
green. The node tests `test_host_refusal_pages.js:50-55` and `test_web_review_fixes.js:75-78`
also pass, because they never feed a blank `ct_mask` or `thermo_mask`. A narrower replacement
(J1) is caught by the node tests only.

Other forms the pin does not see (P4-P7 MISSED):

- `.test (p)` with a space;
- a second filter that uses `.match(`;
- `ZONE_OPTIONAL_KEY_RE.compile(...)`, which rewrites the regex in place;
- a second `.filter` at the call site (`zones_page.html:3033`).

Fix: require the single call to be `ZONE_OPTIONAL_KEY_RE\s*\.\s*test\s*\(`. Also add a node
assertion that `omitBlankOptionalParams(['z0_ct_mask=', 'z0_thermo_mask='])` keeps both entries.
That second check is behavioural and covers every syntactic variant at once.

### LOW-2: the scan still misses left-hand-side forms the docstring does not list

**Fixed in benchfx10** (docstring uncovered list now names the forms and the whole-struct helper clear; BENCHFX8 LOW-1 status line corrected; C scan deliberately not extended)

The docstring (lines 5-17) and the BENCHFX8 LOW-1 status line ("every form except
nested-index-in-pointer-arith") suggest the deref forms are covered. These still give no
violation. Each was inserted after `memcpy(z->coupling_dead_time_s, ...)` in the real file and
MISSED (G1-G7 in the table):

| Form | Why |
|------|-----|
| `(&*z)->hyst_c = 0.0f;` | `_Z_DEREF` has no `&*` |
| `(*(z)).hyst_c = 0.0f;` | `\(\s*\*\s*z\s*\)` does not allow parens around `z` |
| `(&z[0])->hyst_c = 0.0f;` | no `&z[k]` form |
| `(z + 0u)->hyst_c`, `(0 + z)->hyst_c` | only the literal `z + 0` is matched |
| `z[idx[0]].min_off_s = 0u;` | `\bz\s*\[[^\]]*\]` stops at the inner `]` (nesting is handled after the field only) |
| `memset(&(z->hyst_c), 0, 4);` | no parens around `z->f` after `&` |
| `memset((uint8_t *)(z->coupling_coeff), 0, 4);` | cast followed by a parenthesised argument |

Also missed, and not in the uncovered list: `memset(z, 0, sizeof *z)`, a whole-struct clear
through a helper (the docstring names only `*z = tmp`), `memcpy(z->f, ...)` (listed),
and a two-dimensional index `z->f[1][0]` (listed as "a two-level index").

Failure scenario: as for BENCHFX8 LOW-1. The drift test stays green and only the host tests
catch the write. BENCHFX8 ran field writes of this kind against `test_zones_http.c` and they
were CAUGHT there, so the standing suite still stops a bad build. These forms are contrived. Fix:
add them to the docstring's uncovered list, and stop the status line claiming full coverage.
Scanning `\(\s*&\s*\*\s*z\s*\)` and `\(\s*\*\s*\(\s*z\s*\)\s*\)` is cheap if wanted.

### INFO-1: a comment says the DEFAULTED assertion is kept, but it was deleted

**Fixed in benchfx10** (comment now says the assertion was removed)

`test_zone_omit_preserved_drift.py:294-295` says the `keys & ZONE_OMIT_DEFAULTED_KEYS`
assertion "is kept only as an explicit statement of the intent". The commit removed that
assertion. The reasoning is right (line 293 already implies it), but the comment describes code
that is not there. Fix: say that the assertion was dropped because line 293 implies it.

### INFO-2: strncpy coverage is vacuous today

**Fixed in benchfx10** (left as is: harmless until a char-array field becomes omit-preserved)

No omit-preserved field is a `char` array. `name` is the only char array in the zone struct
(`zones_config_json.h:313`), and it is not omit-preserved, so `strncpy(z->name, ...)` is never
checked. The rule is harmless, and it would start to matter only if a string field became
omit-preserved.

### INFO-3: the function extraction stops at the first closing-brace line; `==` counts as an assignment

**Fixed in benchfx10** (body extraction is brace-balanced; `=(?!=)` ignores `==`/`===`)

- `test_zone_omit_preserved_drift.py:299`: `function omitBlankOptionalParams\b.*?\n\s*\}\n`
  ends at the first line that is only `}`. If the body is reformatted so an inner callback
  closes on its own line, the extract stops there. A second `.test(` after that point is then not
  counted (checked with a script: count 1 for a body that has two `.test(` calls).
- Line 297: `\bZONE_OPTIONAL_KEY_RE\s*=` also matches `ZONE_OPTIONAL_KEY_RE === x`. This is a
  false positive, but it fails loud, so it is safe.

### INFO-4: the BENCHFX8 status lines cited a pre-rebase SHA

`REVIEW_BENCHFX8_2026-10-12.md` lines 29, 57 and 101 said "Fixed/Addressed in b0b739347". That
commit is not on any remote branch. Its patch-id for the test file (711f749a...) equals that of
7e9bde161 on origin/dev. All three lines now cite 7e9bde161.

## False-positive check

`_classify_full` on the unmodified `zones_http_post_parse.c` reports no violations and 29
preserved keys. Inserted statements that only read gave no violation:

- `float x = z->hyst_c [0];`
- `if ((z)->hyst_c == 1.0f) {}`
- `y = z[0].hyst_c >= 1.0f;`
- `memmove(buf, z->coupling_coeff, 4);`
- a parse helper given `&z->hyst_c`

A self-assignment `z->hyst_c = z->hyst_c;` is reported, which is harmless. The `_IDX` and
`_Z_DEREF` alternations are disjoint, so the regexes cannot backtrack catastrophically.

## Mutation tables

All runs used `tools\negtest.ps1`. Every run had baseline PASS, `real_tree_unchanged=true` and
`copies_removed=true`. Firmware mutants insert one statement after
`memcpy(z->coupling_dead_time_s, ...)` in `zones_http_post_parse.c`, and every index used is
off-diagonal.

Drift test: `-Preset pytest -PresetArg tests/test_zone_omit_preserved_drift.py`. Every CAUGHT
run collected 10 items and failed one test with an `AssertionError`. None was a collection error.

| Id | Mutation | Drift test |
|----|----------|------------|
| F1 | `z->coupling_coeff [1] = 0.0f;` | CAUGHT (`test_firmware_has_no_later_overrides`) |
| F2 | `( *z ).coil_power_w = 0.0f;` | CAUGHT (same) |
| F3 | `z[0].min_off_s = 0u;` | CAUGHT (same) |
| F4 | `memmove(z->coupling_coeff, z->coupling_coeff + 1, sizeof(float));` | CAUGHT (same) |
| F5 | `memset ((void *) &z->hyst_c, 0, sizeof(float));` | CAUGHT (same) |
| F6 | `(z + 0)->hyst_c = 0.0f;` | CAUGHT (same) |
| F7 | `z->coupling_coeff[idx[1]] = 0.0f;` | CAUGHT (same) |
| G1 | `(&*z)->hyst_c = 0.0f;` | MISSED (LOW-2) |
| G2 | `(*(z)).hyst_c = 0.0f;` | MISSED (LOW-2) |
| G3 | `memset(&(z->hyst_c), 0, sizeof(float));` | MISSED (LOW-2) |
| G4 | `(z + 0u)->hyst_c = 0.0f;` | MISSED (LOW-2) |
| G5 | `memset((uint8_t *)(z->coupling_coeff), 0, sizeof(float));` | MISSED (LOW-2) |
| G6 | `z[idx[0]].min_off_s = 0u;` | MISSED (LOW-2) |
| G7 | `(&z[0])->hyst_c = 0.0f;` | MISSED (LOW-2) |
| P1 | filter adds `&& !/^z\d+_ct_mask=\s*$/.test(p)` | CAUGHT (`test_page_optional_key_regex_pinned_to_preserved_keys`) |
| P2 | `ZONE_OPTIONAL_KEY_RE = /^z\d+_(ct_mask)=\s*$/;` before the function | CAUGHT (same) |
| P3 | filter uses `/^z\d+_(ct_mask\|thermo_mask\|hystc)=\s*$/` only | MISSED (LOW-1) |
| P4 | second filter written as `.test (p)` | MISSED (LOW-1) |
| P5 | second filter `!p.match(/^z\d+_ct_mask=\s*$/)` | MISSED (LOW-1) |
| P6 | `ZONE_OPTIONAL_KEY_RE.compile(...)` before the function | MISSED (LOW-1) |
| P7 | extra `.filter` at the call site (`zones_page.html:3033`) | MISSED (LOW-1) |

Page node tests: `-Command` running `node test_host_refusal_pages.js` and
`node test_web_review_fixes.js` with `-RequireAssertion`. The CAUGHT run failed `FAIL:`
assertions.

| Id | Mutation | Node tests |
|----|----------|------------|
| J1 | filter uses `/^z\d+_(ct_mask\|thermo_mask\|hystc)=\s*$/` only (same as P3) | CAUGHT (`test_host_refusal_pages.js` "blank optional fields dropped", `test_web_review_fixes.js` "omitBlankOptionalParams drops blank optional keys only") |
| J2 | filter uses an inline copy of the full list plus `ct_mask\|thermo_mask` | MISSED (LOW-1) |
| J3 | second filter `!p.match(/^z\d+_ct_mask=\s*$/)` (same as P5) | MISSED (LOW-1) |

J2 has the same shape as P3 for the drift test: one `.test(` and the pinned variable left
unchanged. So no standing test catches J2.
