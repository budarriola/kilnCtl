# Review: SaftyFW guard-fixes batch 3 (2026-10-10)

Scope: the fix batch for `docs/audits/REVIEW_SAFTYFW_GUARD_FIX2_2026-10-10.md`
(HIGH-A, HIGH-B, MED-A, LOW-A/B/C, NIT-A/B):

- `e6877a521` -- the fix itself: volatile-install gate, k_ct compare,
  tc_offset clamp report, tests, comment and stack-attribution fixes
- `2d469f412` -- a comment saying the v1-branch clamp is an unobservable
  backstop
- `2bde32eb7` -- marks the fix2 findings fixed

The batch was reviewed on origin/dev `79c129c85`. This is a review only;
nothing was fixed. Owner rule applied throughout: never disable or blind a
thermal guard.

## Verdict

HIGH-A, HIGH-B, MED-A, LOW-B, LOW-C, NIT-A and NIT-B are fixed correctly.
While heat is possible, a volatile install can no longer blind or loosen any
thermal or current guard. LOW-A is only half fixed: the clamp is detected
and logged, but the result never leaves the console UART. There are no new
HIGH or MED findings.

## Gate walk (APPLY_CONFIG_VOLATILE while heat is possible)

`config_store_volatile_would_loosen_safety()` (`config_store.c:1577`) works
in two steps. First it copies cur's value into next for every allowlisted
field and for every provable tightening. Then it memcmps the whole packed
record up to the CRC. Any byte left different means refuse. Each re-entry
fails closed. `config_store_write_volatile()` (`config_store_flash.c`,
around lines 1678-1720) refuses whenever `heat_possible && would_loosen`.

Allowlisted fields, and why each is safe:

| Field | What reads it | Safe? |
|---|---|---|
| format_version, seq, reserved | bookkeeping only | yes |
| calibration_missing | recomputed by the apply handler (`link_task.c`, around lines 2825-2890) and rechecked by safety_core at enable | yes |
| mains_voltage_v, max_expected_power_w (and their set bits) | power reporting only. Clearing the bit only affects the next enable, which fails safe. | yes |
| telemetry_period_ms | telemetry cadence (`config_params.c:601`). No guard reads it. | yes |
| power_window_s | reporting. `link_task.c:1260` hardcodes 120. | yes |
| i_present_a_manual | presence override. It was K_SAFETY-reviewed in fix2 and is unchanged here. | yes |

Fields the tightening rules accept:

- **abs_max_temp_c (S1)**: accepted only when next is set, positive and no
  more than 3.0e38, and either cur is unset, cur is 0 or less, or next is no
  more than cur (`config_store.c:1616-1617`). Off-values are all refused:
  0, negative, NaN and inf (tests at `test_config_store_flash.c:2104-2113`).
  Unset to 0 or negative is refused (2118, 2122). Unset to 800 is accepted
  (2126); with the bit clear, safety_core already ran S1 as off.
- **max_rate_c_per_min (S8)**: the same rule, at `config_store.c:1626-1627`.
  0, negative and NaN are refused (2143-2149). Unset to 0 is refused (2153).
  Off to 15 is accepted (2157). Off to negative is refused (2161).
- **tc_type**: normalised only when cur's tc_type bit is clear. That state
  is only reachable while uncommissioned, so no enable is possible.

Every other guard input is K_SAFETY in the `k_f1_fields` classification
table (`test_config_store_flash.c`), so any change to it is refused while
heat is possible. That covers the S3/S9/S11/S14/S15 currents, i_normal_a,
ct_cal (zero_counts and **k_ct_v_per_a**), tc_offset_c, every timing and
threshold field, and the E-stop and relay polarity fields. The table
classifies every field, and its padding is asserted at 17 bytes.

**k_ct path**: the memcpy that copied cur's k_ct into next is gone, so a
change to k_ct is now compared and refused (HIGH-B). A resend of the same
k_ct is still accepted, which the new test confirms. The only other way to
change k_ct while ARMED is the durable narrow-change path in
`config_store_write_ex`, and it requires `heat_safe` (K4 de-energized). That
makes it equivalent to an idle write.

## Findings

### LOW-A1: tc_offset clamp report reaches the console only; accessor has zero callers, propagation untested

`boot_load` latches `s_load_tc_offset_clamped` (`config_store_flash.c:785`)
and prints one console line (`:866`). The accessor
`config_store_tc_offset_was_clamped()` (`:881`, declared in
`config_store.h:1559`) has **no callers** anywhere in `firmware/SaftyFW/src`.
Nothing sets a DIAG flag, sends a wire field or reports a readiness item, so
neither the ESP nor the UI ever learns that a stored offset was out of range
and was clamped. Only someone watching the Pico console at boot would see it.

The propagation path also has no tests. Three negtest mutants were MISSED:
M8 drops the clamp flag in `find_latest_ex`, M9 drops it in
`find_latest_multi_ex`, and M10 forces the boot_load latch to false. Only
the unpack-level flag is covered (M7 CAUGHT).

Not a guard bypass: the clamp itself still applies, which M11 shows. Fix
direction: surface the accessor as a DIAG/status bit that the ESP forwards
to readiness. Then add one store-level test that commits an out-of-range
offset and asserts that `config_store_tc_offset_was_clamped()` reads true
after `config_store_load()`.

### NIT-C: 2bde32eb7 credits the wrong SHA

`REVIEW_SAFTYFW_GUARD_FIX2_2026-10-10.md:249` says "All findings fixed in
2d469f412". The fix is `e6877a521`; `2d469f412` is the two-line v1-branch
comment. It also says LOW-A is fixed, which this review rates only half done
(LOW-A1).

### INFO-1: tightening tests use coarse boundaries

M6 adds a tolerance of 0.4 C/min to the max_rate comparison
(`nr <= cr + 0.4f`) and was MISSED, because the raise test steps 20.0 to
20.5. The abs_max raise test steps 1100 to 1101, so it only catches a
tolerance of at least 1 C. A raise by one float ulp (`nextafterf(cur,
INFINITY)`) would pin `<=` exactly. A real `<` to `<=`-style tolerance
would need to be at least that large to slip through. Not a bypass of the
code as written.

### INFO-2: abs_max finite bound is untested from an unset cur (equivalent at system level)

M3 removes `na <= 3.0e38f` and was MISSED. From a set cur, inf is still
refused by `na <= ca`. From an unset cur, the gate would accept inf. But
`validate_ranges` runs `RANGE_F32_FINITE(abs_max_temp_c)`
(`config_params.c:714`) before write_volatile, so inf never reaches the
gate. On top of that, S1 was already off with the bit clear. This is
defence in depth with no test. max_rate behaves the same way, because
off to inf is the same as off.

### INFO-3: (ca <= 0) branch untested

A mutation there can only make the gate refuse more (an unset cur is
already handled by `!cur_abs_set`), so it is not safety-relevant.

### INFO-4 (pre-existing, out of scope): k_ct <= 0 accepted by validation

`CHECK_F32_FINITE` lets k_ct be 0 or negative. Such a value can be stored
by an idle write or a durable write that passes heat_safe. That silently
skips S14/S15 for the channel. It also leaves current sensing
un-commissioned (S9), so the problem shows up at the next enable. This is
not a heat-possible path, and it predates this batch. Consider
`CHECK_F32_POS` for fitted channels.

### INFO-5: clamp log runs pre-scheduler

The `%.1f` snprintf in boot_load runs on the main stack before the
scheduler starts. The per-task stack check does not cover it. It is small
and one-shot.

## LOW-C: v1 clamp mutant equivalence -- confirmed

The v1 branch builds its scratch record from `config_store_default()`
(tc_offset_c = 0) and overlays only tc_type and ct_cal. The clamp there can
never fire, so removing it is an equivalent mutant. The comment in
`2d469f412` is accurate.

## Negative tests

Command: `tools\negtest.ps1 -Preset saftyfw-host -Mutations <12 mutations>
-ExpectPattern "SAFTYFW HOST TESTS: (FAILED|BUILD FAILED)"`. The baseline
passed, `real_tree_unchanged` was true, and the copies were removed. (The
first run without `-ExpectPattern` errored at baseline, because the
preset's default pattern matched passing test titles that contain "FAIL".)

| # | Mutation | Result |
|---|---|---|
| M1 | abs_max: drop `na > 0` | CAUGHT |
| M2 | max_rate: drop `nr > 0` | CAUGHT |
| M3 | abs_max: drop finite bound | MISSED (INFO-2, equivalent at system level) |
| M4 | k_ct re-allowlisted | CAUGHT |
| M5 | abs_max: cur treated as off up to 2000 | CAUGHT |
| M6 | max_rate: +0.4 tolerance | MISSED (INFO-1) |
| M7 | unpack clamp flag dropped | CAUGHT |
| M8 | find_latest_ex propagation dropped | MISSED (LOW-A1) |
| M9 | find_latest_multi_ex propagation dropped | MISSED (LOW-A1) |
| M10 | boot_load latch forced false | MISSED (LOW-A1) |
| M11 | v2-branch clamp call removed | CAUGHT |
| M12 | tc_type always normalised | CAUGHT |

## Stack budget

The SaftyFW target was built in the review worktree, because the check
needs a fresh ELF. The build PASSED and produced SaftyFW.elf.
`check_saftyfw_task_stack_budgets.py` then exited 0, with "9 tasks graded,
7 INDETERMINATE" and every task `[ok]`. safety_core measures
total=2208 B against a ceiling of 2208 B (declared 6144 B, margin 3936 B).
That is exactly at its pinned ceiling, consistent with the re-pin whose
attribution LOW-B corrected.

## Summary

| Fix2 finding | Status |
|---|---|
| HIGH-A max_rate 0 as tightening | fixed |
| HIGH-B k_ct allowlisted | fixed |
| MED-A abs_max off-values | fixed |
| LOW-A clamp reporting | partly fixed (LOW-A1) |
| LOW-B stack attribution | fixed |
| LOW-C v1 clamp | confirmed equivalent |
| NIT-A, NIT-B | fixed |

New in this review: LOW-A1, NIT-C, INFO-1 to INFO-5.
