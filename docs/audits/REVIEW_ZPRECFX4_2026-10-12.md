# Review: zprecfx4 (2026-10-12)

Adversarial review of the zprecfx4 fix batch on `origin/dev`. It fixes
REVIEW_PROFILESFX5_ZPRECFX3 MED-1, LOW-1, LOW-2 and INFO-1. Reviewed in a clean
worktree at dev tip `21cc28ad7`.

## Commits reviewed

| Commit | Content |
|---|---|
| `3a4191f7c` | Setup wizard: step 6 (`z.max_temp_c > absMaxTempC + 0.01`) and step 7 (`fields.absMaxTempC + 0.01 < maxZone`) compare with 0.01 slack; `PROFILES_SAVE_ERR_MSG_LEN` 128 to 192; new host test `test_validate_io_segment_longest_refusal_margin`; wizard JS cases for fractional ceilings and the step 6 ramp revert; pytest `test_real_saved_true_bool_id_is_not_an_id` |
| `6f127aec4` | Doc only: "Fix status" section in REVIEW_PROFILESFX5_ZPRECFX3 |

## Summary

0 HIGH, 0 MED, 1 LOW, 4 INFO.

The MED-1 false refusal is fixed. The Pico GET prints `abs_max_temp_c` at
`%.6g` (`safety_cfg_http.c:381/416`) and zones GET prints `max_temp_c` at
`%.9g` (`zones_http_get.c:448`). The print gap is at most 0.005 below 10000 C,
which is inside the 0.01 slack. The buffer change has one real consumer and fits.
The new tests are valid: every negtest below was caught by a real assertion
failure, not by a crash or a compile error.

## Findings

### LOW-1: the 0.01 slack admits values that firmware's exact divergence check turns into a standing heat-off alarm

- `firmware/KilnFW/App/drivers/http/setup_wizard_page.html:920` (step 6)
- `firmware/KilnFW/App/drivers/http/setup_wizard_page.html:1648` (step 7)
- `firmware/KilnFW/App/drivers/safety/safety_ceiling_policy.c:67` (`guard_raise`)
- `firmware/KilnFW/App/drivers/safety/config_divergence.c:106-108` (exact compare)

The slack matches `SAFETY_CEILING_EPSILON_C` (0.01), so the wizard now agrees with
`guard_raise`. Firmware has two notions of "equal", though:

- `guard_raise` treats a new target within 0.01 above the Pico ceiling as "no
  raise needed".
- `config_divergence_check` compares the `%.9g`-normalized floats exactly.
  `enforce_ceiling_divergence` (`safety_ceiling_sync.c:393` onward, latch at
  :558) forces all relays off and halts a run on any difference.

**Failure scenario, step 6.** The Pico ceiling is 1260.3 and the operator types
a zone max of 1260.305 (seven significant digits).

1. The new compare accepts it, because 1260.305 > 1260.31 is false. The old
   strict compare refused it.
2. `POST /api/zones` commits. `guard_raise` sees 1260.305 <= 1260.3 + 0.01 and
   writes nothing to the Pico.
3. The divergence check sees 1260.305f != 1260.3f, so heat is disabled.
4. Reconcile (`reconcile_on_link_up_impl`, `safety_ceiling_sync.c:906`) calls
   only `guard_raise`, which stays satisfied. The alarm therefore never heals
   until a human edits one of the two values.
5. The alarm text prints `%.2f` (`config_divergence.c:117/122`), so it reads
   `ESP=1260.31 Pico=1260.30`. In the step 7 case it can show two identical
   numbers.

**Failure scenario, step 7.** The zone max is 1260.3 and the operator types an
`abs_max_temp_c` of 1260.295.

1. The new compare accepts it, because 1260.305 < 1260.3 is false.
2. This writes a Pico ceiling 0.005 tighter than the ESP's. That breaks the
   standing "Pico ceiling never tighter" rule.
3. The `/api/safety/commissioning` POST has no firmware-side refusal for a
   tighter `abs_max_temp_c`. The wizard is the only gate, so the same
   unhealable alarm follows.

**Why LOW, not MED.** Both outcomes fail safe: heat goes off and a visible
alarm latches. The bad values need more digits than the page prints back
(`kgR6`, six significant digits), so typing one is deliberate and unusual. The
root cause is in firmware: the `guard_raise` epsilon and the exact divergence
compare disagree. That is pre-existing; any API client could post these values.
The wizard change only removed the UI's accidental protection.

**Suggested fix.** Choose one:

- Compare at print precision rather than adding 0.01. For example, treat the
  values as equal when `kgR6(a) === kgR6(b)`, and refuse otherwise.
- Have `guard_raise` and `apply_lower` write whenever the normalized values
  differ, so reconcile heals the latch.

Then add a wizard test at the boundary: 1260.305 against a ceiling of 1260.3.
Today's tests only probe 5e-5 and 0.2 (see INFO-2).

### INFO-1: stale comment claims the divergence and readiness code use `SAFETY_CEILING_EPSILON_C`

`firmware/KilnFW/App/drivers/safety/safety_ceiling_policy.h:114-127` (pre-existing).

The comment says `safety_ceiling_sync.c`'s divergence enforcement and
`readiness_ceiling_match_status()` use this epsilon. Neither does:

- The only uses are `safety_ceiling_policy.c:67` and `:135`.
- Divergence is an exact compare (`config_divergence.c:108`).
- Readiness takes the exact `diverged` flag (`readiness_http.h:241`).

This comment is what made a 0.01 slack look "consistent with firmware" (LOW-1).

### INFO-2: the wizard slack tests only bound the slack between 5e-5 and 0.2

`firmware/KilnFW/App/test/test_setup_wizard.js` has these step 6 and step 7
cases:

| Case | Probe |
|---|---|
| fracEqual | 1260.30005 vs 1260.3 |
| fracOver | 1260.5 vs 1260.3 |
| fracEq | 1260.3 vs [1260.30005] |
| fracUnder | 1260.1 |

Any slack from about 5e-5 up to 0.2 passes them all. A mutant slack of 0.1 or
0.15 would go unseen. The 0.5-slack negtests below are caught only because 0.5
is past 0.2. Pinning the 0.005 to 0.01 band needs the boundary case from LOW-1.

### INFO-3: the margin test covers only the `validate_io_segment` refusals

`firmware/KilnFW/App/test/test_profiles_http.c:4503-4534`.

`validate_on_off_rules` messages also reach the same buffer. They are not
measured. Measured by hand, the longest is about 104 bytes, against the 121
bytes of the aux-bound io-segment message, so all of them fit in 192 today.

On consumers:

- The only consumer of `PROFILES_SAVE_ERR_MSG_LEN` is
  `ui_page_profile_builder_review.c:93` (`err_msg` in `do_save_apply`).
- `ui_confirm_show` / `lv_msgbox_add_text` copy the text, so the buffer's
  lifetime does not matter.
- No struct, wire format or static assert depends on the size.

### INFO-4: the +64 B lands in a frame the LVGL stack budget check cannot see; the fix SHA in the prior review's fix status is stale

**Stack.** `do_save_apply` is dispatched through `ui_lcd_lock_run_gated`'s
function pointer, and `lvgl_callback_discovery` has no pattern for run_gated.
`check_all_task_stack_budgets` therefore never charges that frame to `lvgl`.

Measured against a pre-change ELF (LEN = 128):

| Item | Bytes |
|---|---|
| `do_save_apply` frame | 256 |
| `do_save_apply` static deepest (indirect calls unresolved) | 3216 |
| `lvgl` budget total, equal to its ceiling | 7520 |
| Honest free in the 10240 B stack | 2420 |

The bench log shows `lvgl` at 30% free (about 3 KB) live. The extra 64 B are
affordable, but this path's headroom is unchecked (pre-existing).

**Stale SHA.** `docs/audits/REVIEW_PROFILESFX5_ZPRECFX3_2026-10-12.md`'s "Fix
status" (`6f127aec4`) cites `9099f81bc`. That object is on no remote branch; the
landed commit is `3a4191f7c`.

## Negative tests (rerun by this review)

All runs used `tools/negtest.ps1` from the review worktree at `21cc28ad7`. Every
baseline passed, every real tree was unchanged and every copy was removed.

| Mutation | Runner | Verdict | Evidence |
|---|---|---|---|
| Step 6: slack removed (`absMaxTempC + 0.01` to `absMaxTempC`) | node `test_setup_wizard.js`, `-RequireAssertion` | CAUGHT | `FAIL: step6: raw 1260.30005 vs printed ceiling 1260.3 is accepted` |
| Step 6: slack 0.5 | same | CAUGHT | `FAIL: step6: a genuinely over fractional max (1260.5 vs 1260.3) is refused` |
| Step 7: slack removed | same | CAUGHT | `FAIL: step7: typed 1260.3 vs raw zone max 1260.30005 is accepted` |
| Step 7: slack 0.5 | same | CAUGHT | `FAIL: step7: a genuinely lower ceiling (1260.1 vs 1260.30005) is refused` |
| `step6RevertInputsOn`: ramp revert removed | same | CAUGHT | `FAIL: step6RevertInputs restores the displayed ramp value` |
| `PROFILES_SAVE_ERR_MSG_LEN` 192 to 128 | `build_host_tests.ps1 -Only profiles_http`, `-ExpectPattern` | CAUGHT | Compiled. `FAIL ...test_profiles_http.c:4533: longest io-segment refusal leaves >= 16 bytes of margin`, `RUN FAILURES (1)`, no build failure |
| `_parse_saved`: drop `not isinstance(i, bool)` | pytest preset, `tests/test_profile_live_http_client.py` | CAUGHT | `AssertionError: True is not None` at `test_profile_live_http_client.py:270` |
