# SimFW — Bench Fixture Bill of Materials

> **Status:** first pass, not yet ordered · **Last reviewed:** 2026-08-23
> **Purpose:** remove the literal blocker — you cannot build the bench
> fixture until parts are ordered, and until this document existed there was
> no orderable list. Every line below traces to a specific source: a
> schematic value read via the KiCad MCP tools, a figure already computed in
> `docs/CURRENT_SENSE.md`/`docs/HARDWARE.md`, or a live Mouser lookup done
> while writing this document (2026-08-20).
>
> **This is a first-pass BOM, not a purchase order.** Four items are
> explicitly flagged "needs bench measurement/confirmation before ordering"
> in section 9 — including a **blocking** one (item 1: the CT transformer's
> unconfirmed primary inductance under direct DAC drive) — do not
> one-click-buy the whole table without reading that section first.
> Everything else is ready to order as written.

---

## 1. Summary table

| # | Item | Qty | MPN (Mouser #) | Unit price | Line total | Status |
|---|---|---|---|---|---|---|
| 1 | Raspberry Pi Pico (RP2040) | 1 | Raspberry Pi **SC0915** (358-SC0915) | $4.00 | $4.00 | In stock 3915, confirmed live |
| 2 | I/O expander, MCP23017 | 2 | Microchip **MCP23017-E/SO** (579-MCP23017-E/SO) | $1.62 | $3.24 | In stock 39,748 |
| 4 | CT coupling transformer | 3 | Triad **TY-300P** (553-TY300P) — **NOT CONFIRMED SUITABLE for direct DAC drive, see §9 item 1 (BLOCKING)** | $7.24 | $21.72 | Ratio (1:1) is correct, but under direct DAC drive (item 18) the transformer must be sized on **primary inductance at 60 Hz**, not turns ratio — the TY-300P's suitability on that measure is unconfirmed and likely wrong for this part class; do not order until §9 item 1 is resolved |
| 7 | DUT power relay | 2 | Omron **G5LE-14-DC12** (653-G5LE-14-DC12) | $1.61 | $3.22 | In stock 6,997 — **2, not 1, see §6** (rationale rewritten 2026-08-23: independent per-domain testing, not ground isolation) |
| 8 | J6 mating header (main-side TC bus) | 2 strips | Würth **61300411121** 1×40 breakaway (710-61300411121) | $0.19 | $0.38 | In stock 69,754; cut to 1×20 |
| 9 | J7 mating connector (safety-side TC bus, 2×6 @ 1.27 mm) | TBD | **Not selected — gender/pitch unconfirmed, see §9** | ~$1–3 | ~$3–9 (est.) | Placeholder, do not order yet |
| 10 | CT jack, fixture-side (mates J13/J15/J17 via patch cable) | 3 | Same Sky/CUI **SJ-3523-SMT-TR** (490-SJ-3523-SMT-TR) | $0.88 | $2.64 | In stock 32,954 |
| 11 | 3.5 mm mono male-male patch cable | 3 | Generic/commodity — no Mouser electronics MPN needed | ~$3.00 | ~$9.00 (est.) | Buy locally/Amazon; any shielded mono cable works |
| 12 | Relay-sense + E-stop series resistors | 6 | Generic 1 kΩ 0805/THT (already stocked from main-board BOM class) | ~$0.02 | ~$0.12 | See §4 (5x relay-sense, K4 included) and §5 (1x E-stop direct drive, replacing the removed CPC1017N — item 6) — internal MCP23017 pull-ups do most of the work; all 6 signals wire with the same series-resistor treatment |
| 13 | Hookup wire + ferrules, for J1/J3/J4/J8/J10/J11 (fixed screw terminals) | 1 lot | Generic 22–24 AWG stranded + ferrule kit | — | ~$10 (est.) | See §8 — these connectors are **fixed**, not pluggable |
| 14 | DC-block / decoupling passives (3× CT channels + series-resistor protection + expanders + I2S DAC output blocking caps) | 1 lot | Generic R/C, values in §3 | — | ~$10 (est.) | Commodity, mostly already-stocked values from main-board BOM. Under the I2S DAC replacement (item 18), each DAC output needs its own series DC-blocking cap ahead of the transformer primary — the UDA1334A's ≈1.65 V DC bias (Vref = 0.5×VDD) will saturate the core / swamp the output stage otherwise (datasheet §14.1) — the PWM path's RC low-pass is no longer needed once the DAC path replaces it |
| 15 | Protoboard/perfboard carrier + sockets for Pico/ICs | 1 | Generic 100×160 mm double-sided perfboard + 2×20 female header (socketed Pico) | — | ~$10 (est.) | Generic |
| 16 | Debug UART / SWD to Debug Probe | 0 | **Reuse existing Raspberry Pi Debug Probe + cables from the `SaftyFW` bench setup** | $0 | $0 | No new purchase |
| 17 | PCA9685 (optional PWM/LED stimulus) | 0 | Not included — PLAN.md marks it optional, not required for the base feature set | — | $0 | Deferred |
| 18 | I2S stereo DAC breakout, UDA1334A | 2 | Adafruit **UDA1334A I2S Stereo DAC** breakout (or equivalent) | ~$7.00 | ~$14.00 | New 2026-08-23 — replaces PWM-based CT waveform synthesis; 2 modules (stereo, 2 ch each) cover 3 CT channels, 16 kHz sample rate; drives the 1:1 isolation transformer (item 4) directly, no amplifier — see §3 for sizing and §9 item 1 (**blocking**: transformer suitability under direct drive is unconfirmed) |

**Items 3 and 5 (the two TI ISO7740DWR digital isolators, and the K4
Vishay 4N35 optocoupler) are removed as of 2026-08-23.** The fixture's
ground is now commoned with the DUT's (`DESIGN_NOTES.md` §3.5) — SPI bus B
wires as direct GPIO like bus A, and K4 senses exactly like K1/K2/K3/K5.
Item numbers are kept stable rather than renumbered, so cross-references
elsewhere in this repo don't break.

**Item 6 (E-stop optoMOS, Littelfuse/IXYS CPC1017N) is removed as of
2026-08-23.** `DESIGN_NOTES.md` §3.4/§3.5/§14: the part was kept through the
ground-commoning pass on the argument that it was "the E-stop loop's
switching element, not just an isolation crossing" — that argument was
wrong. The CPC1017N was itself driven by MCP23017 #1 GPA6, so it was an
extra stage between the same control bit and the same loop; crossing the
ground boundary was its only real function, and that boundary no longer
exists. GPA6 now drives J1's loop directly through the item-12 1 kΩ series
resistor (§5). Item numbers are kept stable rather than renumbered, per this
document's existing convention.

**Rough total: ~$100–113** (down from the original ~$95–110 baseline: the
isolator/4N35/optoMOS removals save ~$8.64 in parts, the 2x UDA1334A modules
add ~$14.00), excluding shipping, tools
(screwdriver, DMM, soldering iron — assumed already on the bench), and the
remaining placeholder lines (#9's connector, and #18's transformer
suitability, now **blocking** — see §9) which could each move the total by
$5–20 once pinned down. This is a bench-fixture BOM for **one** unit; double
everything except the shared bench tools if a second fixture is ever
wanted.

---

## 2. Digital isolator — removed (item 3)

**Item 3 (two TI ISO7740DWR digital isolators) is removed as of 2026-08-23.**
`docs/DESIGN_NOTES.md` §3.5 explains the decision: the fixture's ground is
commoned with the DUT's, so SPI bus B and the safety-side `DRDY_SAFETY`/
`FAULT_SAFETY` pair now wire as direct Pico GPIO, identical to SPI bus A.
This section previously worked out a two-part 3/3 direction split to solve a
fixed-split problem in TI's isolator family; that derivation no longer
applies to any part in this BOM and is not reproduced here — see
`docs/DESIGN_NOTES.md` §14 if the sizing reasoning is needed again.

---

## 3. CT coupling transformer sizing (item 4)

> **Implemented 2026-08-23 (item 18): CT waveform generation moved from
> PWM+RC to 2x UDA1334A I2S DAC modules at 16 kHz, driving this transformer
> directly with no amplifier.** `src/drivers/ct_wave_pwm.{c,h}` is deleted;
> `src/drivers/ct_wave_i2s.{c,h}` + `src/sim/ct_i2s_gen.{c,h}` are the new
> path — see `DESIGN_NOTES.md` §3.3. **Not yet a working CT output:**
> `ct_wave_i2s_out`'s PIO program cannot fit in either PIO block's free
> program memory on the current build (`docs/HARDWARE.md` §1b.7), so the
> fixture halts at boot rather than running silently CT-less; `PLAN.md`
> records the open resolutions. The ratio math below (§3.1–3.3, still 1:1)
> is unaffected — the DAC's output level (≈990 mVrms at 3.3 V supply, NXP
> UDA1334ATS datasheet §14.1) is close enough to the PWM path's ≈1.06 Vrms
> estimate that the same 1:1 conclusion holds, now on a measured datasheet
> spec instead of an estimated duty-cycle-derating figure. **What direct DAC
> drive does change is what the transformer's primary has to present at
> 60 Hz — see §9 item 1, BLOCKING: the TY-300P below is not confirmed
> suitable under this new requirement.**

### 3.1 Target: the board's full-scale sense-input voltage, not a CT's rated current

**Revised 2026-08-23: the ratio target is the board's own full-scale sense
input, not any particular CT's rated current.** Amps-per-volt is a property
of whichever CT the user installs, not of this board and not of this
fixture — the board itself has no opinion on amps. What the board *does*
have an opinion on is voltage: **its sense input is full-scale at ≈1 Vrms**,
and that is the number the fixture has to reach. An earlier pass (§13 of
`docs/DESIGN_NOTES.md`) instead backed out a specific CT's rated current (a
1 V/30 A part) from the ADC's clamp voltage and landed on ~3:1 — that was
reasoning from the wrong target: it assumed a CT the fixture has no business
assuming, and it never asked whether reaching the clamp was a requirement,
only whether it was reachable. The board expects a **self-burdened,
voltage-output CT** — `R72`/`R78`/`R84` (the burden resistors) are DNP on
all three channels, which is a genuine board property, independent of which
CT ends up installed (`SaftyFW/docs/CURRENT_SENSE.md` §2). An SCT-013-030
(1 V at 30 A) is one compatible example, not the assumed or required part.

### 3.2 What the Pico can actually drive into the primary (superseded — kept for the historical PWM-era estimate this section's conclusion cross-checks against)

**The PWM path this subsection describes is deleted (`src/drivers/ct_wave_pwm.{c,h}`,
2026-08-23) — see item 18's blockquote above for the current UDA1334A figure
this subsection's own §3.3 conclusion now relies on instead.** Kept only
because §3.3 explicitly cross-checks the new measured figure against this
old estimate and finds them close enough that the 1:1 ratio conclusion is
unaffected; this is not a description of anything the firmware still does.

Historically: three GPIOs ran ~244 kHz PWM, duty-modulated by a 60 Hz sine
table, into a 2-pole RC low-pass (corner ~1–2 kHz). The RC output was a
**unipolar** sine riding on a ~1.65 V DC bias (half of the 3.3 V logic rail),
with a theoretical maximum swing of ±1.65 V at a 100 % modulation index. Real
duty-cycle firmware always derates below the rails to avoid distortion near
0 %/100 % duty — **this document assumed a practical usable amplitude of
1.5 V peak** (≈91 % of the theoretical max), which was an estimate, not a
measured or firmware-confirmed number. The amplitude-to-duty
*mapping mechanism* (`src/sim/ct_calibration.{c,h}`) applies a real
per-channel `gain`/`offset` linear fit; the compiled-in default table is
still all-uncalibrated, so today's observed behavior is identity
(`pwm_scale = clamp(amps, 0, 1)`) until a bench calibration run populates
real constants.

A series DC-blocking capacitor (sized for a corner well below 60 Hz — e.g.
1 µF into a primary of a few kΩ gives a corner around 20–30 Hz, more than a
decade below 60 Hz) removes the 1.65 V bias before the transformer primary,
leaving:

```
V_pri,pk ≈ 1.5 V   →   V_pri,rms ≈ 1.5 / √2 ≈ 1.06 V
```

### 3.3 Ratio: 1:1

```
Board sense input full scale  ≈ 1 Vrms   (a board property, CT-independent, CURRENT_SENSE.md §2)
V_pri,rms                     ≈ 1.06 V   (from §3.2)
n = 1 (unity)  →  V_sec,rms ≈ 1.06 V, covering the board's full 0–1 Vrms range with margin
```

That is the whole justification for 1:1 — it produces the voltage the board
wants, with no CT part number entering the derivation.

**At 1:1 the fixture can reach the board's 1 Vrms full-scale input but not
meaningfully past it** — `V_sec,rms` tops out around 1.06 V, well short of
the ≈3.26 V rms clamp boundary. That is an accepted tradeoff, stated
narrowly and without assuming a CT: the ADC clips at a certain input
*voltage*, and what *current* that corresponds to depends entirely on
whichever CT is installed — this fixture doesn't know that number and
shouldn't guess it. `CURRENT_FLAG_CLIPPED` is not exercisable by this
fixture at 1:1.

**Where the CT's amps-per-volt actually lives:** the per-channel calibration
table (`src/sim/ct_calibration.h`) — `ct_cal_apply()` is
`clamp(gain[ch] * amps + offset[ch], 0, 1)`, with no CT part hardcoded
anywhere, and every channel ships uncalibrated (identity) until a real bench
sweep against whichever CT is actually installed populates real
`gain`/`offset` constants. That binding is per-install by construction — it
is the answer to "how does the fixture handle a different CT," and it does
not belong in the transformer ratio.

**Candidate part unchanged: Triad Magnetics TY-300P** (audio/signal
line-matching transformer, Mouser 553-TY300P, $7.24 ea, 338 in stock),
usable as a 1:1 coupling transformer even though it was originally sourced
for a step-up role — see §9: **its exact turns/impedance ratio could not be
confirmed in this pass** (datasheet fetch timed out twice; Mouser's
product-search API doesn't expose turns ratio as a structured field).
Confirm from the linked datasheet (`mouser.com/datasheet/3/236/1/TY_300P.pdf`)
before ordering; any comparable 1:1 audio/isolation transformer works if
sourcing changes.

### 3.4 Confidence level

**Higher than under the 3:1 plan, and less sensitive to the open estimate.**
At 3:1, the medium-confidence 1.5 V peak primary-drive estimate directly
gated whether the clamp boundary was reachable at all. At 1:1 it no longer
does — 1.06 V rms clears the board's 1 Vrms full-scale target with margin
even if the real usable drive comes in noticeably below 1.5 V peak.

- **High confidence:** the 0.715 gain and the board's ≈1 Vrms full-scale
  sense input — read directly from `docs/CURRENT_SENSE.md`'s own traced
  schematic values, cross-checked against a working LTspice model
  (`ltspice/currentMon.asc`). Both are board properties, independent of
  which CT is installed.
- **Medium confidence:** the 1.5 V peak primary-drive assumption — a
  reasonable derating guess, not a measured or firmware-confirmed number,
  but the fixture's basic correctness no longer hinges tightly on it.
- **Low confidence / genuinely open:** the transformer's actual turns ratio
  and how its primary impedance interacts with the RC filter's corner and
  the DC-blocking cap — none of this is verifiable without either the
  part's datasheet or a bench measurement once hardware exists.

---

## 4. Relay-sense wetting circuit (item 12) — all five relays direct, including K4

`docs/HARDWARE.md` §3.4/§5 leaves this "not sized." The cheap, correct
approach given what's already in the design:

- **MCP23017 has internal 100 kΩ pull-ups** (the `GPPU` register), no
  internal pull-downs. Wire each sense input so the relay's NO/COM contact,
  when **closed**, pulls the expander pin straight to `GND_Main` (through a
  small series protection resistor, 1 kΩ, to limit fault current if the pin
  is ever accidentally driven), and enable that pin's internal pull-up in
  firmware (`i2c_owner.c` — confirm `GPPU` is actually set for these 5 pins;
  this document does not touch firmware). Open contact reads high via the
  internal pull-up; closed contact reads low. **No dedicated wetting supply
  or external pull-up resistor needed** — this reuses a feature the part
  already has, at the cost of 5×1 kΩ series resistors (a few cents).
- **K4 (contact in `GND_Safty`) wires identically, since 2026-08-23** —
  no optocoupler. `DESIGN_NOTES.md` §3.5: the fixture's ground is commoned
  with the DUT's, so K4's sense signal no longer needs to cross a domain
  boundary. It uses the same 1 kΩ series resistor into the same MCP23017
  pull-up as K1/K2/K3/K5, straight to fixture ground. The Vishay 4N35
  optocoupler this used to require is removed.

---

## 5. E-stop direct drive (item 6 removed 2026-08-23 — see item 12)

**Item 6 (Littelfuse/IXYS CPC1017N E-stop optoMOS) is removed as of
2026-08-23.** It was kept through the ground-commoning pass on the argument
that it was "the E-stop loop's switching element, not just an isolation
crossing" — that argument was wrong: the CPC1017N was itself driven by
MCP23017 #1 GPA6, so it was an extra stage between the same control bit and
the same loop, and crossing the ground boundary was its only real function
(`DESIGN_NOTES.md` §3.4/§3.5/§14). That boundary no longer exists.

**Replacement: GPA6 drives J1's loop directly**, through the same 1 kΩ
series protection resistor every other fixture signal already gets (item 12
above) — no switching element in between.

`docs/HARDWARE.md` §3.6 / `SaftyFW/docs/HARDWARE.md` §5: J1's loop is
GPIO9 with a 1 kΩ pull-up (R10) to `3.3v_Safty` and a 0.01 µF cap (C3).
Because of that pull-up, the fixture cannot simply write a level to GPA6:
driving it high to represent "open" would fight R10 into a different supply
rail. So the control is a **direction toggle**, not a level write:

- **Loop CLOSED (E-stop healthy):** GPA6 configured as OUTPUT, driving LOW.
- **Loop OPEN (E-stop tripped):** GPA6 configured as INPUT, i.e. high-Z,
  letting R10 pull GPIO9 (and this side of the resistor) high.

Boot-time default is INPUT/high-Z (open/STOP) — matching both the
MCP23017's own POR default (`IODIR` resets to all-input) and the board's
fail-safe intent: an unpowered or un-initialised fixture must read STOP,
never a falsely-healthy closed loop.

Loop current when closed, against the same 1 kΩ series resistor:

```
I_loop ≈ 3.3 V / 1 kΩ ≈ 3.3 mA
```

well within the MCP23017's per-pin output drive capability, with the same
comfortable margin the CPC1017N used to have. No new part needed beyond the
1 kΩ series resistor already counted under item 12.

---

## 6. DUT power relay — one vs. two (item 7, DESIGN_NOTES.md open question 5)

`docs/HARDWARE.md` §0 item 6 already flags this as unresolved: the board has
**two independent 12 V inputs**, J18 (main) and J19 (safety), each with its
own SMAJ24CA TVS, feeding electrically separate downstream regulators —
confirmed directly from the schematic: the two domains' bulk input
capacitors live on different sheets (`C9`/`C10`, 470 µF each, on
`/5V Regulator/`; `C53`/`C61`, 470 µF each, on `/SaftyRegulator/`), so there
is no shared copper between the two 12 V rails anywhere downstream of the
connectors.

**Recommendation: two relays, not one.** Originally justified because a
single relay switching a single 12 V feed that the bench operator then
splits to both J18 and J19 would bond the two 12 V returns together,
undermining the ground-domain separation the rest of the fixture used to
preserve. **That rationale is superseded as of 2026-08-23**
(`DESIGN_NOTES.md` §3.5): the fixture's ground is now commoned with the
DUT's anyway, so a single shared relay would no longer be uniquely
problematic on isolation grounds.

**The two relays are kept regardless**, because they earn their keep
independently: each fed from its own bench-supply channel (or a dual-output
bench supply) into its own connector (J18, J19), they let test scenarios
brown out one domain independently of the other — a genuinely useful test
case (`SaftyFW` noticing a main-side power loss while its own domain stays
up, and vice versa) that a single shared relay could never produce.

**Firmware note (out of scope for this BOM, flagged for the owning code
change):** `i2c_owner.c` currently exposes one control bit,
`EXP1_PIN_DUT_POWER`. Two relays need two independent MCP23017 output bits.
MCP23017 #1 has spare pins per `docs/HARDWARE.md` §3.7 (6 spare), so there
is room — this just needs a second named pin and a firmware change, not a
part.

**Firmware closed (2026-08-20):** done — `EXP1_PIN_DUT_POWER_MAIN` (exp1 pin
7, renamed from `EXP1_PIN_DUT_POWER`) and `EXP1_PIN_DUT_POWER_SAFETY` (exp1
pin 10, new) are both independently commanded in `i2c_owner.c`, with the
legacy single-relay protocol command kept as a main-domain-only alias, not
redefined to gang both — see `docs/HARDWARE.md` §3.7 and `docs/PROTOCOL.md`
§5.5. Spare count on exp1 is now 5, not 6.

### Inrush sizing

Per-domain bulk capacitance at the regulator input: **940 µF** (2×470 µF,
Nichicon UCM1H471MNJ1MS, 50 V-rated aluminum electrolytic — confirmed via
`get_kicad_component` on C9/C10 and C53/C61). Worst-case capacitor-charging
peak current model:

```
I_pk ≈ V / (R_source + ESR_total)
```

With an assumed low total path resistance (bench-supply source impedance +
wiring + ~0.05–0.15 Ω combined ESR for two 470 µF electrolytics in
parallel) of roughly 0.2 Ω:

```
I_pk ≈ 12 V / 0.2 Ω ≈ 60 A,  decaying with τ = R·C ≈ 0.2 Ω × 940 µF ≈ 188 µs
(< 5 % of peak within ~3τ ≈ 560 µs)
```

This is a genuine estimate with an assumed, not measured, source
resistance — **flagged in §9 as needing a real scope/current-probe capture**
once hardware exists (DESIGN_NOTES.md open question 5 says exactly this: "inrush
rating vs the board's actual inrush not measured"). Two mitigating factors
worth noting even before that measurement: (1) a mechanical relay's
contacts take several milliseconds to fully close, by which time a
sub-millisecond RC transient has already decayed 3–4 orders of magnitude,
so the contacts are not carrying the raw peak for their full duration; (2)
the same **Omron G5LE-14-DC12** already used elsewhere on this board for
K1–K3/K5 (10 A/250 VAC continuous contact rating, cheap, proven, in stock)
is a reasonable choice here too, but its exact cold-inrush/make-current
rating should be checked against the bench-measured number before relying
on it for repeated `power cycle` scenario testing. If welding is ever
observed at bring-up, the cheap fix is a small series NTC inrush limiter or
a bleed resistor pre-charging the bulk caps before the relay closes — not
addressed further here since it's a response to a measurement that hasn't
been taken yet.

---

## 7. MCP23017 ×2 (item 2)

Already sized in code (`docs/HARDWARE.md` §3.7): 0x20 (fixed-role pins),
0x21 (spare). Straightforward sourcing, no open questions. Add standard
0.1 µF decoupling per part (already covered under item 14's passives lot)
and I2C pull-ups (`i2c_owner.c` header comment is the authority on whether
it expects internal Pico pull-ups or external ones — not re-derived here).

---

## 8. Connectors and cabling (items 8–11, 13)

| Main-board connector | Fixture-side part | Notes |
|---|---|---|
| **J6** (1×20 socket, main TC bus) | 1×20 single-row male header, 2.54 mm, cut from a Würth 61300411121 breakaway strip | **REVERSE PIN ORDER** — `docs/HARDWARE.md` §3.1 has the full mapping (`J6 pin = 21 − fixture plug pin`). Wire the fixture's header exactly as the real daughterboard's J5 would be, not as J6's own pin numbering reads. This is the single easiest way to damage something at bring-up; re-read §3.1's worked example before wiring. |
| **J7** (2×6, 1.27 mm, safety TC bus) | **Not selected — see §9** | Gender/pitch not confirmed against the physical board in any doc read for this task. Do not guess and order; confirm with calipers/visual inspection first. |
| **J13/J15/J17** (3.5 mm CT jacks) | Fixture-side 3.5 mm mono jack (Same Sky/CUI SJ-3523-SMT-TR ×3) wired to each transformer secondary, connected to the board via standard 3.5 mm mono male-male patch cables ×3 | Simpler and cheaper than sourcing a bare 3.5 mm plug-to-wire part; commodity cables are everywhere. |
| **J3/J4/J8/J10/J11** (relay sense terminal blocks) | **None needed — direct wire.** Mouser lists both J1's part (Phoenix 1935161) and J10's part (Phoenix 1935174) under "Fixed Terminal Blocks", not "Pluggable Terminal Blocks" | If this classification is right, these are captive screw terminals soldered straight to the board — the fixture connects with bare stripped/ferruled wire under the screws, no mating connector to buy. **Confirm visually at bring-up** (this document's datasheet fetch for both parts timed out twice and could not verify pluggability directly — see §9). |
| **J1** (E-stop) | Same as above — direct wire into GPA6's 1 kΩ series resistor (§5; no CPC1017N any more) | Phoenix 1935161, same caveat |
| **J20** (spare I/O, 2-pin) | Not specified in any source doc read; treat as simple 2-conductor connection, generic 2.54 mm header/wire | Low risk either way — only 2 spare I/O lines |
| Debug UART / SWD | **Reuse the existing Raspberry Pi Debug Probe and cables from the `SaftyFW` bench setup** | No new purchase; same bench pattern `docs/HARDWARE.md` §1 footnote already calls out |

---

## 9. Still needs measurement/confirmation before ordering

Four items, in order of how much they block:

1. **RESOLVED 2026-08-24 — replacement selected: Hammond 140QEX (Mouser
   546-140QEX).** 1:1 turns ratio (600 Ω CT : 600 Ω CT), frequency response
   rated 20 Hz – 20 kHz (±1 dB reference), and — read directly off Hammond's
   own datasheet (`hammfg.com/files/parts/pdf/140QEX.pdf`, "Inductance @
   1.0 kHz, 1.0 V OC": Primary **10.62 H**; "Impedance @ 1.0 kHz, 1.0 V OC":
   Primary **64.5 kΩ**) — a primary inductance well above the 8 H floor
   derived below. $100.18 ea (1-off), $79.76 @ 100, **42 in stock** at Mouser
   as of 2026-08-24. Drive-budget check against the UDA1334A: at 60 Hz the
   quoted 10.62 H gives Z ≈ 2π·60·10.62 ≈ 4.0 kΩ (computed from the quoted
   inductance, not itself a datasheet line) — above the 3 kΩ RL_min below,
   drawing ≈990 mV / 4.0 kΩ ≈ 247 µA, comfortably under the DAC's
   Io(max) = 1.6 mA. The series DC-blocking cap ahead of the primary (item 4
   below) is unaffected by this choice and is still required.

   **Two caveats to settle before ordering, neither of them a reason not to
   order — but both are the kind of thing that is cheaper to notice now than
   after the part is on the bench.**

   *The inductance figure is quoted at 1 kHz, and is being used at 60 Hz.*
   Core permeability, and therefore primary inductance, is both frequency-
   and level-dependent in a laminated-core transformer, so 10.62 H @ 1 kHz /
   1.0 V is not a promise of 10.62 H @ 60 Hz / ~1 V. The 20 Hz end of the
   quoted response range is decent circumstantial evidence the part does not
   fall apart at 60 Hz, but the ≥8 H acceptance test was written to be
   answered *at the fundamental*, and this answers it next door. Alternate 1
   (Triad HS-56) is the part that answers it directly — its datasheet quotes
   inductance **at 60 Hz**. That is the whole difference between the two, and
   it is why HS-56 stays listed rather than being dropped on cost alone.

   *Verification status, checked 2026-08-24.* Hammond's own PDF could not be
   read from this environment — hammfg.com returns 403 to a plain fetch, and
   the copy that does download yields no extractable text. Distributor spec
   tables corroborate most of the acceptance test independently:

   | Figure | Independently corroborated? |
   |---|---|
   | 1:1 ratio, 600 Ω CT | Yes — Digi-Key, Newark, RS |
   | 20 Hz – 20 kHz, ±1 dB | Yes — Digi-Key |
   | Level rating 10 dB, insertion loss 1.1 dB typ, primary DCR 72.4 Ω | Yes — Digi-Key |
   | **Primary inductance 10.62 H** | **No.** Digi-Key's spec table omits primary inductance entirely; the figure traces back to Hammond's own datasheet and nowhere else reachable from here. |

   So the one figure the ≥8 H acceptance test actually turns on is the one
   still unconfirmed. Read it off the PDF before ordering. The level rating
   is a useful independent sanity check in the meantime: 10 dB into 600 Ω is
   ≈2.45 Vrms, comfortably above the ~990 mVrms this fixture drives.

   Two ranked alternates, both checked against the same acceptance test:
   - **Alternate 1 — Triad Magnetics HS-56 (Mouser 553-HS-56).** Strongest
     possible datasheet match: "Inductance, 5V @ 60Hz 1-6 (short 3 & 4):
     **21–49 H**" is measured *at the fixture's actual fundamental*, not
     inferred from a 1 kHz figure. 1:1 ratio, 10 Hz – 30 kHz range, CMRR
     104 dB @ 60 Hz. Not the pick only on cost: $389.12 ea, ~4x the 140QEX;
     10 in stock. Use this if 140QEX goes out of stock or the extra margin
     matters more than price.
   - **Alternate 2 — Hammond 1140-LN-B (Mouser 546-1140-LN-B), flagged
     near-miss, does not cleanly pass the test.** 1:1 ratio, frequency
     response `-0.03 dB @ 20 Hz` / `+0.09 dB @ 20 kHz` (flat well past
     60 Hz), and a 60 Hz CMRR spec (70 dB) proving the part is characterised
     there — but the datasheet states **Primary Input Impedance only @ 1 kHz
     (12 kΩ)**, never in Henries and never at 60 Hz. Extrapolating 12 kΩ
     @ 1 kHz down to 60 Hz as a simple inductor would predict only ≈720 Ω
     (below the 3 kΩ floor) while the 20 Hz flatness argues the real
     low-frequency impedance is much higher than that naive scaling — the
     two readings of the same datasheet disagree, which is exactly the kind
     of guess this task's own acceptance rule forbids taking on faith. $96.43
     ea, only 5 in stock. Do not order this one as a substitute for 140QEX
     without first getting Hammond to confirm an actual 60 Hz (or Henries)
     figure.

   **Rejected, for the record (do not re-evaluate):**
   - **Triad TY-300P** — eliminated 2026-08-24, recorded below; kept as the
     precedent for how this item's elimination log works.
   - **Triad N-67A** (115 V:115 V, 150 VA power isolation transformer,
     `50/60Hz` explicitly stated, 1:1 in the sense of a mains isolation
     winding) — its own datasheet (`catalog.triadmagnetics.com/asset/
     n-67a.pdf`) states no primary inductance, no primary impedance, and no
     magnetizing/no-load current at any frequency. Per this item's own
     acceptance rule, an omitted low-frequency figure fails the test the
     same way TY-300P's out-of-band figure did — there is nothing to check
     it against. (Also a 7 lb / 150 VA mains part, a poor physical fit for
     driving a ~1 mA-class DAC output, though that alone would not have been
     disqualifying.)
   - **The entire "telecom 600:600 Ω line-matching" family** — Triad TY-145P/
     TY-146P/TY-250P/TY-306P, Bourns LM-NP-1001-B1 (200 Hz – 3.5 kHz per its
     own datasheet), Tamura TTC-294 (300 Hz – 3.5 kHz) — all rejected on
     sight for the same reason as TY-300P: these parts are voice-band
     transformers *deliberately* rolled off below ~200–300 Hz to reject
     60 Hz mains hum on phone lines, so 60 Hz sits below their rated band by
     construction, not by omission. This is a family-level disqualification,
     not a per-part one — no other member of this family is worth
     re-checking for this role.

   Superseded text below (kept for the elimination record the 2026-08-24
   check produced): the UDA1334A datasheet (§13) specifies RL min = 3 kΩ at
   (THD+N)/S < 0.1% and Io(max) = 1.6 mA. For the transformer primary to
   present ≥3 kΩ at 60 Hz (the bottom of the CT waveform's fundamental),
   its primary inductance must be:

   ```
   L ≥ RL_min / (2·π·60 Hz) = 3000 / 377 ≈ 8 H
   ```

   Typical 600:600 Ω audio/telecom **isolation** transformers — the TY-300P's
   class — run **1–3 H**, i.e. only **≈380–1130 Ω at 60 Hz**.

   **This is no longer an inference from the part class. The TY-300P's own
   datasheet disqualifies it** (Triad Magnetics, publish date 2019-05-31,
   saved at `hardware/datasheets/SimFW_TY300P/TY-300P.pdf`, checked
   2026-08-24):

   | Datasheet line | Verbatim | Why it decides the question |
   |---|---|---|
   | Frequency Range | `300 to 3500 Hz` | 60 Hz is a **factor of 5 below the specified low-frequency limit**. Triad does not characterise this part at the fixture's fundamental at all, so there is no primary-inductance figure to qualify it against — the part is out of specification before the 8 H arithmetic above is even applied. |
   | Max. DC Current | `Pri 0 mA`, `Sec 0.80 mA` | **Zero** rated DC on the primary. A directly DAC- or PWM-driven winding carries the driver's DC offset unless it is explicitly blocked, so this role would need a series DC-blocking capacitor — which forms yet another high-pass with the primary, at the same 60 Hz that is already the problem. |
   | Power Level | `-45 dBm to +7 dBm` | +7 dBm into 600 Ω is ≈1.73 Vrms, so the *level* was never the constraint — it comfortably covers the board's ≈1 Vrms full-scale sense input. Confirms the amplitude budget in §3 was sound and isolates the failure to frequency response, not headroom. |
   | Impedance | `600 (4W)` pri, `600/600` sec | **Turns ratio confirmed 1:1** per secondary (two independent 600 Ω secondaries, SEC 1 and SEC 2), which is what §3 assumed. Distributor listings quoting "1:2" are describing the two secondaries in **series**, not a different part. Note also that `4W` here means **4-wire**, not 4 watts — several distributor pages render it as a power rating. |

   So the open question splits cleanly, and only one half survives: the
   **turns ratio is settled and was correct**; the **60 Hz suitability is
   settled and was wrong**. Nothing about this needs a bench measurement —
   it is a datasheet fact. (The replacement search this used to call out as
   "the only remaining work" is done — see the resolution above.) That is well
   under both the 3 kΩ minimum and the DAC's 1.6 mA output-current limit,
   and it shunts most of the 60 Hz signal to ground through the primary
   before it ever reaches the transformer's mutual inductance. **A series
   resistor does not fix this — it only makes the voltage divider worse.**
   The turns ratio (1:1, confirmed correct in §3) was never the constraint
   that matters here; **primary inductance at 60 Hz is.** The right class of
   part is a small **50/60 Hz-rated 1:1 or 6V:6V power/audio transformer**
   (not an audio-signal-line isolation transformer) — its poor high-frequency
   response is irrelevant, since the fixture's waveform of interest tops out
   a few hundred Hz above the 60 Hz fundamental, and at ≈1 V drive it sits
   nowhere near core saturation. **Do not order the TY-300P (or any similar
   600:600 audio isolation transformer) for this role** — that is now a
   closed question, not a caution. The acceptance test for a candidate was a
   datasheet one, runnable without any hardware: it must be **specified at
   50/60 Hz** (not merely un-disqualified there) and either state a primary
   inductance ≥ 8 H or state a primary impedance at 60 Hz ≥ 3 kΩ. A part
   whose datasheet simply omits the low-frequency end fails this test the
   same way the TY-300P does; absence of a limit is not a rating. **Ordering
   is unblocked** — see the resolution and the ranked alternates above.
2. **J7 mating connector (§8).** Gender and exact pitch (1.27 mm is stated
   in `docs/HARDWARE.md`, but not confirmed against the physical board any
   more rigorously than that document's own provisional status already
   admits) are unconfirmed. Ordering the wrong gender wastes the part and
   the lead time. Check with calipers/visual inspection first.
3. **DUT-power inrush (§6).** The 60 A/~190 µs peak-current estimate rests
   on an assumed ~0.2 Ω total source+ESR resistance, not a measurement.
   Once any bench supply and the real board are available, a scope/current-
   probe capture at first power-on will pin this down properly; the G5LE-14
   recommendation should be revisited if that capture shows something
   uglier than the estimate.
4. **DAC output DC bias (§3, item 18) — lower risk, same series as the
   above.** UDA1334A Vref(DAC) = 0.5×VDD, ≈1.65 V DC bias on each output — a
   series DC-blocking capacitor ahead of the transformer primary is required
   (already counted under item 14) or the core saturates against DC and the
   output stage is swamped. Not blocking in the sense of needing a part
   decision, just a reminder this cap is mandatory, not optional.

A fifth, lower-stakes one: **J3/J4/J8/J10/J11/J1's "fixed vs. pluggable"
classification (§8)** — Mouser's category field says "Fixed Terminal
Blocks" for both Phoenix part numbers involved, which is why this BOM
recommends direct-wire rather than a mating connector, but this document
could not fetch either part's datasheet to verify that reading directly (two
timeouts). Low cost either way to get wrong — if it turns out to be
pluggable, the fix is buying a $1 mating plug per connector, not a redesign.

---

## 10. Cross-reference

- `docs/HARDWARE.md` §0/§3/§5 — the pin map, connector mating tables, and
  the external-components list this BOM fills in.
- `docs/DESIGN_NOTES.md` §3.3–3.7 and `docs/PLAN.md` §11 — design rationale
  and the open questions this BOM resolves or narrows (items 2 and 5 in particular).
- `firmware/KilnFW/docs/HARDWARE.md` — J6/J20/relay terminal block/Power
  section facts cited above.
- `firmware/SaftyFW/docs/HARDWARE.md`, `firmware/SaftyFW/docs/
  CURRENT_SENSE.md` — J7/K4/J10/E-stop/CT-jack/AD8542 facts cited above;
  `CURRENT_SENSE.md` §2 is the direct source for §3's transfer-function math.
- Mouser lookups in this document were performed live on 2026-08-20 via
  `mcp__kicad__lookup_mouser_part`; stock/pricing figures are a snapshot
  from that date and will drift.
