# Review: GET /api/zones lossless precision fields (ace9ccbf5) and read-back tolerance (f230b49a4)

Date: 2026-10-12. Reviewer: Claude (Opus), read-only review plus negative tests. No board access.

Commits reviewed (both on origin/dev):

- `ace9ccbf5`: `zones_http_get.c` prints `cal_offset_c`, `max_ramp_c_per_hr`,
  `sanity_rate_c_per_min`, `max_temp_c` and `min_temp_c` at `%.9g`. `json_cap` stays
  7360. Adds `test_zones_get_round_trip_precision_fields`, a `kgR6()` display helper in
  `zones_page.html`, and one PcTools `cases_aux` pinning test.
- `f230b49a4`: rebuilds `zones_http_client._float_close` into a per-field tolerance
  (`_FLOAT_FIELD_PRINT_FORMAT`, `_FLOAT_EPS_REL = 1e-6` with a magnitude floor of 1.0),
  pinned against the firmware by `tests/test_zones_http_float_tolerance_pin.py`.

Verdict: **no HIGH or MED findings.** Five LOW and four INFO findings. Both commits are
improvements over what they replace. The main gaps are two tests that do not pin what
they claim to pin.

## Questions asked, answered

| Question | Answer |
|---|---|
| Is 7360 bytes enough for the true worst case? | Yes. Measured 6885 bytes, so 475 bytes of headroom (host test output, this worktree). The widest `%.9g` float32 output is 15 characters (`-1.23456793e-05`; float32 exponents have two digits), and the max-width test uses exactly that value for all five fields. NaN prints as `nan` (3 characters) and infinity as `-inf` (4), both narrower. A real overflow is caught at runtime by the APPEND snprintf return check and answered with a 500 "did not fit" body, never a silent truncation. |
| Are there `-Wformat-truncation` warnings? | No. The fixer's target build (`C:\wt\zprec_tb.log`, `-Wall -Wextra -Werror`) compiled `zones_http_get.c` with no warnings and passed. GCC cannot size-check a runtime heap buffer in any case. |
| Do consumers break? | No functional breakage. The LCD reads the C structs directly. `backup_export.c:527-531` already used `%.9g`. `main_page.html`, `profiles_page.html` and `safety_config_page.html` compare numerically or repost raw values (the latter is now lossless, which is better). MCP writers repost with `repr(float)`, which strtof reads back exactly. The bench cases use tolerance comparisons or finiteness checks. The setup wizard has a display issue (LOW-1). |
| Does `kgR6` round-trip on a save without edits? | Not exactly. It rounds to 6 significant digits, for example 1234.5678 becomes 1234.57 (LOW-4). That is still much better than the old `%.1f`/`%.3f` and below the 0.5 °C noise floor. |
| Can `kgR6` push a value out of range or step validation? | No. Every bound (cal ±50, ramp 1000, sanity 0..20, max 2500, min -50..200) has at most 6 significant digits, and 6-digit rounding is monotone, so an in-range value stays in range. An off-step value was already off-step before this commit. |
| Is a 1e-6 relative tolerance with a 1.0 floor right for float32 read back at `%.9g`? | It never false-fails: the worst float32-plus-`%.9g` relative error measured over 200000 random values from 1e-8 to 1e4 was 6.3e-8, with 0 false failures. The floor of 1.0 makes it too loose below magnitude 1 (LOW-2). |
| Should an unmapped field refuse? | Yes (LOW-3). |
| Does the table-to-firmware pin parse `zones_http_get.c` robustly? | Adequately today, with limits (INFO-2). |

## Findings

### LOW-1: setup wizard steps 3 and 6 show float noise

Where: `firmware/KilnFW/App/drivers/http/setup_wizard_page.html`, lines 2140 (`.s3cal`),
2583 (`.s6ramp`), 2587 (`.s6maxtemp`) and 2590 (`.s6mintemp`), plus the hint text from
`formatZoneLimitC` at line 597.

Failure scenario: these inputs interpolate the raw GET value with no `kgR6()` wrapper. A
stored `cal_offset_c` of 0.1 now renders as `0.100000001`, -1.3 as `-1.29999995`, and a
`max_temp_c` of 1260.3 shows the hint "1260.30005°C". This is cosmetic only: the
comparisons at lines 933 and 936 are numeric and still work, and a save posts the noisy
string, which strtof parses back to the same float32.

Fix: wrap these values in the same 6-significant-digit helper `zones_page.html` uses.

Mutation evidence: not applicable (display only). Rendering was confirmed by reading the
interpolation against the `%.9g` print.

### LOW-2: f230 tolerance floor of 1.0 lets small-magnitude mismatches pass

Where: `tools/PcTools/src/kilnctrl/zones_http_client.py:1273`,
`_FLOAT_EPS_REL * max(abs(a), abs(b), 1.0)`.

Failure scenario: below magnitude 1 the tolerance is an absolute 1e-6. Real `pid_ki`
values in this repo's presets are 1e-4 to 3e-4. With those values:

- `_float_close(3.4e-5, 3.5e-5, "pid_ki")` returns True, a 3% mismatch.
- `_float_close(0.0, 9e-7, "pid_kd")` returns True.
- `_float_close(0.05, 0.050001, "sanity_rate_c_per_min")` returns True.

So a write that landed a slightly different small gain reads back as verified. This is a
strict improvement over the old absolute 1e-3 tolerance, which accepted 1e-4 against
1.1e-3 (11 times larger), so it is not a regression.

Fix: compare after rounding both sides to float32
(`struct.pack('<f', a) == struct.pack('<f', b)`, or allow at most 1 ULP), or use a
relative tolerance with a tiny absolute floor (for example 1e-12) instead of 1.0.

Mutation evidence (negtest, pytest preset, `-Rev f230b49a4`):

| Mutation | Result | Meaning |
|---|---|---|
| floor 1.0 to 100.0 | MISSED | A 100 times looser floor passes the pin test. |
| floor 1.0 to 1e6 | CAUGHT | Only gross loosening is detected. |
| `_FLOAT_EPS_REL` 1e-6 to 1e-3 | CAUGHT | |

`test_tolerance_below_one_step` checks only at magnitude 10.0, and
`test_one_step_mismatch_fails_half_step_passes` checks only at 1.0, so nothing exercises
magnitudes below 1.

### LOW-3: unmapped fields fall back silently, and `pc_link_abort_silence_ms` is unmapped

Where: `zones_http_client.py:1271`, `_FLOAT_FIELD_PRINT_FORMAT.get(field, "%.9g")`.
`_TOP_FIELD_FORM_KEY` (line 657) includes `pc_link_abort_silence_ms`, which GET prints
at `%.0f` (`zones_http_get.c:278`), but the field is not in the table.

Failure scenario: a preset with a fractional `pc_link_abort_silence_ms` such as 1500.4
reads back as 1500 and is reported as a mismatch (`_float_close(1500.4, 1500.0, ...)`
returns False), so a correct write fails verification. The old 1e-3 tolerance failed it
too, so this is not a regression. More generally, any float field added later gets the
`%.9g` rule silently instead of being forced into the table.

Fix: add `pc_link_abort_silence_ms: "%.0f"` to the table. Make an unknown field raise or
refuse, not default. Extend `test_every_preset_float_field_is_mapped_or_nonfloat` to the
float fields of `_TOP_FIELD_FORM_KEY`.

Mutation evidence:

| Mutation | Result | Meaning |
|---|---|---|
| fallback `"%.9g"` to `"%.0f"` | MISSED | The fallback path is untested. |
| firmware `pc_link_abort_silence_ms` `%.0f` to `%.1f` | MISSED | The pin does not cover top-level fields. |

### LOW-4: the round-trip precision test does not pin 9 digits

Where: `firmware/KilnFW/App/test/test_zones_http.c:7594-7621`
(`test_zones_get_round_trip_precision_fields`).

Failure scenario: the test exists to prevent a regression back to lossy printing, but
every value it uses round-trips at `%.8g`, and `0.1234567f` and `-12.3456f` also
round-trip at `%.7g`. A later change from `%.9g` to `%.8g` would pass the test and
reintroduce lossy GET-merge-POST reposts for every float32 that needs 9 digits (for example 0.100000024f).

Fix: use values that need 9 digits, for example `0.100000024f` (prints `0.10000002` at
`%.8g`, a different float), `12.3000145f` and `123.400024f`.

Mutation evidence (negtest, `build_host_tests.ps1 -Only test_zones_http`, kilnfw
`-ExpectPattern`):

| Mutation | Result |
|---|---|
| `cal_offset_c` `%.9g` to `%.8g` | MISSED |
| `cal_offset_c` `%.9g` to `%.7g` | MISSED |
| `max_temp_c` `%.9g` to `%.8g` | MISSED |
| `max_temp_c` `%.9g` to `%.1f` | CAUGHT |
| `sanity_rate_c_per_min` `%.9g` to `%.3f` | CAUGHT |
| `json_cap` 7360 to 6880 | CAUGHT by the max-width test |

### LOW-5: `kgR6` save without edits is lossy, and the helper has no test

Where: `firmware/KilnFW/App/drivers/http/zones_page.html:631`, used at lines 1433, 1484,
1489, 1499 and 1501. `saveAll` posts `input.value`.

Failure scenario: opening the zones page and pressing Save without editing rewrites
1234.5678 as 1234.57 and 0.1234567 as 0.123457. The drift is below the noise floor and
much smaller than before this commit, but it is still a silent write of values the user
did not touch. `kgR6(undefined)` now renders `0` where the old code left the input empty.
No JS test covers `kgR6`.

Fix (optional): keep the exact GET value in a `data-raw` attribute and post it when the
displayed value is unchanged. Add a small `lint_pages` or JS unit check.

Mutation evidence: none (no JS harness exists for this page).

### INFO-1: stale "779 bytes" headroom text

`zones_http_get.c:86-93` (the `json_cap` comment) and the test's `headroom_msg` in
`test_zones_http.c:7795-7805` still say 779 bytes. The commit message and the measured
value are 475. The test asserts the real value, so this is a documentation-only issue.

### INFO-2: limits of the pin regex

`tests/test_zones_http_float_tolerance_pin.py:17` scans the whole file, comments included,
collects a set of formats per key and passes if any of them matches. Today no key is
printed twice (checked: 54 keys, each with one format), so this is correct for now. A
future second print at a different format, or a commented-out old format string, would
mask a mismatch. The regex also skips dynamically built keys such as
`coupling_c%u`-style names.

Mutation evidence:

| Mutation | Result |
|---|---|
| `hyst_c` `%.3f` to `%.4f` | CAUGHT |
| `max_temp_c` `%.9g` to `%.1f` | CAUGHT |

### INFO-3: NaN prints as `nan`, which is invalid JSON

This was true before `ace9ccbf5` and is not made worse by it. Whether stored values are
always finite was not re-checked in this review.

### INFO-4: `model_k_dc` max-width is pinned at its bound, not its widest output

The max-width test comment already documents this (about 10 bytes per zone unmeasured).
475 bytes of headroom covers it.

## Evidence

- Host test run (`build_host_tests.ps1 -Only test_zones_http`, worktree at `ace9ccbf5`):
  PASS, "GET /api/zones max-width render: 6885 bytes, against json_cap=7360 -- measured
  headroom = 475 bytes".
- kilnfw negtest log: `%TEMP%\negtest_logs\20261012_010835_6pw4`, baseline PASS, 3 CAUGHT
  and 3 MISSED as listed above.
- pytest negtest log: `%TEMP%\negtest_logs\20261012_010931_c4fq` (`-Rev f230b49a4`),
  baseline PASS, 4 CAUGHT and 3 MISSED as listed above.
- Tolerance probe: main-tree venv python against the `f230b49a4` source, 200000 random
  float32 round trips, 0 false failures, worst relative error 6.3e-8.

## Fix status

| Item | Status |
|---|---|
| LOW-1 setup wizard float noise | Fixed: `kgR6` added to `setup_wizard_page.html` and applied to step 3 cal, step 6 ramp/max/min inputs, the limit hint, and the related messages. |
| LOW-2 tolerance floor | Pooled for a later fixer (`zones_http_client.py` is being edited by another agent). |
| LOW-3 unmapped fields | Pooled for a later fixer (same file). |
| LOW-4 9-digit test values | Fixed: test uses 0.100000024f, 123.400024f, 12.3000145f, 1000.00006f (cal/ramp/min/max). cal `%.8g` and `%.7g` mutations CAUGHT; max_temp `%.8g` is caught by float32 analysis (1000.0001 reads back as 1000.000122), not by a commit-message note (REVIEW_ZPRECFX2 INFO-4). |
| LOW-5 kgR6 lossy save, empty for undefined | Fixed: inputs keep `data-raw`/`data-disp`, `kgPost` posts the raw value when unedited; `kgR6(undefined/null)` renders empty; JS test `test_zones_kgr6.js`. |
| INFO-1 headroom 779 | Fixed: 475 in `zones_http_get.c` comment and test message. |
