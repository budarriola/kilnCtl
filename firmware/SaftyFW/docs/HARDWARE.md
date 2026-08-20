# Safety Processor Hardware Map

> **Status:** planning · **Last reviewed:** 2026-08-18
> **Keep this file current.** This is the traced-from-schematic authority for
> the safety domain. If the board is revised, update it in the same commit as
> the schematic and re-run the bench checks in §1. If it disagrees with the
> board, **the board wins.** Checklist at the bottom.

Everything here was traced from **`hardware/mainBoard/output/kiln.pdf`** (KiCad 10.0.4,
the PDF added in commit `c50cded` "add pdf sch of same board"), cross-checked
against `hardware/mainBoard/SaftyProcessor.kicad_sch` and its child sheets. Where a
schematic *net name* and the silicon disagree, this file follows the silicon
and says so.

**Do not trace this board from `hardware/mainBoard/kiln.net`.** That netlist is dated
`2026-07-19`, its `(source)` field still points at the pre-move
`kilnCtl\kiln.kicad_sch`, and it disagrees with the current schematic in at
least three places (see [Stale sources](#stale-sources) at the bottom). It is
almost certainly the origin of the two errors in `firmware/KilnFW/docs/HARDWARE.md`
corrected below.

The safety processor is a **Raspberry Pi Pico (RP2040)**, designator **A1**,
on sheet `/SaftyProcessor/`. It sits in the `GND_Safty` ground domain, which
shares no connection with `GND_Main` except through three optocouplers.

---

## 1. The correction that matters most

> **The isolated UART directions in `firmware/KilnFW/docs/HARDWARE.md` and
> `firmware/KilnFW/docs/SAFETY_LINK.md` are inverted relative to the current board,
> and `KilnFW`'s pin defaults are swapped with them.**

`firmware/KilnFW/docs/SAFETY_LINK.md` "Trap 1" states that the net names are backwards —
that `DataToSafty` is the ESP's RX and `DataFromSafty` is the ESP's TX.
**On this revision of the board that is false. The net names are correct as
written.** Traced from the optocoupler symbols:

| Opto | Pins 1,2 (LED) | Pins 3,4 (transistor) | Therefore |
|------|----------------|-----------------------|-----------|
| U1 | `Fault`, main side, via R11 390R | collector = Pico `mainFault` (GPIO10) + R8 1k pull-up to 3.3v_Safty; emitter = GND_Safty | ESP **drives** fault into the Pico |
| U2 | `DataToSafty`, main side, via R12 390R | collector = Pico `PicoRx` (GPIO5) + R9 1k pull-up to 3.3v_Safty; emitter = GND_Safty | `DataToSafty` = **ESP TX** |
| U3 | `PicoTx` (Pico GPIO4), **safety** side, via R7 390R | collector = `DataFromSafty`, main side; emitter = `MainProcessorDataGnd` | `DataFromSafty` = **ESP RX** |

**U3 is drawn mirrored relative to U1 and U2** — its LED is on pins 1,2 on the
*safety* side, where U1's and U2's LEDs are on pins 1,2 on the *main* side.
Reading U3 with U1/U2's orientation is exactly the mistake that produces the
docs' claim, and is the single easiest thing to get wrong on this whole board.

On the main sheet (`kiln.pdf` p.7, `MainControler`):

| ESP32-S3 GPIO | Net | Direction |
|---|---|---|
| **GPIO4** | `DataToSafty` | ESP **TX** (hierarchical pin drawn as an output) |
| **GPIO5** | `DataFromSafty` | ESP **RX**, with **R15 1k to 3.3V_Main** on this net |
| GPIO6 | `Fault` | ESP **output** (unchanged, docs are right about this) |

**The R15 placement is the independent confirmation.** R15 is a 1k pull-up to
`3.3V_Main` sitting on GPIO5. Under this trace, GPIO5 is the ESP's RX and is
the bare collector of U3 — which is precisely, and only, where a collector
pull-up belongs. Under the docs' trace, GPIO5 would be a push-pull TX output
and R15 would be doing nothing at all. The passive component tells you which
reading is right.

### What this means for `KilnFW`

`KilnFW`'s `Kconfig` defaults **were** `KILNCTL_SAFETY_TX_IO = GPIO5` and
`KILNCTL_SAFETY_RX_IO = GPIO4` — **swapped** with respect to this board. As
shipped, the ESP transmitted into U3's collector and listened on U2's LED
drive, so the link could not work in either direction. **Fixed 2026-08-16**:
defaults are now `TX_IO = GPIO4`, `RX_IO = GPIO5`.

This was a `KilnFW` bug, not a `SaftyFW` one, but it was listed as
**TODO.md item 0.1 (blocking)** because no amount of correct RP2040 firmware
would bring the link up until it was fixed. Two consequences followed from the
corrected map, both also changed on the ESP side:

- The ESP's **internal pull-up belongs on GPIO5** (RX, U3's bare collector),
  not GPIO4. R15 already pulls GPIO5 up externally, so the internal one is
  belt-and-braces rather than load-bearing — `safety_link_start()` now enables
  it on the correct pin.
- `SAFETY_LINK.md`'s boot-time note ("R15 idles the TX net high, which lights
  U2's LED and holds the Pico's RX low — a continuous break") was **wrong for
  this revision, in the safe direction**. R15 is on the ESP's RX, not its TX,
  and nothing on the main board pulls `DataToSafty` anywhere. With the ESP in
  reset, GPIO4 is high-impedance, U2's LED is dark, and R9 holds the Pico's RX
  **high** — a clean idle line, not a break. The Pico still must tolerate a
  break on RX (a half-driven line during ESP boot can produce one), but it is
  no longer the guaranteed steady-state condition the docs describe.

### The passive-component pattern, which is the real proof

The optocoupler outputs are **open collector**, so every crossing has exactly two
resistors and they are not interchangeable: a **390 R** in series with the LED on
the *driven* side, and a **1 k pull-up** on the *open-collector* side. Three
crossings, three of each, and they line up perfectly:

| Crossing | 390 R LED drive is on… | 1 k pull-up is on… | So the driven end is |
|---|---|---|---|
| U1 (fault) | R11, on `Fault` (main) | R8, on `mainFault` (safety) | ESP output — **undisputed** |
| U3 (Pico data) | R7, on `PicoTx` (safety) | **R15**, on `DataFromSafty` = **GPIO5** (main) | Pico output ⇒ GPIO5 is the **ESP's RX** |
| U2 (ESP data) | **R12**, on `DataToSafty` = **GPIO4** (main) | R9, on `PicoRx` (safety) | **GPIO4 is the ESP's TX** |

R12 is a 390 R in series into an LED anode. That is an LED drive resistor and
nothing else — you do not put 390 R in series with a receiver input. And R15 is a
1 k pull-up on GPIO5, which is only meaningful if GPIO5 is an open collector.
The two conclusions are independent of each other and independent of how the
symbols happen to be drawn.

### Verification status

| Evidence | Result | Done |
|---|---|---|
| Optocoupler symbol orientation, `kiln.pdf` p.2 | U3 mirrored vs U1/U2 ⇒ `DataFromSafty` is the ESP's RX | ✅ |
| 390 R / 1 k passive pattern, all three crossings | consistent, three for three | ✅ |
| **R15's net, parsed from `MainControler.kicad_sch`** | **R15 is on the `DataFromSafty` net** — a 1 k pull-up belongs only on an open collector | ✅ |
| Quick pre-check (internal pull-downs, ESP alone) | — | ⬜ |
| **Coordinated two-board GPIO test — pin/direction mapping** | **Confirmed 2026-08-18 by manual physical inspection**: ESP board pin 4 (GPIO4) and board pin 6 (GPIO6) are ESP outputs; Pico board pin 7 (GPIO5) reads ESP GPIO4, Pico board pin 6 (GPIO4) writes to ESP board pin 5 (GPIO5), Pico board pin 14 (GPIO10) is the fault read. Matches this document's map exactly | ✅ |
| Coordinated two-board GPIO test — **electrical toggle/propagation** | Attempted 2026-08-18 via `gpio_probe`/`pico_gpio` + Saleae. ESP GPIO4 was seen to physically toggle (multi-second, inconsistent lag between command and pad transition — worth characterizing before trusting timing on this net) but **no propagation to any Pico-side channel was observed in any capture**, and Pico GPIO4 drive produced no visible edge anywhere. GPIO6 (fault) could not be driven at all — hard-denied unconditionally by `gpio_probe.c`'s deny-list, no override. Leading suspect: `12v_Safty` not powered during the test — **unconfirmed, needs bench check** | ⬜ |
| Saleae capture under real traffic | — | ⬜ |

Three independent lines of schematic evidence agree, and the pin/direction
mapping now also has a fourth: **manual physical confirmation** (2026-08-18).
**None of the electrical propagation is a measurement yet**, so the bench
checks below still stand.

### The definitive test: coordinated, both boards, both directions

Both boards powered (main **and** `12v_Safty`), a minimal **GPIO probe on each
processor**, and the PC driving both sides
([`../../../tools/PcTools/TODO.md`](../../../tools/PcTools/TODO.md) §1).

> **The PC must reach each processor by a path that is not the link under test.**
> ESP over its USB serial, Pico over SWD. You cannot test the isolated UART
> over the isolated UART.

This single sitting verifies **both** traps at once — the pin assignment *and*
the inversion — because every crossing is an optocoupler and must invert:
a high on the driving side lights the LED and pulls the receiving collector low.

**Step A — ESP drives, Pico reads.**
ESP: GPIO4 output, GPIO5 input. Pico: GPIO4, GPIO5, GPIO10 all inputs.

| Drive | Expect if this document is right |
|---|---|
| ESP GPIO4 → **HIGH** | Pico GPIO5 (`PicoRx`) reads **LOW** — U2's LED lit |
| ESP GPIO4 → **LOW** | Pico GPIO5 reads **HIGH** — R9 pulls it up |
| ESP GPIO5 → HIGH/LOW | **no change on any Pico pin** — it is the ESP's own receiver |

**Step B — Pico drives, ESP reads.**
Pico: GPIO4 output. ESP: GPIO4 and GPIO5 inputs.

| Drive | Expect |
|---|---|
| Pico GPIO4 (`PicoTx`) → **HIGH** | ESP GPIO5 reads **LOW** — U3's LED lit |
| Pico GPIO4 → **LOW** | ESP GPIO5 reads **HIGH** — R15 pulls it up |
| Pico GPIO5 driven | **no change at the ESP** |

**Step C — the fault line, which confirms the barrier model as a whole.**

| Drive | Expect |
|---|---|
| ESP GPIO6 → **HIGH** | Pico GPIO10 (`mainFault`) reads **LOW** |
| ESP GPIO6 → **LOW** | Pico GPIO10 reads **HIGH** |

Safe to exercise during bring-up because no Pico firmware acts on `mainFault`
yet — but **leave GPIO6 driven LOW when finished.**

**Reading the result.**

- Responses on ESP **GPIO4** in Step A and ESP **GPIO5** in Step B ⇒ this
  document is correct and `KILNCTL_SAFETY_TX_IO`/`RX_IO` are swapped.
- Responses the other way round ⇒ this document is wrong. **Say so loudly and
  correct it** rather than working around it.
- Every "drive HIGH ⇒ read LOW" is a direct confirmation of the inversion, and
  therefore that the Pico must **not** invert in software (§"What the docs still
  get right").
- A pin that responds in *both* steps would mean the two nets are shorted — a
  board fault, not a documentation question.

### Quick pre-check, ESP alone, if the Pico is not yet available

Not a substitute for the coordinated test, but it costs one read and needs no
safety-domain power, no Pico and no probe.

Configure **both GPIO4 and GPIO5 as inputs with the internal pull-*down***
enabled and read them. The ESP32-S3's internal pull-down is roughly 45 kΩ;
against R15's 1 kΩ to 3.3 V that divider sits at about 3.2 V.

| Pin | Reads | Meaning |
|---|---|---|
| the one carrying R15 | **HIGH** | open collector with an external pull-up ⇒ the ESP's **RX** |
| the other | **LOW** | no external pull-up ⇒ the ESP's **TX** |

Repeating with internal pull-*ups* is the null control: both read high and the
test yields nothing. That non-result is expected and proves the method, not the
pinout.

Static resistance checks, everything unpowered, as an independent cross-check:

| Measure | Expected |
|---|---|
| GPIO4 to `MainProcessorDataGnd` | ~390 R plus a diode drop (R12 into U2's LED) |
| GPIO5 to `MainProcessorDataGnd` | high — a phototransistor, not a diode |

### Once both firmwares exist

Capture the isolated pair with the Saleae and decode `kilnlink` framing
(`tools/PcTools/TODO.md` §2). This is the only check that proves baud, polarity and
the inversion pair together, under real traffic.

### What the docs still get right

**The inversion analysis is unchanged and still correct.** Each direction
crosses exactly one optocoupler, and an optocoupler is an inverter: the
driving side's logic high lights the LED, the receiving side's collector is
pulled low. So exactly one end must invert, `KilnFW` does it in the ESP UART
peripheral via `uart_set_line_inverse(UART_SIGNAL_TXD_INV | UART_SIGNAL_RXD_INV)`,
and **the RP2040 therefore uses its plain hardware UART at standard polarity
with no inversion, no PIO, and no external parts.** Do not "fix" polarity on
the Pico side; two inversions in series cancel the optocouplers' and the link
goes dead.

---

## 2. Pico (A1) pin map

Traced from `kiln.pdf` p.2. This table is authoritative for `SaftyFW`.

| Pico pin | GPIO | Net | Direction | Notes |
|---|---|---|---|---|
| 1 | GPIO0 | `MISO` | in | SPI0 RX from MAX31856 |
| 2 | GPIO1 | `CS0` | out | MAX31856 `~CS`, active low. **10k pull-up to 3.3v_Safty** |
| 4 | GPIO2 | `CLK` | out | SPI0 SCK |
| 5 | GPIO3 | `MOSI` | out | SPI0 TX |
| 6 | GPIO4 | `PicoTx` | out | UART TX → R7 390R → U3 LED → ESP GPIO5 |
| 7 | GPIO5 | `PicoRx` | in | UART RX ← U2 collector. **R9 1k pull-up**, idles high |
| 9 | GPIO6 | `saftyRelay` | out | Q4 gate → K4 coil. **High = relay energized** |
| 10 | GPIO7 | `SDA` | i/o | I2C0 SDA, **R48 2.2k pull-up**, out to J7 pin 6. Nothing answers today |
| 11 | GPIO8 | `SCL` | out | I2C0 SCL, **R49 2.2k pull-up**, out to J7 pin 8 |
| 12 | GPIO9 | `estop` | in | J1 terminal. **R10 1k pull-up + C3 0.01uF**. See §5 |
| 14 | GPIO10 | `mainFault` | in | U1 collector. **R8 1k pull-up. Active LOW.** See §4 |
| 15 | GPIO11 | `thermoFault` | in | MAX31856 `~FAULT`, active low. **R1 10k pull-up** |
| 16 | GPIO12 | `thermoDrdy` | in | MAX31856 `~DRDY`, active low. **R2 10k pull-up** |
| 31 | GPIO26 / ADC0 | `Current1` | analog | Current-sense channel 1 |
| 32 | GPIO27 / ADC1 | `Current2` | analog | Current-sense channel 2 |
| 34 | GPIO28 / ADC2 | `Current3` | analog | Current-sense channel 3 |
| 33 | AGND | `GND_Safty` | — | |
| 35 | ADC_VREF | — | — | **C2 2.2uF to GND_Safty only.** See §6 |
| 36 | 3V3 | `3.3v_Safty` | **power in** | **Back-fed. See §7 — read this before plugging in USB** |
| 17,19,20 | GPIO13/14/15 | — | — | Unconnected |
| 21,22,24–27,29 | GPIO16–22 | — | — | Unconnected |
| 30 | RUN | — | — | Unconnected. No external reset |
| 37 | 3V3_EN | — | — | Unconnected |
| 39,40 | VSYS, VBUS | — | — | **Unconnected** |

Also on the sheet: R3 10k to `GND_Safty` and R4/R5/R6 10k to `3.3v_Safty` on
the SPI group — verify which line each lands on at bring-up before assuming a
default state for MISO/CLK/MOSI.

`firmware/KilnFW/docs/HARDWARE.md`'s Pico section is **correct** on this map (GPIO7/8 =
SDA/SCL, GPIO11/12 = thermoFault/thermoDrdy). The stale `kiln.net` disagrees;
ignore it.

**On-board flash: 2MB.** A1 is a stock Raspberry Pi Pico (`PICO_BOARD=pico` in
`firmware/SaftyFW/CMakeLists.txt`, not `pico_w`/`pico2`/a custom board file),
so its onboard QSPI flash is fixed by Raspberry Pi's own board design at
`PICO_FLASH_SIZE_BYTES = 2 * 1024 * 1024` (pico-sdk's
`src/boards/include/boards/pico.h`) — a hardware fact of the stock board, not
a sourcing ambiguity like the ESP32-S3 module's flash was. TODO.md item 10.1.

---

## 3. Safety relay K4 — the most important system-level constraint

Sheet `/SaftyProcessor/SaftyRelay/` (`SSD.kicad_sch`), `kiln.pdf` p.3.

- **K4 = EE2-12NUH**, 12 V coil from `12v_Safty`, contacts out to **J10**
  (Phoenix 1935174, 3-pin NC/COM/NO).
- Coil low-side switched by **Q4 (BSS138P)** from `saftyRelay` (GPIO6).
  **GPIO6 high = coil energized.** D8 (SS16FP) is the flyback.
- R66/R67 (the "shorting resistors" alternative) are marked **DNP** — K4 is
  the populated option.
- Contacts 3 and 5 are marked unconnected; 8/9/10 go to J10.

The schematic carries this note next to K4:

> **"This relay is not rated for use with heaters"**

**This is by design, not a limitation.** Every relay on this board — K1, K2,
K3, K5 on the main domain and K4 on the safety domain — is a **pilot relay,
present for galvanic isolation only**. None of them ever carries element
current. They exist to drive large external contactors and SSRs, with the
mechanical isolation of a relay contact between the logic and whatever switches
the load.

So "the safety processor cuts power using its relay" is true, but it acts one
stage removed: K4 controls what energizes the load-switching devices. The system
topology `SaftyFW` is written against:

```
   mains ──> [ line contactor ] ──> [ SSRs ] ──> heating elements
                    ^                  ^
                    │ coil             │ control
              ┌─────┴──────┐      K1/K2/K3/K5 pilot relays
  K4 contact ─┤            ├── E-stop      (main processor)
 (safety proc)└────────────┘   (in series)
```

**K4 must be in series with whatever energizes the load-switching stage** — the
line contactor's coil is the right place, because it is upstream of the SSRs and
in a different technology, so K4 can defeat a shorted SSR. Wired so that:

- **K4 energized (GPIO6 high) = heating permitted.** De-energized = contactor
  drops = elements dead.
- Therefore **loss of Pico power, a firmware crash, a watchdog reset, or an
  unprogrammed Pico all result in the contactor being open.** The safe state is
  the state that requires no working software to reach. This polarity is not
  negotiable and every part of `SaftyFW` depends on it.

Two things this topology does and does not buy you, stated plainly:

- It **does** defeat a welded/shorted SSR, because the contactor is upstream of
  the SSRs and in a different technology. That is the single most valuable
  thing this board can do, and it is the reason guard **S3** exists.
- It does **not** defeat a welded *contactor*. Nothing on this board can. If the
  contactor's own contacts weld, guard S3 will detect current-with-no-heat-
  commanded and report it, K4 will drop, and the current will keep flowing.
  That is a real, unclosable-in-firmware gap and it belongs in the safety case,
  not buried in a comment.

**J10 pin identification (traced 2026-08-16, from the K4 symbol's drawn rest
position in `SSD.kicad_sch` — pin 9 is the common/armature, and the drawn
contact blade rests on pin 10):**

| K4 pin | Function | J10 pin |
|---|---|---|
| 8 | NO | 1 |
| 9 | COM | 2 |
| 10 | NC | 3 |

**Wire the line contactor's coil through J10 pins 1 (NO) + 2 (COM).** J10 pin 3
(NC) must not be used for the coil — that pairing stays energized whenever K4
is dead, which is the exact failure this interlock exists to prevent. This
reading comes from the symbol's drawing convention (no NC/NO/COM silkscreen or
schematic text label exists on J10 itself), so it is schematic evidence, not a
measurement — **confirm with a continuity check on the physical board before
final wiring.**

---

## 4. `mainFault` (GPIO10) — and why it is not sufficient

U1's collector, R8 1k pull-up to 3.3v_Safty.

| ESP `Fault` (GPIO6) | U1 LED | GPIO10 reads | Meaning |
|---|---|---|---|
| high | lit | **LOW** | ESP is asserting a fault |
| low | dark | HIGH | ESP says it is healthy |
| ESP unpowered / absent / in reset | dark | **HIGH** | *also reads "healthy"* |

**`mainFault` is active low, and its failure mode is fail-danger.** A dead,
absent, crashed-before-boot, or unpowered main controller is
indistinguishable on this line from a healthy one. There is no hardware path
that fixes this — it is one LED and one pull-up.

The consequence is a firmware requirement, not a nice-to-have: **the UART
liveness timeout, not the fault line, is the real detector of a dead main
controller.** The fault line only carries the cases where the ESP is alive
enough to say so. Guard **S6** is built around this and treats the two signals
as independent evidence, never as redundant copies of each other.

---

## 5. E-stop (GPIO9)

J1 (Phoenix 1935161, 2-pin) → GPIO9, with **R10 1k pull-up to 3.3v_Safty** and
**C3 0.01uF** to ground (a ~10 µs RC — noise filtering only, not debounce).

Because of the pull-up, the only fail-safe wiring is a **normally-closed**
contact to `GND_Safty`:

| GPIO9 | Contact | Meaning |
|---|---|---|
| LOW | closed | E-stop healthy, heating permitted |
| HIGH | open — button pressed, **or a broken wire, or nothing fitted** | **STOP** |

Wired this way, a cut cable, a pulled connector and a pressed button all read
identically, and all read as *stop*. That is the correct behaviour and it is
what `SaftyFW` assumes.

**On an unwired board GPIO9 floats high and the E-stop reads permanently
asserted**, which is why bring-up needs either the button fitted or a
deliberate jumper to `GND_Safty`. Checked 2026-08-16: **no jumper is currently
fitted anywhere on the `estop` net** in the schematic, so an as-built board
with no switch attached will read STOP until one of the two is added. Do not
"fix" this by inverting the sense in
firmware; inverting it makes the broken-wire case read as *healthy*, which
converts the one thing on this board that is honestly fail-safe into one that
is not. If a build genuinely has no E-stop, fit the jumper — a physical,
visible, removable object — rather than a `#define`.

Debounce in software: 50 ms of settled state before acting on a change.

---

## 6. ADC reference

**ADC_VREF (pin 35) is on a net with nothing but C2 (2.2 µF) to GND_Safty.**
That is the normal arrangement for a Pico module: ADC_VREF is fed on-module
from the 3.3 V rail through a filter, and the header pin is a decoupling /
measurement point. C2 is the intended filter cap.

The practical consequence is that **the ADC's reference is the Pico module's
own 3.3 V rail**, which here is being back-fed from `3.3v_Safty` (§7). So
every current reading is *ratiometric against a rail this board does not
regulate for precision*. Ripple on `3.3v_Safty` appears directly as current-
reading noise.

For a **fault detector** this is entirely acceptable — the guards use
generous thresholds and long windows precisely because the measurement is not
laboratory-grade — but it does mean **the reference is not stable enough to
justify tight current thresholds**, and it must be measured on the bench
before any threshold is committed. See `CURRENT_SENSE.md` §5.

Recommended: use the RP2040's internal band-gap ADC channel and the on-module
`VSYS/3` divider at startup as a coarse sanity check that the rail is where it
should be, and refuse to trust current readings if it is not.

---

## 7. Power — read this before connecting USB

- **`3.3v_Safty` is generated on the main board** by IC3 (LT8631) from
  `12v_Safty` (J19 input, SMAJ24CA TVS, its own 5 V and 3.3 V rails).
- **A1 pin 36 (3V3) is connected directly to `3.3v_Safty`.**
- **A1 pins 39 (VSYS) and 40 (VBUS) are unconnected. Pin 37 (3V3_EN) is
  unconnected.**

So the Pico is **back-powered through its 3V3 output pin**, bypassing its own
RT6150 buck-boost regulator entirely and driving that regulator's output
backwards.

> ⚠️ **Do not connect USB to the Pico while `12v_Safty` is applied, until this
> has been checked on the bench.** With USB attached, VBUS powers the
> on-module RT6150, which will try to drive 3.3 V into the same node that IC3
> is already driving. Two regulators fighting over one rail is at best noisy
> and at worst destroys one of them.

This is a hardware review item as much as a firmware one. Options, in the
order the plan recommends evaluating them:

1. **Fit A1 by soldering, and flash over SWD** (SWCLK/SWDIO/GND on the Pico's
   3-pin debug header) with an external probe — a second Pico running
   `debugprobe`, or a Picoprobe/CMSIS-DAP unit. No USB, no contention, and it
   matches the project's existing OpenOCD-over-JTAG workflow for the ESP32.
   **This is the recommended path** and it is what `TODO.md` phase 0 assumes.
2. Socket A1 and pull it to flash over USB on the bench. Workable, but a
   socketed safety processor in a kiln is a vibration/oxidation liability.
3. Cut the 3V3 feed and power the Pico from `5v_Safty` into VSYS through a
   Schottky, which is the arrangement the Pico datasheet actually endorses for
   external power alongside USB. **This is a board change, and is the right
   fix for the next revision.**

Whichever is chosen, `SaftyFW` must **never** assume USB is available at
runtime — no `stdio_usb`, no USB CDC console as the primary log path. Logging
goes over the isolated UART to the ESP (which already forwards it to the PC),
with an optional SWD/RTT path for development.

---

## 7b. Bench connections for programming, debug and console

What physically has to be attached to A1 beyond the board itself.

### The one device that covers both jobs

A **Raspberry Pi Debug Probe**, or a spare Pico flashed with `debugprobe`
firmware. Either presents two interfaces over a single USB cable to the PC:

- **CMSIS-DAP SWD** — programming, reset, halt/step, memory read/write via
  OpenOCD (`tools/PcTools/TODO.md`, "Debug and programming").
- **A USB-serial UART bridge** — an ordinary console, with no USB connected to
  the Pico itself.

Any CMSIS-DAP, J-Link or ST-Link works for the SWD half, but most do not include
the UART bridge, which is the reason to prefer the Debug Probe.

### SWD — 3 wires

The Pico's DEBUG pads are on the module's short edge and are **not part of the
40-pin numbering**, so the main board does not connect to them at all. They are
accessed on the module directly.

| Probe | Pico DEBUG pad |
|---|---|
| SWCLK | SWCLK |
| GND | GND |
| SWDIO | SWDIO |

> **Check physical access before soldering A1 down.** Fitting a 3-pin header on
> the DEBUG pads at build time is cheap insurance against a safety processor
> that cannot be reprogrammed in place.

### Console UART — 3 more wires, on GP16/GP17

UART1 is taken by the isolated link (GP4/GP5), so the console must use **UART0**.
Of UART0's three possible pin pairs, GP0/GP1 and GP12/GP13 are both partly used
by this board — **GP16/GP17 is the only free pair**:

| Probe UART | Pico | Function |
|---|---|---|
| RX | **GP16** (pin 21) | UART0 TX |
| TX | **GP17** (pin 22) | UART0 RX |
| GND | any GND pin | |

Free GPIOs on this board, for reference: **GP13, GP14, GP15, GP16, GP17, GP18,
GP19, GP20, GP21, GP22**. GP20/GP21 are UART1's alternate pair and cannot be
used — UART1 is already the isolated link.

**RTT over the SWD wires is the alternative** and needs no UART pins at all. Use
it when GP16/GP17 are inaccessible on an assembled board; use the UART bridge
when you want a console without a debugger attached.

### No external reset

**RUN (pin 30) is unconnected**, so there is no hardware reset line. OpenOCD
resets over SWD, which is sufficient for development. A wire from RUN to a
button — or to the probe — is worth considering at build time for the same
reason as the DEBUG header.

### ⚠️ Any PC connection into the safety domain bonds the grounds

This applies to **SWD and USB alike**, and it is the constraint most likely to
be overlooked.

A debug probe, or a USB cable to the Pico, ties `GND_Safty` to the PC's ground.
If the ESP32 is **also** plugged into the same PC, then `GND_Main` and
`GND_Safty` are bonded through the PC and **the isolation barrier is bypassed
for the duration.**

The optocouplers still carry the signals, but the thing they exist to provide —
galvanic separation — is not present while you are debugging. Consequences:

- **Never debug the safety domain from a PC with mains-referenced load wiring
  connected.** Bench work only, with the contactor side disconnected.
- Isolation cannot be *tested* while a probe is attached; that check has to
  happen with everything unplugged.
- Mitigate with an isolated USB hub, or a laptop running on battery, if the
  bonding is a problem for a particular measurement.

**This is the strongest argument for the ESP-relayed log path**
(`firmware/CommonFW/docs/LINK_PROTOCOL.md`, LOG relay): it is the only console route with
**no PC connection to the safety domain at all**, so isolation stays intact and
it works in the deployed system rather than only on the bench.

### If USB on the Pico is wanted anyway

`stdio_usb` is the normal Pico workflow and it does work — but on this board it
needs the power feed fixed first (§7), because A1's 3V3 is back-fed and USB puts
the module's regulator in contention with IC3.

**Bodge for the current board**, which is also the right fix for the next
revision: cut the feed to A1 pin 36 (3V3) and instead feed **`5v_Safty` — already
present on J7 pin 2 — into VSYS (pin 39) through a Schottky**. VSYS is the
endorsed external-power input precisely because the module already diode-ORs it
against USB VBUS, so external power and USB then coexist safely. VSYS accepts
1.8–5.5 V, so 5 V less a Schottky drop is comfortably in range.

The ground-bonding caveat above still applies afterwards. Fixing the power feed
makes USB *electrically* safe; it does not restore isolation.

## 8. Safety thermocouple (J7)

Main board **J7** is a 2x06 1.27 mm header on the **safety** domain, carrying
the Pico's SPI and I2C out to the separate `hardware/SaftyThermocoupleBoard/` project:

| Pin | Signal | | Pin | Signal |
|---|---|-|---|---|
| 1 | 3.3v_Safty (via **R51, 0R**) | | 2 | 5v_Safty |
| 3 | `thermoFault` | | 4 | `thermoDrdy` |
| 5 | `MOSI` | | 6 | `SDA` |
| 7 | `MISO` | | 8 | `SCL` |
| 9 | `CLK` | | 10 | GND_Safty |
| 11 | `CS0` | | 12 | GND_Safty |

The daughterboard is the same design as `hardware/ThermocoupleBoard/` with **one**
MAX31856 populated, and makes its own analog 3.3 V with an LT1962.

Driver consequences (all of which `firmware/KilnFW/docs/MAX31856.md` already documents
for the identical part — **read it, and port rather than reinvent**):

- SPI **mode 1** (CPOL=0, CPHA=1), up to 5 MHz. The RP2040's SPI0 on
  GPIO0/1/2/3 supports this natively; `CS0` is driven manually as a GPIO
  rather than by the SPI block, so a multi-byte register burst stays in one
  chip-select frame.
- `~DRDY` (GPIO12) and `~FAULT` (GPIO11) are **real Pico GPIOs with 10k
  pull-ups** — unlike the main board, where the equivalents hang off the
  SX1509 and cost an I2C transfer to read. The safety processor can therefore
  use a true `~DRDY` **interrupt** and read a conversion the moment it is
  ready. Use it; do not poll.
- **`~FAULT` is a level, not an edge**, in the comparator fault mode this
  family of firmware uses. Read the SR register for the reason.

The I2C bus (GPIO7/8, 2.2k pull-ups, out to J7 pins 6/8) has **no device on
it** in the current design. `SaftyFW` should leave it unconfigured — not
half-initialised — and `TODO.md` tracks it as a deliberate no-op.

---

## 9. Current sense

Three identical channels, sheets `/SaftyProcessor/CurrentSense{,1,2}/`, into
ADC0/1/2. The analog front end is **not** a raw AC feed — it is a precision
rectifier with a 1-second peak hold, and that single fact drives most of the
current-sensing firmware design.

**`CURRENT_SENSE.md` is the full treatment. Read it before writing any ADC
code.** Summary of what is on the board, per channel (channel 1 designators):

| Part | Value | Role |
|---|---|---|
| J13 / J15 / J17 | SJ2-35813B | 3.5 mm jack — CT input (the fitted option) |
| J12 / J14 / J16 | 1935161 | screw-terminal CT input — **DNP** |
| R72 / R78 / R84 | 100R 1W | burden — **DNP** ⇒ a **voltage-output CT** is required |
| R90 | 1M | input bleed / DC return to GND_Safty |
| R43 | 10k | rectifier input resistor |
| C44 | 4.7 pF | input filter |
| D12, D13 | BZX84C3V3 ×2, back-to-back | bidirectional input clamp |
| U8A | AD8542 (dual) | inverting precision half-wave rectifier |
| D14, D15 | SS16FP | the rectifier's two diodes |
| R46 | 7.15k | rectifier feedback ⇒ **gain = 7.15k/10k = 0.715** |
| R77 ∥ C57 | 1M ∥ 1 µF | **peak hold, τ = 1 s decay** |
| U8B | AD8542 | unity-gain buffer → ADC |

The three headline consequences:

1. **The ADC sees a rectified peak envelope, not a waveform and not an RMS
   value.** `V_adc ≈ 0.715 × V̂_ct`. No fast sampling task is needed; a slow
   periodic read is correct.
2. **The response is asymmetric.** Rise is one mains half-cycle (~8 ms); decay
   is exponential with **τ = 1 s**, so ~3 s to fall to 5 %. Every current-based
   guard window must be longer than that decay, or it will fire on the tail of
   a legitimately-ended heating burst.
3. **`R72` being DNP means the CT must be self-burdened (voltage output).**
   Fitting a current-output CT (e.g. an SCT-013-**000**) with no burden puts an
   open-circuit CT secondary across D12/D13 — the clamp survives it, but the
   reading is garbage. The CT is not on the BOM; it is an operator-supplied
   part and its V/A figure is a **calibration constant, never a derived one**.

---

## 10. Stale sources

Recorded so the next person does not repeat the trace:

| Source | Status | Evidence |
|---|---|---|
| `hardware/mainBoard/output/kiln.pdf` | **current** | KiCad 10.0.4; values match the `.kicad_sch` sources; added by commit `c50cded` |
| `hardware/mainBoard/*.kicad_sch` | **current** | spot-checked `CurrentSense.kicad_sch` — R43 10k, R46 7.15k, U8 AD8542, matches the PDF |
| `hardware/mainBoard/kiln.net` | **deleted 2026-08-16** | was dated 2026-07-19; `(source)` was the pre-move `kilnCtl\kiln.kicad_sch`; said U10–U12 are LMV321 with 47k/475k (board has AD8542 with 10k/7.15k), put the safety MAX31856 on the main board (it is on the daughterboard via J7), and put `thermoFault`/`thermoDrdy` on GPIO7/8 (they are on GPIO11/12) |
| `firmware/KilnFW/docs/HARDWARE.md` | **corrected 2026-08-16** | Pico pin map, J7 table and power notes all matched already. The three-optocoupler table's U2/U3 reversal is fixed |
| `firmware/KilnFW/docs/SAFETY_LINK.md` | **corrected 2026-08-16** | "Trap 1" and the R15 boot note were wrong, see §1 |
| `ltspice/currentMon.asc` | **matches the built circuit** | same topology and same 10k/7.15k/1M/1µF values as the schematic; a genuinely useful model, see `CURRENT_SENSE.md` |

**`kiln.net` was deleted** — `TODO.md` item 0.4, done. Leaving a stale netlist
in the tree next to a correct schematic is how the two errors in §1 got into
the documentation in the first place.


---

## Completion checklist

**Verify before writing firmware**
- [x] §1 pin/direction mapping confirmed by manual physical inspection 2026-08-18 (ESP pin4/pin6 out, Pico pin7/pin14 in, Pico pin6 out → ESP pin5 in) — [ ] **electrical toggle/propagation still not confirmed**, see Verification status table
- [x] `KilnFW` safety-UART pins corrected (TX→4, RX→5) and its docs fixed (2026-08-16)
- [x] K4 interlock topology identified from the schematic (J10: 1=NO, 2=COM, 3=NC; §3) — [ ] **still needs confirming on the physical part**, the symbol-drawing convention this reading rests on is not a silkscreened label
- [ ] De-energized K4 proven to open the contactor on the real wiring
- [x] E-stop circuit confirmed normally-closed by design (§5) — [ ] switch or deliberate jumper still needs physically fitting, neither is present today
- [ ] Pico power/flash path decided; **USB-vs-back-fed-3V3 contention checked**
- [ ] Debug probe obtained (Raspberry Pi Debug Probe or a spare Pico running `debugprobe`)
- [ ] **DEBUG pads accessible** — 3-pin header fitted before A1 is soldered down
- [ ] Console decided: UART0 on **GP16/GP17**, or RTT over SWD
- [ ] GP16/GP17 physically reachable, if the UART console is chosen
- [ ] RUN (pin 30) reset wire considered at build time
- [ ] **Ground-bonding understood**: any PC debug connection bypasses the isolation barrier; no mains-referenced load wiring attached while debugging
- [ ] CT type confirmed voltage-output (R72 is DNP)
- [x] `hardware/mainBoard/kiln.net` regenerated or deleted (deleted, 2026-08-16)

**Record**
- [ ] Board revision this trace corresponds to, written at the top
- [ ] Any discrepancy found against this document corrected **here**, not worked around
