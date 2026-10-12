# Review: benchfx7 (b7549bdd3, cd81f8d31), 2026-10-12

Adversarial review of the two dev commits that fix REVIEW_BENCHFX6 LOW-1, LOW-2
and LOW-3:

- `tools/PcTools/tests/test_zone_omit_preserved_drift.py`: rewritten field-write
  scan (`_classify_full`, lines 34-91) with Y1-Y6 / P1 / P4 / P6 self-tests
  (lines 172-193).
- `firmware/KilnFW/App/drivers/http/zones_page.html:2750`: `ZONE_OPTIONAL_KEY_RE`
  also matches `coilpower|hystc|minons|minoffs`.
- `firmware/KilnFW/App/test/test_host_refusal_pages.js:54-55`: new assertion for
  those four keys.

Reviewed at origin/dev d78551461 (both commits are ancestors; nothing later
touches the three files or `zones_http_post_parse.c`).

## Verdict

No HIGH or MED findings. Dropping a blank coilpower/hystc/minons/minoffs from the
POST is safe: the firmware omit-preserves all four from the stored zone, and no
write after parsing resets them. The drift scan has real bypasses and one false
positive, but every bypass found is still caught by the KilnFW host tests in
`test_zones_http.c`, so none reaches a build that passes the standing suite.
Four LOW findings, three INFO.

## Safety of dropping a blank (verified against firmware)

`firmware/KilnFW/App/drivers/http/zones_http_post_parse.c`:

| Key | Lines | Omitted key behaviour |
|-----|-------|-----------------------|
| hystc | 258-268 | `z->hyst_c = current_z->hyst_c` |
| minons | 275-287 | `z->min_on_s = current_z->min_on_s` |
| minoffs | 289-301 | `z->min_off_s = current_z->min_off_s` |
| coilpower | 316-327 | `z->coil_power_w = current_z->coil_power_w` |

`zones_config_json_field_present()` counts a blank `key=` as present, and
`zones_config_json_parse_float_field()` rejects `len <= 0`, so before the page
change a blank produced a 400. Omitting the key keeps the live stored value. No
later statement in `zones_http_post_parse.c` or `zones_http_post.c` writes the
four fields on the POST path; the only zeroing (`zones_http_post.c:128-130`) is in
the separate aux-conversion path `zones_http_zone_free_for_aux()`. The required
keys (window/minon/minoff at lines 562-597, maxtemp/mintemp at 546-555) still 400
when missing, so the page regex cannot silently drop them.

## Findings

### LOW-1: drift scan has syntactic bypasses (caught only by host tests)

**Status: FIXED in 8066eaeb6 for ++/--, spaced and parenthesised deref, memset without &; macro, memcpy and the conditional default inside the omit else are documented in the test docstring as covered by test_zones_http.c.**

`test_zone_omit_preserved_drift.py:70-71` matches `z->field` or `(*z).field`
followed by an `=`-family operator. It does not see:

- `++` / `--` on a field (Z1, Z2);
- a write through a macro (Z4) or a helper such as `memcpy(&z->hyst_c, ...)` (Z5);
- whitespace around `->` (Z6) or `(z)->` (Z7);
- `memset(z->coupling_coeff, ...)` without `&` on an array field (Z11): the
  memset rule at lines 88-90 only matches `memset(&z->field`;
- a conditional default inside the omit `else` (Z10): the inner
  `else { z->coil_power_w = current_z->coil_power_w; }` satisfies the exact
  omit-branch regex, and the `z->coil_power_w = 0.0f` before it is exempted by the
  before-span rule (lines 78-81) because its condition does not mention
  `current_z`. This is a structural escape inside the omit branch itself.

All eight are CAUGHT by the host tests (table below), so the scan is a second
line, not the only one. Fix options: allow `\s*` around `->`, accept `\(z\)->`,
add a `(?:\+\+|--)` form, extend the memset rule to `memset\(\s*&?\s*z->`, and
reject any write to the field inside the omit `else` other than the exact
`= current_z->field` statement.

### LOW-2: false positive on an equivalent comparison (FP1)

**Status: FIXED in 8066eaeb6 (operator class is real compound operators only; FP1 rewrite now passes).**

The compound-operator class `[-+*/|&^%<>]*` (line 71) includes `<` and `>`, so
`z->hyst_c >= ZONE_HYST_C_MIN` reads as a `>=` compound assignment. Rewriting the
range check at `zones_http_post_parse.c` (`(z->hyst_c != 0.0f && z->hyst_c <
ZONE_HYST_C_MIN)`) as the equivalent `!(z->hyst_c == 0.0f || z->hyst_c >=
ZONE_HYST_C_MIN)` fails `test_firmware_has_no_later_overrides`
("hystc: compound assignment to z->hyst_c before the omit branch"), while the host
tests pass. Fails loud, so LOW. Fix: restrict the class to real compound
operators, `(?:<<|>>|[-+*/%&|^])?=(?!=)`, so `>=`, `<=` are not matched.

### LOW-3: self-test gaps in the drift test

**Status: FIXED in 8066eaeb6 (self-tests for S1, S6, S11).**

Three scan clauses can be removed without any self-test failing:

- S1: reverting the operator class to the old `[-+*/|&]?` (no self-test uses
  `^=`, `%=`, `<<=` or `>>=`);
- S6: dropping `field not in _ALLOWED_RHS` from the before-span exemption
  (lines 78-81); the P4 case is also caught by the RHS rule, so the clause is not
  isolated;
- S11: dropping the `(elifs and field not in _ALLOWED_RHS)` arm of the else-if
  rule (lines 65-67); no self-test has `else if (have_legacy)` on a non-exempt
  field.

Add one self-test per clause.

### LOW-4: page regex not pinned to the firmware or Python preserved list

**Status: FIXED in 8066eaeb6 (test_page_optional_key_regex_pinned_to_preserved_keys; J7 CAUGHT; zones_page.html comment corrected).**

`zones_page.html:2705-2713` says `ZONE_OPTIONAL_KEY_RE` "lists exactly the ones a
blank input may drop, and test_zone_omit_preserved_drift.py pins the firmware
side". Nothing ties the page regex to that list. Adding the omit-DEFAULTED
`ct_mask|thermo_mask` (`zones_http_client.py:1357`, `ZONE_OMIT_DEFAULTED_KEYS`) to
the regex passes `test_host_refusal_pages.js` (J7 MISSED); a blank would then
silently remap or clear the mask instead of being refused. Adding a required key
(J5 minon/minoff, J6 maxtemp) also passes, but the firmware still answers 400 for
those, so they are harmless. Fix: a JS host test that parses the regex
alternation and checks it against the preserved key list (or at least refuses
`ct_mask`/`thermo_mask` and the required keys), or soften the comment.

### INFO-1: blank or unparsable input is now a silent no-op

**Status: ACCEPTED by owner instruction: a blank is silently ignored and the page says "Saved."; not changed.**

A blank coilpower/hystc/minons/minoffs, or text a `type=number` input reports as
`""` (for example `1,500`), is now dropped. The page shows "Saved." and
`loadCurrent()` reloads the stored value (around `zones_page.html:3060`). Before,
this was a 400. Safe (no reset to default), but the user is not told the edit was
ignored.

### INFO-2: coilpower read from the limits terminal stack

The coilpower push (`zones_page.html:2884`) reads from `limitsStack`, the
settings-source terminal zone, but `coil_power_w` is not in `SRC_GROUP_LIMITS`
(`zones_config_accessors.h:340`). Pre-existing. A dropped blank on the terminal
zone leaves each inheriting zone at its own stored value.

### INFO-3: unrelated host-test line in Z2 run

The Z2 mutant log also shows `test_zones_http.c:2469` ("F5: kiln_cfg import_blob
refuses mid-run with the run-active reason") failing, alongside the expected
min_off_s assertions. The baseline run passed, so this is either a flake or a
side effect of the mutation; worth a second look if it recurs.

## Mutation tables

All runs: `tools\negtest.ps1`, baseline PASS, `real_tree_unchanged=true`,
`copies_removed=true`. Firmware mutants Z1-Z9 insert one statement after the
coilpower block (`zones_http_post_parse.c:316-327`).

### Firmware mutants vs drift test and host tests

Drift test: `-Preset pytest -PresetArg tests/test_zone_omit_preserved_drift.py`.
Host tests: `build_host_tests.ps1 -OutDir {OUT} -Only zones` with the kilnfw-host
ExpectPattern. Every host CAUGHT below compiled and failed a real assertion
(`FAIL test_zones_http.c:<line>`), none is a build failure.

| Id | Mutation | Drift test | Host tests (assertion) |
|----|----------|------------|------------------------|
| Z1 | `z->min_on_s++;` | MISSED | CAUGHT (6671, 6734) |
| Z2 | `--z->min_off_s;` | MISSED | CAUGHT (6672, 6734) |
| Z3 | `z->hyst_c =\n 0.0f;` | CAUGHT | CAUGHT (6670, 6734, 6745) |
| Z4 | `#define ZHP_RESET(fld) z->fld = 0.0f` + use | MISSED | CAUGHT (6670, 6734, 6745) |
| Z5 | `memcpy(&z->hyst_c, &zhp_zero, ...)` | MISSED | CAUGHT (6670, 6734, 6745) |
| Z6 | `z -> hyst_c = 0.0f;` | MISSED | CAUGHT (6670, 6734, 6745) |
| Z7 | `(z)->hyst_c = 0.0f;` | MISSED | CAUGHT (6670, 6734, 6745) |
| Z8 | `z->min_off_s ^= 1u;` | CAUGHT | CAUGHT (6672, 6734) |
| Z9 | `z->min_off_s <<= 1;` | CAUGHT | CAUGHT (6672, 6734) |
| Z10 | conditional `z->coil_power_w = 0.0f` inside the omit else | MISSED | CAUGHT (6780) |
| Z11 | `memset(z->coupling_coeff, 0, ...)` after line 1000 | MISSED | CAUGHT (7018, 7020, 6928, 7256, 7258) |
| FP1 | equivalent `!(z->hyst_c == 0.0f \|\| z->hyst_c >= ZONE_HYST_C_MIN)` | FAILED (false positive, LOW-2) | passes (correct) |

### Drift test self-mutants

| Id | Mutation | Result |
|----|----------|--------|
| S1 | operator class back to `[-+*/\|&]?` | MISSED (LOW-3) |
| S2 | drop `(*z).` alternative | CAUGHT |
| S3 | index only `\[\w+\]` | CAUGHT |
| S4 | memset rule removed | CAUGHT |
| S5 | before-span `current_z` condition removed | CAUGHT |
| S6 | `_ALLOWED_RHS` exclusion dropped from exemption | MISSED (LOW-3) |
| S7 | compound exclusion dropped from exemption | CAUGHT |
| S8 | `m.start() >= bstart` dropped | CAUGHT |
| S9 | RHS compared by identifier prefix only | CAUGHT |
| S10 | else-if condition check disabled | CAUGHT |
| S11 | `(elifs and field not in _ALLOWED_RHS)` arm dropped | MISSED (LOW-3) |

Every CAUGHT failed `test_omit_branch_exact_same_field_only` or
`test_firmware_has_no_later_overrides` with an `AssertionError`.

### Page regex mutants (`zones_page.html:2750`)

Run: `node firmware/KilnFW/App/test/test_host_refusal_pages.js`, `-RequireAssertion`.

| Id | Mutation | Result |
|----|----------|--------|
| J1-J4 | remove coilpower / hystc / minons / minoffs | CAUGHT ("blank coilpower/hystc/minons/minoffs dropped") |
| J5 | `minons?\|minoffs?` (adds required minon/minoff) | MISSED, harmless (firmware 400) |
| J6 | add required `maxtemp` | MISSED, harmless (firmware 400) |
| J7 | add omit-defaulted `ct_mask\|thermo_mask` | MISSED (LOW-4) |
