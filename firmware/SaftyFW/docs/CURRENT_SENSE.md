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
| `k_ct_v_per_a` | power estimate, **and the presence threshold's domain conversion** | **Calibrated by the ESP's zone current-sweep (§5.1)**; CT datasheet or a clamp meter as the manual override | e.g. 0.0333 V/A for a 1 V/30 A CT |
| `gain` | power estimate only | 0.715 nominal, refined if the resistors are not 1 % | 0.715 |
| `mains_voltage_v` | power estimate only | The installation's nominal supply voltage | 240 |

Only the first two affect guard behaviour. **A wrong `k_ct_v_per_a` produces a
wrong number on a display and changes nothing else** — which is the whole
benefit of the scope limit in §0: current accuracy is a display concern, not a
safety one.

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


### 5.2 Channel 3 (Current3/GPIO28-ADC2): the summed-heater CT (2026-09-05)

Fitted here only, reading **summed** current of all heaters (not per-zone):
1 A/V, ~+59 mV pin offset. Same rectifier/peak-hold front end as channels 1/2
(HARDWARE.md §9) — no new model needed. +59 mV is this channel's
`zero_counts[2]` (measure per the Commissioning check step 1, not a manual
mV-to-counts conversion); `k_ct_v_per_a[2] = 0.989` gives 1 A/V at the pin
(0.715·√2 ≈ 1.011, so 1/1.011 ≈ 0.989). Commission via the existing
`config_params.c` ids: `0x0304` (`zero_counts[2]`, U16) and `0x030Au`
(`k_ct_v_per_a[2]`, F32). Both refused unconditionally while ARMED, no grace
exception (`config_store_decide_write()`). Once committed,
`current_presence_is_flowing()` (`current_presence_policy.h`) reads these
same two fields, so S9/S11 whole-board presence works immediately — no
firmware change needed.

---

## 4. Measured noise floor 2026-09-06 — BLOCKED, not measured

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

**Why no raw-counts path exists (traced in code, `current_sense.c`):**
- `current_sense_sample()` (`src/current_sense.c:230-250`) computes
  `counts_avg` via `cs_read_channel_counts()` as a **local variable inside
  the sampling loop**. It is converted to `amps[n]` and `present[n]`
  immediately and never written to any `static`/global storage — there is
  no symbol a debug read could target.
- `current_snapshot_t` (`src/snapshots.h:58-64`, the only thing
  `current_task_get_snapshot()` publishes) carries `amps[3]`, `clipped[3]`,
  `present[3]`, `calibrated` — **no counts field**.
- The wire protocol's `SAFETY_CMD_POWER` frame (`link_task_send_power()`)
  likewise only ever carries derived amps/watts (`current_sense_power_t`),
  never counts.
- With every channel uncalibrated (`k_ct_v_per_a == 0`), `amps[n]` reads a
  hard `0.0f` (`cs_counts_to_amps()`'s documented behavior, §5's
  2026-08-24 note) — so even the amps path carries no information to invert
  back into counts. Computing counts from amps per the commissioning plan's
  formula (`zero_counts = zero_mv/1000 * gain * 4096/3.3`) is not possible
  either: there is no committed `zero_mv`/`k_ct` for channel 3 yet (§5.2
  gives the intended values, `zero_counts[2]` from param `0x0304`,
  `k_ct_v_per_a[2] = 0.989` from `0x030A`, but neither has been committed on
  this board — `safety_get_ct_cal` reports channel 2 uncalibrated).
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

### Tooling gap

To make CT_COMMISSIONING_PLAN.md step 0 (and the commissioning check in §5
above) actually runnable, one of the following is needed — implementation
intentionally NOT done as part of this pass:

1. **Firmware**: publish `counts_avg[3]` (or the full pre-conversion
   `uint32_t` per channel) into `current_snapshot_t` (`src/snapshots.h`)
   alongside `amps[3]`, written by `current_sense_sample()`
   (`src/current_sense.c:230-250`) the same tick it already computes
   `counts_avg` locally. Cheap (3 more `uint32_t`, no new sampling), and it
   is the only way to see the ADC's actual value independent of whether
   `k_ct_v_per_a`/`zero_counts` have been committed.
2. **Link protocol**: a way to get that value off the Pico — either add it
   to the existing `SAFETY_CMD_POWER` frame, or a new lightweight
   diagnostic/debug command (`CommonFW/docs/LINK_PROTOCOL.md`), since the
   commissioning page and any future noise-floor tooling both need it live,
   not just over SWD.
3. **PcTools MCP**: a `kiln_call` tool (e.g. `safety_get_ct_raw_counts` or a
   `current_sense` group) that polls whatever the above exposes at a known
   rate and returns `{channel, counts, timestamp_ms}` per sample, so a
   noise-floor capture script can log CSV rows without hand-rolling framing.
   `tools/PcTools/src/kilnctrl/noise_floor.py` is a different, unrelated
   tool (PID run-to-run repeat spread) — do not confuse the two; a CT
   equivalent does not exist yet.

Until one of these lands, CT_COMMISSIONING_PLAN.md step 0 cannot be executed
against this board, and its downstream steps that depend on "is per-heater
open detection viable on this bench" (step 3's under-current warn) remain
undecided rather than merely unmeasured.

---

## Completion checklist

Sampling/conversion/snapshot-publishing built and build-verified 2026-08-16
(`src/current_sense.{c,h}`, `src/tasks/current_task.{c,h}`) — zero-warning
build under `-Wall -Wextra -Werror`. **Not hardware-verified** — no
RP2040/CT hardware attached to the build machine.

- [x] `adc_owner`: round-robin ADC0/1/2, 16× oversample, 20 Hz/channel, first-after-mux sample discarded — manual `adc_select_input()`/`adc_read()` polling (no free-running FIFO capture, per this doc's own "no DMA, no free-running capture" in §4), documented as a deliberate deviation from `ARCHITECTURE.md` §8's general round-robin guidance
- [x] Peak-envelope conversion per §2 — **no RMS accumulator, no DMA capture**
- [~] `zero_counts` measured at runtime after ≥5 min idle; drift reported — mechanism only (`current_sense_recalibrate_zero()`); no caller enforces the idle/no-relay precondition or reports drift yet (Phase 9, config_store)
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
