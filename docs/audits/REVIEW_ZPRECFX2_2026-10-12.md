# Review: zones precision review fixes (7725f516f)

Date: 2026-10-12. Reviewer: Claude (Opus). Read-only review plus negative tests. No board access.

Commit reviewed (on origin/dev): `7725f516f`, "zones GET precision review fixes: setup
wizard kgR6, lossless unedited save, 9-digit round-trip test values, headroom 475". It
addresses LOW-1, LOW-4, LOW-5 and INFO-1 of `docs/audits/REVIEW_ZONESPREC_2026-10-12.md`.

Verdict: **one MED, two LOW, four INFO.** The zones page part (LOW-5) is correct. The
wizard part (LOW-1) causes a regression: step 6 now asks the user to confirm a ceiling
change on a save where nothing was edited, and steps 3 and 6 save the rounded value. This
is the same lossy save that LOW-5 fixed on the zones page.

## Questions asked, answered

| Question | Answer |
|---|---|
| Is the unedited-field check in `kgPost` correct? | Yes on the zones page. `data-disp` is the same string as the input's `value` attribute, and a `type=number` input returns a valid float string unchanged (exponent forms such as `3.1e-7` are valid too). Typing a value and then typing the original back posts the raw value, which is what the user asked for. A field the user clears is `''`, which is not equal to `data-disp`, so `''` is posted as before. The page is °C only (no unit conversion). No preset or wizard code writes these five inputs on this page. One code path does: `copyCustomStackValues` (INFO-2). |
| Can a raw value be stale? | No new risk. `data-raw` comes from the same `current` GET snapshot that the displayed value comes from. Every re-render (`loadCurrent` after a save, or `renderZones` when the thermo/relay count changes) rebuilds both. There is no zone switch; all zones are rendered at once. If the user types while a save is in flight, the page skips the re-render, and the old `current.generation` makes the next save fail with 409 `zones_config_stale`, so no lost update. |
| Does the wizard still validate limits at full precision? | It validates the rounded value, and that is better than before. `validateStep6` compares `parseFloat(kgR6(raw))` against the Pico's `abs_max_temp_c`, which `/api/safety/commissioning` prints at `%.6g` (`safety_cfg_http.c:416`). Both sides are now rounded the same way. Before the fix, a raw 1234.5649 compared against a printed ceiling of 1234.56 failed for no real reason. Rounding up by up to 0.005 °C stays inside `SAFETY_CEILING_EPSILON_C` (0.01), so it never triggers a write to the Pico ceiling. The wizard has no check that max is above min, but it never had one; the firmware enforces it. Step 7 still compares raw zone maxima with no rounding (INFO-1). |
| Is 1000.00006f a sound test value? | Yes. The nearest float32 is 1000.00006103515625, one ulp (6.1e-5) above 1000. `%.6g` and `%.7g` print `1000` and `%.8g` prints `1000.0001`, which reads back as 1000.000122. So only `%.9g` round-trips. 0.100000024f, 123.400024f and -12.3000145f also need all 9 digits (checked with float32 emulation). 0.00031f does not (LOW-2). |
| Does the commit make the `test_zones_type_toggle.js` failure worse? | Yes, slightly. At the parent commit `ed930dd18` it fails with `kgR6 is not defined`. At `7725f516f` and at the dev tip `3d1ff07d4` it fails with `kgRawAttrs is not defined`, because the `zoneBlock` template range it extracts now calls `kgRawAttrs` too. A fix that adds only `kgR6` to that sandbox will still fail. No other `test_*.js` file regressed: all the others pass at the parent, at the commit and at the dev tip. |

## Findings

### MED-1: wizard step 6 asks to confirm an unchanged ceiling, and steps 3 and 6 save the rounded value

Where: `firmware/KilnFW/App/drivers/http/setup_wizard_page.html`. `readLiveZones` (around
line 2631) parses the inputs, which now hold `kgR6(raw)`. `step6LimitConfirmLines` (line
929) compares that against the raw GET value with `!==`. `step3ReadCurrentZones` (line
2154) and `zoneToPostParams` then post the parsed value.

Failure scenario: before this commit the step 6 inputs held the raw `%.9g` string, so an
unedited save parsed back to exactly the GET value and produced no confirm lines. Now, for
any stored `max_temp_c` or `max_ramp_c_per_hr` that is not exactly a 6-digit decimal, the
two values differ. That covers every fractional value as float32 stores it, such as 123.4
(prints `123.400002`) or 0.1. Pressing "Save & mark done" without editing then opens the
consequence-bearing confirm "This changes the temperature/ramp ceilings that bound every
firing:", with lines such as `Zone 1 max ramp: 123.4°C/hr -> 123.4°C/hr`. Both sides of
the arrow look identical, because `formatZoneLimitC` rounds both. Reproduced in Node
against the commit's own functions:

```
123.400002 -> ["Zone 1 max temp: 123.4°C -> 123.4°C","Zone 1 max ramp: 123.4°C/hr -> 123.4°C/hr"]
1234.5678  -> ["Zone 1 max temp: 1234.57°C -> 1234.57°C", ...]
1250       -> []
```

A confirm for a safety ceiling that fires on a no-op save teaches the user to click through
it. If the user confirms, the wizard writes the rounded value: 1234.5678 becomes 1234.57.
For values that are short decimals this is harmless, because 123.4 parses back to the same
float32. Step 3 does the same to `cal_offset_c`, without a confirm. This brings back, in the
wizard, the lossy unedited save that LOW-5 removed from the zones page.

Fix: use the zones page approach in the wizard. Either keep `data-raw`/`data-disp` on
`.s3cal`/`.s6maxtemp`/`.s6ramp`/`.s6mintemp` and read them through a `kgPost` equivalent
before `parseFloat`, or compare in `step6LimitConfirmLines` with
`kgR6(was) !== kgR6(now)` and fall back to the GET value whenever the displayed value is
unchanged. `step6RevertInputs` must then reset the raw attributes too. Add a wizard test:
an unedited step 6 with max 123.400002 produces no confirm lines and posts the raw value.

### LOW-1: the wizard's `kgR6` use has no test (negtest evidence)

Where: `setup_wizard_page.html` (`kgR6` and its seven call sites). `test_zones_kgr6.js`
extracts only `zones_page.html`'s copy. `test_setup_wizard.js` checks
`formatZoneLimitC(1250) === '1250°C'`, which passes with or without rounding.

Evidence: see the negtest table. Every wizard mutation (the NaN guard, `formatZoneLimitC`,
the step 3 and step 6 inputs, `step6RevertInputs`) is MISSED by the whole JS suite. The two
`kgR6` copies can also drift apart unseen.

Fix: add a wizard case to `test_zones_kgr6.js` (or `test_setup_wizard.js`) that covers
`formatZoneLimitC(1260.30005)` returning `'1260.3°C'`, `kgR6(NaN)` returning `''`, and the
MED-1 no-op save.

### LOW-2: `sanity_rate_c_per_min` test value does not pin `%.9g`

Where: `firmware/KilnFW/App/test/test_zones_http.c`,
`test_zones_get_round_trip_precision_fields`, `z->sanity_rate_c_per_min = 0.00031f`.

Failure scenario: 0.00031f round-trips at `%.6g`, `%.7g` and `%.8g` (all print `0.00031`).
So a mutation that weakens only the sanity field's format to any of those passes. The
value still catches the original bug (the old `%.3f` printed `0.000`). The other four
values each need all 9 digits.

Fix: use a value that needs 9 digits and is still inside the 0..20 range, for example
`0.000310000003f`, or `1.00000012f`, which is one ulp above 1. Keep a sub-0.0005 value
somewhere so the `%.3f` regression stays covered.

### INFO-1: step 7 still shows and compares raw zone maxima

`setup_wizard_page.html:2799-2844` and `validateStep7` (line 1640) use the raw
`z.max_temp_c`. The hint therefore prints, for example, "1260.30005 °C". When the
`s7AbsMax` field holds the Pico's `%.6g` value, a raw zone max such as 1234.5649 compared
with 1234.56 refuses. This predates the commit and is the same class as LOW-1 of the
earlier review, but that fix did not reach step 7.

### INFO-2: inherited pre-fill does not carry the raw value

`copyCustomStackValues` (`zones_page.html:1939`) copies the displayed `value` from the
source stack, and the destination keeps its own `data-raw`/`data-disp`. If the source's
displayed value differs from the destination's, the 6-digit display is posted, which is
lossy only past the 6th digit. If the two displays match, the destination's own raw is
posted, a difference of under 6 significant digits. Both effects are far below the noise
floor. Noted so the lossless claim is understood as "unedited, not copied".

### INFO-3: typing exactly the displayed value posts the raw value

If the user deliberately types `1234.57` over a displayed `1234.57` (raw 1234.5678), the
page posts 1234.5678. This is intended and negligible.

### INFO-4: the fix-status row refers to a note the commit message does not have

`REVIEW_ZONESPREC_2026-10-12.md`, LOW-4 row: "max_temp `%.8g` see commit message". The
commit message has no such note. By the float32 analysis above, a `%.8g` mutation on
`max_temp_c` is caught (`1000.0001` reads back as 1000.000122).

## Negative tests

Runner: `tools\negtest.ps1 -Command` with `-RequireAssertion`, on dev tip `3d1ff07d4`
(which contains `7725f516f`). Each run executes every
`firmware/KilnFW/App/test/test_*.js` except `test_zones_type_toggle.js`, which fails on dev
for an unrelated reason (see above) and would make the baseline fail. The mutation JSON was
kept in a private scratch directory.

Baseline PASS. Result: 4 CAUGHT, 5 MISSED. negtest exited 1, the real tree was unchanged
and the copies were removed. (An earlier identical run reported ERROR only because this
document was written into the worktree during the run. Its verdicts were the same.)

| Mutation | File | Verdict | Caught by |
|---|---|---|---|
| `kgPost` always posts `el.value` (no raw branch) | zones_page.html | CAUGHT | `FAIL: unedited posts raw` |
| `kgPost` posts raw even when edited | zones_page.html | CAUGHT | `FAIL: edited posts value` |
| `kgR6` NaN guard returns `0` | zones_page.html | CAUGHT | `FAIL: NaN renders empty` |
| `kgR6` without the undefined/null early return | zones_page.html | CAUGHT | `FAIL: null renders empty` |
| `kgR6` NaN guard returns `0` | setup_wizard_page.html | MISSED | - |
| `formatZoneLimitC` without `kgR6` | setup_wizard_page.html | MISSED | - |
| `.s6maxtemp` input without `kgR6` | setup_wizard_page.html | MISSED | - |
| `.s3cal` input without `kgR6` | setup_wizard_page.html | MISSED | - |
| `step6RevertInputs` without `kgR6` | setup_wizard_page.html | MISSED | - |

Note: removing only the `undefined` part of the zones guard cannot be caught, because
`Number(undefined)` is NaN and the `isFinite` fallback still returns `''`. The `null` case
is the one that catches it.

## Method

- Ran every `test_*.js` in Node at `ed930dd18` (parent), `7725f516f` and `3d1ff07d4`.
- Emulated float32 in Python (`struct`) to check which precisions round-trip each test value.
- Exercised the wizard's `kgR6`, `formatZoneLimitC` and `step6LimitConfirmLines`,
  extracted from the commit, in Node.
- Read `safety_cfg_http.c` (the `%.6g` ceiling print) and `safety_ceiling_policy.c`
  (`SAFETY_CEILING_EPSILON_C`) to judge the full-precision ceiling interaction.

## Fix status

| Finding | Status |
|---|---|
| MED-1 | Fixed: the wizard has `kgRawAttrs`/`kgPost` (copies of the zones page helpers); `.s3cal`, `.s6maxtemp`, `.s6ramp`, `.s6mintemp` carry `data-raw`/`data-disp` and are read through `kgPost`. `readLiveZones` and `step6RevertInputs` are now top-level `step6ReadLiveZones`/`step6RevertInputsOn` so they are testable. An unedited step 6 gives zero confirm lines and posts the raw value. |
| LOW-1 | Fixed: `test_setup_wizard.js` covers null/NaN, `formatZoneLimitC`, unedited vs edited posting, revert, step 3 read-back, and a drift pin on the three helper lines. The 5 listed mutations (plus 2 more) are CAUGHT by failing assertions. |
| LOW-2 | Fixed: `sanity_rate_c_per_min` test value is 0.000100027195f (needs all 9 digits, still below 0.0005). |
| INFO-4 | Fixed: the LOW-4 note in REVIEW_ZONESPREC_2026-10-12.md is corrected. |

## INFO triage (2026-10-12)

- INFO-1: won't fix: step 7 raw-max comparison predates the commit and a mismatch only shows past the 6th significant digit; it refuses toward safe (the user re-enters the shown value). Not worth a wizard change without a bench report.
- INFO-2: won't fix: the review itself states the effect is far below the noise floor; "lossless" means unedited, not copied.
