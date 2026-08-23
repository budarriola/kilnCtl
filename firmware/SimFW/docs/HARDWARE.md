# SimFW — Fixture Hardware Map

> **Status:** planning — bench harness not yet built · **Last reviewed:** 2026-08-23
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

2. **Gap, not a collision: 10 GPIOs that DESIGN_NOTES.md section 3.6 budgets for have
   no owning file yet.** The 8 direct-GPIO DRDY/`~FAULT` lines (DESIGN_NOTES.md
   3.4/3.6, "decided") and the 2-pin debug UART to the Debug Probe (DESIGN_NOTES.md
   3.6, "same bench pattern as `SaftyFW`") are not claimed anywhere in
   `src/`. **This document assigns them for the first time** (§1) so that
   whoever writes that driver has a number to build against instead of
   picking a sixth independent guess. Whoever writes it must use exactly
   these numbers or update this table in the same commit.
   **Partially closed 2026-08-20 (§0 item 8):** the 4 `~DRDY` lines
   (`DRDY_MAIN_0/1/2`, `DRDY_SAFETY`) now have owner files —
   `spi_emu_a.c`/`spi_emu_b.c` — and use exactly §1's numbers. Still
   unclaimed: the 4 `~FAULT` lines (GPIO19/21/22 main, GPIO27 safety) and
   the 2 debug-UART pins (GPIO0/1). The warning above stands for those 6.

3. **The remaining budget fits with exactly the margin PLAN.md predicted.**
   15 pins already claimed + 10 pins newly assigned here = 25 of the Pico's
   26 header GPIOs, 1 spare (GPIO28) — matching DESIGN_NOTES.md 3.6's "Total = 25 of
   the Pico's 26 header GPIO — tight but it fits" *before* anyone had actually
   done the arithmetic against all four files at once. Had any one of the
   four files picked one GPIO differently, this would not have come out even.

4. **`SaftyFW`'s own debug-UART convention (GP16/GP17) cannot be reused
   verbatim.** `ct_wave_pwm.c` already claims GPIO16 for CT channel 0's PWM
   carrier. Copying `SaftyFW/docs/HARDWARE.md` §7b's GP16/GP17 pair onto this
   fixture would silently collide. This document uses GPIO0/GPIO1 instead
   (UART0's other native pin pair, fully free) — see §1's footnote.

5. **CONTRADICTION found between the two main-board hardware docs on J7 pin
   1 — still matters, for a narrower reason since 2026-08-23.**
   `firmware/KilnFW/docs/HARDWARE.md` ("Safety thermocouple board (J7 -> J1)")
   says J7 pin 1 is "(no connect)". `firmware/SaftyFW/docs/HARDWARE.md` §8
   says J7 pin 1 is `3.3v_Safty (via R51, 0R)`. These cannot both be true of
   the same physical connector. This document follows `SaftyFW`'s version as
   the more recently reviewed, more narrowly scoped safety-domain source
   (which has already corrected the `KilnFW` doc on other J7-adjacent facts —
   see that doc's §10 "Stale sources") — **but this is unverified and must be
   confirmed by continuity check before anything is wired near J7 pin 1.**
   **Revised 2026-08-23 (`DESIGN_NOTES.md` §3.5): the digital isolators this
   used to power from J7's safety rail are gone — the fixture's ground is now
   commoned with the DUT's, so nothing on the fixture needs to *draw* power
   from J7 pin 1 anymore.** The original reason this mattered (feed the
   isolator's isolated side, or leave it unpowered) is retired along with the
   isolators. **It still matters for a narrower reason: not back-feeding an
   unintended rail.** The fixture's J7 harness must leave pin 1 unconnected
   regardless of which doc is right — if it actually carries `3.3v_Safty` and
   a fixture wire lands on it expecting "no connect," that back-feeds the
   safety board's 3.3 V rail from whatever the fixture happens to drive
   there. Confirm by continuity/voltage check before wiring the J7 harness
   (§6 step 7).

6. **Resolved 2026-08-20 (`docs/BOM.md` §6): two relays, not one.** The
   design gap this item originally flagged — the DUT-power relay (DESIGN_NOTES.md
   3.4, "decided") being a single MCP23017 output bit
   (`EXP1_PIN_DUT_POWER`, `i2c_owner.c`) driving one relay, while the main
   board has *two independent* 12 V inputs, J18 (main domain) and J19
   (safety domain), each with its own TVS and no shared copper downstream
   (confirmed: `C9`/`C10` bulk caps on `/5V Regulator/`, `C53`/`C61` on
   `/SaftyRegulator/`, different sheets) — is resolved in favor of **two
   independent relays**, not a common feed downstream of one. Originally
   justified because a single relay bridging both domains would bond
   `GND_Main` and `GND_Safty` through the shared 12 V return — **that
   rationale no longer applies as of 2026-08-23** (`DESIGN_NOTES.md` §3.5):
   the fixture's ground is commoned elsewhere anyway. **The two relays are
   kept regardless**, because they let test scenarios brown out one domain
   independently of the other (a real test case: `SaftyFW` noticing a
   main-side power loss while its own domain stays up, and vice versa) — a
   capability worth keeping on its own merits. **Firmware closed (this
   pass):** `i2c_owner.c`
   now exposes both relays as independently named/commanded outputs
   (`EXP1_PIN_DUT_POWER_MAIN` = exp1 pin 7, `EXP1_PIN_DUT_POWER_SAFETY` =
   exp1 pin 10, one of the 6 spare pins §3.7 recorded) — see §3.7 below and
   `docs/PROTOCOL.md` §5.5 for the protocol-level detail.

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

8. **Resolved 2026-08-20: the `~DRDY` pin contradiction between the SPI
   engines and §1 is settled in favor of §1. The code moved.** Commit
   `4221f70` claimed GPIO21/22/26 for main-side `~DRDY` and GPIO27 for the
   safety side; §1 had already assigned those four to `FAULT_MAIN_1`,
   `FAULT_MAIN_2`, `DRDY_SAFETY` and `FAULT_SAFETY`. `spi_emu_a.c` now uses
   **GPIO2/3/17** and `spi_emu_b.c` uses **GPIO26**, matching §1 exactly; no
   number in this document changed. Three things decided it, in order of
   weight:

   - **Provenance.** §1 (commit `f7230a5`) pre-dates the `~DRDY` code
     (`4221f70`) and pre-assigned all eight lines with an explicit rule —
     "whoever writes that driver must use exactly these numbers or update
     this table in the same commit" (§0 item 2). `4221f70` did neither: it
     never cites this document, and the pin block it edited still carried the
     header "no traced HARDWARE.md exists yet". So it is a later *commit* but
     not a later *decision* — it is precisely the "sixth independent guess"
     §0 item 2 was written to prevent, not a reconsideration of §1.
   - **Domain grouping.** §1 puts the only two isolator-crossing direct-GPIO
     lines (`DRDY_SAFETY` = 26, `FAULT_SAFETY` = 27) adjacent to each other
     and next to the GPIO28 spare, so the six signals crossing the two
     ISO7740DWRs (§3.2) occupy GPIO12–15 + 26/27 and nothing else. The code's
     map made GPIO26 a GND_Main line and GPIO27 a GND_Safty one, interleaving
     domains on the header and pushing `FAULT_SAFETY` down among GPIO2/3/17/19
     with the main-side pins. For a fixture whose reason to exist includes
     keeping `GND_Main` and `GND_Safty` apart (§4, bring-up step 5), §1's
     grouping has a real layout benefit and the code's has none.
   - **Blast radius.** §1's numbers are already cited by the J6 and J7 mating
     tables (§3.1, §3.2), §4's isolation map, and `docs/BOM.md` §2's isolator
     channel split. The code's numbers appear nowhere outside the two `.c`
     files. Moving the code touched two `#define` blocks; moving the document
     would have touched five tables across two files.

   **Hard constraints did NOT settle it** — recorded so nobody assumes they
   did. `~DRDY` is a plain SIO pin toggled between output-low and input-Hi-Z
   (the open-drain emulation, `max31856_pio_engine.c`'s
   `init_input_pin`/`drdy_sync`); §2's PIO adjacency rules and
   `config_is_sane()`'s checks cover `SCLK`/`MOSI`/`CS` only and say nothing
   about `drdy_gpio[]`. **Both assignments were electrically legal.** The
   choice was made on provenance and layout, and — like everything else here
   — remains **provisional and unverified against hardware**.

9. **Full pin-map re-check done in the same pass (2026-08-20): no further
   two-owner GPIO collisions.** Every claim in `src/` was enumerated against
   §1 — `i2c_owner.c` 4/5, `spi_emu_a.c` 6–11 + 2/3/17, `spi_emu_b.c` 12–15 +
   26, `ct_wave_pwm.c` 16/18/20 — 18 distinct GPIOs, no overlap, and the
   7 pins §1 assigns with no owner file yet (0, 1, 19, 21, 22, 27, plus the
   GPIO25 LED) are claimed by nothing in code. Three *near*-misses were
   checked and are not collisions, recorded so they are not re-litigated:

   - **PWM pacer slice 3 shadows GPIO6/7/22/23.** `ct_wave_pwm.c`'s pacer
     slice is unbound to any pin, but slice 3's own candidate outputs are
     GPIO6/GPIO22 (channel A) and GPIO7/GPIO23 (channel B). GPIO6/7 are SPI
     bus A `SCLK`/`MOSI` (PIO function), GPIO22 is `FAULT_MAIN_2` (SIO),
     GPIO23 is not a header pin — none is muxed to `GPIO_FUNC_PWM`, so the
     free-running pacer reaches no pin. **Latent trap:** any future
     `gpio_set_function(6|7|22, GPIO_FUNC_PWM)` would silently put the pacer
     carrier on a claimed line. No slice is free of claimed pins at 25-of-26
     occupancy, so this is inherent, not fixable by moving the pacer.
   - **CT channel-B pins.** The three CT carriers are on even GPIOs
     (16/18/20 = channel A of slices 0/1/2); the matching channel-B pins are
     GPIO17/19/21 = `DRDY_MAIN_2`/`FAULT_MAIN_0`/`FAULT_MAIN_1`. `arm_dma()`
     writes only the `CC` register's low halfword and never sets those pins
     to PWM function, so channel B is inert on all three. This is the same
     adjacency §1b.4's slice-packing optimization would exploit — which is
     exactly why that optimization needs GPIO17 to move first.
   - **MCP23017 #1 bit allocation** (§3.7): pins 0–10 all distinct, 11–15
     spare, matching `i2c_owner.c`. No two-owner bit.

---

## 1. Authoritative Pico pin map

Every GPIO on the fixture's Pico. "Owner file" is blank where this document
is the first place the assignment is made (§0 item 2) — the future driver
that claims that pin must cite this table.

| GPIO | Signal | Owner file | Isolation domain | Main-board destination |
|---|---|---|---|---|
| 0 | Debug UART0 TX → Debug Probe RX | *(none yet — assign here)* | GND_Main (bench-local, not board-referenced) | n/a (bench probe only) |
| 1 | Debug UART0 RX ← Debug Probe TX | *(none yet)* | GND_Main | n/a |
| 2 | `DRDY_MAIN_0` (open-drain) | `spi_emu_a.c` (cites this table, §0 item 8) | GND_Main | J6 pin 17 (`thermoDrdy_0`) |
| 3 | `DRDY_MAIN_1` (open-drain) | `spi_emu_a.c` | GND_Main | J6 pin 15 (`thermoDrdy_1`) |
| 4 | I2C0 SDA | `i2c_owner.c` (provisional, own header comment) | GND_Main | n/a (internal: MCP23017 #1/#2) |
| 5 | I2C0 SCL | `i2c_owner.c` | GND_Main | n/a (internal) |
| 6 | SPI bus A SCLK | `spi_emu_a.c` | GND_Main | J6 pin 9 (`CLK`) |
| 7 | SPI bus A MOSI (fixture input) | `spi_emu_a.c` | GND_Main | J6 pin 11 (`MOSI`) |
| 8 | SPI bus A MISO (fixture output, tri-stated) | `spi_emu_a.c` | GND_Main | J6 pin 10 (`MISO`) |
| 9 | SPI bus A CS0 | `spi_emu_a.c` | GND_Main | J6 pin 8 (`CS0`) |
| 10 | SPI bus A CS1 | `spi_emu_a.c` | GND_Main | J6 pin 7 (`CS1`) |
| 11 | SPI bus A CS2 | `spi_emu_a.c` | GND_Main | J6 pin 6 (`CS2`) |
| 12 | SPI bus B SCLK | `spi_emu_b.c` | GND_Safty (direct, no isolator — `DESIGN_NOTES.md` §3.5) | J7 pin 9 (`CLK`) |
| 13 | SPI bus B MOSI (fixture input) | `spi_emu_b.c` | GND_Safty (direct, no isolator — `DESIGN_NOTES.md` §3.5) | J7 pin 5 (`MOSI`) |
| 14 | SPI bus B MISO (fixture output) | `spi_emu_b.c` | GND_Safty (direct, no isolator — `DESIGN_NOTES.md` §3.5) | J7 pin 7 (`MISO`) |
| 15 | SPI bus B CS0 | `spi_emu_b.c` | GND_Safty (direct, no isolator — `DESIGN_NOTES.md` §3.5) | J7 pin 11 (`CS0`) |
| 16 | CT PWM ch0 (zone 0 carrier) | `ct_wave_pwm.c` | crosses transformer → floating | J13 (via RC + isolation xfmr) |
| 17 | `DRDY_MAIN_2` (open-drain) | `spi_emu_a.c` | GND_Main | J6 pin 13 (`thermoDrdy_2`) |
| 18 | CT PWM ch1 (zone 1 carrier) | `ct_wave_pwm.c` | crosses transformer → floating | J15 (via RC + isolation xfmr) |
| 19 | `FAULT_MAIN_0` (open-drain) | *(none yet)* | GND_Main | J6 pin 18 (`thermoFault_0`) |
| 20 | CT PWM ch2 (zone 2 carrier) | `ct_wave_pwm.c` | crosses transformer → floating | J17 (via RC + isolation xfmr) |
| 21 | `FAULT_MAIN_1` (open-drain) | *(none yet)* | GND_Main | J6 pin 16 (`thermoFault_1`) |
| 22 | `FAULT_MAIN_2` (open-drain) | *(none yet)* | GND_Main | J6 pin 14 (`thermoFault_2`) |
| — | PWM pacer slice 3 (no GPIO bound) | `ct_wave_pwm.c` | n/a | n/a |
| — | DMA_IRQ_1 (not a pin) | `ct_wave_pwm.c` | n/a | n/a |
| 25 | Heartbeat LED (on-board, not a header pin) | *(none yet, DESIGN_NOTES.md 3.6)* | n/a | n/a |
| 26 | `DRDY_SAFETY` (open-drain) | `spi_emu_b.c` (cites this table, §0 item 8) | GND_Safty (direct, no isolator — `DESIGN_NOTES.md` §3.5) | J7 pin 4 (`thermoDrdy`) |
| 27 | `FAULT_SAFETY` (open-drain) | *(none yet)* | GND_Safty (direct, no isolator — `DESIGN_NOTES.md` §3.5) | J7 pin 3 (`thermoFault`) |
| **28** | **SPARE — the one pin DESIGN_NOTES.md 3.6 leaves free** | — | — | — |

**25 of 26 header GPIOs assigned, 1 spare (GPIO28).** (GPIO23/24 are not
header pins on a stock Pico; GPIO25 is the on-board LED, also not a header
pin — both excluded from the 26-pin budget, per DESIGN_NOTES.md 3.6's own framing.)

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

## 1b. DMA channel budget

The RP2040 has **12 DMA channels** (`NUM_DMA_CHANNELS`), a single global pool
with no per-peripheral partitioning, and **2 DMA IRQ vectors**. Unlike GPIOs,
nothing in the build reserves one implicitly: `CMakeLists.txt` links
`hardware_dma` and nothing else that claims a channel (no pico-sdk stdio
backend is enabled, and TinyUSB's RP2040 device port is FIFO-driven), so every
claim in the fixture comes from one of the two files below. Same convention as
§1: this table is authoritative, and a third claimant must update it in the
same commit.

### 1b.1 The claim table

Both owners claim through `dma_claim_unused_channel()`, so the *identities*
below are allocation-order outcomes, not fixed assignments. What is fixed is
the **count** each subsystem takes and the vector it owns.

| Owner file | Role | Count | Scales with | DMA IRQ |
|---|---|---|---|---|
| `ct_wave_pwm.c` | CT sine carrier: streams a 256-entry `uint16_t` duty table into one PWM slice's channel-A compare halfword, paced by pacer slice 3's wrap DREQ | 3 | `CT_WAVE_PWM_NUM_CHANNELS` (1 per CT zone) | `DMA_IRQ_1` |
| `max31856_pio_engine.c` (bus A, PIO0) | `dma_sniff[i]` — one per RX state machine, armed on that SM's RX-FIFO-not-empty DREQ, `transfer_count = 1`, captures the next transaction's address word into `bus->addr_capture`, chains to load | 3 | `channel_count` (1 per emulated chip) | `DMA_IRQ_0` |
| `max31856_pio_engine.c` (bus A, PIO0) | `dma_load` — no DREQ, fires the instant sniff chains to it; its one write to the data channel's `al3_read_addr_trig` both supplies the read address and starts it, and raises `DMA_IRQ_0` | 1 | fixed, per bus | `DMA_IRQ_0` |
| `max31856_pio_engine.c` (bus A, PIO0) | `dma_data` — 512-byte read-address ring into the TX FIFO, paced by TX-FIFO-not-full; this is the channel that actually puts response bytes on MISO | 1 | fixed, per bus | `DMA_IRQ_0` (status polled, IRQ not enabled on it) |
| `max31856_pio_engine.c` (bus B, PIO1) | `dma_sniff[0]` | 1 | `channel_count` | `DMA_IRQ_0` |
| `max31856_pio_engine.c` (bus B, PIO1) | `dma_load` | 1 | fixed, per bus | `DMA_IRQ_0` |
| `max31856_pio_engine.c` (bus B, PIO1) | `dma_data` | 1 | fixed, per bus | `DMA_IRQ_0` |

### 1b.2 The arithmetic

```
ct_wave_pwm.c        = CT_WAVE_PWM_NUM_CHANNELS                     = 3
bus A (spi_emu_a.c)  = SPI_EMU_A_CHANNEL_COUNT (3) + load 1 + data 1 = 5
bus B (spi_emu_b.c)  = SPI_EMU_B_CHANNEL_COUNT (1) + load 1 + data 1 = 3
                                                               total = 11
RP2040 total                                                         = 12
                                                             SPARE   =  1
```

General form, so the next change can be checked without re-reading the code:

```
channels = 3 (CT zones) + sum over buses of (chips_on_bus + 2)
```

**11 of 12 claimed, 1 spare — independently verified against the source, not
carried over from the commit message that first stated it.** The two vectors
are disjoint (`DMA_IRQ_1` for CT, `DMA_IRQ_0` shared by both SPI buses via
`s_dma_irq_installed`), and neither driver pokes another owner's channel
registers.

Claim order at boot: `ct_wave_pwm_init()` runs inside `wave_owner_start()`
**before** `vTaskStartScheduler()`, while both SPI engines claim from their own
task bodies **after** it. So CT takes the low channel numbers and the SPI
engines take the rest — but nothing depends on that, and nothing should.

### 1b.3 Why none of the 11 is slack

Every one of the 8 SPI-side channels sits in the first-byte path that
`docs/SPI_ACCESS_AUDIT.md` §9 exists to describe. That path was moved off the
CPU because an ISR-staged version could not meet the deadline, so "free a
channel by having the CPU do it" is not available here at any price:

* **Sniff channels cannot be pooled across chips.** A DMA channel waits on
  exactly one DREQ. Each emulated chip has its own RX state machine and
  therefore its own RX-FIFO-not-empty DREQ, and all of a bus's chips are idle
  and eligible simultaneously — the fixture does not know which CS will fall
  next. One pooled sniffer would catch one chip and miss the other two.
* **The load channel is the one deliberate luxury, and it is still not
  free-able.** `max31856_pio_engine.c`'s own header notes that sniff could
  write `al3_read_addr_trig` directly, saving the hop and ~5 sysclk cycles.
  That would free 1 channel per bus (2 total) *and* be faster — but it
  destroys `bus->addr_capture`, the only place the address byte survives as a
  value in SRAM. `handle_load_done()` needs it to set `txn_is_write`, to call
  `max31856_regs_cs_assert()`, and hence for the whole write-transaction and
  ~DRDY-release path. Recovering it instead from the data channel's
  `read_addr`/`transfer_count` at CS rise depends on abort semantics that
  cannot be tested without fixture hardware. **Not recommended.**
* **The data channel is per-bus and per-PIO by construction** (its DREQ is
  that bus's TX FIFO). Nothing to share.

### 1b.4 The one channel that could be freed, if one is ever needed

**`ct_wave_pwm.c` can go from 3 channels to 2, at zero CPU cost, by pairing
two CT zones onto one PWM slice.** An RP2040 PWM slice's `CC` register packs
channel A in bits [15:0] and channel B in [31:16], so a single 32-bit DMA
transfer sets *both* duties at once. Two zones moved to an adjacent even/odd
GPIO pair (a slice's A and B outputs, e.g. GPIO16/17) would share one DMA
channel streaming a 256-entry `uint32_t` interleaved table; the third zone
keeps its own slice and channel. Total table bytes are unchanged, the pacer
DREQ is unchanged, and the CPU stays entirely out of the loop.

Costs, stated so the trade is visible: the two paired zones share one
completion IRQ and therefore one zero-crossing table-swap event (harmless —
they are already phase-locked to the same pacer slice); GPIO17 is assigned to
`DRDY_MAIN_2` in §1 and — as of §0 item 8 — is now *driven* by `spi_emu_a.c`
rather than merely reserved, so moving it is a code change plus a §1/§3.1
edit, not a paper one; and the clean
one-zone-one-slice symmetry `ct_wave_pwm.c`'s header argues for is lost.

**All three zones cannot collapse onto one channel.** A slice has only two
channels, and a single DMA channel cannot write three slices' `CC` registers:
they are 20 bytes apart, which is neither a power-of-two write ring nor a
uniform increment.

A read-address ring on the CT channels would *not* free a channel either. It
would let the table free-run without re-arming, which would retire the
`DMA_IRQ_1` handler — but that handler is what performs the zero-crossing-gated
table swap, which is the feature.

### 1b.5 What happens today if a claim fails

**Fixed.** Every `dma_claim_unused_channel()` call site still passes
`required = false` (so each one can name exactly which subsystem/channel
failed before halting, rather than relying on the SDK's generic panic
message), but a failed claim is checked explicitly and routed through
`drivers/simfw_fatal.h`'s `simfw_fatal(subsystem, reason_fmt, ...)` instead of
returning `false` up a chain `main.c` used to discard with `(void)`.
`simfw_fatal()` never returns: it blinks the onboard LED (GPIO25 — its first
owner, since no heartbeat body has ever claimed it) into a fast burst then
solid-on, then calls `panic()` with the formatted subsystem+reason message —
the same halt mechanism the neighbouring `pio_claim_unused_sm(pio, true)`
calls already use for PIO state-machine exhaustion, closing the asymmetry
recorded below. Verified by temporarily draining the whole 12-channel pool
before `ct_wave_pwm_init()`'s per-zone claim and confirming (compiled ARM
disassembly + `.rodata` inspection) that the forced failure reaches
`simfw_fatal()` with the correct subsystem name and message, not a silent
`return false`; reverted after confirming.

| Call site | Arg | On failure |
|---|---|---|
| `ct_wave_pwm.c` (per CT zone) | `false` | `simfw_fatal("ct_wave_pwm", "DMA channel exhausted claiming zone %u of %u ...")`. Halts before `vTaskStartScheduler()` — this claim runs from `main()`, pre-scheduler, so no core is left running to enumerate USB or answer commands. |
| `max31856_pio_engine.c` (`dma_data`) | `false` | `simfw_fatal("max31856_pio_engine", "dma_data channel exhausted on pio%u ...")`. |
| `max31856_pio_engine.c` (`dma_load`) | `false` | `simfw_fatal("max31856_pio_engine", "dma_load channel exhausted on pio%u (dma_data already claimed; ...)")`. |
| `max31856_pio_engine.c` (per `dma_sniff[i]`) | `false` | `simfw_fatal("max31856_pio_engine", "dma_sniff[%u] channel exhausted on pio%u, channel_count=%u ...")` — halting here (rather than returning `false`) is also what closes the "mid-loop sniff failure is worse than an idle bus" hazard this section used to describe: `simfw_fatal()` never returns, so the half-initialised bus state (`s_bus_for_pio_index[]` published, `dma_load` armed with `DMA_IRQ_0` enabled) is never reachable by the other bus's shared `irq_handler_dma()`. |
| `max31856_pio_engine.c` (`!publish_base(...)`, RX SM claim loop) | n/a (not a claim) | **Fixed, this pass.** `simfw_fatal("max31856_pio_engine", "publish_base failed for channel %u on pio%u ...")` instead of `return false` with `i` already-claimed-and-enabled RX state machines leaked. See the non-DMA-leak note below for why `simfw_fatal()` (not an unwind loop) is the right fix here too. |

Three things about this posture are worth stating plainly:

1. **The failure is no longer silent, for the two DMA-exhaustion call sites.**
   `main.c`'s comment about "nothing that can fail beyond `xTaskCreate()`" has
   been corrected in place; what a `_start()` function can still fail on
   (and what its caller's `(void)` still discards) is a plain FreeRTOS
   allocation failure (`xTaskCreate()`/`xQueueCreate()`/
   `xSemaphoreCreateMutex()` under heap pressure) — unrelated to DMA, and
   still "logged-and-continued, nothing fatal" pending a real `log_task` body.
2. **The unwind gap is moot for DMA-caused partial state, not separately
   patched.** `simfw_fatal()` is `noreturn` and halts synchronously inside the
   failing claim, so execution can never continue to a point where a
   previously-claimed-but-now-stranded DMA channel, PIO SM, or PIO program
   offset gets reused or serviced by anything else — there is no return path
   left to unwind *from*.
3. **The non-DMA leak this section used to flag — `max31856_pio_engine_init()`'s
   `!publish_base(...)` failure path, mid-loop over already-enabled RX state
   machines — is fixed too, and by the same mechanism, not by unwinding.**
   `publish_base()` can only fail on `MAX31856_RESP_IMAGE_ALIGN` misalignment,
   and `config_is_sane()` — this function's very first check — already walks
   every `cfg->images[i][b]`, including the exact pointer
   (`cfg->images[i][0]`, unchanged by the plain assignment that becomes
   `bus->images[i][0]`) `publish_base()` is later handed, against the
   identical alignment test. So the branch is **proven unreachable** for any
   `cfg` that made it past `config_is_sane()`: firing it means that guarantee
   was violated after the fact (memory corruption of `bus`/`cfg` between the
   two checks), not a normal runtime condition — there is no well-defined
   state to unwind *back to* when the invariant the caller relied on is
   already false. Routed through `simfw_fatal()` rather than adding unwind
   logic for a path that cannot be taken by construction, matching the
   DMA-exhaustion precedent's "should be impossible; treat firing as a
   programming error" posture. Verified by forcing `publish_base()` to always
   fail (temporarily corrupting its `base` computation) and confirming via
   ARM disassembly + `.rodata` inspection that the RX-SM-claim loop's first
   iteration reaches `simfw_fatal()` with the new message string, not a
   silent `return false`; reverted after confirming.

**Cross-core halt: fixed, this pass.** `panic()` (like the PIO precedent it
matches) halts only the CALLING core. `ct_wave_pwm.c`'s claim runs
pre-scheduler (single core), so it halts boot outright — no core-1 task has
been launched yet, so there is nothing to notify. `spi_emu_a.c`/
`spi_emu_b.c`'s claims run from tasks pinned to `SIMFW_CORE_RT_PATH` (core 1,
`task_priorities.h`) *after* the scheduler has started, so a claim failure
there used to freeze only core 1 while core 0 (`usb_owner`/`telemetry`/
`cmd_task`/etc., `SIMFW_CORE_ELASTIC_PATH`) kept running and USB stayed
enumerated. `simfw_fatal()` now pushes a sentinel word over the RP2040 SIO
inter-core FIFO (`pico_multicore`) before doing anything else; a
`SIO_IRQ_PROC0` handler installed once from `main()` (core 0, before
`vTaskStartScheduler()` — the only point core 0 is guaranteed to be the only
running core) receives it and halts core 0 too: interrupts disabled, spin
forever, no scheduler tick, no task runs again. A **true whole-board halt**
was chosen over the alternative of leaving core 0 alive but having
`telemetry`/`cmd_task` refuse to report "healthy": the onboard LED
`simfw_fatal()` already drives solid-on is the bench-visible "do not trust
this fixture" signal a zero-tooling bench operator relies on, and a board
that still answers USB traffic — even truthfully, even while reporting its
own death — undermines that signal more than a "logged and continued"
approach helps a remote operator who is not there to read a log. Only the
core-1-originated direction is wired up (no core-0-originated `simfw_fatal()`
call exists today); a future one would need the mirror (a `SIO_IRQ_PROC1`
handler installed from code that actually runs on core 1) added at that time.
Verified by disassembling the linked ELF: `simfw_fatal()` pushes
`0xFA7A1004` before `panic()`; `simfw_fatal_install_cross_core_halt()` is
called from `main()` before `vTaskStartScheduler()`; the installed handler
compares each popped FIFO word against the same `0xFA7A1004` constant and,
on match, executes `cpsid i` + an unconditional self-branch (never returns).
As a negative control, the comparison constant was temporarily changed to a
mismatched value and rebuilt: the disassembly then showed the handler
draining and returning normally instead of reaching the halt block,
confirming the branch is a genuine equality gate, not a tautology. Both
forcing edits reverted after confirming.

**Formerly an asymmetry, now consistent:** the PIO claims in the same
functions use `pio_claim_unused_sm(pio, true)` — `required = true`, which
**panics**. DMA claims now reach the same halt via `simfw_fatal()` on
exhaustion (`required = false`, checked explicitly, so the message can name
the exact subsystem/channel rather than relying on the SDK's generic
"No PIO state machines available"-style text). The `if (sm < 0) return false`
guards after the PIO claims, and `spi_emu_a.c`'s comment explaining the
idle-loop fallback "if the PIO block cannot supply 4 state machines", both
still describe a path that cannot be taken — true before this pass and true
after it, for the same reason (`required = true` never returns negative).

### 1b.6 Correction to §0 item 7

§0 item 7's first bullet states that `spi_emu_a/b.c` and
`max31856_pio_engine.c` "use PIO FIFOs and a GPIO IRQ, never DMA", and that
`ct_wave_pwm.c`'s choice of `DMA_IRQ_1` was therefore an unnecessary guess.
That was true when §0 was written and is **false as of commit `4221f70`**
(`docs/SPI_ACCESS_AUDIT.md` §9's DMA-fed Plan B). The SPI engines now claim 8
of the 12 channels and own `DMA_IRQ_0`. `ct_wave_pwm.c`'s defensive pick of
`DMA_IRQ_1` turned out to be exactly right, and is now load-bearing rather
than harmless. §1's pin map already carries the `DMA_IRQ_1` row; `DMA_IRQ_0`
belongs to `max31856_pio_engine.c` and binds no pin.

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
plugs into J6 in its place (DESIGN_NOTES.md §2's connection diagram), so the
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

### 3.2 Fixture → J7 (safety-side thermocouple bus)

Main board **J7** is a 2×06, 1.27 mm header, **straight pin-for-pin** to the
safety daughterboard's **J1** (no reversal, unlike J6) —
`firmware/SaftyFW/docs/HARDWARE.md` §8.

**Direct GPIO, no digital isolator** (`DESIGN_NOTES.md` §3.5, decided
2026-08-23: the fixture's ground is commoned with the DUT's) — wired the same
way as SPI bus A on J6. Series resistors give the same first-plug-in
protection bus A already gets (`PLAN.md` §15).

| J7 pin | Signal | Fixture GPIO | Notes |
|---|---|---|---|
| 1 | `3.3v_Safty` (via R51, per `SaftyFW` doc) — **see §0 item 5, contradicts `KilnFW` doc** | — | leave unconnected; nothing on the fixture needs to draw power from this pin (`DESIGN_NOTES.md` §3.5) |
| 2 | `5v_Safty` | — | |
| 3 | `thermoFault` | GPIO27 (`FAULT_SAFETY`) | direct GPIO |
| 4 | `thermoDrdy` | GPIO26 (`DRDY_SAFETY`) | direct GPIO |
| 5 | `MOSI` | GPIO13 | direct GPIO, fixture input |
| 6 | `SDA` | — | not wired |
| 7 | `MISO` | GPIO14 | direct GPIO, fixture output |
| 8 | `SCL` | — | not wired |
| 9 | `CLK` | GPIO12 | direct GPIO, fixture input |
| 10 | GND_Safty | fixture GND_Main | tied to the fixture's common ground (`DESIGN_NOTES.md` §3.5) |
| 11 | `CS0` | GPIO15 | direct GPIO, fixture input |
| 12 | GND_Safty | fixture GND_Main | same as pin 10 |

### 3.3 Fixture → CT jacks (J13/J15/J17)

Each channel: fixture PWM GPIO → 2-pole RC low-pass (DESIGN_NOTES.md 3.3) →
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
| K4 | J10 | `EXP1_PIN_K4` = 4 | **contact is in GND_Safty; senses directly, no opto stage, since 2026-08-23** (`DESIGN_NOTES.md` §3.5) — wires exactly like K1/K2/K3/K5 |

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
(DESIGN_NOTES.md §3.4): **`EXP1_PIN_ESTOP_DRIVE` (exp1 pin 6, `i2c_owner.c`)
wires directly across J1's two terminals through a 1 kOhm series protection
resistor, in place of the button** — no switching element in between any
more. (The CPC1017N optoMOS that used to sit here is removed, 2026-08-23 —
it was itself driven by this same GPA6 bit, so it was only ever an extra
stage crossing the fixture/DUT ground boundary, and that boundary no longer
exists per DESIGN_NOTES.md §3.5.)

Because SaftyFW's GPIO9 side has its own fail-safe pull-up (R10, 1 kOhm to
`3.3v_Safty` — `SaftyFW/docs/HARDWARE.md` §5), the fixture cannot simply
write a level to GPA6: driving it high to represent "open" would fight R10
into a different supply rail. Instead `i2c_owner_set_estop()` toggles GPA6's
**direction**: loop closed (healthy) = GPA6 configured as OUTPUT driving
LOW; loop open (STOP) = GPA6 configured as INPUT, i.e. high-Z, letting R10
pull GPIO9 (and this side of the resistor) high. `configure_exp1()` sets
GPA6 to **INPUT at boot** — matching both the MCP23017's own POR default
(IODIR resets to all-input) and the board's fail-safe intent: an unpowered
or un-initialised fixture must read STOP, never a falsely-healthy closed
loop.

### 3.7 Fixture → DUT 12 V power

| From | Via | To |
|---|---|---|
| Bench supply (per-domain channel, or a dual-output supply) | fixture power-in connector → fixture relay #1 (`EXP1_PIN_DUT_POWER_MAIN`, exp1 pin 7, existing) → fixture power-out connector | J18 (main 12 V in) |
| Bench supply (independent channel) | fixture power-in connector → fixture relay #2 (`EXP1_PIN_DUT_POWER_SAFETY`, exp1 pin 10, added this pass — was a spare pin) → fixture power-out connector | J19 (safety 12 V in) |

**Resolved (§0 item 6, `docs/BOM.md` §6): two independent relays, not one
relay with a common downstream feed.** Originally justified by avoiding
bonding `GND_Main` and `GND_Safty` through a shared 12 V return — that
rationale no longer applies now that the fixture's ground is commoned
elsewhere anyway (`DESIGN_NOTES.md` §3.5). **The two relays are kept
regardless**: they give independent per-domain power-cycle/brownout testing
that a single shared relay could never produce (`SaftyFW` noticing a
main-side power loss while its own domain stays up, and vice versa) — a
capability worth keeping on its own merits. `i2c_owner.c` implements both
relays' control bits, each with its own named setter/getter
(`i2c_owner_set/_get_dut_power_main_on()`, `..._safety_on()`) and its own
protocol command (`DUT_POWER_SAFETY_SET`/`GET` = `0x09`/`0x0A`, alongside the
pre-existing `DUT_POWER_SET`/`GET` = `0x06`/`0x08` which keep their original
main-domain-only meaning — see `docs/PROTOCOL.md` §5.5). There is
deliberately no combined "set both" call, so a caller always makes the
two-domain decision explicitly rather than getting it as a side effect of
one legacy command. Consumed 1 of the 6 spare pins recorded in this
section's earlier pass, leaving 5 spare (exp1 pins 11..15). **Inrush sizing
is a separate, still-open item** — see §5's DUT power relay row and
`docs/BOM.md` §6's estimate (~60 A / ~190 µs from ~940 µF per-domain bulk
capacitance and an assumed ~0.2 Ω source resistance) — this is an estimate
pending a bench scope/current-probe capture, not a measured figure.

---

## 4. Ground-domain map (revised 2026-08-23 — fixture ground commoned with the DUT's)

Per `DESIGN_NOTES.md` §3.5, restated against the pin map above. The fixture's
ground is commoned with the DUT's: no galvanic isolation across the fixture
boundary except the CT channels, which stay floating like a real CT.

| Fixture signal group | Domain | Notes |
|---|---|---|
| I2C0 (both MCP23017s), SPI bus A, `DRDY_MAIN_*`/`FAULT_MAIN_*`, SPI bus B, `DRDY_SAFETY`/`FAULT_SAFETY`, relay sense K1/K2/K3/K5/K4, `Fault` line sense, E-stop direct drive, J20 IO_3/IO_4, debug UART, DUT-power relay control (both relays) | GND_Main, bonded to GND_Safty at the fixture | All direct GPIO/direct sense — no digital isolators, no optocoupler, no optoMOS. SPI bus B's two TI ISO7740DWR isolators, K4's 4N35 opto, and the E-stop loop's CPC1017N optoMOS are all removed (`DESIGN_NOTES.md` §3.5) — each was only ever crossing this same now-nonexistent ground boundary |
| 3× CT channels | floating (neither domain) | isolation transformer, **1:1** (revised 2026-08-23, reversing the 2026-08-20 revision to ~3:1 — the earlier change backed out a specific CT's rated current from the ADC's clipping voltage; the board's own sense input is full-scale at ≈1 Vrms regardless of which CT is fitted, and 1:1 delivers that with margin. Tradeoff: `CURRENT_FLAG_CLIPPED` is not exercisable at 1:1 — see `DESIGN_NOTES.md` §3.3 and `docs/BOM.md` §3), per channel |

**Standing rule retired 2026-08-23 (`DESIGN_NOTES.md` §3.5):** the removable
ground jumper and the old "run the standard library with it out" rule no
longer apply — the fixture's ground is always common with the DUT's, by
design. Bring-up step 5 (§6) is rewritten accordingly.

---

## 5. External components (not on the Pico)

| Part | Qty | Role | Sizing status |
|---|---|---|---|
| CT isolation transformer, **1:1** audio/isolation (revised 2026-08-23, reversing the 2026-08-20 revision to ~3:1) | 3 | One per CT channel, between the RC-filtered PWM output and the J13/J15/J17 jack | **Ratio corrected:** the board's sense input is full-scale at ≈1 Vrms (a board property, independent of whichever CT is installed — `SaftyFW/docs/CURRENT_SENSE.md` §2), fully covered at 1:1 with margin from an estimated ~1.5 Vpk usable Pico drive — see `docs/DESIGN_NOTES.md` §3.3, `docs/BOM.md` §3. Tradeoff accepted: `CURRENT_FLAG_CLIPPED` is not exercisable at 1:1. **Still open:** the candidate part's (Triad TY-300P) actual turns ratio is unconfirmed against its datasheet, and the compiled-in CT calibration table remains all-uncalibrated (identity behavior) — the calibration *mechanism* itself now exists, see §6 step 4's correction (M-D) |
| Relay-sense wetting circuit | 5 | One per relay (K1/K2/K3/K5/K4, all direct, since 2026-08-23) into MCP23017 #1 inputs | **Resolved (`docs/BOM.md` §4):** no dedicated wetting supply needed — MCP23017's internal 100 kΩ pull-ups (`GPPU`) plus a 1 kΩ series resistor per contact, all five relays wired identically. K4's 4N35 opto stage is removed (`DESIGN_NOTES.md` §3.5) — the ground-commoning decision means it no longer needs to cross a domain boundary |
| E-stop series resistor | 1 | 1 kΩ, in series between `EXP1_PIN_ESTOP_DRIVE` and J1's E-stop loop | **Resolved (`docs/BOM.md` §5, 2026-08-23):** CPC1017N optoMOS removed — GPA6 drives the loop directly (output-low = closed / input = open) through the same 1 kΩ series-resistor protection every other fixture signal already gets; loop current when closed ≈3.3 mA (3.3 V / 1 kΩ pull-up) |
| DUT 12 V power relay | **2** (resolved 2026-08-20, §0 item 6 — one per domain, not one shared) | Fixture's own 12 V feed to J18 (relay #1) and J19 (relay #2) independently | Part: Omron G5LE-14-DC12 (10 A/250 VAC continuous, already used elsewhere on the main board), per `docs/BOM.md` §6. **Still open (PLAN.md §11 item 5):** inrush rating vs the board's actual inrush not measured — `docs/BOM.md` §6 estimates ~60 A / ~190 µs from ~940 µF per-domain bulk capacitance and an assumed ~0.2 Ω source resistance; this is an estimate, not a measurement, and needs a scope/current-probe capture at first power-on |
| MCP23017 | 2 | 0x20 (fixed-role pins) and 0x21 (spare) on I2C0 | Sized; already in code |
| PCA9685 (optional) | 0–1 | PWM/LED stimulus, not required for the base feature set | Not needed unless a test calls for analog-ish stimulus |

---

## 6. Bring-up checklist

Expanded from PLAN.md section 14. Each step's pass criterion is concrete;
run the listed `kilnsim` CLI command or MCP tool where one exists today (from
`tools/PcTools/src/kilnsim/cli.py` and `mcp_server.py` — commands not yet
implemented are called out as gaps rather than invented). **This stays a
concise per-step checklist, not the procedure** — for the actual first
bench session (pre-flight hardware list, wiring order, troubleshooting
tables, what to record afterward), follow `docs/BENCH_RUNBOOK.md`, which
expands this same ten-step order in full.

- [ ] **Step 1 — Pico alone: USB CDC + protocol + heartbeat.**
  Pass: `kilnsim state` connects (auto-detected port or `--port`), returns a
  JSON telemetry snapshot with no transport error, protocol/firmware version
  fields populated.
  Command: `kilnsim state`

- [ ] **Step 2 — Expanders on I2C: read/write, interrupt lines if used.**
  Pass: both MCP23017s ACK on I2C0 (0x20, 0x21); writing then reading back a
  spare, non-reserved pin on either expander round-trips.
  Command: `kilnsim io write <pin> on|off --exp 0|1` /
  `kilnsim io read <pin> --exp 0|1` — the CLI's `io` group now exists
  (`tools/PcTools/src/kilnsim/cli.py`); this was the MCP-only workaround
  noted in an earlier revision of this checklist, now stale.

- [ ] **Step 3 — SPI A loopback (scripted master on spare pins): register
  machine correct.**
  Pass: a scripted transaction against a channel returns the expected
  register image with zero underruns.
  Command: `kilnsim selftest` (PLAN.md §13 layer-2 loopback self-check) now
  exists and covers protocol round-trip/command-group reachability/event
  continuity, but its own help text is explicit that hardware-only checks
  (SPI master loopback, CT→ADC loopback) report `NOT_RUNNABLE`, never faked
  — it does not replace this step. For the real M-A proof (a second Pico as
  reference SPI master, sweep + Saleae capture), follow
  `docs/BENCH_RUNBOOK.md` §4 step 3 rather than duplicating that procedure
  here. Cross-check via `kilnsim state`'s SPI transaction/underrun counters
  before and after.

- [ ] **Step 4 — CT synthesis into a scope/DMM through the transformer:
  waveform + levels.**
  Pass: commanded amplitude produces a clean 60 Hz waveform at the jack
  within the expected voltage range for the fitted transformer/CT
  calibration.
  Command: `kilnsim ct amps <channel> <amps>` then `kilnsim ct state
  <channel>` — the CLI's `ct` group now exists; judge on waveform
  cleanliness, not absolute amplitude. **Correction:** the amplitude
  calibration *mechanism* now exists (`src/sim/ct_calibration.{c,h}`,
  `pwm_scale = clamp(gain[ch] * amps + offset[ch], 0, 1)`) — this is no
  longer a bare `IDENTITY` placeholder in the code sense. What ships today is
  the compiled-in *default table*, and that table is deliberately
  all-uncalibrated (`ct_calibration_defaults.h`, every channel's `calibrated`
  flag false), which makes the observed behavior identical to the old
  IDENTITY placeholder (`pwm_scale = clamp(amps, 0, 1)`) until a real bench
  calibration run (`tools/ct_calibration/`) produces per-channel constants
  and `tools/gen_ct_cal_table.py` regenerates the default header. Still
  pending M-D hardware; the plumbing to consume a real table is not.

- [ ] **Step 5 — PRE-DUT SANITY CHECK. Do not skip, do not reorder.**
  **Revised 2026-08-23:** the old "no continuity between fixture GND_Main and
  GND_Safty" pass criterion is retired — the fixture's ground is now
  deliberately commoned with the DUT's (`DESIGN_NOTES.md` §3.5), so
  continuity between those two labels is the *expected*, correct result, not
  a fault. There is no jumper to check for either. What this step checks
  instead: verify the CT transformer orientation and 1:1 wiring against §4's
  table (the CT channels are still the one part of this fixture that stays
  floating from both domains, and getting that wrong is still a real risk).
  **Do this with a meter, on the bench, every time the harness is rebuilt —
  not once and trusted forever.**
  No `kilnsim` command substitutes for a physical check.

- [ ] **Step 6 — DUT thermocouple path: J6 unplugged from the real
  daughterboard, fixture in its place, `KilnFW` booted — temperatures
  appear.**
  Pass: `KilnFW`'s own UI/telemetry shows plausible, non-zero, non-fault
  temperatures on all three main-side channels.
  Command: `kilnsim preset fast_test` then watch via `kilnctrl`'s
  `mcp__kilnctrl__thermo_read` / `thermo_get_reports` on the DUT side, and
  `kilnsim state` on the fixture side to confirm what it believes it is
  reporting.

- [ ] **Step 7 — Safety path: J7 direct GPIO, `SaftyFW`'s single channel
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
  Commands: `mcp__kilnctrl__io_set_relay` (DUT side) and `kilnsim relay
  states` / `kilnsim relay edges --since-seq N` (fixture side) — the CLI's
  `relay` group now exists.

- [ ] **Step 9 — E-stop + fault line + DUT power relay, one at a time.**
  Pass (E-stop): `kilnsim estop open` then `kilnsim estop closed` produces
  the expected STOP/healthy transition on the DUT's safety status.
  Pass (fault line): a fault forced on the DUT's `Fault` GPIO is visible in
  the fixture's sense state.
  Pass (DUT power): each `--domain` cycle reboots only that domain's
  downstream load and its telemetry shows the gap. **Correction: §0 item 6
  (J18/J19, one relay vs two) is now fully implemented, not just
  design-resolved** — `i2c_owner.c` drives two independent MCP23017 output
  bits (`EXP1_PIN_DUT_POWER_MAIN`/`EXP1_PIN_DUT_POWER_SAFETY`, §3.7), with
  matching wire commands (`DUT_POWER_SET`/`GET` main-domain-only,
  `DUT_POWER_SAFETY_SET`/`GET` for the safety domain, `docs/PROTOCOL.md`
  §5.5). Run the two domains' power cycles **separately**, not as one
  combined action — there is deliberately no "power both" command, so a
  scenario that wants both domains power-cycled issues both commands; note
  which domain(s) you actually exercised in the bench log (§6 of
  `docs/BENCH_RUNBOOK.md`).
  Commands: `kilnsim estop open|closed`, `kilnsim power cycle --off-ms N
  --domain main`, `kilnsim power cycle --off-ms N --domain safety`.

- [ ] **Step 10 — First closed-loop firing on `fast_test` preset.**
  Pass: `kilnsim run scenarios/baseline_firing.yaml` (once that scenario file
  exists per DESIGN_NOTES.md §8) exits 0, with the DUT's PID visibly regulating a
  simulated zone through relay cycling with no fixture intervention.
  Command: `kilnsim run <scenario.yaml> [--seed N] [--report out.json]`.

---

## 7. USB identity

The fixture Pico's native USB CDC link (`src/tasks/usb_owner.c` +
`src/tasks/usb_descriptors.c`, DESIGN_NOTES.md section 4.1/5) presents a claimed,
fixture-specific USB identity so `kilnsim`'s PC-side auto-detect
(`tools/PcTools/src/kilnsim/link.py`) cannot latch onto the wrong RP2040 on a
bench that also has the `spi_test_master` reference Pico
(`tools/spi_test_master/`) and the safety processor's Debug Probe plugged in
at the same time.

| | Value | Source |
|---|---|---|
| VID | `0x2E8A` | Raspberry Pi's own vendor ID, reused informally (see below) |
| PID | `0xF00A` | Fixture-specific, chosen for this project |
| Manufacturer string | `kilnCtl` | `src/tasks/usb_descriptors.c` |
| Product string | `SimFW Bench Fixture` | `src/tasks/usb_descriptors.c` |
| Serial number | RP2040 flash unique ID, 16 hex chars | `src/tasks/usb_descriptors.c`, via `pico_get_unique_board_id_string()` |

**VID choice:** this fixture is a one-off in-house bench tool, never mass
produced and never sold, so no formal USB-IF VID has been (or will be)
purchased for it. Rather than TinyUSB's own generic `0xCafe` placeholder
(what this firmware used before this section existed), it informally reuses
Raspberry Pi's VID `0x2E8A` — the RP2040 this fixture runs on is itself a
Raspberry Pi part, and every other RP2040 on this bench (the `spi_test_master`
Pico, the Debug Probe) already enumerates under that same VID via pico-sdk's
own stock descriptors. This is informal, undocumented-by-RPi use of their VID
for a tool that will never ship — accepted deliberately for that reason, not
a claim of Raspberry Pi's endorsement or a formal sub-license.

**PID choice — checked against:** the RPi-documented PIDs under `0x2E8A`
found in this toolchain's pico-sdk checkout and general RPi USB-ID
references:

| PID | What it is | Where confirmed |
|---|---|---|
| `0x0003` | RP2040 BOOTSEL / bootrom mass-storage mode | RPi USB ID references (not vendored in this pico-sdk checkout — the bootrom isn't pico-sdk source) |
| `0x0004` | Picoprobe / Debug Probe, CDC interface | RPi USB ID references |
| `0x0009` | pico-sdk stock `stdio_usb` CDC, non-RP2040 boards (e.g. RP2350) | confirmed directly: `src/rp2_common/pico_stdio_usb/stdio_usb_descriptors.c`, `#if PICO_RP2040 ... #else #define USBD_PID (0x0009)` |
| `0x000A` | pico-sdk stock `stdio_usb` CDC, RP2040 boards — **this project's own old placeholder**, and still `spi_test_master`'s pre-this-change default | confirmed directly: same file, `#define USBD_PID (0x000a) // Raspberry Pi Pico SDK CDC for RP2040` |
| `0x000C` | Raspberry Pi Debug Probe, CMSIS-DAP v2 interface | RPi USB ID references |

All of these are low, sequentially-allocated values. `0xF00A` sits far
outside that range — a deliberate nod to the old placeholder PID (`0x000A`)
this fixture used to share with every other stock pico-sdk CDC example —
so a newly-registered official RPi PID (which has so far only ever grown
that low range upward) cannot collide with it. `tools/spi_test_master/`
(`CMakeLists.txt`) claims the adjacent `0xF00B` for the same reason, one
digit apart so the two bench tools are easy to tell apart by eye in a USB
descriptor dump; see that file's own comment for the pico-sdk
`USBD_PID`/`USBD_PRODUCT` override mechanism it uses (stock
`pico_enable_stdio_usb`, not a hand-written descriptor file like this
fixture's own).

**Distinguishing the three bench RP2040s by eye:** `kilnsim`'s auto-detect
matches on VID:PID (`tools/PcTools/src/kilnsim/link.py`'s `SIMFW_VID_PID =
"2E8A:F00A"`) plus a protocol PING, so it should already pick the right port
without operator help in the common case. For a human checking `lsusb` /
Windows Device Manager directly:

- **SimFW fixture Pico:** `2E8A:F00A`, product string "SimFW Bench Fixture",
  serial = RP2040 flash unique ID.
- **`spi_test_master` reference Pico:** `2E8A:F00B`, product string
  "spi_test_master (kilnCtl bench)", serial = its own RP2040 flash unique ID
  (pico-sdk's stock `stdio_usb` fills this in automatically, same mechanism).
- **Safety processor's Debug Probe:** `2E8A:0004` (CDC) / `2E8A:000C`
  (CMSIS-DAP) — untouched, not this project's firmware.

---

## 8. Cross-reference

- Design rationale for every signal above: `firmware/SimFW/docs/DESIGN_NOTES.md`
  sections 2, 3.1–3.7, 14.
- DMA channel budget (§1b): `firmware/SimFW/docs/SPI_ACCESS_AUDIT.md` §9 for
  why the 8 SPI-side channels exist and why none of them can be given back to
  the CPU; `firmware/SimFW/tools/check_single_owner.ps1` for the
  `hardware/dma.h` two-owner rule that mirrors it.
- Main-board authoritative wiring: `firmware/KilnFW/docs/HARDWARE.md`,
  `firmware/SaftyFW/docs/HARDWARE.md`, `firmware/SaftyFW/docs/
  CURRENT_SENSE.md`.
- Code this document reconciles: `firmware/SimFW/src/tasks/i2c_owner.c`,
  `firmware/SimFW/src/tasks/spi_emu_a.c`, `firmware/SimFW/src/tasks/
  spi_emu_b.c`, `firmware/SimFW/src/drivers/max31856_spi_slave.pio`,
  `firmware/SimFW/src/drivers/max31856_pio_engine.{c,h}`,
  `firmware/SimFW/src/drivers/ct_wave_pwm.{c,h}`, `firmware/SimFW/src/tasks/
  wave_owner.c`, `firmware/SimFW/src/drivers/mcp23017.{c,h}`.
