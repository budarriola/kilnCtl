# Current Sensing

> **Status:** planning · **Last reviewed:** 2026-08-16
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

This scope limit makes the calibration burden much lighter than it first
appears, and the two jobs have very different accuracy needs:

| Job | Accuracy required | Consequence of being wrong |
|---|---|---|
| Load-active | **Within a factor of ~2** | `i_present_a` only has to sit between measurement noise and a conducting element — one to two orders of magnitude apart |
| Power estimate | As good as you care to make it | A wrong number on a display |

So a mis-rated CT degrades the GUI's power figure and leaves every guard working
exactly as before. That is the right way round.

---

## 1. The circuit

Traced from `mainBoard/output/kiln.pdf` p.4. Designators are channel 1's;
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

`k_ct` is **not derivable from anything in this repository.**

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
| `HEATER_WINDOW_MS` | **60000** (60 s) | `App/drivers/profile_executor.c:27` |
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

- **Sample rate: 20 Hz per channel** (round-robin across ADC0/1/2 in the
  RP2040's single SAR, so 60 conversions/s total — a rounding error against
  the ADC's 500 kS/s).
- **Oversample and average**: take 16 back-to-back conversions per channel per
  sample and mean them. Cheap, and it buys ~2 bits against the RP2040 ADC's
  well-documented noise.
- **No DMA, no free-running capture, no FFT, no RMS accumulator.** All of that
  is what you would need if the ADC saw the raw waveform. It does not.
- Feed a **slow first-order filter** (τ ≈ 0.5 s) on top, for reporting only.
  **Guards evaluate the unfiltered sample**, so that filter can never delay a
  trip — the same discipline `KilnFW` applies to raw-versus-calibrated
  readings in `thermal_guard.h`.

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
| `k_ct_v_per_a` | power estimate only | CT datasheet, then confirmed against a clamp meter | e.g. 0.0333 V/A for a 1 V/30 A CT |
| `gain` | power estimate only | 0.715 nominal, refined if the resistors are not 1 % | 0.715 |
| `mains_voltage_v` | power estimate only | The installation's nominal supply voltage | 240 |

Only the first two affect guard behaviour. **A wrong `k_ct_v_per_a` produces a
wrong number on a display and changes nothing else** — which is the whole
benefit of the scope limit in §0: current accuracy is a display concern, not a
safety one.

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
   same conductor. Adjust `k_ct_v_per_a` to match. **This step only affects the
   power estimate** — skipping it leaves every guard fully functional and the
   GUI's wattage wrong.
4. Command the relay off and record the decay. Confirm it reaches < 5 % within
   ~4 s. A much slower decay means C57 or R77 is wrong; a much faster one means
   something is loading the hold node.
5. Repeat for channels 2 and 3.

**Step 2 is the one that gates the guards.** Until it passes on all three
channels, **S3 and S4 must be left disabled** (`TODO.md` phase 5). A correlation
guard fed by a mis-mapped CT is worse than no guard: it will trip on healthy
firings and stay quiet on the failure it exists to catch. Steps 1 and 4 are
also guard-relevant (zero, and decay behaviour). Step 3 is cosmetic.


---

## Completion checklist

- [ ] `adc_owner`: round-robin ADC0/1/2, 16× oversample, 20 Hz/channel, first-after-mux sample discarded
- [ ] Peak-envelope conversion per §2 — **no RMS accumulator, no DMA capture**
- [ ] `zero_counts` measured at runtime after ≥5 min idle; drift reported
- [ ] Clip detection → `CURRENT_FLAG_CLIPPED` = "power estimate invalid", **not a trip**
- [ ] `i_conducting_a` and `conduction_fraction` reported separately (§3b)
- [ ] `p_avg_w` only when `mains_voltage_v` is configured; `—` otherwise
- [ ] Commissioning §5 run in full and results recorded
- [ ] **§5 step 2 (one relay at a time) passed on all three channels** — gates S3/S4
- [ ] Decay verified <5 % within ~4 s
- [ ] Low-duty soak: 15 % duty, 60 s window, 1 h, **no S4 warning storm**
