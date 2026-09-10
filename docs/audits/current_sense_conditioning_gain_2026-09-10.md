# Current-sense conditioning gain: 0.715 vs. 2.0 — resolved

**Status:** resolved, no firmware/config change needed. **Date:** 2026-09-10.

## The dispute

- Owner statement: the CT is 1 V AC rms per 1 A AC rms, and the conditioning
  circuit produces 2 V DC for a 1 V rms AC input (known zero offset aside).
- Committed firmware: `CS_DEFAULT_GAIN = 0.715f`
  (`firmware/SaftyFW/src/current_sense.c:45`), used in
  `amps = v_adc / (gain * sqrt(2) * k_ct)` (`current_sense.c:194`).
- `k_ct_v_per_a = 1.0` (owner's 1 V/1 A CT figure) was not in dispute.

`zones_current_sweep_engine.c:196-208` already carried this exact discrepancy
as an open, unresolved flag ("using the smaller 0.715 is the conservative
choice ... Revisit once the k_ct/gain discrepancy above is resolved for real
hardware"). This audit closes that flag.

## What the schematic actually says

Independently re-derived from `hardware/mainBoard/CurrentSense.kicad_sch`
(HEAD `a382380f`), not just from the existing doc trace — grepped the raw
`.kicad_sch` s-expressions for the reference/value properties directly:

```
R43  Value "10k"
R46  Value "7.15k"
```

R46/R43 = 7.15k / 10k = **0.715**, exactly matching `CS_DEFAULT_GAIN`. This
matches `firmware/SaftyFW/docs/CURRENT_SENSE.md` §1's existing trace (HEAD
`3e463f9d`) component-for-component (U8A/U8B AD8542, D14/D15, R77/C57 peak
hold, R90 DC return), which this audit treats as independently corroborated
rather than merely re-quoted.

The topology is a **precision half-wave rectifier (inverting "superdiode")
feeding a peak-hold**, not an RMS-to-DC converter and not a plain divider:

- U8A's `-` input takes the signal through R43 (10k), feedback through R46
  (7.15k) — a straightforward inverting-amplifier gain of R46/R43 = 0.715
  applied to the CT's instantaneous voltage, while D14 is inside the feedback
  loop (so its diode drop is divided out — no 0.3 V Schottky offset in the
  transfer function).
- R77 (1M) ∥ C57 (1µF) is a **peak hold** (τ ≈ 1.0 s): it captures the
  half-wave rectifier's *peak*, not an average or RMS value.
- So for a sinusoidal CT output of peak amplitude `V̂_ct`:
  `V_adc ≈ 0.715 · V̂_ct = 0.715 · √2 · V_ct_rms ≈ 1.011 · V_ct_rms`.

This is why the firmware's `sqrt(2)` is present at all: 0.715 is a **peak-only**
gain (derived purely from a resistor ratio, independent of waveform shape);
`sqrt(2)` is the separate, standard peak-to-rms conversion for a sinusoid,
needed specifically because the circuit outputs a peak-held value rather than
a true RMS-to-DC conversion. These are two distinct physical facts, not two
stages describing the same thing — **`sqrt(2)` does not double-count what
0.715 already represents.** The firmware's formula
(`v_adc / (gain * sqrt(2) * k_ct)`) inverts exactly this chain and is correct
as written.

`ltspice/currentMon.asc` — a working simulation of this exact circuit,
matching topology and R43/R46/R90/R77/C57 values — is driven with
`SINE(0 2.8284 60)`, i.e. **2.0 Vrms**. That figure is the assumed *full-scale
input amplitude* the designer picked for the sim (2.8284 Vpk = 2.0 Vrms,
matching an SCT-013-030-class 1V/30A CT at roughly 2x nameplate = 60A), not a
transfer-function gain. This is almost certainly the source of the "2.0"
figure in the dispute: a full-scale test-signal amplitude, mistaken for (or
conflated with) the conditioning stage's gain.

## Verdict

**0.715 is correct, confirmed independently from the schematic's actual
resistor values, not just re-derived from the existing doc.** The owner's "2 V
DC per 1 V rms" figure does not match the physical circuit as built — R46/R43
would have to be 2.0 (e.g. 20k/10k) for that to be true, and it is not
(7.15k/10k). `sqrt(2)` is legitimate peak-to-rms conversion, not
double-counting.

This is stated plainly because the evidence points that way, not by default:
git history (`git log -S"CS_DEFAULT_GAIN" --oneline`) shows 0.715 was present
in `655caa41` (SaftyFW's very first commit) as a schematic-derived resistor
ratio, not a measured calibration or a guess — consistent with, not
contradicted by, the schematic re-derivation above. If a bench measurement
(known AC rms voltage injected at the CT input jack, DC peak-hold output read
at the ADC pin with a meter) later disagrees with 1.011 V_adc per V_ct_rms,
that measurement — not this analysis — should win; the schematic proves
design intent, not as-built correctness of every solder joint. That
measurement has not been made as part of this audit (no board write/flash
permitted while the 45 °C plateau capture is running) and is the natural next
step to fully close this out.

## Consequences

- `k_ct_v_per_a = 1.0` (owner's CT spec): unchanged, not in dispute.
- `gain[ch]` / `CS_DEFAULT_GAIN = 0.715`: **no change** — confirmed correct.
- `ZONE_SWEEP_NORMAL_NOISE_FLOOR_A = 0.045f` (45 mA,
  `firmware/KilnFW/App/drivers/control/zones_current_sweep_engine.c:209`):
  **no change** — it was already derived using the committed 0.715 gain, which
  this audit confirms is the correct one. The 16 mA figure that would follow
  from a 2.0 gain does not apply.
- The open "discrepancy flagged, not resolved" comment in
  `zones_current_sweep_engine.c` (lines 196-208) is now stale and should be
  updated by whoever next touches that constant to say gain is confirmed
  0.715, citing this audit — left as a follow-up rather than done here to
  avoid touching production code mid-capture and to avoid colliding with the
  separate in-flight work (noted below) making the noise floor track live
  calibration instead of being a fixed amps constant.
- Coordination note: another agent is reportedly making the noise floor track
  live calibration rather than remain a fixed amps constant. This audit does
  not touch `zones_current_sweep_engine.c`; whichever change lands should cite
  this resolution (0.715 confirmed) rather than re-opening the gain question.

## Fixture power draw, independently re-derived

Sampled `logs/coupling/cplval75_proof_20260909.jsonl`'s raw `ct_counts` (all
three zones commanded, summed-CT channel index 2): idle ≈ 55-65 counts,
loaded ≈ 147-170 counts, i.e. a delta of roughly 100 counts on top of a
zero_counts baseline in the same range documented elsewhere (§5,
`CURRENT_SENSE.md`) as 63.

Using the firmware's own formula, Vref=3.3 V, 12-bit ADC, k_ct=1.0:

```
v_adc = 100 * 3.3 / 4096 ≈ 0.0806 V
gain=0.715: I = 0.0806 / (0.715 * 1.41421) ≈ 0.0797 A  -> P = 120V * I ≈ 9.6 W
gain=2.0:   I = 0.0806 / (2.0   * 1.41421) ≈ 0.0285 A  -> P = 120V * I ≈ 3.4 W
```

This data point is ambiguous by itself — it brackets the owner's two stated
numbers (~4 W central estimate, <10 W ceiling) on either side of the two gain
candidates, and is far too noisy (delta of ~100 counts out of a ~19-count
peak-to-peak idle noise band) to settle a factor-of-2.8 dispute on its own.
It is reported here as requested, but the schematic's resistor-ratio evidence
above is the deciding evidence, not this rough power estimate.

## What was NOT done

No `.kicad_sch`/`.kicad_pcb`/`.kicad_pro` file was modified (read-only grep
only). No board write, flash, firing, or configuration change was made — the
45 °C plateau capture was left undisturbed throughout. No production code was
edited as part of this audit; the noise-floor constant and the stale
"discrepancy flagged" comment are left for a follow-up commit.
