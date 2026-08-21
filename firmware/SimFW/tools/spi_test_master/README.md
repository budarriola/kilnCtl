# spi_test_master -- SimFW M-A bench procedure

**Status: build-verified only.** No hardware is attached in the environment
this tool was built in. Nothing below has been run against a real Pico or a
real SimFW slave -- treat every claim about behavior as a prediction from
reading the code, not a report of an observed result, until someone runs
this on the bench and updates this line.

## What this is

`firmware/SimFW/docs/PLAN.md` section 10 names **M-A** ("SPI slave proof of
concept") as the project's risk-first milestone: prove the PIO MAX31856
*slave* emulator (`../../src/drivers/max31856_spi_slave.pio` +
`max31856_pio_engine.c`) can survive real SPI timing before any framework
code is built on top of it. Its exit criterion is a Saleae capture of
correct mode-1 multi-byte reads at **4 MHz** (the thermocouple SPI clock's
decided ceiling on both real masters, `docs/PLAN.md` section 3.2.1 —
originally targeted at 5 MHz before that cap was decided; this tool's own
sweep still exercises rates above 4 MHz for headroom characterization, see
below) with **zero TX underruns over >=10k transactions**.

PLAN.md explicitly allows proving this against "a third Pico as a scripted
test master first" instead of going straight to the real bench ESP32. That
is what this directory is: a second, disposable RP2040 firmware
(`src/main.c` + `spi_master.c` + `seq.c`) that acts as a **known-good SPI
master** -- RP2040 hardware SPI, not PIO, because being the master is the
easy direction and a hand-rolled hardware-SPI master is trivially
trustworthy, which is the entire point of a reference master. It clocks
SimFW's slave with scripted, self-verifying MAX31856 register transactions
and reports pass/fail without needing a human (or a logic analyzer) to
adjudicate every byte.

It is **not** part of SimFW's own build (`../../CMakeLists.txt` never
references this directory) and it does **not** speak SimFW's `benchproto`
framed binary protocol -- its own link is a plain-text line protocol you can
drive from a terminal.

## Wiring

**Both sides' pin assignments below are provisional** until
`../../docs/HARDWARE.md` is finalized (another pass is writing that doc
right now) -- treat this table as "what the code currently does," not as a
frozen pinout. Re-derive it from source before trusting it for a real bench
session; here is where each number came from:

- SimFW slave-side pins: `../../src/tasks/spi_emu_a.c` (bus A, GPIO6-11) and
  `../../src/tasks/spi_emu_b.c` (bus B, GPIO12-15) -- `#define
  SPI_EMU_A_SCLK_GPIO` etc. in each file.
- This tool's master-side pins: `src/spi_master.h`'s `SPI_MASTER_*_GPIO`
  defines and `spi_master_cs_gpio[]`.

Bus A (ESP-side, 3 emulated channels sharing one MISO -- exercises the
tri-state/multi-CS path):

| Signal | SimFW slave GPIO (bus A) | spi_test_master GPIO |
|---|---|---|
| SCLK | 6 | 18 |
| MOSI (slave RX) | 7 | 19 (this tool's SPI TX) |
| MISO (slave TX) | 8 | 16 (this tool's SPI RX) |
| CS0 | 9 | 20 |
| CS1 | 10 | 21 |
| CS2 | 11 | 22 |
| GND | common | common -- **do not skip this**, both boards need a shared ground reference |

Bus B (safety-side, 1 emulated channel):

| Signal | SimFW slave GPIO (bus B) | spi_test_master GPIO |
|---|---|---|
| SCLK | 12 | 18 |
| MOSI (slave RX) | 13 | 19 |
| MISO (slave TX) | 14 | 16 |
| CS0 | 15 | 20 (CS index 0 -- `CS 1/2/3` are simply unused when only bus B is wired) |
| GND | common | common |

Test one bus at a time (the RP2040 only has one master clock domain in use
here; wiring both buses' SCLK to the same master pin simultaneously would
have two slave engines trying to answer the same clock, which is not a
useful test of either). Move the 4-wire (+GND) harness between bus A's and
bus B's headers to switch which one you're exercising.

Both boards need **3.3V logic levels** (both are RP2040s here, so this
should already be true) and a **common ground** -- if bus A's SimFW GPIOs
and this tool's GPIOs are ever on boards with different supplies, tie
grounds before powering anything.

## Flashing both Picos

Same toolchain as the rest of the repo (see `../../README.md`'s Build
section and `../../CMakeLists.txt`'s header comment for the full
requirements/versions). PowerShell, SDK cloned to `C:\pico-tools`:

```powershell
$env:PICO_SDK_PATH = "C:\pico-tools\pico-sdk"
```

**SimFW itself** (the DUT / slave side -- build from `../../`):

```powershell
cmake -G Ninja -B ..\..\build -S ..\.. -DPICO_BOARD=pico `
  "-DPICO_TOOLCHAIN_PATH=C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin"
cmake --build ..\..\build
```

Flash `../../build/SimFW.elf` over SWD/OpenOCD (no `.uf2` yet, same picotool
gap `../../README.md` documents).

**spi_test_master** (this directory, the reference master):

```powershell
cmake -G Ninja -B build -DPICO_BOARD=pico `
  "-DPICO_TOOLCHAIN_PATH=C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin"
cmake --build build
```

Flash `build/spi_test_master.elf` over SWD/OpenOCD, same as SimFW. (This
tool's own picotool/`.uf2` situation is identical to SimFW's -- `.elf` only,
disabled in `CMakeLists.txt` with the same comment.) BOOTSEL drag-and-drop
would also work once a `.uf2` is available, since this firmware needs no
special reset sequencing.

After flashing, plug the spi_test_master Pico into USB (separately from
whatever debug probe you used to flash it) -- it enumerates as a USB CDC
serial port. Open it in any terminal (PuTTY, `screen`, `python -m
serial.tools.miniterm`, etc.) at any baud rate (USB CDC ignores the baud
field) and press Enter; you should see the `spi_test_master ready...`
banner and can type `HELP`.

## Command set

Plain-text, one command per line, human-typeable. Full list is also
printed by the firmware itself (`HELP`):

| Command | Effect |
|---|---|
| `PING` | replies `PONG` -- connectivity check |
| `RATE <hz>` | sets the SPI clock rate; replies `OK RATE <actual_hz>` (the RP2040's divider isn't exact for every value) |
| `MODE <0\|1>` | sets CPHA (0 or 1); CPOL is always 0. Default 1 -- SPI mode 1 (CPOL=0, CPHA=1), matching PLAN.md 3.2 / the real KilnFW and SaftyFW MAX31856 drivers. See "The mode-1 edge resolution" below for why `MODE 0` exists as a diagnostic. |
| `CS <0..3>` | selects which CS line the next transaction(s) assert |
| `READ <addr_hex>` | single-byte read, replies `OK READ <addr> <value>` |
| `WRITE <addr_hex> <val_hex>` | single-byte write, replies `OK` |
| `READN <addr_hex> <n>` | multi-byte auto-increment read (n=1..16), replies `OK READN <addr> <n bytes as hex>` |
| `SEQ single <addr_hex> <expect_hex>` | one-shot verified single-byte read |
| `SEQ wrrd <addr_hex> <hexbytes>` | write N bytes (auto-increment) then auto-increment readback, verified |
| `SEQ autoinc <addr_hex> <expect_hexbytes>` | one-shot verified multi-byte auto-increment read |
| `SEQ b2b <expect_cr1_hex> <count>` | back-to-back single-byte CR1 reads with minimal inter-frame gap |
| `SOAK <count>` | runs `count` self-checking iterations (the M-A soak primitive -- see below); prints progress every 1000 and a final `SOAK DONE ... result=PASS\|FAIL` line |
| `STATS` | prints cumulative transactions/bytes/mismatches/suspected-first-byte-late counters |
| `RESET` | clears the cumulative counters (does **not** reset the slave -- there is no wiring for that from this tool; power-cycle or SWD-reset the SimFW Pico if you need a truly clean slave state) |
| `HELP` | prints this command list |

Everything through the same USB CDC link, so `run_soak.py` (below) is just
scripting these same commands.

## Soak/sweep design

The soak primitive (`SOAK <n>`, implemented in `src/seq.c`'s
`seq_soak_iteration()`) does two self-checking things per iteration and
folds in the multi-CS exercise, so one call covers most of the task brief's
required cases without a separate mode per case:

1. **Multi-byte auto-increment write-then-readback** on the 7-byte block
   `CJHF..CJTO` (addresses 0x03-0x09) -- plain R/W registers with no
   self-clearing bits or CJ-disable-gated writability (unlike CR0 or
   CJTH/CJTL), so "what we wrote" is an unambiguous oracle for "what must
   read back," and this exercises exactly the multi-byte auto-increment
   read path PLAN.md 3.2.1's 1.6us latency budget is about. The pattern
   written is a cheap per-iteration LFSR-ish function of the iteration
   number, not a fixed constant, so a soak can't pass by accident of every
   iteration writing the same bytes back to themselves.
2. **Single-byte read of CR1**, verified against a value the runner
   establishes once at the start of the soak (a `WRITE`+readback of CR1 to
   its reset default, 0x03) rather than assumed -- correct even if the
   slave wasn't freshly reset before the soak started.
3. **CS rotation**: each iteration selects CS `iteration % 3`, cycling
   through bus A's three emulated channels. This is the task brief's "read
   from one CS while the others are idle, verify no contention garbage"
   case -- MISO contention between channels would corrupt the compared
   bytes in step 1 or 2 above, so it's caught by the existing byte-compare
   with no separate detector needed. (On bus B, only CS0 exists, so the
   rotation harmlessly always lands on CS0.)
4. **Back-to-back framing** happens implicitly: `spi_master_transact()`
   deasserts and reasserts CS with no inserted delay between the write and
   read halves of step 1, or between soak iterations -- there is no
   artificial gap anywhere in the soak loop.

`STATS`/`SOAK DONE` report exactly what the task brief asks for:
transactions attempted, bytes compared, mismatches, and *suspected*
first-byte-late events -- a heuristic (byte[0] wrong, every later byte in
the same transaction right) for PLAN.md 3.2.1's "TX FIFO underrun" failure
mode, not a direct underrun detector. Only the slave's own PIO-side
counters (`max31856_pio_stats_t.first_byte_late` in
`../../src/drivers/max31856_pio_engine.h`) or a Saleae capture can prove an
underrun actually happened -- this tool's number is a pointer at what to go
look at, not the proof itself.

### `run_soak.py` sweep

```powershell
python run_soak.py --port COM5
```

Default sweep: 100 kHz, 500 kHz, 1, 2, 3, 4, 5 MHz, 10,000 transactions
(iterations, each contributing 2 SPI transactions -- the write and the
read) per rate point. **The M-A exit criterion's actual rate is 4 MHz** (the
thermocouple SPI clock's decided cap on both real masters, `docs/PLAN.md`
section 3.2.1) -- the 5 MHz point in this default sweep is kept as headroom
characterization above the real ceiling, not as the pass bar; a slave that
also passes at 5 MHz says something useful about margin, but only the
4 MHz result is required. Override with `--rates` and `--count`; `--mode 0`
reruns the whole sweep with CPHA=0 for diagnosing the ambiguity below
without reflashing.

Output is a plain table, one row per rate point (requested Hz, actual Hz
achieved, PASS/FAIL, failed iteration count, mismatch count, suspected
first-byte-late count) plus an overall PASS/FAIL and a reminder that the
table alone is not M-A's exit criterion -- pair the passing rate's run with
a Saleae capture (see below) before calling M-A done.

**Why 10,000 per point, not just at 5 MHz:** the sweep's value is finding
*where* (if anywhere) the slave starts failing, not just confirming pass/fail
at the target rate. A slave that passes at 1 MHz but fails at 3 MHz tells you
the 1.6us latency budget (PLAN.md 3.2.1) is the live issue and roughly how
much margin is missing; a slave that fails at every rate including 100 kHz
tells you the problem isn't timing at all (wrong edge, wrong pin, bad wiring
-- see the troubleshooting table below).

Requires `pyserial` (`pip install pyserial`) -- not vendored, not part of
any existing repo virtualenv; install it before running.

## Interpreting results / troubleshooting

| Symptom | Likely cause (PLAN.md 3.2.1 reference) |
|---|---|
| All reads come back `0xFF` (or the slave never seems to respond) | MISO not actually tri-stating/driving -- check the TX program's `set pindirs` logic in `max31856_spi_slave.pio`'s `max31856_spi_tx_a`/`tx_b`; or CS wiring is backwards (idle-low instead of idle-high, so the slave never sees an assert); or GND not shared between boards |
| All reads come back `0x00` | MISO stuck driven low, or the RX program's channel/CS `jmp_pin` mapping is wrong so the slave never sees its own CS line assert and stays parked at `wait_cs` -- check `cs_gpio[]` ordering and the "must be consecutive ascending" contract in `max31856_pio_engine.h` |
| First byte of a multi-byte read is wrong, every later byte is right | Classic first-byte-late / TX-FIFO-underrun signature -- PLAN.md 3.2.1's core risk. This is exactly what `suspected_first_byte_late` in `STATS`/`SOAK DONE` is counting. Means Plan A (ISR staging, `max31856_pio_engine.c`'s IRQ handler) is missing its <1us budget at this rate; PLAN.md's Plan B (precomputed full-image streaming) or a documented max-SCLK ceiling below 5 MHz is the next step |
| Passes at low rates, starts failing intermittently above some N MHz | Same first-byte-late family, but the sweep is what actually finds N -- run `run_soak.py` and read off the first FAIL row. Compare N against the 1.6us budget's implied max rate |
| Reads work but come back scrambled/garbage-looking specifically when a *different* CS was active on the previous transaction (multi-CS/bus A only) | MISO tri-state contention between channels -- two TX SMs (or one TX SM with the idle-test logic wrong) both think they should drive MISO. Check `poll_idle`/`x!=y` compare logic in `max31856_spi_tx_a` and that `cs_gpio[0..2]` really are 3 consecutive GPIOs as the header comment requires |
| Every response byte is shifted by roughly one byte, or every bit looks off-by-one within a byte | Should not happen post-fix (see "The mode-1 edge resolution" below) unless the `.pio` has regressed -- diff `max31856_spi_rx`/`max31856_spi_tx_a`/`max31856_spi_tx_b` against the corrected version, or try `MODE 0` as a diagnostic to confirm it's an edge problem at all |
| Writes silently don't take (`SEQ wrrd` always FAILs even at very low rates) | Either the RX program isn't reaching `write-data` bytes at all (transaction framing bug -- check the address-byte-then-N-data-bytes shape is intact) or `spi_emu_a/b`'s task-loop write-back code (drains the RX FIFO after CS-rise, applies write rules) isn't running/isn't wired up yet |

### The mode-1 edge resolution (previously an open ambiguity -- now settled)

`../../src/drivers/max31856_spi_slave.pio` originally had a header comment
flagging an internal inconsistency in PLAN.md 3.2.1's wording, and the RX/TX
programs as first written sampled MOSI on SCLK's **rising** edge and changed
MISO on SCLK's **falling** edge -- textbook SPI **mode 0** (CPHA=0), not the
mode 1 (CPHA=1) every comment in that file claimed.

This has been resolved from code + datasheet, no bench measurement needed:
- KilnFW's `MAX31856.c` sets ESP-IDF `spi_device_interface_config_t.mode = 1`
  (`#define MAX31856_SPI_MODE 1`).
- SaftyFW's `spi_owner.c` sets `spi_set_format(spi0, 8, SPI_CPOL_0,
  SPI_CPHA_1, SPI_MSB_FIRST)` -- explicit CPOL=0/CPHA=1, agreeing with
  KilnFW.
- KilnFW's `docs/HARDWARE.md` SPI bus section states "the thermocouple
  parts run SPI mode 1."
- The MAX31856 datasheet's own Table 5 ("Serial Interface Function", page
  15, footnoted "CPHA bit polarity must be set to 1"), CPOL=0 row: SDI
  ("Data bit latch") happens on SCLK **falling**; SDO ("Next data bit
  shift") happens on SCLK **rising**. That is the datasheet describing the
  MAX31856 itself, as a slave, sampling on falling and shifting on rising --
  exactly the role this PIO program plays.

So the PIO programs were backwards, not the "mode 1" label or PLAN.md's
description of the target mode. `max31856_spi_rx`, `max31856_spi_tx_a`, and
`max31856_spi_tx_b` have been corrected: RX now samples MOSI on the
**falling** edge, TX now shifts MISO on the **rising** edge. `spi_master.c`'s
default (`MODE 1`, genuine RP2040 hardware SPI mode 1) was already correct
and needs no change; it should now pass cleanly against the fixed fixture.
`MODE 0` remains available as a diagnostic escape hatch (`run_soak.py --mode
0`) for future regressions in this area, but is no longer the "which one is
actually right" question it used to be -- if `MODE 1` ever starts failing
again, that points at a real regression in the `.pio` edges, not at a
residual ambiguity.

## Where the Saleae fits

M-A's actual, literal exit criterion (PLAN.md section 10) is a Saleae
capture, not this tool's pass/fail table -- `run_soak.py`'s table is
necessary supporting evidence (it's what tells you *when* to trigger the
capture and gives you the transaction-count denominator) but is not a
substitute for it.

**Signals to capture:** SCLK, MOSI, MISO, and at minimum CS0 from whichever
bus is under test (all three CS lines on bus A if channels are available --
multi-CS behavior is part of what needs proving). Use the wiring table
above to find the physical test points; probe on the SimFW slave side so
the capture reflects what the DUT actually saw, not what the master
intended to send.

**Recommended capture procedure:**
1. Wire per the table above, flash both boards, connect the Saleae probes
   to the slave-side SCLK/MOSI/MISO/CS lines.
2. Run `run_soak.py` at your target rate (**4 MHz** for the literal M-A
   criterion -- the real masters' decided cap, `docs/PLAN.md` section
   3.2.1; a 5 MHz capture is optional extra headroom evidence, not a
   substitute) with `--count 10000` or more, starting the Saleae capture
   just before issuing the `SOAK` command and stopping it just after
   `SOAK DONE` prints.
3. Confirm in the capture software: SCLK period matches the requested rate
   within the RP2040 divider's expected error, MOSI address bytes match
   what `seq.c` sent (0x03/0x83 for the write-readback block, 0x01 for the
   CR1 read), MISO response bytes match what `run_soak.py`'s table says
   passed, and CS framing shows no gap-free/overlapping assertions.

**What a passing capture looks like:** every transaction's response bytes
on MISO match the expected values from the corresponding `SEQ`/`SOAK`
iteration (cross-check a sample against this tool's own `STATS` output --
zero mismatches), MISO is cleanly tri-stated (mid-rail/floating, not driven)
whenever the relevant bus's CS lines are all idle-high, and there is no
visible gap between the address byte's last clock edge and the first
response bit's transition wider than the 1.6us budget (PLAN.md 3.2.1) --
i.e., no visible "hang" on MISO after the address byte before data starts
appearing. Zero TX underruns over the run is the number this tool's
`suspected_first_byte_late` counter (cross-checked against the slave's own
`max31856_pio_stats_t.first_byte_late`, if a link exists to read it) should
also report as zero for the capture to be worth keeping as M-A evidence.
