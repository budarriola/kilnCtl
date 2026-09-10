# S14/S15 follow-up: summed k_ct fix and averaging feasibility — 2026-09-10

Follow-up to `docs/audits/s14_s15_ct_calibration_sweep_2026-09-10.md`
(commit `a2ce8aba`), which found two defects while attempting to arm S14
(per-channel over-current, WARN) and S15 (per-zone under-current, WARN) on
this bench: (A) the summed-topology `k_ct_v_per_a` derivation path could
never execute, and (B) the fixture's real current sits at or below the
sweep's noise floor for a single pass. This note resolves A in code and
answers B by analysis, without running new heating on the board.

## A — summed-topology k_ct derivation: oversight, now fixed

Verified against `firmware/KilnFW/App/drivers/control/zones_current_sweep_task.c`
before touching anything: `zone_sweep_task_record_ct_channels()`'s summed
branch deliberately leaves `s_ct_derive.derived_mask` at 0 for the whole run,
and `zone_sweep_plan_k_ct()`'s per-channel loop is gated on that same mask,
so the k_ct-scale half of the sweep could never run on a summed board.

**This was an oversight, not a documented decision.** The code's own comment
justified leaving `derived_mask` at 0 by saying that leaves
"`zone_sweep_push_ct_channel_map()`/`zone_sweep_push_k_ct_v_per_a()`
naturally no-op afterward — neither of those Pico-side fields means
anything in this topology." That reasoning is correct for the CT **map**
(there is no per-relay channel to attribute in a single shared CT) but wrong
for the **scale factor**: a single shared CT still needs exactly one V/A
calibration, and summed topology makes deriving it *simpler* than the
per-zone case (one whole-kiln total, obtained safely because the sweep
already forces every other relay off while measuring one zone — see
`zone_sweep_hw_energize()`'s `0xFF` mask), not harder or meaningless.

Fixed in commit `7b681e0c`: added `zone_sweep_plan_k_ct_summed()`, which
accumulates `s_ct_derive.measured_total_a` from each zone's own
`zone_sweep_summed_normal_a()` result as the sweep runs, refuses if any zone
stayed below the noise floor (`s_sweep.summed_unmeasured_mask != 0` — an
incomplete total scales k_ct low, same reasoning as the per_zone path's
`unresolved_zone_mask` refusal), honors a manually-calibrated shared channel
(CT_COMMISSIONING_PLAN.md step 1), and otherwise calls the same
`zone_sweep_derive_k_ct()` the per_zone path already uses.
`s_ct_derive.derived_mask` itself is untouched, so
`zone_sweep_push_ct_channel_map()`'s "not applicable" reporting for the map
is unaffected — only the k_ct half changes.

Five new host tests cover it (all in `test_zones_http.c`, all passing,
1758/1758 checks in that binary): a clean run deriving the shared channel;
an incomplete pass refusing; a manually-calibrated channel being skipped; and
a negative-control proving the pre-existing per_zone path is untouched by the
new branch (`summed_unmeasured_mask` set to `0xFF` while `s_ct_topology_summed`
stays false still plans all three channels the old way). Negative-tested by
hand per standing practice: force-broke `zone_sweep_plan_k_ct_summed()` to
`return 0` unconditionally, confirmed the `zones_http` host-test executable
went RED, restored the source by hand, confirmed `git diff` on the file was
empty, then rebuilt to confirm GREEN again (34/34 host test executables).

**This fix does not, by itself, arm S14/S15 on this bench** — see B below.
It fixes a real gap for any summed-CT board (which is every board with one
shared CT, including this one) whose load is large enough to clear the
noise floor; this bench's load is not.

## B — is more averaging within reach, and does it matter here?

### The math

`ZONE_SWEEP_NORMAL_NOISE_FLOOR_A` (0.045 A) was derived from the bench's own
observed idle spread: `current_a[2]` wanders 60–79 raw ADC counts around
`zero_counts[2]=63`, a ~19-count peak-to-peak band, converted via
`current_sense.c`'s own `I = delta_counts * Vref / 4096 / (gain*sqrt(2)*k_ct)`
formula (`Vref=3.3V`, `gain=0.715`, `k_ct=1.0`) to ~15 mA, then 3x'd for
margin.

Treating that 19-count band as the *per-poll* (500 ms) spread of
`current_a[]` — which is what the sweep's own existing per-zone window
already averages 8 of, over its 4000 ms `ZONE_SWEEP_SAMPLE_MS` window — gives
a per-poll sigma of roughly 19/4 counts ≈ 3.8 mA, and a post-averaging sigma
after the *already-implemented* 8-sample window of about 3.8/sqrt(8) ≈
1.3 mA. Under this model, a 33–90 mA true signal sits 25–70 sigma above the
noise the sweep *already* averages down to in one normal 4-second run — it
should have been trivially resolved on the very first pass, by a wide
margin, without any additional averaging at all.

It was not resolved. `summed_unmeasured_mask=7` — every zone stayed below
the fixed 45 mA floor on the actual bench run. That mismatch is the load-
bearing fact here: if within-pass random noise, honestly modeled from the
ADC's own count resolution, is already sub-2 mA after the averaging the
sweep already does, then the measurement's real limiting factor on this
board is **not** simple sample-to-sample ADC jitter. Something else —
larger than the counting-noise model predicts, and not reduced by averaging
more polls or more passes — is standing between the fixture's true current
and a resolvable reading.

### What averaging fixes, and what it cannot

Averaging (more polls per pass, or more repeated passes combined) shrinks
*independent, random* noise as 1/sqrt(N) — real, and already largely spent
by the sweep's existing 8-sample window per the calculation above. It does
**nothing** for a *systematic* error: `zero_counts[2]=63` is a measured
front-end offset, not a distribution, and any error in that single number
(mis-calibration, temperature drift since it was set, the flagged-but-
unresolved gain discrepancy between `CS_DEFAULT_GAIN=0.715` and the owner-
stated 2.0 V/V rms conditioning figure) shows up as a constant bias in every
sample and survives averaging over any number of them completely unchanged.
Repeating the sweep 10 times and averaging the results would shrink the
already-small ~1.3 mA random component further (to ~0.4 mA at 10 passes,
~50 s of additional relay cycling) but would not move a systematic offset
error by one microamp.

Given the observed shortfall is far larger than the random-noise model
predicts it should be, the more likely explanation is that the residual
here is dominated by exactly that unresolved systematic term (or that the
fixture's true current is smaller than the 33–90 mA nameplate-derived
estimate — both point the same direction), not by anything more averaging
touches.

### Practicality and recommendation

Implementing repeated-pass averaging is cheap in principle (a loop around
the existing per-zone measurement, on the order of tens of seconds of extra
relay cycling for a meaningful pass count) but was **not implemented**,
because the analysis above gives no reason to expect it would change the
outcome: the component averaging reduces is already, by this bench's own
published noise figures, far smaller than the shortfall actually observed.
Spending relay cycles and bench time on it would not produce a defensible
number — it would risk producing a *falsely confident* one, arming S14/S15
on a value whose main source of error was never characterized at all. That
is exactly the "fabricated/unvalidated threshold" the owner's standing
instruction prohibits, wearing the more convincing costume of "we averaged
it eight ways."

### Residual uncertainty and the arming decision

After accounting for what averaging can and cannot do: the random component
is small (sub-2 mA today, and could be pushed lower with more passes if it
mattered), but the dominant, *undetermined* systematic component — the
zero_counts/gain calibration uncertainty this and the prior audit both flag
as open, with no live-read path from the ESP side to even measure it today
— is not bounded by any number in either audit. A guard armed on
`i_normal_a` derived from this bench would be arming against a threshold
whose largest source of error has no known size. That is not a defensible
margin at any pass count.

**S14 and S15 cannot be honestly armed on this fixture, full stop — not
because the code cannot measure carefully enough, but because this ~4 W
bench load is intrinsically too small relative to an unresolved systematic
offset in the current-sense front end to produce a trustworthy
`i_normal_a` no matter how it is averaged.** This is not a defect in the
guards or the sweep: S14/S15 exist for a real kiln drawing amps, where a
33–90 mA-scale offset uncertainty is negligible against the signal, not for
a fixture whose entire expected draw is the same order of magnitude as that
uncertainty. The honest and correct state for both guards on this board
today is DORMANT, with the reason recorded on the board
(`i_normal_a not measured`) — unchanged by this note. Arming either guard
on a real kiln load, once one is available, requires no further firmware
work beyond what already exists (including this note's finding-A fix for
boards whose `max_expected_power_w`/measured total genuinely clear the
floor); it does not require solving the offset-calibration gap first, since
a real kiln's signal is large enough to swamp it the way this fixture's
signal is not.

## Board state

No new hardware operations were performed for this note — `safety_get_status`
and `profiles_get_exec_status` were checked read-only at the start of this
session (link up, `SaftyFW armed`, `trip_mask 0x0000`, `state=0` idle, no
profile running) and nothing on the board was touched afterward. Relays were
not re-verified because none were commanded; the prior audit's end-of-session
verification (`docs/audits/s14_s15_ct_calibration_sweep_2026-09-10.md`,
"Board state at end of session") stands.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
