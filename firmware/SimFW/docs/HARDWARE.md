# SimFW — Fixture Hardware Map

> **Status:** planning — bench harness not yet built · **Last reviewed:** 2026-08-20
> **Keep this file current.** This is the traced/reconciled authority for the
> fixture's own wiring — its Pico pin map, its connectors, and how they mate
> to the kilnCtl main board. If the physical harness disagrees with this
> file, **the harness wins**, and this file gets corrected in the same
> session, the same way `SaftyFW/docs/HARDWARE.md` treats its own board.
>
> **Every pin number, connector mapping, and part in this document is
> PROVISIONAL.** No fixture hardware has ever been connected. Nothing below
> has been continuity-checked, oscilloscoped, or run against a real
> `KilnFW`/`SaftyFW` board. This document reconciles four independently
> written, never-cross-checked source files (`i2c_owner.c`, `spi_emu_a/b.c`,
> `ct_wave_pwm.c`, `mcp23017.c`) plus the main-board docs — it removes the
> *disagreement* between those sources, not their unverified-against-silicon
> status. Bring-up step 5 (below) is the first point any of this touches a
> real board.

---

## 0. Blocking issues and open gaps found while writing this document

Read this section first.

1. **No GPIO collision and no budget overrun exist among the pins already
   chosen in code.** `i2c_owner.c` (I2C0), `spi_emu_a.c`/`spi_emu_b.c` (PIO
   SPI buses), and `ct_wave_pwm.c` (CT PWM) claim 15 distinct GPIOs with zero
   overlap — verified pin-by-pin in §1. This is the good news; it is not
   something any of those four files could have known on their own, since
   none of them could see the others' choices.

2. **Gap, not a collision: 10 GPIOs that PLAN.md section 3.6 budgets for have
   no owning file yet.** The 8 direct-GPIO DRDY/`~FAULT` lines (PLAN.md
   3.4/3.6, "decided") and the 2-pin debug UART to the Debug Probe (PLAN.md
   3.6, "same bench pattern as `SaftyFW`") are not claimed anywhere in
   `src/`. **This document assigns them for the first time** (§1) so that
   whoever writes that driver has a number to build against instead of
   picking a sixth independent guess. Whoever writes it must use exactly
   these numbers or update this table in the same commit.

3. **The remaining budget fits with exactly the margin PLAN.md predicted.**
   15 pins already claimed + 10 pins newly assigned here = 25 of the Pico's
   26 header GPIOs, 1 spare (GPIO28) — matching PLAN.md 3.6's "Total = 25 of
   the Pico's 26 header GPIO — tight but it fits" *before* anyone had actually
   done the arithmetic against all four files at once. Had any one of the
   four files picked one GPIO differently, this would not have come out even.

4. **`SaftyFW`'s own debug-UART convention (GP16/GP17) cannot be reused
   verbatim.** `ct_wave_pwm.c` already claims GPIO16 for CT channel 0's PWM
   carrier. Copying `SaftyFW/docs/HARDWARE.md` §7b's GP16/GP17 pair onto this
   fixture would silently collide. This document uses GPIO0/GPIO1 instead
   (UART0's other native pin pair, fully free) — see §1's footnote.

5. **CONTRADICTION found between the two main-board hardware docs on J7 pin
   1.** `firmware/KilnFW/docs/HARDWARE.md` ("Safety thermocouple board (J7 ->
   J1)") says J7 pin 1 is "(no connect)". `firmware/SaftyFW/docs/HARDWARE.md`
   §8 says J7 pin 1 is `3.3v_Safty (via R51, 0R)`. These cannot both be true
   of the same physical connector. This document follows `SaftyFW`'s version
   as the more recently reviewed, more narrowly scoped safety-domain source
   (which has already corrected the `KilnFW` doc on other J7-adjacent facts —
   see that doc's §10 "Stale sources") — **but this is unverified and must be
   confirmed by continuity check before the fixture's isolated-side power
   feed is wired**, since getting this wrong means either back-feeding an
   unintended 3.3 V rail or leaving the isolator side unpowered.

6. **Open design gap: the DUT-power relay (PLAN.md 3.4, "decided") is a
   single MCP23017 output bit (`EXP1_PIN_DUT_POWER`, `i2c_owner.c`), but the
   main board has *two independent* 12 V inputs** — J18 (main domain) and
   J19 (safety domain), each with its own TVS, per `firmware/KilnFW/docs/
   HARDWARE.md`'s Power section. A single fixture relay can only brown out
   the whole DUT if the bench operator wires both J18 and J19 from a common
   point *downstream* of that one relay — nothing in PLAN.md or the code
   says this explicitly. **Flagged for resolution before M-E** (§6, step 9);
   until resolved, `power_blip`-class scenarios brown out at most the domain
   the fixture is actually wired to.

7. **Two things checked and found NOT to be problems, recorded so nobody
   re-litigates them:**
   - `ct_wave_pwm.c` picked `DMA_IRQ_1` specifically to avoid a guessed
     collision with `spi_emu_a/b.c`'s DMA use. That guess was unnecessary —
     `spi_emu_a/b.c`/`max31856_pio_engine.c` use PIO FIFOs and a GPIO IRQ,
     never DMA — but the choice is harmless and is left as-is.
   - Bus A's PIO0 allocation (3 RX state machines + 1 shared TX state
     machine = 4) exactly fills PIO0's 4-SM budget with none to spare; bus
     B's PIO1 allocation (1 RX + 1 TX = 2) leaves 2 SMs free on PIO1. Neither
     overruns.

---

## 1. Authoritative Pico pin map

Every GPIO on the fixture's Pico. "Owner file" is blank where this document
is the first place the assignment is made (§0 item 2) — the future driver
that claims that pin must cite this table.

| GPIO | Signal | Owner file | Isolation domain | Main-board destination |
|---|---|---|---|---|
| 0 | Debug UART0 TX → Debug Probe RX | *(none yet — assign here)* | GND_Main (bench-local, not board-referenced) | n/a (bench probe only) |
| 1 | Debug UART0 RX ← Debug Probe TX | *(none yet)* | GND_Main | n/a |
| 2 | `DRDY_MAIN_0` (open-drain) | *(none yet)* | GND_Main | J6 pin 17 (`thermoDrdy_0`) |
| 3 | `DRDY_MAIN_1` (open-drain) | *(none yet)* | GND_Main | J6 pin 15 (`thermoDrdy_1`) |
| 4 | I2C0 SDA | `i2c_owner.c` (provisional, own header comment) | GND_Main | n/a (internal: MCP23017 #1/#2) |
| 5 | I2C0 SCL | `i2c_owner.c` | GND_Main | n/a (internal) |
| 6 | SPI bus A SCLK | `spi_emu_a.c` | GND_Main | J6 pin 9 (`CLK`) |
| 7 | SPI bus A MOSI (fixture input) | `spi_emu_a.c` | GND_Main | J6 pin 11 (`MOSI`) |
| 8 | SPI bus A MISO (fixture output, tri-stated) | `spi_emu_a.c` | GND_Main | J6 pin 10 (`MISO`) |
| 9 | SPI bus A CS0 | `spi_emu_a.c` | GND_Main | J6 pin 8 (`CS0`) |
| 10 | SPI bus A CS1 | `spi_emu_a.c` | GND_Main | J6 pin 7 (`CS1`) |
| 11 | SPI bus A CS2 | `spi_emu_a.c` | GND_Main | J6 pin 6 (`CS2`) |
| 12 | SPI bus B SCLK | `spi_emu_b.c` | crosses isolator → GND_Safty | J7 pin 9 (`CLK`) |
| 13 | SPI bus B MOSI (fixture input) | `spi_emu_b.c` | crosses isolator → GND_Safty | J7 pin 5 (`MOSI`) |
| 14 | SPI bus B MISO (fixture output) | `spi_emu_b.c` | crosses isolator → GND_Safty | J7 pin 7 (`MISO`) |
| 15 | SPI bus B CS0 | `spi_emu_b.c` | crosses isolator → GND_Safty | J7 pin 11 (`CS0`) |
| 16 | CT PWM ch0 (zone 0 carrier) | `ct_wave_pwm.c` | crosses transformer → floating | J13 (via RC + isolation xfmr) |
| 17 | `DRDY_MAIN_2` (open-drain) | *(none yet)* | GND_Main | J6 pin 13 (`thermoDrdy_2`) |
| 18 | CT PWM ch1 (zone 1 carrier) | `ct_wave_pwm.c` | crosses transformer → floating | J15 (via RC + isolation xfmr) |
| 19 | `FAULT_MAIN_0` (open-drain) | *(none yet)* | GND_Main | J6 pin 18 (`thermoFault_0`) |
| 20 | CT PWM ch2 (zone 2 carrier) | `ct_wave_pwm.c` | crosses transformer → floating | J17 (via RC + isolation xfmr) |
| 21 | `FAULT_MAIN_1` (open-drain) | *(none yet)* | GND_Main | J6 pin 16 (`thermoFault_1`) |
| 22 | `FAULT_MAIN_2` (open-drain) | *(none yet)* | GND_Main | J6 pin 14 (`thermoFault_2`) |
| — | PWM pacer slice 3 (no GPIO bound) | `ct_wave_pwm.c` | n/a | n/a |
| — | DMA_IRQ_1 (not a pin) | `ct_wave_pwm.c` | n/a | n/a |
| 25 | Heartbeat LED (on-board, not a header pin) | *(none yet, PLAN.md 3.6)* | n/a | n/a |
| 26 | `DRDY_SAFETY` (open-drain) | *(none yet)* | crosses isolator → GND_Safty | J7 pin 4 (`thermoDrdy`) |
| 27 | `FAULT_SAFETY` (open-drain) | *(none yet)* | crosses isolator → GND_Safty | J7 pin 3 (`thermoFault`) |
| **28** | **SPARE — the one pin PLAN.md 3.6 leaves free** | — | — | — |

**25 of 26 header GPIOs assigned, 1 spare (GPIO28).** (GPIO23/24 are not
header pins on a stock Pico; GPIO25 is the on-board LED, also not a header
pin — both excluded from the 26-pin budget, per PLAN.md 3.6's own framing.)

### Footnote: why GPIO0/1 for the debug UART, not GP16/17

`SaftyFW/docs/HARDWARE.md` §7b uses GP16/GP17 for its console UART because
that is UART0's only fully-free native pin pair on *that* board's layout.
On this fixture, GPIO16 is already CT channel 0's PWM carrier
(`ct_wave_pwm.c`), so reusing GP16/17 here would collide. GPIO0/GPIO1 are
UART0's other native TX/RX pair (RP2040 GPIO function table: UART0 on
GPIO0/1, GPIO12/13 [taken by SPI bus B here], GPIO16/17 [taken by CT here]);
they are unclaimed by anything else in this design, so this document assigns
the debug UART there instead. If a future revision frees GP16/17 by moving
the CT channel, GP0/1 stays valid regardless — no reason to move it again.

---

## 2. PIO pin-adjacency constraints (NOT freely reassignable)

`max31856_spi_slave.pio`'s RX and TX programs use fixed relative-GPIO-offset
arithmetic instead of taking every pin as an independent parameter (the PIO
instruction set only gives each program one `IN_BASE`/`JMP_PIN`/`OUT_BASE`;
everything else has to be reached by a constant offset from one of those). A
future edit that "tidies" the pin map without respecting these rules will
build cleanly and then silently watch the wrong GPIO. The rules, taken
directly from `max31856_spi_slave.pio`'s header comment and
`max31856_pio_engine.c`'s `sm_config_set_*_pins` calls:

1. **`SCLK = MOSI_gpio − 1` (mod 32).** The RX program's `IN_BASE` is set to
   the bus's MOSI pin; SCLK is read via a fixed offset of 31 from `IN_BASE`,
   which only lands on the right pin if SCLK sits exactly one GPIO number
   below MOSI. (Both buses satisfy this today: bus A SCLK=6, MOSI=7;
   bus B SCLK=12, MOSI=13.)
2. **`CS0 = SCLK + 3` (mod 32).** The shared TX program's `IN_BASE` is set to
   `CS0`; SCLK is read from it via a fixed offset of 29. Equivalently,
   CS0 must sit exactly 3 GPIO numbers above SCLK. (Bus A: SCLK=6, CS0=9.
   Bus B: SCLK=12, CS0=15.)
3. **On bus A, CS0/CS1/CS2 must be 3 *consecutive ascending* GPIOs.** The TX
   program's idle-detect (`in pins, 3`) and the per-channel RX programs' own
   `jmp_pin` wiring both assume this. (Bus A: CS0=9, CS1=10, CS2=11 — holds.)
4. **MISO has no offset constraint** — it is passed directly as `OUT_BASE`/
   `SET_BASE` and may be any GPIO not otherwise claimed.
5. **All of the above arithmetic wraps modulo 32**, not modulo the Pico's
   real ~30 GPIOs — this is a documented PIO idiom (pin-index math always
   wraps mod 32 regardless of how many real pins exist), not a bug, but it
   means the "offset 31" and "offset 29" language above is exact, not
   approximate.
6. Bus B follows the identical SCLK/CS0 relationship (rule 1–2) but has only
   one CS line, so rule 3 does not apply to it.

**Both buses' current provisional GPIOs already satisfy every rule above** —
verified in this pass. If any future change moves SCLK, MOSI, or a CS line
on either bus, re-derive rules 1–3 by hand and update both the `.c` file's
comment and this table in the same commit; the PIO assembler will not catch
a violation, it will simply run against the wrong pin.

---

## 3. Connector mating tables

### 3.1 Fixture → J6 (main-side thermocouple bus) — **REVERSE PIN ORDER TRAP**

Main board **J6** is a 1×20 **socket**; the real thermocouple daughterboard's
**J5** is a 1×20 **header**, and per `firmware/KilnFW/docs/HARDWARE.md`
("Thermocouple daughterboard (J6 -> J5)") **the two mate in reverse pin
order: J6 pin 1 = J5 pin 20.** The fixture unplugs the real daughterboard and
plugs into J6 in its place (PLAN.md §2's connection diagram), so the
fixture's own J6 plug must be wired exactly as J5 would be — meaning **the
same reversal applies to the fixture's connector**, not just to the discarded
daughterboard. Get this backwards and the fixture drives 5 V into what it
thinks is `CLK`, or worse.

**Worked example:** J6's physical pin 8 carries `CS0`. On the mating
fixture/J5-style plug, `CS0` sits at connector **pin 13** (`21 − 8 = 13`),
**not** pin 8. Every row below follows the same `J6 pin = 21 − (fixture plug
pin)` rule.

| J6 pin | Fixture plug pin | Signal | Fixture GPIO | Notes |
|---|---|---|---|---|
| 20 | 1 | 5V | — | Power feed question, see §5 open item |
| 19 | 2 | 3.3V | — | Unused by the real TC board; leave unconnected |
| 18 | 3 | `thermoFault_0` | GPIO19 (`FAULT_MAIN_0`) | |
| 17 | 4 | `thermoDrdy_0` | GPIO2 (`DRDY_MAIN_0`) | |
| 16 | 5 | `thermoFault_1` | GPIO21 (`FAULT_MAIN_1`) | |
| 15 | 6 | `thermoDrdy_1` | GPIO3 (`DRDY_MAIN_1`) | |
| 14 | 7 | `thermoFault_2` | GPIO22 (`FAULT_MAIN_2`) | |
| 13 | 8 | `thermoDrdy_2` | GPIO17 (`DRDY_MAIN_2`) | |
| 12 | 9 | GND | fixture GND_Main | |
| 11 | 10 | `MOSI` | GPIO7 | fixture **input** (ESP32 is bus master) |
| 10 | 11 | `MISO` | GPIO8 | fixture **output**, tri-stated when no CS asserted |
| 9 | 12 | `CLK` | GPIO6 | fixture input |
| 8 | 13 | `CS0` | GPIO9 | fixture input |
| 7 | 14 | `CS1` | GPIO10 | fixture input |
| 6 | 15 | `CS2` | GPIO11 | fixture input |
| 5 | 16 | `SDA` | — | not wired — fixture's own I2C0 is a separate internal bus |
| 4 | 17 | `SCL` | — | not wired |
| 3 | 18 | GND | fixture GND_Main | |
| 2 | 19 | GND | fixture GND_Main | |
| 1 | 20 | GND | fixture GND_Main | |

At least one GND pin must be tied for SPI bus A to have a common reference
between the ESP32 (master) and the fixture (slave) — tie all four listed for
margin, matching what the real daughterboard does.

### 3.2 Fixture → J7 (safety-side thermocouple bus, isolated)

Main board **J7** is a 2×06, 1.27 mm header, **straight pin-for-pin** to the
safety daughterboard's **J1** (no reversal, unlike J6) —
`firmware/SaftyFW/docs/HARDWARE.md` §8.

| J7 pin | Signal | Fixture GPIO | Notes |
|---|---|---|---|
| 1 | `3.3v_Safty` (via R51, per `SaftyFW` doc) — **see §0 item 5, contradicts `KilnFW` doc** | — | isolator-side supply candidate; confirm before wiring |
| 2 | `5v_Safty` | — | |
| 3 | `thermoFault` | GPIO27 (`FAULT_SAFETY`) | via digital isolator |
| 4 | `thermoDrdy` | GPIO26 (`DRDY_SAFETY`) | via digital isolator |
| 5 | `MOSI` | GPIO13 | via isolator, fixture input |
| 6 | `SDA` | — | not wired |
| 7 | `MISO` | GPIO14 | via isolator, fixture output |
| 8 | `SCL` | — | not wired |
| 9 | `CLK` | GPIO12 | via isolator, fixture input |
| 10 | GND_Safty | — | fixture-side isolated ground reference — **not** tied to fixture GND_Main |
| 11 | `CS0` | GPIO15 | via isolator, fixture input |
| 12 | GND_Safty | — | same as pin 10 |

Direction split across the isolator: 3 channels board→fixture (`CLK`,
`MOSI`, `CS0`), 3 channels fixture→board (`MISO`, `thermoDrdy`,
`thermoFault`) — check this 3/3 split against whatever isolator part is
actually purchased (see §5); some 6-channel parts are fixed at a different
forward/reverse split (e.g. 4/2) and would not fit this bus without
re-routing a channel.

### 3.3 Fixture → CT jacks (J13/J15/J17)

Each channel: fixture PWM GPIO → 2-pole RC low-pass (PLAN.md 3.3) →
isolation transformer primary (GND_Main-referenced) → transformer secondary
(floating, isolated from **both** GND_Main and GND_Safty, exactly like a real
CT) → jack tip/sleeve.

| Zone | Fixture GPIO | Jack |
|---|---|---|
| 0 | GPIO16 | J13 (`Current1` on the safety side, `SaftyFW/docs/HARDWARE.md` §9) |
| 1 | GPIO18 | J15 (`Current2`) |
| 2 | GPIO20 | J17 (`Current3`) |

**R72/R78/R84 (the burden resistors on the safety board's input, one per
channel) are DNP on the real board** — `SaftyFW/docs/CURRENT_SENSE.md` §2:
"the board expects a self-burdened, voltage-output CT." The fixture's
transformer secondary must present as that same kind of voltage-output
source, not a current-output one, or the safety board's clamp diodes (D12/D13
per channel) will conduct and the reading will saturate regardless of what
the fixture commands.

### 3.4 Fixture → relay sense terminal blocks (J3/J4/J8/J11) and K4 (J10)

**Caution — a numbering trap exists on the main board, but it does not
reach this fixture's own code.** `firmware/KilnFW/docs/HARDWARE.md`'s SX1509
section: *"Relay numbering is not K numbering. `Relay1` is K3, `Relay2` is
K1, `Relay3` is K2, `Relay4` is K5."* That trap is specific to the real
main board's SX1509 register/signal naming. `i2c_owner.c` already sidesteps
it entirely by naming its own MCP23017 pins directly after the K-designator
(`EXP1_PIN_K1`..`EXP1_PIN_K5`, `EXP1_PIN_K4`) — no renumbering needed on the
fixture side. State it here anyway so nobody "fixes" the fixture's naming to
match the SX1509's `Relay1..4` scheme and reintroduces the trap.

| Relay | Terminal block | Fixture exp1 pin (`i2c_owner.c`) | Domain |
|---|---|---|---|
| K1 | J3 (NC/COM/NO) | `EXP1_PIN_K1` = 0 | GND_Main |
| K2 | J4 | `EXP1_PIN_K2` = 1 | GND_Main |
| K3 | J8 | `EXP1_PIN_K3` = 2 | GND_Main |
| K5 | J11 | `EXP1_PIN_K5` = 3 | GND_Main |
| K4 | J10 | `EXP1_PIN_K4` = 4 | **contact is in GND_Safty; the wetting/opto stage must cross to GND_Main before this MCP23017 pin** (PLAN.md 3.4: "opto-isolated for K4") |

Each sense circuit supplies a small wetting voltage through the relay's
NO/COM (and optionally NC) contact into the expander input — see §5's
external-components list for the wetting circuit itself.

### 3.5 Fixture → J20 (spare main-board I/O)

| J20 pin | Signal | Fixture exp1 pin |
|---|---|---|
| 1 | `IO_3` | `EXP1_PIN_J20_IO3` = 8 |
| 2 | `IO_4` | `EXP1_PIN_J20_IO4` = 9 |

### 3.6 Fixture → E-stop loop

The safety board's E-stop connector (`J1` on the `SaftyProcessor` sheet,
Phoenix 1935161, 2-pin — `SaftyFW/docs/HARDWARE.md` §5) normally carries a
normally-closed button or a jumper; **as-built, with neither fitted, GPIO9
floats high and reads permanent STOP.** The fixture becomes that jumper
(PLAN.md 3.4): its E-stop optoMOS output wires across J1's two terminals in
place of the button, driven by `EXP1_PIN_ESTOP_DRIVE` (exp1 pin 6,
`i2c_owner.c`). `configure_exp1()` idles this pin **de-asserted (low)** at
boot — confirm at bring-up whether that idle state presents a *closed*
(healthy) or *open* (STOP) contact to GPIO9, since the whole point of the
fixture-as-jumper is to default to a known, intentional state rather than
the as-built float.

### 3.7 Fixture → DUT 12 V power

| From | Via | To |
|---|---|---|
| Bench supply | fixture power-in connector → fixture relay/high-side switch (`EXP1_PIN_DUT_POWER`, exp1 pin 7) → fixture power-out connector | J18 (main 12 V in) **and/or** J19 (safety 12 V in) |

**Open gap (§0 item 6):** only one relay/one control bit exists in the
current design, but J18 and J19 are two independent inputs, each with its
own TVS (`firmware/KilnFW/docs/HARDWARE.md`, Power section). To brown out
the *whole* DUT with a single fixture relay, both J18 and J19 must be wired
from a common point downstream of that relay by the bench operator — not
something the code or PLAN.md currently states. Resolve before bring-up
step 9 (§6).

---

## 4. Isolation boundary map

Per PLAN.md 3.5, restated against the pin map above:

| Fixture signal group | Domain | Crosses via |
|---|---|---|
| I2C0 (both MCP23017s), SPI bus A, `DRDY_MAIN_*`/`FAULT_MAIN_*`, relay sense K1/K2/K3/K5, `Fault` line sense, J20 IO_3/IO_4, debug UART, DUT-power relay control | GND_Main | — (native) |
| SPI bus B, `DRDY_SAFETY`, `FAULT_SAFETY` | GND_Safty | 6-channel digital isolator (ISO7741-class), powered from J7's safety-side rail (§0 item 5) on the isolated side |
| 3× CT channels | floating (neither domain) | isolation transformer, 1:1, per channel |
| K4 relay sense | GND_Safty at the contact, GND_Main at the MCP23017 | optocoupler in the wetting circuit (§3.4) |
| E-stop | GND_Safty at J1 | optoMOS, GND_Main-side control |

**Standing rule (PLAN.md 3.5, unchanged here):** a deliberate, labeled,
removable jumper may common the grounds for early breadboard bring-up, but
**the standard test library must run with it out.** Bring-up step 5 (§6)
exists specifically to verify this before the DUT is ever touched.

---

## 5. External components (not on the Pico)

| Part | Qty | Role | Sizing status |
|---|---|---|---|
| 6-channel digital isolator, ISO7741-class | 1 | SPI bus B (4 ch) + `DRDY_SAFETY`/`FAULT_SAFETY` (2 ch) | Direction split must be 3 board→fixture / 3 fixture→board (§3.2) — confirm against the specific part's fixed channel directions before ordering |
| CT isolation transformer, 1:1 audio/isolation | 3 | One per CT channel, between the RC-filtered PWM output and the J13/J15/J17 jack | **Open (PLAN.md §11 item 2):** transfer function not sized — must be read off the AD8542 `CurrentSense` sheets / `SaftyFW/docs/CURRENT_SENSE.md` §2 (gain 0.715, full-scale ≈98 A rms for a 1 V/30 A CT or ≈326 A rms for a 1 V/100 A CT) so "N amps commanded" maps to a plausible CT secondary voltage, not an arbitrary one |
| Relay-sense wetting circuit | 5 | One per relay (K1/K2/K3/K5 direct, K4 through an opto stage) into MCP23017 #1 inputs | Not sized; a small voltage source + resistor per contact, per PLAN.md 3.4 |
| E-stop optoMOS | 1 | In series with J1's E-stop loop, driven by `EXP1_PIN_ESTOP_DRIVE` | Not sized |
| DUT 12 V power relay / high-side switch | 1 (see §0 item 6 re: whether 1 is enough) | Fixture's own 12 V feed to J18 (and/or J19) | **Open (PLAN.md §11 item 5):** inrush rating vs the board's actual inrush not measured |
| MCP23017 | 2 | 0x20 (fixed-role pins) and 0x21 (spare) on I2C0 | Sized; already in code |
| PCA9685 (optional) | 0–1 | PWM/LED stimulus, not required for the base feature set | Not needed unless a test calls for analog-ish stimulus |

---

## 6. Bring-up checklist

Expanded from PLAN.md section 14. Each step's pass criterion is concrete;
run the listed `kilnsim` CLI command or MCP tool where one exists today (from
`tools/PcTools/src/kilnsim/cli.py` and `mcp_server.py` — commands not yet
implemented are called out as gaps rather than invented).

- [ ] **Step 1 — Pico alone: USB CDC + protocol + heartbeat.**
  Pass: `kilnsim state` connects (auto-detected port or `--port`), returns a
  JSON telemetry snapshot with no transport error, protocol/firmware version
  fields populated.
  Command: `kilnsim state`

- [ ] **Step 2 — Expanders on I2C: read/write, interrupt lines if used.**
  Pass: both MCP23017s ACK on I2C0 (0x20, 0x21); writing then reading back a
  spare, non-reserved pin on either expander round-trips.
  Command: MCP tools `io_write` / `io_read` (`mcp_server.py`) — **no `kilnsim`
  CLI subcommand exists for raw I/O yet**; the CLI's own subcommand list
  (`state`/`preset`/`fault`/`estop`/`power`/`run`/`monitor`) has no `io`
  entry. Use `sim_raw_command` or the MCP `io_write`/`io_read` tools, or add
  the CLI subcommand before this bench session.

- [ ] **Step 3 — SPI A loopback (scripted master on spare pins): register
  machine correct.**
  Pass: a scripted transaction against a channel returns the expected
  register image with zero underruns.
  Command: PLAN.md 13.2's `kilnsim selftest` mode **is not implemented yet**
  (no such subcommand in `cli.py`). Interim: inspect the SPI
  transaction/underrun counters in the telemetry frame (`kilnsim state`,
  PLAN.md 5.3) before and after a manual scripted burst.

- [ ] **Step 4 — CT synthesis into a scope/DMM through the transformer:
  waveform + levels.**
  Pass: commanded amplitude produces a clean 60 Hz waveform at the jack
  within the expected voltage range for the fitted transformer/CT
  calibration.
  Command: MCP tool `ct_set_amps(channel, amps)` (`mcp_server.py`) — **no
  `kilnsim ct` CLI subcommand exists yet.**

- [ ] **Step 5 — GROUND-DOMAIN CHECK BEFORE FIRST DUT CONTACT. Do not skip,
  do not reorder.**
  With the bring-up jumper **OUT**: verify **no continuity** between fixture
  GND_Main and GND_Safty; verify isolator and transformer orientation against
  §4's table. This is the single check protecting the real board from the
  fixture becoming an unintended ground strap (PLAN.md 3.5, 15's top risk
  row). **Do this with a meter, on the bench, every time the harness is
  rebuilt — not once and trusted forever.**
  No `kilnsim` command substitutes for a physical continuity check.

- [ ] **Step 6 — DUT thermocouple path: J6 unplugged from the real
  daughterboard, fixture in its place, `KilnFW` booted — temperatures
  appear.**
  Pass: `KilnFW`'s own UI/telemetry shows plausible, non-zero, non-fault
  temperatures on all three main-side channels.
  Command: `kilnsim preset fast_test` then watch via `kilnctrl`'s
  `mcp__kilnctrl__thermo_read` / `thermo_get_reports` on the DUT side, and
  `kilnsim state` on the fixture side to confirm what it believes it is
  reporting.

- [ ] **Step 7 — Safety path: J7 via isolator, `SaftyFW`'s single channel
  reads.**
  Pass: the safety Pico's own thermocouple reading tracks the fixture's zone
  0 (or configured blend) temperature.
  Command: `kilnsim state` (fixture side) cross-checked against
  `mcp__kilnctrl__thermo_read` / `mcp__kilnctrl__safety_get_status` (DUT
  side).

- [ ] **Step 8 — Relay sense: command relays via existing kilnctrl tools,
  fixture sees edges.**
  Pass: commanding K1/K2/K3/K5/K4 through `kilnctrl` produces a matching edge
  in the fixture's relay-edge log within one debounce window (~24 ms worst
  case, `mcp23017.h`).
  Commands: `mcp__kilnctrl__io_set_relay` (DUT side) and MCP tool
  `relay_get_edges` / `relay_get_states` (fixture side) — **no `kilnsim relay`
  CLI subcommand exists yet.**

- [ ] **Step 9 — E-stop + fault line + DUT power relay, one at a time.**
  Pass (E-stop): `kilnsim estop open` then `kilnsim estop closed` produces
  the expected STOP/healthy transition on the DUT's safety status.
  Pass (fault line): a fault forced on the DUT's `Fault` GPIO is visible in
  the fixture's sense state.
  Pass (DUT power): `kilnsim power cycle --off-ms 500` reboots the DUT and
  its telemetry shows the gap — **first confirm §0 item 6's J18/J19 wiring
  question is resolved**, or this step only proves one domain browns out.
  Commands: `kilnsim estop open|closed`, `kilnsim power on|off|cycle
  [--off-ms N]`.

- [ ] **Step 10 — First closed-loop firing on `fast_test` preset.**
  Pass: `kilnsim run scenarios/baseline_firing.yaml` (once that scenario file
  exists per PLAN.md §8) exits 0, with the DUT's PID visibly regulating a
  simulated zone through relay cycling with no fixture intervention.
  Command: `kilnsim run <scenario.yaml> [--seed N] [--report out.json]`.

---

## 7. Cross-reference

- Design rationale for every signal above: `firmware/SimFW/docs/PLAN.md`
  sections 2, 3.1–3.7, 14.
- Main-board authoritative wiring: `firmware/KilnFW/docs/HARDWARE.md`,
  `firmware/SaftyFW/docs/HARDWARE.md`, `firmware/SaftyFW/docs/
  CURRENT_SENSE.md`.
- Code this document reconciles: `firmware/SimFW/src/tasks/i2c_owner.c`,
  `firmware/SimFW/src/tasks/spi_emu_a.c`, `firmware/SimFW/src/tasks/
  spi_emu_b.c`, `firmware/SimFW/src/drivers/max31856_spi_slave.pio`,
  `firmware/SimFW/src/drivers/max31856_pio_engine.{c,h}`,
  `firmware/SimFW/src/drivers/ct_wave_pwm.{c,h}`, `firmware/SimFW/src/tasks/
  wave_owner.c`, `firmware/SimFW/src/drivers/mcp23017.{c,h}`.
