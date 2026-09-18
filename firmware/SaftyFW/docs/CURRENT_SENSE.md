# Current Sensing

> **Status:** planning · **Last reviewed:** 2026-09-18
> **Keep this file current.** If the front end, the sampling, or the calibration
> procedure changes, update it in the same commit. If it disagrees with the
> code, **the code wins.** Checklist at the bottom.

Three identical channels on `ADC0`/`ADC1`/`ADC2` (GPIO26/27/28), one per
current transformer, on sheets `/SaftyProcessor/CurrentSense{,1,2}/`.

The whole of this document exists because of one fact that is easy to miss and
expensive to get wrong:

> **The ADC does not see a current waveform. It sees a rectified peak envelope
> with a one-second decay.**

Write the sampler for that, not for an RMS meter.

---

## 0. Scope — what these channels are for

Two jobs, both modest:

1. **Load-active detection.** Is current flowing, yes or no, against a coarse
   `i_present_a` threshold. This is what guards S3, S4 and S9 consume.
2. **A power estimate**, reported to the ESP for the GUI. **No guard reads it.**

**These channels are not a protection device.** There is no over-current guard
and no under-current guard, by design. Fusing, breaker sizing and element
protection belong to the electrical installation, which responds in
milliseconds and is rated for the fault currents involved. A firmware trip
reading through a 1 s peak-hold, an operator-supplied CT ratio and an
unregulated ADC reference could not match that, and its existence would imply a
guarantee the hardware cannot back.

### 0.1 A board with no CTs at all

CTs are **optional hardware**, and since 2026-08-28 that is a state the
firmware can represent rather than one it merely tolerates: the commissioning
field `ct_installed` (`0x0109`, `SaftyFW/src/config_params.c`) asks the
question outright, and answering *no* is what makes a CT-less board
commissionable.

It has to be **asked**, not defaulted or inferred, because nothing on either
board can see whether a CT is clamped around a wire, and a zero reading is
exactly what an absent CT and an idle fitted CT both produce. Both wrong
answers are bad in opposite directions:

- **Answering *yes* on a board with no CTs** leaves `ct_channel_map` required,
  so the board never commissions and never heats — *and* the uncalibrated
  channel's offset floor (§1's rectifier output with nothing driving it) reads
  as "current present" on every tick through
  `current_presence_policy.c`'s counts-domain fallback, which trips **S3**
  (`LOAD_STUCK_ON`) on a perfectly healthy board.
- **Answering *no* on a board that does have CTs** silently disarms S3, S4, S9
  and S14.

Answering *no* switches those four guards off **and reports them off**
(`safety_guard_state_t::ct_guards_disabled`) rather than letting them read as
quietly passing. One guard is genuinely degraded rather than disabled: **S6b**
keeps its unconditional `link_dead_hard_s` backstop but loses its faster,
current-keyed tier, because "is heat on" has no local answer without a
sensor. `docs/GUARD_TEST_MATRIX.md` §9 is the per-guard table and the
reasoning.

**2026-09-06, `ct_topology` (Pico side done, `CT_COMMISSIONING_PLAN.md` step
3):** this section previously assumed one CT per zone. That is now
`ct_topology = per_zone` (param `0x031F`, default, unchanged behaviour). A
`summed` board has ONE CT (channel 3/GPIO28) reading every zone at once:
channels 1-2 report not-fitted (never a plausible 0.00 A), S14 compares
channel 3 against the sum of `i_normal_a[]` for zones commanded on right
now, and a new guard, **S15** (WARN-only, per zone, commanded-sum-minus-
measured `> 0.7×` that zone's normal for 30 s), flags that the shared CT's
deficit is consistent with **one of the commanded zones** having an open
heater. The deficit is a single shared-CT scalar tested against each
commanded zone's own (smaller) threshold, so more than one zone's WARN bit
can set from the same single fault — a shared CT alone cannot attribute the
deficit to one zone the way three separate CTs could, and S15's per-zone
bits must be read as "the fault is in one of these," never as every flagged
zone being independently faulty. ESP-side commissioning UI/display for this
is still pending.

Channels 1-2 reporting not-fitted means `amps_valid[0]`/`[1]` are always
false in `summed` mode (`safety_core.c`) — and since S3/S4/S9/S11 each gate
their current-based checks on `in->amps_valid[ch]` per channel, this disarms
those four guards' checks on channels 0/1 specifically, the same way
answering "no CTs" disarms them on all three (see the "Answering *no*..."
paragraph above) — just scoped to two of three channels here instead of all
three. Channel 2 stays valid, so S3/S4/S9/S11 keep working on it normally.

---

This scope limit makes the calibration burden much lighter than it first
appears, and the two jobs have very different accuracy needs:

| Job | Accuracy required | Consequence of being wrong |
|---|---|---|
| Load-active | **Within a factor of ~2** | `i_present_a` only has to sit between measurement noise and a conducting element — one to two orders of magnitude apart |
| Power estimate | As good as you care to make it | A wrong number on a display |

So a mis-rated CT degrades the GUI's power figure and leaves every guard working
exactly as before. That is the right way round.

---

### 0.2 Two topologies: per-zone or one summed CT (2026-09-06)

Everything above and through §5.2 describes the **per-zone** design: one CT
per zone, `ct_channel_map` resolving which channel belongs to which relay.
Since `CT_COMMISSIONING_PLAN.md` step 3, a board may instead answer
`ct_topology = summed` (`0x031F`, KilnFW commissioning page): one shared CT
(channel 3, Current3/GPIO28-ADC2, §5.2) reads every zone at once. In that
mode the mapping check (§5's Commissioning check, `GUARD_TEST_MATRIX.md`
§3.3) is skipped entirely — there is no per-relay mapping to resolve — and
the zone current-sweep instead derives each zone's normal as `sum(with zone
on) - sum(idle)` from channel 3 alone. Channels 1/2 are simply not fitted in
this topology (`amps_valid[0]`/`[1]` always false); S14 compares channel 3
against the sum of commanded zones' `i_normal_a[]`, and a new WARN-only
guard S15 flags a likely open heater. Per-zone remains the default and the
safe silent choice for an uncommissioned board.

**2026-09-06, sweep refusal and unmeasured zones (`31a59aa`).** The summed
sweep now refuses outright (`ZONE_SWEEP_REFUSE_CT_TOPOLOGY_UNKNOWN`) if the
ESP's safety-param cache has never been fetched from the Pico, rather than
defaulting an unfetched cache to `per_zone` and running the wrong
derivation. And a zone whose `sum(with zone on) - sum(idle)` comes out
negative is reported as **not measured** (`summed_unmeasured_mask`, in the
sweep status) instead of clamping to a persisted zero — a persisted zero
would otherwise leave that zone's S14/S15 checks silently inert forever,
since a zero `i_normal_a` reads as "any current is excess."

## 1. The circuit

Traced from `hardware/mainBoard/output/kiln.pdf` p.4. Designators are channel 1's;
channels 2 and 3 are identical (`R78/R83/…`, `R84/R89/…`).

```
  CT ──> J13 ──┬── R90 1M ──┐         ┌──── R46 7.15k ───┐
  (3.5mm)      │            │         │                  │
            R72 100R        │      ┌──┴──┐               │
             (DNP)          │      │ D15 │ (clamp)       │
               │            │      └──┬──┘               │
              GND          GND        │                  │
                            │      ┌──┴──┐   ┌─────┐     │
       ── R43 10k ──┬───────┴──────┤ −   │   │ D14 │     │
                    │              │ U8A ├───┤ ──▷│──┬───┴──┬──┐
                  C44 4.7pF        │ +   │   └─────┘  │      │  │
                  D12/D13 ⧎        └──┬──┘          R77 1M  C57 │
                    │                 │              1µF    │   │
                   GND               GND             GND    GND │
                                                                │
                                                    ┌───────────┴─┐
                                                    │  U8B (×1)   ├──> ADCn
                                                    └─────────────┘
```

- **U8 is an AD8542** (dual, rail-to-rail in and out, single 3.3 V supply).
  U8A is the rectifier, U8B the output buffer. `U8C` on the sheet is just the
  power-pin symbol with C45 0.1 µF.
- **U8A's `+` input is at GND_Safty**, and the signal enters the `−` input
  through R43. This is an **inverting precision half-wave rectifier**
  ("superdiode"): D14 conducts on the input's negative half-cycles, D15 clamps
  the op-amp's output on the positive ones so it never saturates.
- **Gain is set by R46/R43 = 7.15k / 10k = 0.715**, and because the diode sits
  *inside* the feedback loop, its forward drop is divided out. There is no
  0.3 V Schottky offset in the transfer function.
- **R77 ∥ C57 = 1 MΩ ∥ 1 µF is a peak hold**, τ = **1.0 s**. It charges through
  D14 from the op-amp's output (fast, current-limited only by the AD8542's
  ~50 mA short-circuit current) and discharges only through R77 (slow).
- **D12/D13 are two BZX84C3V3 back-to-back**, giving a bidirectional clamp at
  roughly ±(3.3 + 0.7) V. This is the input's survival protection, not part of
  the signal path.
- **R90 (1M) is the DC return** for the CT secondary. Without it the input node
  floats and the rectifier output wanders.

### `ltspice/currentMon.asc` is a working model of this circuit

Unusually for a stray simulation file, it matches: same topology, same
10k / 7.15k / 1M / 1µF, driven by `SINE(0 2.8284 60)` — 2.0 Vrms at 60 Hz.
**Use it.** It is the cheapest way to sanity-check a threshold before
committing it, and it already encodes the designer's intent about full-scale
input level.

---

## 2. Transfer function

For a sinusoidal CT output of amplitude `V̂_ct`:

```
V_adc  ≈  0.715 · V̂_ct          (steady state, while current is flowing)
       =  0.715 · √2 · V_ct_rms
       ≈  1.011 · V_ct_rms
```

which is a pleasant accident: **for a sine wave the ADC reading is numerically
close to the CT's RMS output voltage.** Do not lean on that. It is only true
for a clean sinusoid, and a phase-angle-fired SSR does not produce one. It is a
useful bench sanity check, not a calibration.

Converting to amps requires the CT's volts-per-amp figure:

```
I_rms  =  V_adc / (0.715 · √2 · k_ct)        [k_ct in V_rms per A_rms]
```

`k_ct` is **not derivable from anything in this repository** — but it *is*
derivable, on a live installation, from two numbers the operator already gives
the commissioning flow. See §5.1.

### The CT is not on the BOM

`R72` (the 100 Ω burden) is **marked DNP on all three channels**. That is a
deliberate choice and it means the board expects a **self-burdened,
voltage-output CT** — an SCT-013-**030** (1 V at 30 A) or similar, *not* an
SCT-013-**000** (current output, 50 mA at 100 A, needs a burden).

Consequences:

- **Fitting a current-output CT is a wiring error the firmware cannot detect.**
  With R72 absent the secondary sees only R90 (1 M) and the clamp diodes: the
  CT drives itself into its own compliance limit, the clamp conducts, and the
  reading saturates at a value that has nothing to do with the primary current.
  Put this in the build documentation and check it at commissioning.
- If a current-output CT must be used, **fit R72** and recompute — do not try
  to correct it in software.
- `k_ct`, the full-scale current, and the per-channel zero are **measured
  configuration**, stored in flash, never compiled-in constants. See §5.

### Range and clipping

The ADC and the buffer both top out at the 3.3 V rail, and the input clamp
starts conducting at about ±4 V, so:

```
V_adc full scale = 3.3 V  ⇒  V̂_ct ≈ 4.6 V  ⇒  V_ct_rms ≈ 3.26 V
with a 1 V/30 A CT:  I_fs ≈ 98 A rms
with a 1 V/100 A CT: I_fs ≈ 326 A rms
```

**Clipping must be detected and reported, never silently returned as a
number.** A channel reading within ~50 mV of the rail is not "the maximum
current" — it is "at least this much current, and the measurement is no longer
valid". `SaftyFW` reports it as a distinct `CURRENT_FLAG_CLIPPED` state, which
means **"the power estimate for this channel is not valid"** and nothing more.

A clipped channel still reads as **load active**, which is the only thing any
guard cares about. Clipping is not a trip condition — see §0 below.

---

## 3. Dynamics — the part that shapes the guards

| Event | Response |
|---|---|
| Current starts | First negative half-cycle charges C57. Full reading in **< 10 ms**. |
| Current stops | Exponential decay, **τ = 1 s**. 37 % at 1 s, 5 % at 3 s, 1 % at 4.6 s. |

**Rise is effectively instant; fall takes seconds.** Every guard that reasons
about current must be built around that asymmetry:

- A guard that says *"current is present"* can respond quickly and be believed.
- A guard that says *"current has stopped"* must wait **at least 4 s** after
  the last commanded-on edge before the reading means anything.

### Interaction with the main board's 60-second heater window

This is the dominant timing constraint in the whole design, and it comes from
`KilnFW`:

| `KilnFW` constant | Value | Source |
|---|---|---|
| `HEATER_WINDOW_MS` | **60000** (60 s) | `App/drivers/control/profile_executor.c:27` |
| `HEATER_MIN_ON_MS` | 2000 | `profile_executor.c:28` |
| `HEATER_MIN_OFF_MS` | 2000 | `profile_executor.c:29` |
| `PROFILE_EXECUTOR_TICK_MS` | 1000 | `profile_executor.h:174` |

The main board renders duty as a **60-second time-proportioned window**. At
20 % duty a relay is on for 12 s and off for 48 s — and the peak-hold has
fully decayed after about 5 s of that. So for roughly 43 seconds of every
minute, **a perfectly healthy 20 %-duty zone reads zero current.**

Two rules fall out of this, and they are the reason guards S3 and S4 are shaped
the way they are:

1. **"Relay commanded on *right now*" is nearly useless as a correlation
   signal.** A snapshot of the relay mask tells you almost nothing about
   whether current *should* be flowing at this instant.
2. **Correlation must be done over a window of at least two heater windows
   (≥ 120 s), using "was any relay commanded on at any point during the
   window", not the instantaneous mask.**

That is exactly why the protocol extension in `LINK_PROTOCOL.md` carries a
`relay_recent_mask` field alongside `relay_now_mask`. The ESP already knows its
own window phase; making it compute "did I command this relay on during the
last N seconds" is one line there and removes the need for the Pico to model
the ESP's scheduler. **Do not try to reconstruct duty cycle on the Pico by
integrating the current reading** — with a 1 s peak-hold in front of it, the
information is not there to recover.

---

## 3b. The power estimate

Reported to the ESP for the GUI. **No guard reads it**, so it can be as
approximate as the hardware forces without any safety consequence.

Three quantities per channel, published every telemetry frame:

| Field | Meaning |
|---|---|
| `i_conducting_a` | RMS amps **while the load is actually conducting** — the peak-hold reading converted per §2, sampled only when above `i_present_a` |
| `conduction_fraction` | fraction of the last `power_window_s` (default **120 s**) in which the channel read above `i_present_a` |
| `p_avg_w` | `mains_voltage_v × i_conducting_a × conduction_fraction`, **only if `mains_voltage_v` is configured**; otherwise absent |

### Why `i_conducting_a` and duty are reported separately

A single averaged amps figure would be meaningless here. The front end holds the
peak while conducting and decays over ~3 s when it stops, so a naive average
over a 60 s heater window at 20 % duty produces a number that is neither the
conducting current nor the average current — it is an artefact of the RC.

Splitting it gives the ESP two honest numbers it can combine correctly, and
gives the operator the more diagnostically useful one: **`i_conducting_a` is the
element's actual draw**, and a drift in it over months is the earliest sign of
elements ageing out.

### The ESP should compute the authoritative figure, not the Pico

`conduction_fraction` is the Pico's *estimate* of duty and it has a known bias:
the 1 s decay tail keeps the channel above threshold for ~2–3 s after conduction
stops, so at 20 % duty (12 s on in 60 s) it measures ~15 s and reports ~25 %.
The overestimate shrinks as duty rises and vanishes at 100 %.

The ESP does not have this problem — **it knows the true commanded duty
exactly**, because it generated it. So:

- The **ESP's** power figure — `mains_voltage_v × i_conducting_a × true_duty` —
  is the one to display.
- The **Pico's** `conduction_fraction` is a cross-check. A large, sustained
  disagreement between commanded duty and measured conduction is itself
  interesting: it is S3/S4 territory expressed as a continuous quantity rather
  than a threshold.

`p_avg_w` from the Pico exists for the case where the ESP has no context to
apply (idle, or a manually switched relay), and is explicitly labelled an
estimate.

### `mains_voltage_v` is a nominal, and that is a real limit

There is no voltage measurement anywhere in this design. `mains_voltage_v` is a
commissioning constant (230, 240, 208, whatever the install is). Kiln elements
are resistive, so **power goes as V²** — a 5 % supply sag under full load, which
is entirely normal on a kiln circuit, is a 10 % error in the power figure.

Report it as an estimate, present it as an estimate, and do not build anything
on top of it that needs better than ±10 %. Energy totalisation over a firing is
fine at that accuracy; billing is not.

---

## 4. Sampling

Because the front end has already done the demodulation, the sampler is
simple. Deliberately so.

- **Acquisition rate: 100 Hz per channel, publish rate unchanged at 20 Hz**
  (owner-directed increase, 2026-09-18, reconciled below). Round-robin
  across ADC0/1/2 in the RP2040's single SAR either way.
- **Oversample and average**: take 16 back-to-back conversions per channel per
  acquisition tick and mean them. Cheap, and it buys ~2 bits against the
  RP2040 ADC's well-documented noise, per tick.
- **No DMA, no free-running/round-robin FIFO capture, no FFT, no RMS
  accumulator.** All of that is what you would need if the ADC saw the raw
  waveform. It does not — see the reconciliation note immediately below for
  why this still holds at the higher rate.
- Feed a **slow first-order filter** (τ ≈ 0.5 s) on top, for reporting only.
  **Guards evaluate the unfiltered sample**, so that filter can never delay a
  trip — the same discipline `KilnFW` applies to raw-versus-calibrated
  readings in `thermal_guard.h`.

### 2026-09-18 reconciliation: 100 Hz acquisition, and why the four bans above still stand

The owner directed a rate increase to 100 Hz. That is built
(`current_sense_acquire_tick()`/`current_sense.c`), but it is built as
**5 bounded acquisition ticks per existing 50 ms publish pass**, not as a
change to the publish cadence, the filter time constant, or the power
window — none of `CS_FILTER_TAU_S`, `SAFTYFW_PERIOD_CURRENT_TASK_MS`,
`CS_POWER_WINDOW_S`/`CS_POWER_WINDOW_SAMPLES`, or
`CURRENT_TASK_CT_AUTO_ZERO_TARGET_SAMPLES` needed to change, because none of
them are paired against the *acquisition* rate — they are paired against the
*publish* rate, which this change does not touch. See
`docs/audits/ct_sampling_mains_aliasing_review_2026-09-18.md` for the full
paired-constant audit.

**What this buys, honestly.** Going from 16x to effectively 5×16=80x
oversample per publish pass buys roughly `sqrt(80/16) = sqrt(5) ≈ 2.24×`
further reduction in the ADC's own broadband/quantization noise, **if that
noise is uncorrelated sample to sample** — the same assumption the existing
16x oversample already leans on. Measured idle noise on the one fitted
channel (§4 above, 2026-09-06 run): std ≈ 4.678 counts / ≈3.8 mA. A 2.24×
reduction on that figure is std ≈ 2.09 counts / ≈1.7 mA, if the noise source
is genuinely broadband. **This does NOT apply to correlated pickup** — mains
hum coupled into the front end, ripple on the unregulated `3.3v_Safty`
reference rail, anything periodic — averaging more samples of a correlated
signal does not shrink it, because the samples are not independent draws of
random noise. Nobody has re-measured the noise floor at the new rate against
real hardware to confirm which regime dominates; the number above is a
projection from the 2026-09-06 measurement's own std, not a new
measurement, and should be re-taken (same `safety_capture_ct_counts()`
procedure) before it is trusted for anything more than "probably better,
possibly by a lot less than 2.24×."

**Why free-running/round-robin FIFO capture, DMA, FFT and an RMS
accumulator are all still banned, at 100 Hz exactly as at 20 Hz:** every one
of those techniques is a way of getting more information out of a *raw AC
waveform*. The front end in front of this ADC is not one — it is a
rectified peak-envelope with a 1 s decay (§1, R77‖C57). There is no
waveform for an RMS accumulator to integrate, no periodic signal for an FFT
to resolve, and free-running/round-robin FIFO capture only buys anything
over manual `adc_select_input()`+bounded-`adc_read()` polling when the goal
is to keep up with a fast-changing analog input across many channels — this
module's own discard+16-oversample-per-tick burst, called 5x per publish
pass, already achieves the round-robin-across-ADC0/1/2 requirement without
free-running mode's FIFO-reordering cost (see `current_sense.c`'s header
comment). A mains-cycle-locked sampler was also explicitly NOT built, for
the same reason: there is no mains cycle visible at this ADC to lock to.

### RP2040 ADC errata

The RP2040's ADC has a known INL discontinuity (missing codes around the
512/1536/2560/3584 boundaries) documented in the RP2040 datasheet §4.9.

**This does not matter here and no correction should be implemented.** The
worst-case error is a few LSB out of 4096 on a measurement whose thresholds are
set with tens-of-percent margins, taken against a reference that is itself the
unregulated-for-precision `3.3v_Safty` rail (see `HARDWARE.md` §6). Adding a
correction table would create the false impression that this channel is precise.
It is a fault detector. Say so in the code comment and move on.

---

## 5. Calibration

Per channel, stored in flash, all **measured**:

| Constant | Affects | How it is obtained | Typical |
|---|---|---|---|
| `zero_counts` | **both** | Mean ADC reading with the CT fitted and **no primary current**, over ≥ 10 s | small positive; op-amp Vos and D14 leakage, *not* 0 |
| `i_present_a` | **guards** | Set between the noise floor and a conducting element. Coarse by design | 2.0 A |
| `k_ct_v_per_a` | power estimate, **and the presence threshold's domain conversion** | **Calibrated by the ESP's zone current-sweep (§5.1)**, or manually via the commissioning page's A_fs/zero_mv fields (below) — the operator types the probe's own rated amps and zero-current output, and the ESP derives `k_ct_v_per_a = 1/A_fs`/`zero_counts` and pushes them | e.g. 0.0333 V/A for a 1 V/30 A CT |
| `gain` | power estimate, **and the presence threshold's domain conversion** (same commissioned branch as `k_ct_v_per_a` above) | 0.715 nominal, refined if the resistors are not 1 % | 0.715 |
| `mains_voltage_v` | power estimate only | The installation's nominal supply voltage | 240 |

**Corrected 2026-09-18 — this table used to say `gain` was cosmetic, and that
was false** (owed as of commit `3fa86a69`). `zero_counts` and `i_present_a`
affect guard behaviour unconditionally. `k_ct_v_per_a` and `gain` affect it
**conditionally**: both are factors of the commissioned-branch domain
conversion in `current_presence_is_flowing()`
(`firmware/SaftyFW/src/current_presence_policy.c`), which computes
`v_present = i_present_a · gain · √2 · k_ct_v_per_a` and then tests
`counts_avg − zero_counts > v_present · 4096 / vref`. Raising either constant
raises the counts threshold, so a channel reads as *not* flowing sooner;
lowering either makes it read as flowing sooner. That branch runs whenever
`k_ct_v_per_a > 0 && i_present_a > 0 && gain > 0`, and `gain` is always `> 0`
at the only production call site — `current_sense.c` substitutes
`CS_DEFAULT_GAIN` for a cal field `<= 0.0f` before calling (its
`resolved_gain`). When `k_ct_v_per_a <= 0` (uncommissioned) the conversion is
bypassed entirely for the fixed
`CURRENT_PRESENCE_POLICY_FALLBACK_MARGIN_COUNTS` floor, and neither constant
matters.

`present[n]` is what `current_any_present()` (`snapshots.h`) ORs across
channels into `safety_guard_input_t::any_current_present`, so the guards that
inherit a wrong `gain` are: **S3** (`SAFETY_TRIP_LOAD_STUCK_ON`, the welded-SSR
guard — presence with nothing commanded), **S9** (presence after K4 was
de-energized; `trip_ineffective` latch, or `s9_uncommissioned_warn`), **S6b**
(`SAFETY_TRIP_LINK_DEAD`'s soft `link_timeout_s`-with-current-present branch),
**S4** (WARN only — `relay_commanded_continuously && !any_current_present`),
and **S11** (frozen-sensor, armed from the same fact via safety_core.c's
`.heat_commanded = any_current_present`).

What remains true, and is the scope limit in §0: no guard tests a current
*magnitude* — there is no over/under-current guard, only presence. But
presence is a safety fact, so "display concern only" is the wrong summary for
either constant. Both are coarse-tolerance in practice (`i_present_a` is
documented as tolerating one to two orders of magnitude of slack, and `gain`
is a 1 % resistor ratio), so refining `gain` within its tolerance does not
move the threshold meaningfully — a grossly wrong one does.

**2026-08-24 note, made true by this date's commit, not before it.** From
Phase 6 (this file's original commit) until 2026-08-24, this claim was
*false*: `current_sense_set_cal()` was never called anywhere in `src/`, so
`k_ct_v_per_a` stayed `0.0f` forever, and `cs_counts_to_amps()` (the ONLY
producer of `current_snapshot_t.amps[]`, which `current_any_present()` used
to compare against `i_present_a`) hard-returned `0.0f` whenever
`k_ct_v_per_a <= 0.0f`. That silently disabled presence detection outright —
S3, S9, S11, and S6b's current-gated trip could never fire, on any board,
commissioned or not. The 2026-08-24 fix wires `current_sense_set_cal()` from
`config_store` (boot, and live on `SAFETY_CMD_COMMIT_CONFIG`,
`current_task_reload_cal()`) AND decouples presence detection from
`k_ct_v_per_a` entirely: `current_presence_policy.h`'s
`current_presence_is_flowing()` compares the raw ADC delta against a
counts-domain threshold — derived from `i_present_a`/`gain`/`k_ct_v_per_a`
when `k_ct_v_per_a` IS commissioned (bit-for-bit the old decision), or a
fixed, deliberately sensitive fallback margin when it is not. `amps[n]`
itself still reads `0.0f` (honestly) when `k_ct_v_per_a` is uncommissioned —
that half of this section's claim was always, and still is, true — but
`current_snapshot_t.present[n]` (what guards actually read via
`current_any_present()`) no longer depends on it. See that header's own
comment for the full safe-direction reasoning, including the one guard (S4)
this fallback's sensitivity trades against.

`I = max(0, (counts − zero_counts)) · vref / 4096 / (0.715 · √2 · k_ct)`

Three points worth stating explicitly:

- **`zero_counts` is not zero and must not be assumed to be.** Single-supply
  output cannot go below ground, so the op-amp's offset appears as a small
  positive floor. Subtracting a compiled-in 0 makes every channel read a
  permanent phantom current; subtracting a measured zero and clamping the
  result at 0 is correct.
- **A `zero_counts` that drifts is itself a fault.** Re-measure it whenever the
  system has been idle with no relay commanded on for > 5 minutes, and if it
  has moved by more than a few counts, report it. That is a free diagnostic for
  a failing op-amp or a damaged clamp diode.
- **Calibrate with the kiln, not with a bench supply.** The CT clamps around a
  specific conductor, and which conductors it encircles is a wiring decision.
  A CT on the wrong leg of a 3-phase or split-phase supply reads a plausible
  number that is not the number you think it is.

### Commissioning check

Before any threshold is trusted, run this and record the results:

1. All relays off, kiln cold: record `zero_counts` per channel. Expect a stable
   small value.
2. Command **one** relay on at 100 % duty. Confirm **exactly one** channel
   responds, and that it is the channel you expect. *This is the only way to
   discover a CT plugged into the wrong jack*, and a swapped CT silently
   destroys the correlation guards S3/S4.
3. With that relay on, compare the computed amps against a clamp meter on the
   same conductor. Adjust `k_ct_v_per_a` to match. **On a board driven by
   KilnFW this step is now automatic** — see §5.1; the clamp meter is the
   override, not the procedure. Skipping it leaves every guard fully functional
   and the GUI's wattage wrong.
4. Command the relay off and record the decay. Confirm it reaches < 5 % within
   ~4 s. A much slower decay means C57 or R77 is wrong; a much faster one means
   something is loading the hold node.
5. Repeat for channels 2 and 3.

### 5.1 `k_ct_v_per_a` is calibrated by the ESP, not typed (2026-08-28, KilnFW M12b)

Step 3 above asks for a number nobody has at commissioning time, on a page that
asked for it in V/A. In practice it was never entered: `k_ct_v_per_a` stayed at
`config_store.c`'s `memset(0)` on every board. That is **not** the harmless
outcome §5's table used to imply — with `k_ct_v_per_a <= 0`,
`current_presence_policy.c` falls back to a fixed counts-domain margin instead
of converting the operator's own `i_present_a`, and every reported amps/watts
figure reads `0.0`.

KilnFW's zone current-sweep (`App/drivers/http/zones_http.c`) now calibrates it. That
sweep is already this section's step 2, run automatically: one zone's relay(s)
on, every other relay forced off, all three channels recorded separately. It
therefore has both halves of a calibration nobody has to type:

- **Expected**, from the two commissioning answers already collected
  (`COMMISSIONING_UX.md` Q3/Q4): `I_expected = max_expected_power_w /
  mains_voltage_v` — the whole kiln's current at full output.
- **Measured**, as the sum over every zone of that zone's own dominant CT
  channel. One zone at a time summed is the same total a simultaneous
  full-output firing would draw, and it is the only version of that total a
  fixture can measure without ever energizing two zones at once.

Since §2's transfer function makes the reported amps inversely proportional to
`k_ct`, the correction is a single scale factor applied to each resolved
channel's own committed value:

```
k_new[c]  =  k_old[c] · (I_measured_total / I_expected_total)
```

**The dependence on a prior `k_old` is inherent, not a shortcut.** The ESP↔Pico
link carries amps, never raw counts, so a `k_old` still at `0.0` makes every
channel read `0.0 A` and there is nothing to scale — the sweep refuses and says
so rather than inventing a starting value. (The CT-*map* derivation already had
the same dependence: it needs ≥ 2 A on a channel to resolve anything.)

Every one of these refuses the whole calibration, with a reason shown on both
the zones and commissioning pages:

| Refusal | Why it is not a scale error |
|---|---|
| A zone that did not resolve to a CT, or two zones sharing one | The whole-kiln total is short by that zone's share, and `k` would be scaled **down** by exactly that much, silently |
| `mains_voltage_v` or `max_expected_power_w` unset | There is no expected current to compare against — and an *unset* answer is not a zero |
| `k_old <= 0` | Uncommissioned: every reading was `0.0 A` |
| Measured total < 2 A | Below a conducting element, the ratio is noise |
| Correction outside 0.2×–5× | §0 scopes this chain to "within a factor of ~2"; a bigger disagreement is a nameplate in the wrong units, a CT on the wrong conductor, or a current-output CT fitted where a voltage-output one belongs — scaling `k` would make the amps *look* right while moving the presence threshold to match the error |
| Result outside 0.0005–0.5 V/A | Not a CT |
| The `ct_channel_map` push failed | Both writes stage into the same buffer on the Pico; committing on top of an abandoned staging would carry its leftovers into flash |

The write itself goes over the ordinary `SET_PARAM`/`COMMIT_CONFIG` path — the
Pico cannot and must not tell a calibrated write from a typed one — and carries
the same discipline as the `ct_channel_map` push: an ACKed, un-rejected commit
is **not** proof, so the record is re-fetched live and every channel must read
back bit-exactly; and every failure arm re-stages the affected channels back to
whatever the Pico has actually committed (or to `0.0`, the uncommissioned value,
for a channel that never had one — which is the *safe* side of
`current_presence_policy.c`'s branch).

**Step 2 is the one that gates the guards.** Until it passes on all three
channels, **S3 and S4 must be left disabled** (`TODO.md` phase 5). A correlation
guard fed by a mis-mapped CT is worse than no guard: it will trip on healthy
firings and stay quiet on the failure it exists to catch. Steps 1 and 4 are
also guard-relevant (zero, and decay behaviour). Step 3 is cosmetic.


### 5.1b Manual entry: probe rating and zero offset, in the operator's own units (2026-09-06)

`CT_COMMISSIONING_PLAN.md` step 1. §5.1's sweep derives `k_ct_v_per_a` from
the nameplate power/voltage and a measured total; the commissioning page
also accepts a direct, per-channel `A_fs` (the number printed on the probe —
amps at 1 V full-scale output) and `zero_mv` (the probe's own output at zero
current, mV), converted on the ESP: `k_ct_v_per_a = 1/A_fs`,
`zero_counts = zero_mv/1000 · gain · 4096/3.3`. Sanity ranges only —
`A_fs` in [0.1, 2000] A, `zero_mv` in [-200, 200] mV — nothing assumes a 1 A
probe. Each channel's last write is tagged manual/sweep/auto-zero
(ESP-local, `safety_cfg_store.c`); **manual always wins over the sweep** —
a manually-entered channel is skipped by §5.1's derivation outright, never
silently overwritten on the next sweep run.

### 5.2 Channel 3 (Current3/GPIO28-ADC2): the summed-heater CT (2026-09-05)

Fitted here only, reading **summed** current of all heaters (not per-zone):
1 A/V, ~+59 mV pin offset. Same rectifier/peak-hold front end as channels 1/2
(HARDWARE.md §9) — no new model needed. +59 mV is this channel's
`zero_counts[2]` (measure per the Commissioning check step 1, not a manual
mV-to-counts conversion); for a 1 A/V probe, `k_ct_v_per_a[2] = 1.000`, not
0.989 as an earlier revision of this section claimed. `k_ct_v_per_a` is
simply `1/A_fs` (`safety_ct_cal_convert()` in
`firmware/KilnFW/App/drivers/safety/safety_cfg_store.c`) — for a probe rated
1 A/V, `A_fs = 1`, so `k_ct_v_per_a = 1`. The front-end gain (`gain[n]`,
`0.715` default, `config_store.c`) and the `√2` peak-to-RMS factor are
**applied separately** by `cs_counts_to_amps()`
(`firmware/SaftyFW/src/current_sense.c`: `amps = v_adc / (gain * CS_SQRT2 *
k_ct)`) — they must not be folded into `k_ct` itself. The earlier
`0.715·√2 ≈ 1.011, so 1/1.011 ≈ 0.989` derivation double-counted both
factors: at any `gain` other than 0.715 it would silently produce a wrong
`k_ct`, because the conversion multiplies `gain` back in on top of it.
Commission via the existing `config_params.c` ids: `0x0304`
(`zero_counts[2]`, U16) and `0x030Au` (`k_ct_v_per_a[2]`, F32). Both refused
unconditionally while ARMED, no grace exception
(`config_store_decide_write()`). Once committed, `current_presence_is_flowing()`
(`current_presence_policy.h`) reads these same two fields, so S9/S11
whole-board presence works immediately — no firmware change needed.

**Bench state (2026-09-18):** the live board's own
`GET /api/safety/commissioning` now reads `k_ct_v_per_a[2] = 1`,
`zero_counts[2] = 63`, `gain[2] = 0.715` — the gain half is committed, not
merely outstanding. This does not arm S14/S15: they remain DORMANT because
`i_normal_a` (the per-zone expected-current baseline) is still unmeasured,
a separate, still-true limitation. See §5.3's now-superseded "Bench state
(2026-09-08)" note below.

### 5.3 Legacy `ct_cal[]` gain/offset correction — PC write surface removed 2026-09-08

`config_store.h`'s `ct_cal[3]` (per-channel `calibrated`/`gain`/`offset`,
`SAFETY_CMD_SET_CT_CAL` 0x19 / `SAFETY_CMD_GET_CT_CAL` 0x22) is a **separate,
later** correction stage applied by `current_sense.c` to `amps[n]` *after*
the §5 physics conversion above (`ct_amps_cal_apply()`, `ct_amps_cal.h`). It
is genuinely read — it feeds the S14/S15 WARN-only over/under-current
display thresholds — but it has **never** fed `zero_counts`/`k_ct_v_per_a`
or `current_presence_is_flowing()`, so it cannot affect S3/S4/S9 (the
guards that actually decide whether current is present) no matter what is
written to it.

An agent trying to clear a latched S3 (`LOAD_STUCK_ON`) trip called the PC
tool this used to expose (`safety_set_ct_cal`) expecting it to commission
the CT. It did not: the trip stayed latched until `zero_counts[2]` (§5.2's
field, a completely different one) was corrected instead
(`CT_COMMISSIONING_PLAN.md` step 6a). The name overlap with the *actually
live* commissioning endpoint — the ESP's `POST
/api/safety/commissioning/ct_cal`, which despite sharing the word "ct_cal"
writes `k_ct_v_per_a`/`zero_counts` from operator-entered `A_fs`/`zero_mv`
— made this easy to reach for by mistake.

**Removed 2026-09-08:** `devices.safety_set_ct_cal()`,
`SafetyClient.set_ct_cal()` and the `safety_set_ct_cal` MCP tool
(`tools/PcTools/src/kilnctrl/{devices_safety,safety,mcp_server_safety}.py`).
`safety_get_ct_cal` (read-only) is kept for visibility into whatever the
record already holds. `tools/check_ct_cal_write_surface.ps1` /
`tools/PcTools/scripts/ct_cal_write_surface_check.py` fail the standing
check suite if `SAFETY_CMD_SET_CT_CAL` or a `set_ct_cal(...)` call ever
comes back into `kilnctrl/safety.py` or `kilnctrl/mcp_server_safety.py`.

**Firmware left unchanged, deliberately.** `SAFETY_CMD_SET_CT_CAL`'s Pico
handler (`link_task_handle_set_ct_cal()`), `config_store.h`'s `ct_cal[3]`
record bytes (persisted, A/B-sector flash, both processors already
flashed), and `ct_amps_cal.h`/`ct_amps_cal_apply()` are all still compiled
in. Removing them is a persisted-record-layout change (a migration, not a
delete — see `CT_COMMISSIONING_PLAN.md`'s "record layout" note) and a
protocol change on flashed safety-processor firmware; neither was done here.
The PC-side removal above is sufficient on its own: with no caller left,
`SAFETY_CMD_SET_CT_CAL` is unreachable from any tool or UI, and the
commissioning page's `ct_cal[0..2].{gain,offset,calibrated}` rows
(`safety_commissioning_page.html` ids 784-792) were already read-only.

**Bench state (2026-09-08):** channel 2's `zero_counts[2] = 63`, confirmed
and holding the S3 trip clear. Gain calibration (`A_fs`/`zero_mv` →
`k_ct_v_per_a[2]`/committing a real `gain[2]`) is still outstanding — a
bench step needing a known load with the owner present — so `amps[2]`
reads `0.00 A` and S14/S15 stay DORMANT on that channel until it is done.

**Superseded 2026-09-18:** the gain half is now committed —
`GET /api/safety/commissioning` reads `k_ct_v_per_a[2] = 1`,
`zero_counts[2] = 63`, `gain[2] = 0.715` (see §5.2's 2026-09-18 bench-state
note) — so `amps[2]` no longer reads a hard `0.00 A` from an uncalibrated
gain. S14/S15 still stay DORMANT, but for the separate, still-true reason
that `i_normal_a` is unmeasured, not because the gain was never entered.

---

## 4. Measured noise floor — MEASURED 2026-09-06 (history below predates the measurement)

`CT_COMMISSIONING_PLAN.md` step 0 asked for a 60 s, 20 Hz raw-counts capture
on channel 3 (GPIO28/ADC2, the only fitted probe) with all relays off and no
heating. That capture was **not obtained**: no path from the running board
to raw ADC counts exists today, on either the wire protocol or the debug
(SWD/OpenOCD) side, so there is nothing to sample.

**Pre-checks that did pass** (via `kilnctrl` MCP, no `SET_PARAM`/
`COMMIT_CONFIG`, no flash):
- `safety_get_status`: link up, SaftyFW armed, not tripped, `currents 0.00 A,
  0.00 A, 0.00 A` (0.00 across all three because nothing is calibrated, not
  because current is absent).
- `io_read`: relays R1-R4 all 0 (off).
- `profiles_get_exec_status`: `state=0`, no run in progress.
- `safety_get_ct_cal`: all three channels **uncalibrated**.
- `safety_get_commissioning`: `ct_installed=0` — the commissioning flow does
  not yet know a CT is fitted at all (channel 3's 2026-09-05 fit, §5.2,
  predates re-running commissioning), so S14 reads DORMANT.

**RESOLVED 2026-09-06 — the raw-counts path now exists end to end** (see
"Tooling gap" below, all three items landed): `current_sense_sample()`
(`src/current_sense.c:230-250`) stores `counts_avg` into
`current_snapshot_t.counts_avg[3]` (`src/snapshots.h:74`), `link_task.c:1099`
carries it over the wire in `SAFETY_CMD_POWER`'s V2 (61-byte) layout
(`CommonFW/docs/LINK_PROTOCOL.md` Frame E, `KILNLINK_POWER_FLAG_COUNTS_VALID`),
and `tools/PcTools/src/kilnctrl/kilnlink_codec.py`'s `decode_power()` plus
`safety_capture_ct_counts()`/`mcp_server_safety.py` read it PC-side. The
paragraph below describes the **pre-2026-09-06** state and is kept for
history; do not use it to conclude the gap is still open.
- With every channel uncalibrated (`k_ct_v_per_a == 0`), `amps[n]` reads a
  hard `0.0f` (`cs_counts_to_amps()`'s documented behavior, §5's
  2026-08-24 note) — so even the amps path carries no information to invert
  back into counts. Computing counts from amps per the commissioning plan's
  formula (`zero_counts = zero_mv/1000 * gain * 4096/3.3`) is not possible
  either: there is no committed `zero_mv`/`k_ct` for channel 3 yet (§5.2
  gives the intended values, `zero_counts[2]` from param `0x0304`,
  `k_ct_v_per_a[2] = 1.000` (1/A_fs for a 1 A/V probe — see §5.2's
  2026-09-18 correction; an earlier revision of this section wrongly said
  0.989) from `0x030A`, but neither had been committed on this board as of
  this pre-2026-09-06 snapshot — `safety_get_ct_cal` reported channel 2
  uncalibrated. Both are committed on the board today; see §5.2/§5.3's
  2026-09-18 bench-state notes).
- Setting a calibration value to unlock the amps path was in scope for this
  task's task order **but was excluded by the task's own constraints** (no
  `SET_PARAM`/`COMMIT_CONFIG`), since that is exactly the kind of
  config-schema write CT_COMMISSIONING_PLAN.md step 1/2 wants done
  deliberately, with refusal checks, not as a side effect of a noise-floor
  measurement.

No CSV was written under `firmware/SaftyFW/docs/data/` — there is no capture
to save. The 3σ smallest-detectable-step figures for a 1 A and a 50 A probe
cannot be computed without a real counts noise measurement; do not
substitute a datasheet or theoretical estimate for one, since the whole
reason this step exists (CT_COMMISSIONING_PLAN.md's "Facts below" §3) is
that the ADC reference is the unregulated `3.3v_Safty` rail and the actual
noise on this board has never been measured.

See "Tooling gap" immediately below for what closes this.

### Tooling gap — CLOSED 2026-09-06

All three items below have landed; kept as a record of what was needed, not
as an open task list.

1. **Firmware, done**: `counts_avg[3]` is published in `current_snapshot_t`
   (`src/snapshots.h:74`), written by `current_sense_sample()`
   (`src/current_sense.c:230-250`) the same tick it already computed
   `counts_avg` locally.
2. **Link protocol, done**: added to the existing `SAFETY_CMD_POWER` frame
   as its V2 (61-byte) extension — `CommonFW/docs/LINK_PROTOCOL.md` §6
   Frame E, `kilnlink_power.h`'s `KILNLINK_POWER_LEN_V2`/
   `KILNLINK_POWER_FLAG_COUNTS_VALID`.
3. **PcTools MCP, done**: `decode_power()`
   (`tools/PcTools/src/kilnctrl/kilnlink_codec.py`) and
   `safety_capture_ct_counts()`/`mcp_server_safety.py` expose it PC-side.
   `tools/PcTools/src/kilnctrl/noise_floor.py` remains a different,
   unrelated tool (PID run-to-run repeat spread) — do not confuse the two.

CT_COMMISSIONING_PLAN.md step 0 can now be executed against this board with
this path; whether it has actually been run and what it measured is tracked
there, not here.

### Measured noise floor — RUN 2026-09-06

Executed via `safety_capture_ct_counts()` (default 60 s, relays off, no
firing, no autotune): 262 samples over 60.1 s (4.36 Hz achieved poll rate;
every 0.2 s poll recorded, not just samples where the count changed).

| Channel | mean (counts) | std (counts) | min | max |
|---|---|---|---|---|
| 1 (unfitted) | 16.23 | 0.418 | 16 | 17 |
| 2 (unfitted) | 17.00 | 0.000 | 17 | 17 |
| 3 (GPIO28/ADC2, fitted summed-heater CT) | 66.89 | 4.678 | 60 | 76 |

Channels 1 and 2 have no CT installed (§0.2/§5.2 — only channel 3 is
fitted) and read a near-constant low floor; not meaningful as a noise-floor
measurement, just recorded for completeness.

Conversion, RP2040 12-bit ADC against the unregulated `3.3v_Safty` rail
(`LSB = 3.3 V / 4096 = 0.8057 mV/count`), channel 3's fitted probe at 1 A : 1
V transconductance with its measured +59 mV idle offset:

```
mA = (counts * 0.8057 mV/count - 59 mV) / (1 V/A) * 1000
```

Mean: 66.89 counts -> 53.90 mV -> (53.90 - 59) = -5.1 mA (near zero, as
expected with no current flowing — within the offset's own measurement
error; per standing practice, sub-10 mA disagreements are not chased).
Noise std: 4.678 counts -> 3.77 mV -> **std ≈ 3.8 mA**. Observed range
(min/max over the 60 s window): 60-76 counts -> 48.34-61.23 mV ->
roughly -10.7 mA to +2.2 mA around zero, i.e. **~13 mA peak-to-peak** on
this fitted channel with the board idle.

This closes step 0 of `CT_COMMISSIONING_PLAN.md`: the ADC reference noise on
this board, measured rather than assumed, is on the order of a few mA std /
~13 mA peak-to-peak on the one fitted (1 A:1 V) probe. Use this figure, not a
datasheet estimate, when picking `i_present_a`/S14's `overcurrent_pct`
margins against real board noise.

---

## Completion checklist

Sampling/conversion/snapshot-publishing built and build-verified 2026-08-16
(`src/current_sense.{c,h}`, `src/tasks/current_task.{c,h}`) — zero-warning
build under `-Wall -Wextra -Werror`. **Not hardware-verified** — no
RP2040/CT hardware attached to the build machine.

- [x] `adc_owner`: round-robin ADC0/1/2, 16× oversample, 20 Hz/channel, first-after-mux sample discarded — manual `adc_select_input()`/`adc_read()` polling (no free-running FIFO capture, per this doc's own "no DMA, no free-running capture" in §4), documented as a deliberate deviation from `ARCHITECTURE.md` §8's general round-robin guidance
- [x] Peak-envelope conversion per §2 — **no RMS accumulator, no DMA capture**
- [x] `zero_counts` measured at runtime, operator-triggered (CT_COMMISSIONING_PLAN.md step 2, 2026-09-06): `current_sense_recalibrate_zero()` itself is still unused (it blocks its caller, which is unsafe on the wire path -- see below); the caller instead is `current_task.c`'s own `current_task_ct_auto_zero_begin()`/`_poll()`, accumulating one raw sample per its normal period (200 samples ≈10 s), reached over the link by `SAFETY_CMD_CT_AUTO_ZERO_BEGIN`/`GET_CT_AUTO_ZERO`/`CT_AUTO_ZERO_STATUS` (`LINK_PROTOCOL.md` §4/§6). The idle/every-relay-off-≥5s/K4/trip/profile/autotune preconditions and the 100 mV-at-probe drift-vs-real-current refusal are enforced ESP-side (`safety_cfg_http.c`'s `ct_auto_zero_post_handler()`), not by this file — unchanged from the original mechanism's own "preconditions are the caller's job" contract.
- [x] Clip detection → reported per-channel via `current_snapshot_t.clipped[n]` = "power estimate invalid", **not a trip**
- [x] `i_conducting_a` and `conduction_fraction` reported separately (§3b), in `current_sense_power_t`
- [x] `p_avg_w` only when `mains_voltage_v` is configured; `NAN` otherwise
- [x] `SAFETY_CMD_POWER` (Frame E) send wired up 2026-08-18
      (`firmware/SaftyFW/src/tasks/link_task.c`'s `link_task_send_power()`,
      2s cadence) — `current_sense_power_t` gained `mains_voltage_v`/
      `calibrated`/`any_clipped`/`p_total_w`/`energy_wh` so the frame can be
      built without leaking `current_sense_cal_t` into `link_task.c`. Guards
      S3/S4 are still NOT wired to `any_current_present` — that is correctly
      gated on step 2 below, not done here.
- [ ] Commissioning §5 run in full and results recorded — needs real hardware, not done
- [ ] **§5 step 2 (one relay at a time) passed on all three channels** — gates S3/S4 — needs real hardware, not done
- [ ] Decay verified <5 % within ~4 s — needs real hardware, not done
- [ ] Low-duty soak: 15 % duty, 60 s window, 1 h, **no S4 warning storm** — needs S4 (Phase 7) and real hardware, not done

### Known limitation: uncalibrated defaults are a nuisance, not a hazard

With the current zero-initialized placeholder (`zero_counts = 0`,
`i_present_a = 0`, `calibrated = false`) and a hypothetically-configured
`k_ct_v_per_a` (a partial-commissioning state), the op-amp/ADC's genuine
small positive zero-current floor (§5: "op-amp Vos and D14 leakage, not 0")
would compute as a nonzero phantom current, and `i_present_a = 0` sets the
load-active bar at "any current at all" — so this combination would read
**load active from noise alone**. This is the safe-direction failure: it
over-reports presence rather than under-reporting it, so a future guard
built on top would be more likely to nuisance-trip on a healthy idle kiln
than to miss a real fault, which is the same direction §0 already prefers.
`current_snapshot_t.calibrated` is `false` throughout this state, which is
the intended signal that none of this should be trusted yet.

**Superseded 2026-08-24** by the presence-decoupling fix described in §5
above: `config_store_default()` now ships `i_present_a = 2.0 A` (not `0`),
and presence detection (`current_snapshot_t.present[n]`,
`current_presence_policy.h`) no longer derives from `amps[n]`/`k_ct_v_per_a`
at all, so the "phantom current from noise" mechanism this paragraph
describes no longer reaches the S3/S4 presence fact. **It is not display-only,
though**: `safety_guards.c`'s S14 (`in->amps[ch] > threshold_a`, a per-channel
overcurrent WARN keyed off the commissioned `i_normal_a[ch]`) reads the same
raw `amps[n]` directly, gated only by `i_normal_valid[ch]` and
`relay_commanded_now_for_ct[ch]` — not by `current_snapshot_t.calibrated`. A
board whose zero-current offset has never been calibrated
(`calibrated == false`) but whose per-channel `i_normal_a[ch]` has already
been recorded (a partial-commissioning ordering this doc does not forbid)
can still feed the same noise-inflated `amps[ch]` into S14 and produce a
spurious WARN — a guard effect, not merely a reported-figure one. `calibrated`
(current_sense_cal_t) is likewise no longer the gate on presence — see
`current_task_reload_cal()`'s own comment (`src/tasks/current_task.c`) for
what it gates instead.
