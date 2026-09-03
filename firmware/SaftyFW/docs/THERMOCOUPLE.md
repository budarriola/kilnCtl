# The Safety Thermocouple

> **Status:** planning · **Last reviewed:** 2026-08-16
> **Keep this file current.** If the driver, the register setup or the type
> guidance changes, update it in the same commit. If it disagrees with the code,
> **the code wins.** Checklist at the bottom.

One MAX31856 on the `SaftyThermocoupleBoard` daughterboard, reached over the
Pico's SPI0 through J7. This is the sensor that guards S1, S2, S5, S8, S10, S11
and S12 all read, so it is the single most consequential component in the
system.

`firmware/KilnFW/docs/MAX31856.md` is the reference for the part itself — registers,
fixed-point formats, and the traps. **Port that driver rather than rewriting
it.** This document covers only what differs here, and the decisions that are
`SaftyFW`'s to make.

---

## 1. What differs from the main board

| | Main board (3 channels) | Safety board (1 channel) |
|---|---|---|
| Bus | Shared SPI with the ILI9488 display at a different mode and clock | **SPI0, sole device** |
| `~CS` | GPIO14/17/18 | **GPIO1**, with a 10 k external pull-up |
| `~FAULT` | Real ESP GPIOs | **GPIO11**, 10 k pull-up (R1) |
| `~DRDY` | **SX1509 expander pins** — costs an I2C transfer to read | **GPIO12**, 10 k pull-up (R2) — a real GPIO |
| Bus contention | Four devices, one owner task, interleaved traffic | None |

**Two of these are outright improvements and the driver should exploit both.**

### `~DRDY` is a real interrupt here

On the main board `~DRDY` lands on the I/O expander, so `MAX31856.c` needs a
`MAX31856_set_drdy_provider()` hook and degrades to an elapsed-time estimate of
staleness — which, as that doc says plainly, **cannot detect a part that has
silently stopped converting.**

Here `~DRDY` is GPIO12 with a pull-up. So:

- Attach a **falling-edge interrupt** and read on the edge. No polling, no
  elapsed-time guessing.
- **Staleness becomes a hardware fact**, not an inference. If `~DRDY` has not
  asserted within ~2× the expected conversion interval, the part has stopped
  converting — and *that* is the failure `KilnFW` explicitly cannot see. Feed it
  straight into S5 as a sensor-invalid condition.
- Keep the ordering `MAX31856.c` already documents: **sample `~DRDY` before the
  register burst**, because reading `CJTH`/`CJTL` is what releases it high again.

### The bus is not shared

No display, no second thermocouple, no arbitration. `spi_owner` still exists —
one task owning the interface is the house pattern, and J7 carries an I2C bus
that may one day gain a device — but it will never block on another driver's
transaction, which removes a whole class of latency question from the guard
path.

Clock: the part's limit is 5 MHz, but 4 MHz is an enforced **ceiling** on both
masters (`SPI_OWNER_BAUDRATE_HZ` here, `THERMO_SPI_CLOCK_HZ` in KilnFW) because
the SimFW bench fixture's slave emulation misses its first-byte deadline above
that, silently shifting a burst by one byte — see
`firmware/SimFW/docs/SPI_ACCESS_AUDIT.md` §9. Use **4 MHz** in **SPI mode 1**
(CPHA must be 1).

---

## 2. Thermocouple type — a real decision, not a default

The MAX31856 supports B/E/J/K/N/R/S/T. `KilnFW` ships **type K** for the zone
thermocouples. **The safety thermocouple should not automatically match**, and
which type is right depends on `tc_placement_mode` (`SAFETY_MODEL.md` §3).

### If `CHAMBER_AGREED` and the kiln fires above ~1200 °C, type K is the wrong choice

Type K's practical continuous limit is around **1260 °C** for heavy gauge, and
considerably lower — 1100 °C or so — for the thin wire usually fitted. Cone 10 is
**1285–1305 °C**. So a type-K safety thermocouple at cone 10 is operating at or
past its limit, where it suffers:

- **Drift**, tens of degrees over relatively few firings, always in a direction
  the reading cannot self-detect;
- **Green rot** — preferential oxidation of chromium in the positive leg in
  low-oxygen atmospheres, which is exactly what a loaded, reducing kiln provides.
  It produces a *low* reading. A safety sensor that reads low as it ages fails in
  the dangerous direction;
- Short service life at temperature.

**Type S** (Pt–10 % Rh) or **type R** is the standard choice for high-fire
ceramics, good to 1450–1600 °C, and the MAX31856 linearizes both natively.

The trade is output level: type S produces roughly **10 µV/°C** against type K's
**41 µV/°C**, so noise and cold-junction error matter about four times as much,
and cabling discipline matters correspondingly more. For a *safety* sensor
whose thresholds carry tens of degrees of margin, that is an easy trade — a
sensor that reads 30 °C low because it has aged is far worse than one that is
2 °C noisier.

**Recommendation:**

| Placement | Peak temperature | Type |
|---|---|---|
| `CHAMBER_AGREED` | above ~1150 °C | **S or R** |
| `CHAMBER_AGREED` | below ~1150 °C | K is fine |
| `EXTERNAL_OVERHEAT` | shell / exhaust / enclosure — a few hundred °C at most | **K**, comfortably |

### The type is configuration, and a mismatch is a silent hazard

`tc_type` must be a commissioning field, and it must match the thermocouple
physically fitted. **A mismatch does not produce an error — it produces a
plausible, wrong number.** Feeding a type-S thermocouple's millivolts through the
type-K linearization reads roughly a quarter of the true temperature: a 1250 °C
chamber reports around 320 °C, comfortably below every threshold, forever.

Nothing in the electronics can detect this. Two partial mitigations, both worth
having:

- **`THERMO_FAULT_TCRANGE`** fires when the reading falls outside the configured
  type's range — which catches a K-configured-as-S mismatch (reading absurdly
  high) but *not* the dangerous direction above.
- **S10**, in `CHAMBER_AGREED`, compares against the zone thermocouples and would
  catch a 900 °C disagreement immediately. This is one of the better arguments
  for declaring `CHAMBER_AGREED` where it is physically true.

Beyond that it is a commissioning check: at a known soak, the safety reading must
agree with a reference. Put it in the commissioning list (`TODO.md` phase 8) and
record the result.

**A third, narrower layer (2026-08-24, TODO.md Phase 3):**
`max31856_tc_range_policy.c` re-checks every decoded reading against
`config_store`'s *own* commissioned `tc_type`, independent of whatever the
MAX31856's CR1 register is actually running right now, using the datasheet's
per-type linearization range (`MAX31856.pdf` p.12, Table 1) rather than a
range hard-coded to K. Be precise about what this does and does not close:
it does **not** help the wrong-sensor-physically-fitted scenario just
described above (a real Type-S junction read through a Type-K LUT lands
*inside* K's own wide range, whether the check compares against the part's
CR1 or against config_store's belief — both currently agree "K" in that
scenario, and 320 °C is simply a valid K reading). **This is still true after
the update below — nothing in this pass closes that specific hazard.** What
it *does* catch is config_store's belief and the part's actual CR1 register
disagreeing (e.g. a transient SPI failure during `max31856_configure()` left
the part on a stale/default type while config_store believes a different
one was committed) — a case the existing `THERMO_FAULT_TCRANGE` bit cannot
see, because that bit only ever compares against whatever CR1 the chip
itself is currently running. Feeds S5 (marks the reading invalid, same as
any other bad-read cause) rather than a new trip — see `SAFETY_MODEL.md`'s
S5 section and `max31856_tc_range_policy.h`'s own header comment for the
full argument.

**Updated 2026-08-24 (`CONFIG_STORE_SET_TC_TYPE`, `config_store.h`):**
the paragraph above used to say this check runs unconditionally even on a
never-commissioned (default Type K) board, because `tc_type` had no
commissioning bit to tell "operator confirmed K" apart from "nobody has
ever touched this". It now does:

- **Genuinely commissioned:** the operator's own type gets the exact
  datasheet band above, unchanged from before.
- **Never commissioned:** applying K's own tight band to a value nobody
  confirmed was asserting precision this check could not honestly claim, so
  an uncommissioned board now gets `max31856_tc_range_is_plausible_
  uncommissioned()` instead — the union of all eight types' ranges
  (-210 °C..+1820 °C), a pure garbage floor rather than a type-specific
  check. This is a deliberate *widening* versus the old K-only behaviour on
  the hot end (K's own ceiling is 1372 °C; the union's is B's 1820 °C) —
  not a regression, because this check was never the primary ceiling for an
  uncommissioned board (S1's `abs_max_temp_c` gating and the MAX31856's own
  `THERM_FAULT_TCRANGE` bit are both unaffected). The fact that the tight
  band is inactive is surfaced, not silent: an uncommissioned `tc_type` now
  keeps `calibration_missing` true (`CONFIG_REFERENCE.md` §1/§7), the same
  wire-visible signal every other no-safe-default field uses.

**Part B, same date — CR1 readback verification (`max31856.c`):**
`max31856_configure()` now reads CR1 back once, immediately after writing
it, and compares TC TYPE[3:0] against the type it was asked to write
(`max31856_tc_range_policy.h`'s `max31856_cr1_readback_check()`). This closes
the gap the original paragraph above only partially argued around: before
this, a CR1 write that silently failed (the OTHER writes in that same
function surviving) left the part running a stale/different type with
nothing to say so — not this file's own plausibility band (which is keyed
off `config_store`'s belief, not the part's real register, by design) and
not `THERM_FAULT_TCRANGE` (which compares against whatever CR1 the part
actually holds — the same wrong register this failure produces). A mismatch,
or a readback of `0x00`/`0xFF` (treated as a dead/shifted bus, per
`spi_owner.c`'s own baudrate-margin comment about a shifted burst returning
plausible-looking wrong numbers, not "some other real type") both mark the
reading invalid, feeding S5 exactly like the plausibility band — no new
trip. The readback happens once, at configure-time (boot, and any future
detected-reset re-assert), not on `thermo_task`'s hot per-sample path; the
per-sample cost is one cached boolean read (`max31856_tc_type_verified()`).

---

## 3. Borrowing a main-board thermocouple

`tc_source` (`SAFETY_MODEL.md` §3) allows the safety processor to use one of the
**main board's** zone thermocouples instead of, or alongside, its own:

| `tc_source` | Reading comes from | Independence |
|---|---|---|
| `OWN_J7` | the Pico's own MAX31856 | **full** |
| `BORROWED_ZONE` | zone `borrowed_zone_index`, via the context frame | **none** — see below |
| `BOTH` | own sensor is primary; borrowed is a continuous cross-check | full, plus a cross-check |

### What borrowing costs

A borrowed reading is measured by the main board's MAX31856, read by the main
board's SPI driver, packed by the main board's firmware and delivered over a
link the main board controls. **Every one of those is a component the safety
processor exists to distrust**, and a fault in any of them is now inside the
safety path.

It is still a reasonable configuration for a board built without the J7
daughterboard — a borrowed reading beats no temperature at all — but it must be
chosen deliberately. In `BORROWED_ZONE` the safety processor sets
`SAFETY_FLAG_BORROWED` in every status frame, and the GUI must label the
temperature accordingly.

**`BOTH` is the configuration to aim for.** It is the only one where a drifting
or frozen sensor on *either* board is visible from the other, which directly
attacks the "six of nine failure modes read low" problem in §5.

### A borrowed channel must be proven to be updating

This is the requirement that shapes the protocol. A MAX31856 on the main board
that has stopped converting keeps returning its last value; the ESP forwards it
faithfully every 500 ms; and from the Pico's side **that is indistinguishable
from a kiln holding a steady soak.**

So the context frame carries a per-zone **`sample_counter`**, incremented by the
ESP only when it actually consumes a fresh conversion — never merely because it
built a frame (`firmware/CommonFW/docs/LINK_PROTOCOL.md` §4). Three distinct failures then
become three distinct diagnoses:

| Symptom | Guard | Meaning |
|---|---|---|
| `sample_counter` frozen | **S13** | that channel stopped converting |
| counter advancing, value frozen while heat is on | **S11** | the sensor itself is stuck |
| no frames arriving | **S6** | the link, not the sensor |

A borrowed reading whose counter is stale is treated as **invalid immediately**,
not merely old — it is a number of unknown age.

### And it must agree

A borrowed channel is one of the sensors the zone guards already use, measuring
the same chamber, so it is by definition `CHAMBER_AGREED` — `tc_placement_mode`
is forced to it, and a configuration that says otherwise is **rejected rather
than reconciled**. S10 (disagreement) is therefore always active in this mode,
and in `BOTH` it compares the two sources directly with the own sensor as
reference. That is the strongest sensor cross-check available anywhere in this
design.

### Type checking a borrowed channel

The context frame carries each zone's configured `tc_type`. The Pico compares it
against `borrowed_type_expected` and warns on a change: it means someone
reconfigured that channel on the main board, and the safety processor's
plausibility ranges were built for the old one.

---

## 4. Every thermocouple may be a different type

Nothing requires the four thermocouples in this system to match, and §2's
guidance means they often should not — a chamber safety sensor may want type S
while the zone sensors stay type K, or a shell-mounted safety sensor stays K
while a high-fire zone moves to S.

The wire already supports it: `THERMO_CMD_CONFIG_CHANNEL`'s `byte2 = tc_type` is
per-channel (`firmware/KilnFW/App/drivers/uart_task_ids.h`). What does **not** yet support
it is `KilnFW`'s configuration — all three zone channels are configured
identically today.

So the plan is:

- **`SaftyFW`**: `tc_type` is a per-sensor commissioning field, applying to the
  J7 sensor.
- **`KilnFW`**: `zone_cfg_t` gains a per-zone `tc_type`, exposed on the
  Thermocouples & Zones page and pushed to the part at init
  (`firmware/SaftyFW/TODO.md` 0.14).
- **The context frame** carries each zone's type, so the Pico is never guessing
  what a borrowed reading means.

**Plausibility ranges are per-type, not global.** Type K's range is roughly
−200…1372 °C; type S is −50…1768 °C; type T tops out near 400 °C. A "temperature
out of range" check hard-coded to one type will either miss a fault on a
wider-range sensor or fire spuriously on a narrower one. Drive it from the
configured type, and let `THERMO_FAULT_TCRANGE` — which the part computes
against its *own* configured type — be the primary signal.

---

## 5. Configuration

Written once at init, re-asserted if the part is ever seen to reset.

| Register | Setting | Why |
|---|---|---|
| `CR0` CMODE | **1 — automatic conversion** | Free-running, ~100 ms/conversion, paired with the `~DRDY` interrupt. One-shot would make the guard path depend on the Pico remembering to ask |
| `CR0` 50/60 Hz | **match the local mains** | The notch filter is the main defence against mains pickup on a long thermocouple run. Only changeable while conversions are off — stop, write, restart, exactly as `KilnFW` does |
| `CR0` OCFAULT[1:0] | **enabled**, shortest setting the lead resistance allows | This is what produces `THERMO_FAULT_OPEN`, the detector for a thermocouple that has fallen off |
| `CR0` CJ disable | **0 — cold junction enabled** | Needed for compensation, and S12 reads it |
| `CR0` FAULT mode | **comparator** | Bits clear themselves when the condition clears; no `FAULTCLR` handshake to get wrong. Matches `KilnFW` |
| `CR1` AVGSEL | **4 samples** | ~230 ms/conversion. Quieter than 1, still far faster than any thermal event. 16 would add nothing but latency |
| `CR1` TC TYPE | **commissioned** (§2) | Not a compile-time constant |
| `MASK` (02h) | **unmask OPEN, OVUV, TCRANGE, CJRANGE**; mask TCHIGH/TCLOW/CJHIGH/CJLOW | Default is `FFh` = everything masked. The `~FAULT` pin should mean "the sensor is broken", not "a threshold was crossed" — thresholds are the guards' job |
| `CJTO` (09h) | 0 unless a measured offset exists | ±8 °C range, 0.0625 °C/LSB |
| `LTHFTH/L`, `LTLFTH/L` | leave wide open | S1 owns the ceiling, in software, where it can be mode-dependent and firing-aware. Duplicating it in the part would create two thresholds to keep in sync |

**Set `MASK` deliberately — the reset default masks everything.** Leaving it at
`FFh` leaves `~FAULT` permanently inactive, so the pin looks healthy no matter
what happens, and S5 loses its fastest signal.

### Thresholds live in software, not in the part

Tempting to push S1's ceiling into `LTHFTH/L` and let the part flag it. Don't:
the ceiling is `min(abs_max, firing_max + margin)` in `CHAMBER_AGREED`, so it
changes per firing; and a threshold in two places is a threshold that will
disagree with itself. The part's comparators stay wide open and the guards do
the comparing.

---

## 6. Accuracy budget — and why the margins are what they are

Worth writing down, because it is the justification for every generous number in
`SAFETY_MODEL.md`.

| Source | Typical contribution at ~1250 °C |
|---|---|
| MAX31856 thermocouple conversion | ±2 °C |
| Cold-junction sensor | ±0.7 °C, degrading toward the ±125 °C limits |
| Thermocouple tolerance, class 1 type K (±0.4 %) | **±5 °C** |
| Thermocouple tolerance, class 1 type S (±0.25 %) | ±3 °C |
| Drift / ageing over service life | **tens of °C, unbounded and undetectable** |
| Mounting position vs. the thermal point of interest | tens of °C |

The electronics are the *smallest* term by a wide margin. Everything that
matters is the sensor and where it is.

Three consequences that shape the design:

- **A threshold set 10 °C above an expected value is meaningless** — it is inside
  the sensor stack's own uncertainty. Hence S2's 75 °C margin and S1's
  100 °C firing margin.
- **Drift is unbounded and silent**, which is why S10 (comparison) and S11
  (frozen detection) exist at all: they catch sensor failures that no accuracy
  spec addresses.
- **Sub-degree resolution is not precision.** The part reports 0.0078125 °C per
  code. Never display or log the safety temperature to more than **1 decimal
  place**, or the reading acquires an authority it has not earned.

---

## 7. Failure modes and which guard catches each

| Failure | Detected by | Direction of error |
|---|---|---|
| Open circuit / fallen off | `THERMO_FAULT_OPEN` → S5 | reads open, unambiguous |
| Short across the leads | Reads cold-junction temperature → S10, S11 | **reads low — dangerous** |
| SPI bus failure | Transfer error → S5 | no reading |
| Part stopped converting | **`~DRDY` silence → S5** (only possible here, not on the main board) | reading freezes → also S11 |
| Reversed polarity | Reads backwards; falling reading while heating → S10, and `KilnFW`'s guard 2 logic if ported | **reads low — dangerous** |
| Wrong `tc_type` configured | S10 in `CHAMBER_AGREED`; commissioning check otherwise | usually **low — dangerous** |
| Drift / green rot with age | S10 in `CHAMBER_AGREED`; otherwise **nothing** | **reads low — dangerous** |
| Cold junction out of range | `THERMO_FAULT_CJRANGE` → S5; S12 catches it earlier | reading biased |
| Extension wire of the wrong type | Nothing, unless S10 | offset, direction depends |

**Six of these fail low, and reading low is the dangerous direction** — a
too-cold reading never trips a ceiling. That asymmetry is the strongest
practical argument for declaring `CHAMBER_AGREED` wherever it is physically
honest, because S10 is the only guard that catches most of them, and it is the
one that is disabled in `EXTERNAL_OVERHEAT`.

It is also the argument for the **second independent safety thermocouple** in
`TODO.md` phase 8. Two sensors that disagree tell you something is wrong; one
sensor reading low tells you nothing at all.


---

## Completion checklist

**Driver**
- [x] `max31856.c`/`.h` ported from `KilnFW`'s `MAX31856.c`/`.h` (ported, not
      rewritten — same register map, same four fixed-point conversions, same
      comparator-fault-mode logic, same failure-honesty discipline; single
      channel, no bus-sharing machinery, per §1's "the bus is not shared").
      2026-08-16
- [x] SPI0 mode 1, 4 MHz, `CS0` (GPIO1) as a plain GPIO — `src/spi_owner.c`
- [x] `~DRDY` (GPIO12) falling-edge **interrupt**, sampled *before* the
      register burst — `src/tasks/thermo_task.c`'s `thermo_drdy_isr()`
      wakes the task via a task notification; the burst read happens on wake,
      before anything else, so ~DRDY is released promptly
- [x] **`~DRDY` silence detection** feeding S5 — `thermo_task.c` waits on the
      notification with a timeout of 2× `max31856_conversion_time_ms()`; a
      timeout publishes `thermo_snapshot_t.valid = false` without even
      attempting a burst read, which `safety_core.c` maps straight onto
      `safety_guard_input_t.tc_valid = false` — the stopped-converting
      failure `KilnFW` cannot see. **Build-verified only** — no MAX31856 is
      attached to the build machine, so the silence path has not been
      observed against real hardware
- [x] NaN, never 0, on any invalid reading — `max31856_read()` fills the
      output struct first with NaN/`spi_failed = true` before anything can
      fail, same discipline as the KilnFW original

**Configuration**
- [x] `tc_type` a commissioning field (runtime parameter of
      `max31856_configure()`), not a compile-time constant — currently
      supplied by `main.c` as `MAX31856_TC_TYPE_PLACEHOLDER` (type K) because
      no `config_store` exists yet (Phase 9) to source the real
      per-installation decision from §2. **This is not a decision that K is
      correct for this kiln** — whoever wires `config_store` must replace
      that call site
- [x] **`MASK` (02h) set explicitly** — `MAX31856_DEFAULT_FAULT_MASK` (0xFC:
      OPEN + OVUV unmasked, the four threshold faults masked; TCRANGE/CJRANGE
      have no mask bit at all, so they are unmaskable by construction) —
      `max31856.c`
- [x] `CR0`: auto-convert, 60 Hz notch, OCFAULT mode 1, comparator fault mode
      — `max31856_configure()`. **50/60 Hz is currently hardcoded to 60 Hz**,
      the same placeholder status as `tc_type` (not yet a `config_store`
      field); update this line if that changes
- [x] `CR1`: AVGSEL = 4 samples (`MAX31856_AVGSEL_4_SAMPLES`)
- [x] Part thresholds left wide open — `max31856_configure()` never touches
      `CJHF/CJLF`/`LTHFTH/L`/`LTLFTH/L`, so they stay at their power-on
      full-scale defaults; S1 owns the ceiling, in software
- [x] Per-type plausibility ranges, driven from the configured type —
      `max31856_tc_range_policy.c`, wired into `thermo_task.c`, feeding S5.
      2026-08-24: gated on `config_store_is_tc_type_set()` -- a genuinely
      commissioned type gets its own exact datasheet band, an uncommissioned
      one gets the union of all eight types' ranges instead (§2's "Updated
      2026-08-24" note above has the full argument)
- [x] CR1 readback verification at configure-time — `max31856_configure()`
      (`max31856.c`) reads CR1 back once, right after writing it, and
      compares TC TYPE[3:0] against what it asked for
      (`max31856_tc_type_verified()`, §2's "Part B" note above). This closes
      the "was the write actually accepted" half of the gap this checklist
      item used to describe.
- [ ] Automatic config re-assertion if the part is ever seen to have reset
      — **still not done**. The CR1 readback above only runs inside
      `max31856_configure()` itself (boot, and any FUTURE explicit re-assert
      call); nothing yet independently detects a live part reset mid-run
      and calls `max31856_configure()` again on its own. A part that resets
      itself after a successful boot-time configure would revert to its
      power-on CR1 (Type K, `MAX31856_AVGSEL_4_SAMPLES` off) with nothing
      to notice until the NEXT explicit configure call, which nothing today
      triggers automatically.

**Borrowed source**
- [x] `tc_source` implemented: `OWN_J7` / `BORROWED_ZONE` / `BOTH` (verified
      2026-09-03: `config_store.h`'s `tc_source` field, `config_params.c`
      SET/GET, contradiction-with-`tc_placement_mode` rejected at
      `config_params_validate()`, host-tested)
- [ ] `SAFETY_FLAG_BORROWED` set in status frames when borrowing — still
      genuinely open, no such bit exists in `CommonFW` yet
- [x] `tc_placement_mode` forced to `CHAMBER_AGREED`; contradictory config
      **rejected** (verified 2026-09-03, same validator as above)
- [x] S13 implemented against `sample_counter` (`context_borrowed_sample_counter_advancing()`, `src/snapshots.h`) —
      and the guard itself (`SAFETY_TRIP_BORROWED_STALE`, `safety_guards.c`)
      consumes it, not just the plumbing
- [x] Borrowed `tc_type` compared against `borrowed_type_expected` — done
      2026-09-03, `context_borrowed_type_mismatch()` (`src/snapshots.h`),
      called from `safety_core_build_input()`, logs `LOG_LEVEL_WARN` once on
      the transition into mismatch. Diagnostic only, no new guard/trip.
- [x] `BOTH` mode cross-compares the two sources via S10 (verified
      2026-09-03: `safety_guards.c`'s S10 block compares against
      `nearest_zone_measured_c` regardless of `OWN_J7` vs `BOTH`, matching
      this doc's own "And it must agree" section above)

**Commissioning**
- [ ] Thermocouple type chosen deliberately per §2, per sensor
- [ ] Known-soak check against a reference instrument — the only way to catch a type mismatch
- [ ] Open-circuit test reports `THERMO_FAULT_OPEN`, not a plausible number
- [ ] Accuracy budget (§6) reviewed against the thresholds actually set
- [ ] Safety temperature displayed to **1 decimal place at most**
