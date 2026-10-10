# Review: SaftyFW guard-fixes batch 2 (2026-10-10)

Scope: the fix batch for `docs/audits/REVIEW_SAFTYFW_GUARD_FIXES_2026-10-09.md`
HIGH-1, MED-1, LOW-1, LOW-3, LOW-4 and INFO (LOW-2 documented only). The
commits are `20da358a0` (volatile-install gate), `c815092ff` (tc_offset load
clamp), `9c9b2245b` and `546cc2878` (tests), `163980001` (docs, link_task
comment, stack re-pin), and `2b5fadbe3`/`78dba11f7` (doc SHA fixes). They
were reviewed on origin/dev `78dba11f7`. This is a review only; nothing was
fixed.

Owner rule applied throughout: never disable or blind a thermal guard.

## Findings

### HIGH-A: max_rate_c_per_min nonzero -> 0 passes as a "tightening" and turns S8 off mid-firing

`config_store_volatile_would_loosen_safety()`
(`firmware/SaftyFW/src/config_store.c:1574-1580`) treats any set -> set
change with `next->max_rate_c_per_min <= cur->max_rate_c_per_min` as a
provable tightening. But 0 is a legal value with the field set:
SET_PARAM 0x0204 uses `CHECK_F32_RANGE_OR_ZERO`
(`config_params.c:495`) and `validate_ranges` uses
`RANGE_F32_RANGE_OR_ZERO` (`config_params.c:775`). S8 runs only
`if (cfg->max_rate_c_per_min > 0.0f)` (`safety_guards.c:765`). In S8, 0
means "off": it is the loosest possible value, not the tightest.

Failure scenario: the kiln is firing with max_rate = 300 C/min. The ESP (a
bug, a bad journal replay, or a hostile LAN client with admin) sends
SET_PARAM 0x0204 = 0, then APPLY_CONFIG_VOLATILE. validate_ex accepts it,
the gate sees 0 <= 300 and calls it a tightening, and write_volatile
installs it. `safety_core_load_guard_cfg()` picks it up on the next tick,
and S8 (runaway rate) is disabled with K4 energized. That is exactly the
"blind a guard while heat is possible" case HIGH-1 was meant to close. The
old per-field list had the same hole, but this batch now documents it as a
proven tightening (`config_store.c:1533`), and no test covers it.

Fix direction: in the tightening rule, treat `<= 0` and NaN as the loosest
value for max_rate. Accept set -> set only when `next > 0 && next <= cur`,
or when `cur <= 0` (off -> any enabled value). Add a gate test for 300 -> 0.

### HIGH-B: k_ct_v_per_a is allowlisted as "power estimate only", but it scales the S3/S9/S11/S6b presence threshold and gates S14/S15 and S9

The allowlist (`config_store.c:1527`, copy at `:1557`) lets every
`k_ct_v_per_a[]` change through while heat is possible. Its justification
("power estimate only", repeated in `current_task.c:244-248`) is stale:

- `current_presence_is_flowing()` (`current_presence_policy.c:12-25`) uses
  `k_ct_v_per_a` directly. When k_ct > 0 the counts threshold is
  `i_present_a * gain * sqrt2 * k_ct * 4096 / vref`. `current_sense.c:387`
  feeds it the live `s_cal.k_ct_v_per_a[n]`. A 1000x larger k_ct raises
  the presence threshold 1000x. A k_ct <= 0 drops the channel to the
  fallback margin branch.
- `safety_core.c:1272-1277` makes `amps_valid_for_ct[ch]` depend on
  `k_ct > 0`. With k_ct <= 0, S14/S15 (overcurrent and under-current
  against i_normal_a) are skipped for that channel.
- `config_store_current_sensing_commissioned()` (`config_store.h:1048-1062`)
  requires k_ct > 0 on every fitted channel. safety_core reloads it at
  `safety_core.c:501` and it feeds S9.
- `cs_counts_to_amps()` (`current_sense.c:265-275`) scales the amps S14
  compares by `1 / k_ct`.

Validation does not bound k_ct. It is `CHECK_F32_FINITE` only, so
negatives pass. The link_task apply handler calls
`current_task_reload_cal()` right after install, so the change takes effect
at once.

Failure scenario: while firing, an APPLY_CONFIG_VOLATILE carries
`k_ct_v_per_a[2]` x1000 (or a typo of mV/A for V/A). The gate allowlists
it, and the next current pass needs 1000x the real current to report
"present". S3 (current with K4 off: welded contactor) and S11/S6b presence
facts never fire, and S14 reads 1/1000 of the real amps, so overcurrent
never trips. A negative k_ct instead silently disables S14/S15 and
un-commissions S9. Either case blinds a current guard with heat possible.

This is also inconsistent with the rest of the same formula: `gain[]` and
`zero_counts[]` are compared (refused), but k_ct, the third factor in that
product, is not.

Fix direction: drop `k_ct_v_per_a` from the allowlist so it is compared
like gain. If the ESP needs to retune it mid-run, allow only a
"provably more sensitive" direction, but that is presence-tighter and
overcurrent-looser at the same time, so there is no safe direction:
compare it. Also correct the stale comments at `current_task.c:244-248`
and the `config_store.c:1527` allowlist row.

### MED-A: the tightening rule trusts caller validation; write_volatile itself never validates

`config_store_write_volatile()` (`config_store_flash.c:1655-1690`) installs
whatever the gate passes and never runs `validate_ranges`. The gate's
abs_max rule (`config_store.c:1568-1573`) accepts `next <= cur` with the bit
set, so `abs_max_temp_c = 0` (or any negative value)
counts as "lowered". S1 runs only `if (cfg->abs_max_temp_c > 0.0f)`
(`safety_guards.c:649`), so 0 disables S1.

Today this is caught only because the one caller, link_task's apply handler
(`link_task.c:~2800-2873`), runs `validate_ex` first, and abs_max uses
`RANGE_F32_POS` when set. The gate is meant to be fail-closed by itself
("a field added later is refused by default"), but for abs_max and max_rate
its safety depends on a check in a different file. A second caller, or a
refactor that reorders validate and install, reopens S1-off-mid-firing with
no test failing.

Failure scenario: a later change adds a second install path (a console
command or a journal replay helper) that skips validate_ex. It sends
abs_max = 0 while firing. The gate sees 0 <= 1300, accepts it, and S1 is
off.

Fix direction: in the gate, require `next > 0` and `isfinite(next)` for
both tightening rules (same fix as HIGH-A), or call `validate_ranges()`
inside write_volatile and refuse on failure.

### LOW-A: the tc_offset load clamp is silent

`config_store_clamp_stored_tc_offset()` (`config_store.c:742-752`, called
at `:822`, `:889`, `:958`) clamps a stored offset beyond
+/-`CONFIG_PARAMS_TC_OFFSET_ABS_MAX_C` to the bound with no log line, no
reject_info and no wire flag. Before this batch the slot was refused,
which was loud and refused heat.

Failure scenario: a record written by older firmware has
`tc_offset_c = +80` (the real sensor reads 80 C low, and that offset was
the correction). The new firmware loads it, clamps it to +50 and boots
clean. The Pico's thermocouple now reads 30 C cooler than the real
temperature, so S1 (abs_max) trips 30 C late. S10 (ESP/Pico cross-check)
may catch the divergence, but S10 is WARN-only. The opposite case (a
stored -80 clamped to -50) makes the Pico read 30 C hot, which is the safe
direction.

The clamp is the right direction for "do not brick a board over one
field", but an S1-relevant value changed silently. Fix direction: log it
once at boot and surface it (reject_info or a commissioning flag) so the
ESP's readiness page can show "safety TC offset clamped, re-commission".

### LOW-B: stack re-pin is misattributed

`check_saftyfw_task_stack_budgets.py:375-377` re-pins `safety_core` from
2184 to 2208 B and says the growth is from this batch's load-time clamp.
This batch did not touch `safety_core.c` or `safety_guards.c`, and the
clamp runs only at boot load (`config_store_unpack_ex`, from the boot/load
path), never in the safety_core task. The 24 B growth most likely came from
the earlier F1-F7 batch `d53125fe`, which changed safety_core/safety_guards
and did not re-pin. Headroom is fine: `SAFETY_CORE_STACK_WORDS` is
256 * 6 = 6144 B, so ~3.9 KB remain. Only the comment is wrong. A wrong
attribution here means the next reviewer looking for what grew the stack
will look in the wrong commit.

### NIT-A: the probe doc comment overstates "GRACE/INIT/TRIPPED never trigger the gate"

`config_store_flash.c:1613-1614` says GRACE/INIT/TRIPPED never trigger the
gate. That is true only for the ARMED fallback. With the link_task probe
registered (`link_task.c:3470`), stale or absent ESP context counts as heat
possible, so in GRACE or idle right after boot (before the first context
frame) the gate applies. This is fail-closed and correct, but an early ESP
config replay that loosens a field is refused until context arrives. Fix
direction: reword the comment so nobody "fixes" the code to match it.

### NIT-B: mains_voltage_v is described as "no guard trips"

The allowlist row (`config_store.c:1525-1526`) is accurate for guards, but
mains_voltage_v's fields_set bit feeds `calibration_missing`, which gates
REQUEST_ENABLE. The normalisation copies cur's bit into the compare record,
so clearing the bit mid-run is not seen. The effect is limited to the next
enable (it does not trip or blind a running guard), and the handler
recomputes calibration_missing anyway. Worth one word in the comment.

## Items checked and found sound

- **Probe race and registration order.** `config_store_write_volatile()`
  has one caller (`link_task.c:2873`). The apply, enable and context
  handlers are all serialised in link_task, and an enable accepted just
  before the apply is covered by the enable-age window. The probe is
  registered in `link_task_start()` (`link_task.c:3470`) before the task is
  created. Before the context mutex exists, `s_context_lock != NULL` is
  false, so the probe reports "context never received", which counts as
  heat possible (fail-closed). A 50 ms lock timeout also counts as heat
  possible.
- **Other allowlist entries.** `max_expected_power_w`, `power_window_s` and
  `telemetry_period_ms` are not read by any guard (`power_window` is
  hardcoded to 120 s at `link_task.c:~1260`). `i_present_a_manual` is safe
  because `i_present_a` itself is compared after the handler's finalize
  step. The pack covers every struct field
  (`test_volatile_gate_classifies_every_record_byte`). memcmp on packed
  bytes treats any NaN bit-pattern change as a change, which is
  conservative.
- **NaN on abs_max/max_rate.** `next <= cur` is false for a NaN `next`, so
  the field is not normalised and the memcmp refuses it. A NaN `cur` makes
  every `next` fail the compare, which also refuses. Both fail closed (but
  see HIGH-A and MED-A for 0 and negatives).
- **tc_type.** Normalised only when cur is unset. TC_TYPE is in
  `all_required_set`, so a board with it unset cannot energize; unset ->
  set while "heat possible" is reachable only through the probe's stale
  context case.
- **Backfill before compare.** `config_store_backfill_legacy_ct_topology()`
  now runs before the gate (`config_store_flash.c:1676-1679`), so a legacy
  record no longer reads as a ct_topology change.

## Test quality: negtest spot-check

Mutations the fixer did not try, run with
`tools\negtest.ps1 -Preset saftyfw-host -Mutations <file>` from the review
worktree:

Baseline (unmutated) passed.

| Mutation | Result |
|---|---|
| Remove the clamp call at `config_store.c:822` (current-format load) | CAUGHT (`test_unpack_clamps_out_of_bound_tc_offset`) |
| Remove the clamp call at `config_store.c:889` (v2 migration branch) | MISSED |
| Remove the clamp call at `config_store.c:958` (v1 migration branch) | MISSED |
| tc_type normalised even when cur is set (`!cur_tc_set \|\| cur_tc_set`) | CAUGHT |
| abs_max tightening accepts a raise of up to +100 C (`<= cur + 100`) | MISSED |
| max_rate tightening accepts a raise of up to +10 C/min (`<= cur + 10`) | MISSED |
| `config_store_heat_possible()` fallback returns false instead of ARMED | CAUGHT |
| abs_max set -> unset counted as a tightening | CAUGHT |

negtest's final verdict line reads ERROR only because this review doc was
written into the worktree while it ran (the "real tree changed" guard);
`git status` showed that file and nothing else, and every mutated copy was
removed.

### LOW-C (from the spot-check): the tightening compare and two of three clamp sites are untested

- The tests prove that abs_max and max_rate are compared at all, but not
  the direction of the compare. A gate that accepts abs_max 1300 -> 1400
  or max_rate 300 -> 310 while firing passes the whole suite. That is a
  direct S1/S8 loosening the gate exists to refuse. Add one test per
  field: heat possible, raise by a small amount, expect refusal; and an
  equal-value resend, expect accept.
- The clamp is exercised only through the current-format load branch
  (`config_store.c:822`). Removing the call in the v2 migration branch (`:889`)
  or in the v1 migration branch (`:958`) leaves every test green, so an
  old-format record with an out-of-bound offset would go back to being
  refused (LOW-1 regressed) with no test failing. Add v1- and v2-packed records
  with tc_offset 80 to the clamp test.

## Summary

| Id | Severity | One line |
|---|---|---|
| HIGH-A | HIGH | max_rate 300 -> 0 counts as a tightening; installs mid-firing and turns S8 off |
| HIGH-B | HIGH | k_ct_v_per_a allowlisted, but it scales the presence threshold and gates S14/S15/S9; can blind current guards mid-firing |
| MED-A | MED | gate's tightening rule depends on caller validation; abs_max 0 would pass as "lowered" and disable S1 |
| LOW-A | LOW | tc_offset load clamp is silent; a clamped +80 makes S1 trip 30 C late with no warning |
| LOW-B | LOW | safety_core stack re-pin comment blames this batch; growth came from d53125fe |
| LOW-C | LOW | tests miss the tightening direction (abs_max +100, max_rate +10 pass) and 2 of 3 clamp sites |
| NIT-A | NIT | probe comment says GRACE/INIT/TRIPPED never trigger the gate; with the probe they do (fail-closed) |
| NIT-B | NIT | mains_voltage_v "no guard" row omits its calibration_missing (enable gate) role |
