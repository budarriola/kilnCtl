# SimFW — Bench Fixture Bill of Materials

> **Status:** first pass, not yet ordered · **Last reviewed:** 2026-08-20
> **Purpose:** remove the literal blocker — you cannot build the bench
> fixture until parts are ordered, and until this document existed there was
> no orderable list. Every line below traces to a specific source: a
> schematic value read via the KiCad MCP tools, a figure already computed in
> `docs/CURRENT_SENSE.md`/`docs/HARDWARE.md`, or a live Mouser lookup done
> while writing this document (2026-08-20).
>
> **This is a first-pass BOM, not a purchase order.** Three items are
> explicitly flagged "needs bench measurement/confirmation before ordering"
> in section 9 — do not one-click-buy the whole table without reading that
> section first. Everything else is ready to order as written.

---

## 1. Summary table

| # | Item | Qty | MPN (Mouser #) | Unit price | Line total | Status |
|---|---|---|---|---|---|---|
| 1 | Raspberry Pi Pico (RP2040) | 1 | Raspberry Pi **SC0915** (358-SC0915) | $4.00 | $4.00 | In stock 3915, confirmed live |
| 2 | I/O expander, MCP23017 | 2 | Microchip **MCP23017-E/SO** (579-MCP23017-E/SO) | $1.62 | $3.24 | In stock 39,748 |
| 3 | Digital isolator, quad, unidirectional | 2 | TI **ISO7740DWR** (595-ISO7740DWR) | $3.56 | $7.12 | In stock 104 — see §2 sizing |
| 4 | CT coupling transformer | 3 | Triad **TY-300P** (553-TY300P) — **ratio unconfirmed, see §9** | $7.24 | $21.72 | In stock 338, ratio needs datasheet confirm |
| 5 | K4 relay-sense optocoupler | 1 | Vishay **4N35** (78-4N35) | $0.66 | $0.66 | In stock 6,150 |
| 6 | E-stop optoMOS (SSR) | 1 | Littelfuse/IXYS **CPC1017N** (849-CPC1017N) | $0.86 | $0.86 | In stock 273,541 |
| 7 | DUT power relay | 2 | Omron **G5LE-14-DC12** (653-G5LE-14-DC12) | $1.61 | $3.22 | In stock 6,997 — **2, not 1, see §6** |
| 8 | J6 mating header (main-side TC bus) | 2 strips | Würth **61300411121** 1×40 breakaway (710-61300411121) | $0.19 | $0.38 | In stock 69,754; cut to 1×20 |
| 9 | J7 mating connector (safety-side TC bus, 2×6 @ 1.27 mm) | TBD | **Not selected — gender/pitch unconfirmed, see §9** | ~$1–3 | ~$3–9 (est.) | Placeholder, do not order yet |
| 10 | CT jack, fixture-side (mates J13/J15/J17 via patch cable) | 3 | Same Sky/CUI **SJ-3523-SMT-TR** (490-SJ-3523-SMT-TR) | $0.88 | $2.64 | In stock 32,954 |
| 11 | 3.5 mm mono male-male patch cable | 3 | Generic/commodity — no Mouser electronics MPN needed | ~$3.00 | ~$9.00 (est.) | Buy locally/Amazon; any shielded mono cable works |
| 12 | Relay-sense wetting resistors | 5 | Generic 1 kΩ 0805/THT (already stocked from main-board BOM class) | ~$0.02 | ~$0.10 | See §4 — internal MCP23017 pull-ups do most of the work |
| 13 | Hookup wire + ferrules, for J1/J3/J4/J8/J10/J11 (fixed screw terminals) | 1 lot | Generic 22–24 AWG stranded + ferrule kit | — | ~$10 (est.) | See §8 — these connectors are **fixed**, not pluggable |
| 14 | RC filter / DC-block / decoupling passives (3× CT channels + isolators + expanders) | 1 lot | Generic R/C, values in §3 | — | ~$10 (est.) | Commodity, mostly already-stocked values from main-board BOM |
| 15 | Protoboard/perfboard carrier + sockets for Pico/ICs | 1 | Generic 100×160 mm double-sided perfboard + 2×20 female header (socketed Pico) | — | ~$10 (est.) | Generic |
| 16 | Debug UART / SWD to Debug Probe | 0 | **Reuse existing Raspberry Pi Debug Probe + cables from the `SaftyFW` bench setup** | $0 | $0 | No new purchase |
| 17 | PCA9685 (optional PWM/LED stimulus) | 0 | Not included — PLAN.md marks it optional, not required for the base feature set | — | $0 | Deferred |

**Rough total: ~$95–110**, excluding shipping, tools (screwdriver, DMM,
soldering iron — assumed already on the bench), and the two placeholder
lines (#4's ratio and #9's connector) which could each move the total by
$5–20 once pinned down. This is a bench-fixture BOM for **one** unit; double
everything except the shared bench tools if a second fixture is ever wanted.

---

## 2. Digital isolator — direction-split verification (item 3)

`docs/HARDWARE.md` §5 flags this exact question: *"some 6-channel parts are
fixed at a different forward/reverse split (e.g. 4/2) and would not fit this
bus without re-routing a channel."* Verified against §3.2's own pin table:

| Direction | Signals | Count |
|---|---|---|
| Board (safety Pico, master) → fixture | `CLK`, `MOSI`, `CS0` | 3 |
| Fixture → board | `MISO`, `DRDY_SAFETY`, `FAULT_SAFETY` | 3 |

**Exactly 3/3 across 6 channels.** No common multi-channel reinforced
isolator ships as a fixed 3-forward/3-reverse 6-channel part — TI's family
tops out at 4 channels per package with fixed splits of **4/0** (ISO7740),
**3/1** (ISO7741), or **2/2** (ISO7742); none of those alone gives 3/3, and
there is no 6-channel sibling to check instead.

**Resolution: two TI ISO7740DWR (quad, all-4-channels-same-direction,
SOIC-16),** one wired for the 3 board→fixture signals (1 spare channel), one
wired for the 3 fixture→board signals (1 spare channel). This sidesteps the
fixed-split problem entirely — each package only ever carries one direction,
so there is no split to get wrong — at the cost of 2 spare channels going
unused (available for a future safety-side signal if one is ever added).
Confirmed in stock (104 units, Mouser 595-ISO7740DWR, $3.56 ea qty 1 / $2.68
ea qty 10).

Both are powered from the isolated (safety) side off J7's `3.3v_Safty` per
`docs/HARDWARE.md` §4 — subject to the J7-pin-1 contradiction already flagged
in that document (§0 item 5) and unchanged by this BOM.

---

## 3. CT coupling transformer sizing (item 4) — the open question this task most needed to answer

### 3.1 Target, from the board side (already established in `CURRENT_SENSE.md` §2)

Traced from `hardware/mainBoard/output/kiln.pdf` p.4 by a prior pass and
confirmed here by reading `docs/CURRENT_SENSE.md` directly (R43 10k, R46
7.15k, AD8542 U8):

```
V_adc  ≈  0.715 · V̂_ct                      (gain, inverting rectifier R46/R43)
ADC/buffer full scale = 3.3 V ⇒ clamp conducts ~±4 V ⇒ practical ceiling:
  V̂_ct ≈ 4.6 V peak  ⇒  V_ct,rms ≈ 3.26 V
  with a 1 V/30 A CT model:  I_fs ≈ 98 A rms  (the "clipping" boundary)
```

That is the **ceiling** the fixture should be able to reach at least once
(to exercise `CURRENT_FLAG_CLIPPED` — a real, tested code path per
`CURRENT_SENSE.md` §2). It is not the *typical* operating point: a
resistive kiln element at 240 V draws on the order of 10–25 A per zone
(2.4–6 kW), i.e. `V_ct,rms` in the ~0.33–0.83 V range for the same 1 V/30 A
CT model. The transformer has to comfortably cover the typical range and
*reach* the clipping boundary, not necessarily sit there all day.

### 3.2 What the Pico can actually drive into the primary

From `docs/PLAN.md` §3.3: three GPIOs run ~244 kHz PWM, duty-modulated by a
60 Hz sine table, into a 2-pole RC low-pass (corner ~1–2 kHz). The RC output
is a **unipolar** sine riding on a ~1.65 V DC bias (half of the 3.3 V logic
rail), with a theoretical maximum swing of ±1.65 V at a 100 % modulation
index. Real duty-cycle firmware always derates below the rails to avoid
distortion near 0 %/100 % duty — **this document assumes a practical usable
amplitude of 1.5 V peak** (≈91 % of the theoretical max), which is an
estimate, not a measured or firmware-confirmed number. **Correction:** the
amplitude-to-duty *mapping mechanism* (`src/sim/ct_calibration.{c,h}`) is no
longer an unfinished placeholder — it applies a real per-channel
`gain`/`offset` linear fit. What remains unfinished is the *table*: the
compiled-in default is all-uncalibrated, so today's observed behavior is
still identity (`pwm_scale = clamp(amps, 0, 1)`) until a bench calibration
run populates real constants — the real ceiling still depends on whatever
modulation-index cap the uncalibrated path ends up using.

A series DC-blocking capacitor (sized for a corner well below 60 Hz — e.g.
1 µF into a primary of a few kΩ gives a corner around 20–30 Hz, more than a
decade below 60 Hz) removes the 1.65 V bias before the transformer primary,
leaving:

```
V_pri,pk ≈ 1.5 V   →   V_pri,rms ≈ 1.5 / √2 ≈ 1.06 V
```

### 3.3 Required step-up ratio

```
n = V_sec,pk / V_pri,pk = 4.6 V / 1.5 V ≈ 3.07   →   call it 3:1
```

**A 1:1 transformer (what `docs/PLAN.md` §3.3 currently says is "decided")
cannot reach the 98 A clipping boundary** — at 1:1 the best the fixture can
do is `V_ct,rms ≈ 1.06 V`, which on the 1 V/30 A model is only:

```
I_max(1:1) ≈ 1.06 V / (0.715 · √2 · 0.0333 V/A) ≈ 31.5 A rms
```

That is **fine for the typical 10–25 A operating range** (comfortable
margin), but it means the fixture, as currently specced, structurally
cannot drive the safety board into `CURRENT_FLAG_CLIPPED` or test the top
third of the ADC's range. Given the fixture's whole purpose is exercising
edge cases the real kiln shouldn't be pushed into, this is worth fixing
before ordering, not after.

**Recommendation: order a ~3:1 (or better) step-up transformer**, not the
1:1 PLAN.md currently names. Candidate: **Triad Magnetics TY-300P** (audio/
signal line-matching transformer, Mouser 553-TY300P, $7.24 ea, 338 in
stock) — but see §9: **its exact turns/impedance ratio could not be
confirmed in this pass** (its datasheet fetch timed out twice from this
environment; Mouser's product-search API doesn't expose turns ratio as a
structured field). Confirm the ratio from the linked datasheet
(`mouser.com/datasheet/3/236/1/TY_300P.pdf`) before ordering; if it isn't
≥3:1 loaded into the AD8542 stage's input impedance, either pick a
different Triad/Xicon line-matching part with a documented ≥3:1 ratio, or
fall back to a 1:1 transformer plus a simple ×3 non-inverting op-amp gain
stage ahead of it (single-supply rail-to-rail op-amp off the Pico's 3.3 V or
the fixture's 5 V rail, AC-coupled) — more parts, but decouples the gain
question from transformer sourcing entirely.

### 3.4 Confidence level

**Medium, not high.** Split out what's solid from what's assumed:

- **High confidence:** the 0.715 gain, the ~4.6 V/3.26 V clipping figures,
  and the 1V/30A-CT full-scale math — all read directly from
  `docs/CURRENT_SENSE.md`'s own traced schematic values, which that
  document cross-checked against a working LTspice model (`ltspice/
  currentMon.asc`).
- **Medium confidence:** the 1.5 V peak primary-drive assumption — a
  reasonable derating guess, not a measured or firmware-confirmed number.
- **Low confidence / genuinely open:** the transformer's actual turns
  ratio and how its primary impedance interacts with the RC filter's
  corner and the DC-blocking cap — none of this is verifiable without
  either the part's datasheet (fetch failed in this pass) or a bench
  measurement once hardware exists.

---

## 4. Relay-sense wetting circuit (item 12) — K1/K2/K3/K5 direct, K4 via optocoupler

`docs/HARDWARE.md` §3.4/§5 leaves this "not sized." The cheap, correct
approach given what's already in the design:

- **MCP23017 has internal 100 kΩ pull-ups** (the `GPPU` register), no
  internal pull-downs. Wire each of the K1/K2/K3/K5 sense inputs so the
  relay's NO/COM contact, when **closed**, pulls the expander pin straight
  to `GND_Main` (through a small series protection resistor, 1 kΩ, to limit
  fault current if the pin is ever accidentally driven), and enable that
  pin's internal pull-up in firmware (`i2c_owner.c` — confirm `GPPU` is
  actually set for these 5 pins; this document does not touch firmware).
  Open contact reads high via the internal pull-up; closed contact reads
  low. **No dedicated wetting supply or external pull-up resistor needed** —
  this reuses a feature the part already has, at the cost of 5×1 kΩ series
  resistors (a few cents).
- **K4** (contact in `GND_Safty`, per §0 item 6/§3.4): the same idea, but
  through the 4N35 optocoupler's phototransistor stage so the isolation
  boundary is respected. LED side: wetted from a safety-side rail (via J7 —
  subject to the same J7-pin-1 confirmation already flagged) through a 1 kΩ
  series resistor when the K4 contact closes, giving an LED current of a
  few mA (well within the 4N35's rated forward current and CTR range for a
  reliable digital output). Phototransistor collector-emitter sits on the
  `GND_Main` side, pulled up by the MCP23017's internal pull-up exactly like
  the other four channels.

---

## 5. E-stop optoMOS (item 6)

`docs/HARDWARE.md` §3.6 / `SaftyFW/docs/HARDWARE.md` §5: J1's loop is
GPIO9 with a 1 kΩ pull-up to `3.3v_Safty` and a 0.01 µF cap. Loop current
when closed:

```
I_loop ≈ 3.3 V / 1 kΩ ≈ 3.3 mA
```

**CPC1017N** (Littelfuse/IXYS optoMOS, single-pole normally-open) is rated
for continuous load currents well above this (typical single-channel
optoMOS parts in this family handle 100+ mA continuous) — 3.3 mA is a
trivial fraction of its rating, so this part is comfortably sized with
large margin. $0.86 ea, 273,541 in stock — no sourcing risk.

---

## 6. DUT power relay — one vs. two (item 7, PLAN.md open question 5)

`docs/HARDWARE.md` §0 item 6 already flags this as unresolved: the board has
**two independent 12 V inputs**, J18 (main) and J19 (safety), each with its
own SMAJ24CA TVS, feeding electrically separate downstream regulators —
confirmed directly from the schematic: the two domains' bulk input
capacitors live on different sheets (`C9`/`C10`, 470 µF each, on
`/5V Regulator/`; `C53`/`C61`, 470 µF each, on `/SaftyRegulator/`), so there
is no shared copper between the two 12 V rails anywhere downstream of the
connectors.

**Recommendation: two relays, not one.** This isn't just "more thorough" —
a single relay switching a single 12 V feed that the bench operator then
splits to both J18 and J19 downstream of that one relay would **bond the
two 12 V returns together**, which very likely commons `GND_Main` and
`GND_Safty` through the shared supply return path. That directly undermines
the isolation discipline the rest of this fixture (transformers, opto­
couplers, digital isolators) exists to preserve, and it's exactly the kind
of thing bring-up step 5's ground-continuity check (`docs/HARDWARE.md` §6)
is there to catch — better to not build the mistake in the first place.

Two independent relays, each fed from its own bench-supply channel (or a
dual-output bench supply) into its own connector (J18, J19), preserve the
isolation and additionally let test scenarios brown out one domain
independently of the other — a genuinely useful test case (`SaftyFW`
noticing a main-side power loss while its own domain stays up, and vice
versa) that a single shared relay could never produce.

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
once hardware exists (PLAN.md open question 5 says exactly this: "inrush
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
| **J1** (E-stop) | Same as above — direct wire into the CPC1017N's output leads | Phoenix 1935161, same caveat |
| **J20** (spare I/O, 2-pin) | Not specified in any source doc read; treat as simple 2-conductor connection, generic 2.54 mm header/wire | Low risk either way — only 2 spare I/O lines |
| Debug UART / SWD | **Reuse the existing Raspberry Pi Debug Probe and cables from the `SaftyFW` bench setup** | No new purchase; same bench pattern `docs/HARDWARE.md` §1 footnote already calls out |

---

## 9. Still needs measurement/confirmation before ordering

Three items, in order of how much they block:

1. **CT transformer exact ratio (§3).** The arithmetic here shows a ~3:1
   step-up is needed to reach the ADC's clipping boundary, and that a 1:1
   part (what PLAN.md currently names) would cap the fixture at ~31.5 A —
   fine for typical currents, but unable to test the clipping path. Confirm
   Triad TY-300P's actual turns/impedance ratio from its datasheet
   (`mouser.com/datasheet/3/236/1/TY_300P.pdf` — this document's automated
   fetch of it timed out twice) before ordering 3 of them; if it isn't
   ≥3:1, either pick a different part with a documented ratio or add the
   ×3 op-amp gain-stage fallback described in §3.3.
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

A fourth, lower-stakes one: **J3/J4/J8/J10/J11/J1's "fixed vs. pluggable"
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
- `docs/PLAN.md` §3.3–3.7, §11 — design rationale and the open questions
  this BOM resolves or narrows (items 2 and 5 in particular).
- `firmware/KilnFW/docs/HARDWARE.md` — J6/J20/relay terminal block/Power
  section facts cited above.
- `firmware/SaftyFW/docs/HARDWARE.md`, `firmware/SaftyFW/docs/
  CURRENT_SENSE.md` — J7/K4/J10/E-stop/CT-jack/AD8542 facts cited above;
  `CURRENT_SENSE.md` §2 is the direct source for §3's transfer-function math.
- Mouser lookups in this document were performed live on 2026-08-20 via
  `mcp__kicad__lookup_mouser_part`; stock/pricing figures are a snapshot
  from that date and will drift.
