# SimFW — MAX31856 SPI access-pattern audit

Answers `docs/PLAN.md` §11 open question 4 and is the code-derived half of
M-A's exit criterion (the other half is the Saleae capture, which this
document does **not** substitute for — see §8).

Audited 2026-08-20 against:

| Role | Files |
|---|---|
| Master 1 — ESP32-S3 | `firmware/KilnFW/App/drivers/MAX31856.c`, `.../espInterfaces/esp_spi_owner.c` |
| Master 2 — RP2040 safety | `firmware/SaftyFW/src/max31856.c`, `firmware/SaftyFW/src/spi_owner.c` |
| Responder — PIO | `firmware/SimFW/src/drivers/max31856_spi_slave.pio`, `max31856_pio_engine.c` |
| Responder — register machine | `firmware/SimFW/src/sim/max31856_regs.c` / `.h` |
| Part | `hardware/datasheets/ThermocoupleBoard_Sensor_Temperature/MAX31856.pdf` |

---

## 0. The one-sentence answer

**No — neither real master ever emits a write-then-read, a repeated start, or
any other multi-phase transaction within a single CS assertion; every
transaction either master produces is exactly one CS-low window containing one
address byte followed by 1–6 data bytes, direction fixed for the whole window
by bit 7 of that address byte, so the PIO responder needs no special case for
phase changes and the PIO program can be frozen on that point.**

This is structural, not incidental. Both masters funnel every MAX31856 byte
through a single-call transport that asserts CS, runs exactly one hardware SPI
transfer, and deasserts CS:

* `esp_spi_owner.c:26-34` — `gpio_set_level(cs,0)` → one `spi_device_transmit()`
  → `gpio_set_level(cs,1)`. There is no API by which a caller can hold CS
  across two transfers; `spi_owner_transfer()` is the only entry point
  (`MAX31856.c:101-105` states this as a driver invariant).
* `firmware/SaftyFW/src/spi_owner.c:62-65` — `gpio_put(CS,0)` → one
  `spi_write_read_blocking()`/`spi_write_blocking()` → `gpio_put(CS,1)`.

The only two-phase-looking access anywhere in the repo is `ILI9488.c:1611`
followed by `:1629` (command byte, then a 4-byte read) — and even that is two
*separate* CS assertions, on the display's own CS, on a different bus. The
thermocouple responder can never see it.

---

## 1. Transaction shape inventory

Wire convention for both masters and the part: MSB-first, 8-bit bytes, SPI
mode 1 (CPOL=0/CPHA=1), address byte first, bit 7 = 0 → read, bit 7 = 1 →
write (`MAX31856.c:124`, `:148`; `SaftyFW/src/max31856.c:41`, `:54`). The
part's address counter auto-increments for every byte after the address byte
"as long as CS remains low" (datasheet p.15, *Serial Interface*).

| ID | Master | Bytes in one CS | MOSI | MISO | Emitting code |
|---|---|---|---|---|---|
| **W1** | both | 2 | `80h\|reg`, value | ignored | KilnFW `MAX31856.c:154-157` → `:140-152`; SaftyFW `max31856.c:39-43` |
| **W2** | KilnFW only | 3 | `80h\|reg`, v0, v1 | ignored | `MAX31856.c:888`, `:890`, `:893` (CJHF/CJLF, LTHFTH/L, LTLFTH/L pairs) |
| **W3** | KilnFW only (debug) | 2..17 | `80h\|reg`, v0..vN-1 | ignored | `MAX31856.c:140-152`, N ≤ `MAX31856_MAX_BURST_LEN` = 16 (`MAX31856.h:88`) |
| **R1** | both | 7 | `0Ah`, six `00h` | x, CJTH, CJTL, LTCBH, LTCBM, LTCBL, SR | KilnFW `MAX31856.c:1038`; SaftyFW `max31856.c:191` |
| **R2** | KilnFW only | 2 | `0Fh`, `00h` | x, SR | `MAX31856.c:1146` |
| **R3** | KilnFW only (debug) | 2..17 | `reg`, N × `00h` | x, reg..reg+N-1 | `MAX31856.c:1242`, N ≤ 16, and `:1232` rejects `reg + N > 16` |

"x" = the byte MISO carries while the address byte is going out; both masters
explicitly discard it (`MAX31856.c:132-134`, `SaftyFW/src/max31856.c:60-63`).

Concrete W1 call sites (all single-register, all 2 bytes): KilnFW `:564`
(FAULTCLR), `:738`/`:746`/`:755` (CR0 stopped → CR1 → CR0 running), `:840`
(CR0), `:932` (CJTO), `:953` (MASK), `:981`/`:1190` (one-shot trigger),
`:1265` (debug poke). SaftyFW `:136`/`:143`/`:151`/`:157` — the same
CR0→CR1→MASK→CR0 commissioning ladder, four separate CS assertions.

### What is *not* in the inventory

* No repeated start / phase change inside one CS (§0).
* No read burst that crosses `0Fh`. R1 starts at `0Ah` and ends exactly on
  `0Fh`; R3 is range-checked at `MAX31856.c:1232`. The auto-increment
  wrap is therefore **unreachable by either real master**, though
  `tools/spi_test_master` can reach it (`seq.c:94`, len ≤ 16 from any
  address).
* No transaction longer than 17 bytes; `spi_bus_config_t.max_transfer_sz` is
  pinned to that (`MAX31856.c:333`).
* No CS held low across two logical operations, and no idle-with-CS-low: both
  transports deassert immediately after the transfer returns.
* No CS deassert mid-byte on any normal path.
* Neither master drives MOSI with anything but zeros during a read
  (`MAX31856.c:122-124` memsets `tx`; `SaftyFW/src/max31856.c:53`).

---

## 2. Per-shape verdicts against the responder

All verdicts are **after** the three defect fixes in §3; before them, every
read shape was broken (D2) and MISO was never tri-stated (D1).

| Shape | Verdict | Responder path checked |
|---|---|---|
| W1 | **Handled.** Address byte opens a write txn, one data byte applies `apply_write_rule()`. | `max31856_pio_engine.c:119-144` (address branch), `:157-159` (write byte) → `max31856_regs.c:234-241`, `:131-175` |
| W2 | **Handled.** Second data byte lands on `reg+1` via the same auto-increment the part uses. | `max31856_regs.c:240` |
| W3 | **Handled** for N ≤ 16 within `00h..0Fh`; addresses `10h..7Fh` now correctly swallow the write instead of aliasing (§3, D4). | `max31856_regs.c:138-144` |
| R1 | **Handled.** Address byte snapshots the image and stages byte 0; each dummy byte stages the next. Ends exactly on SR. | `max31856_pio_engine.c:128-140`, `:160-167` → `max31856_regs.c:179-192`, `:218-231` |
| R2 | **Handled.** Degenerate 1-register case of R1. | same |
| R3 | **Handled**, including the datasheet's `FFh`-for-invalid-address behaviour past `0Fh` (§3, D4). | `max31856_regs.c:223-226` |
| Wrong-direction byte inside a txn | **Handled.** Direction is latched once at `cs_assert` and both clock functions no-op on mismatch. | `max31856_regs.c:219-221`, `:235-237` |
| CS deassert mid-byte (aborted txn) | **Handled.** RX SM resyncs on a clean byte boundary; the engine closes the txn and resets the TX SM. | `.pio:139` (`jmp pin, resync`), `max31856_pio_engine.c:206-232` |
| Back-to-back transactions with no idle gap | **Handled after D2's fix**, and *only* because of it — see §5. | `max31856_pio_engine.c:85-97` |

---

## 3. Defects found and fixed

### D1 — TX state machine never tri-stated MISO and never re-synced at CS boundaries (critical)

Both TX programs decide "is any CS asserted" with

```
in pins, N ; mov x, isr ; jmp x!=y, active
```

against a `set`-loaded constant (`.pio:153` `set y, 7` for bus A, `.pio:181`
`set y, 1` for bus B). `IN` deposits the sampled bits at whichever end of the
ISR the **IN shift direction** says — and
`max31856_spi_tx_*_program_get_default_config()` inherits
`pio_get_default_sm_config()`, which leaves IN shifting **right**. Shifting
right into a zeroed ISR puts N bits at `ISR[31:32-N]`, so an idle bus A read
back `0xE0000000`, never `7`.

Consequence: `jmp x!=y, active` was taken unconditionally. The SM never
executed `set pindirs, 0`, so **MISO was driven at all times instead of
tri-stated** (`.pio:70-72` states tri-stating as a hard requirement for the
three-chips-one-MISO bus A), the per-bit mid-byte CS re-check at `.pio:167`
never fired, and the SM never returned to `poll_idle` — meaning it never
re-entered a known state between transactions.

**Fix:** `max31856_pio_engine.c:311-323` (`sm_config_set_in_shift` at `:323`) — added
`sm_config_set_in_shift(&tc, false /*shift left*/, false /*no autopush*/, 32)`
for the TX SM. The `.pio` programs themselves are correct as written; the bug
was entirely in the C configuration, which is exactly the failure class the
`.pio` header already warns about ("enforced by the C glue …, not by pioasm
itself — get this wrong in the .c file and these programs silently watch the
wrong pin").

### D2 — the engine stages one more response byte than the master ever clocks, and never flushed it (critical)

For an N-register read the master clocks **N+1** bytes (address + N dummies).
The RX side sees N+1 bytes and therefore stages N+1 response bytes: one from
the address byte (`max31856_pio_engine.c:128-136`) and one per dummy byte
(`:141-155`). But the TX SM can emit at most one bit per SCLK cycle, and the
first byte-time is consumed receiving the address, so **at most N bytes ever
leave on MISO**. The surplus byte stayed in the TX FIFO/OSR.

Nothing reset the TX SM at CS deassert (old `gpio_cs_deassert_callback`), so
that surplus byte led the *next* CS assertion. What the master would actually
see: transaction 2 returns `[stale byte from txn 1, reg[a], reg[a+1], …]` —
every byte shifted one position late, with a plausible-looking but wrong
temperature — and a further surplus byte accumulating each time until the
4-deep TX FIFO jams and the SM stalls permanently. Not a desync that
self-heals: it compounds.

**Fix:** `max31856_pio_engine.c:85-97` new `tx_reset()` — disable, clear
FIFOs, `pio_sm_restart()` (this is what clears the OSR/ISR shift counters and
the stalled state), `pio_sm_exec(jmp offset_tx)` to re-enter at the `set y, N`
preamble, explicitly re-tri-state MISO, re-enable. Called unconditionally from
`gpio_cs_deassert_callback()` (`:229`) — unconditionally so that a CS glitch
that never completed an address byte also cannot leave the SM mid-byte.

### D3 — `first_byte_late` fired on every single transaction (instrumentation)

The old `poll_tx_underrun()` counted any latched `FDEBUG.TXSTALL`. But the TX
SM *structurally* stalls at the start of every transaction: it enters
`byte_loop` on the first SCLK edge after CS falls, while the first response
byte cannot exist until the address byte has finished arriving eight clocks
later. So the counter incremented once per transaction forever, making
PLAN §3.2.1's "a run with nonzero underruns is flagged invalid" rule
vacuous.

**Fix:** split into `tx_clear_stall()` (`:56`, discard the structural stall,
called right after the address byte is handled at `:140`) and
`tx_note_stall()` (`:61`, count a stall latched *after* a byte was already
staged — a genuine mid-burst underrun — called at `:162` before staging each
subsequent byte and once at CS deassert for reads, `:220`).
`max31856_pio_engine.h`'s field comment now records the narrower meaning and
that first-byte lateness specifically is **not** observable in-band.

### D4 — register machine used a 4-bit address space; the part uses 7 (fidelity)

`MAX31856_ADDR_MASK` masked the address byte with `0x0F` and both clock
functions incremented `% MAX31856_REG_COUNT`, i.e. `0Fh → 00h`. The datasheet
(p.15) says otherwise:

> "The address continues to increment through all memory locations as long as
> CS remains low. If data continues to be clocked in or out, the address will
> loop from 7Fh/FFh to 00h/80h. Invalid memory addresses report an FFh value.
> Attempting to write to a read-only register will result in no change to that
> register's contents."

So the old code (a) wrapped six registers too early and (b) aliased e.g.
address `15h` onto CR1 instead of reporting `FFh`. `max31856_regs.h:62-75`
even claimed the 4-bit wrap "matches the real part", which it does not.

**Fix:** `max31856_regs.h:73-75` (`ADDR_MASK` → `& 0x7F`, new
`MAX31856_ADDR_SPACE` / `MAX31856_INVALID_ADDR_VALUE`),
`max31856_regs.c:223-226` (reads of `10h..7Fh` return `FFh`), `:141-148`
(writes there are swallowed but still set `master_has_written`, same rule as
the read-only registers), `:226`/`:240` (increment `% 0x80`).

**Reachability:** neither real master can hit this (§1). `tools/spi_test_master`
can, and the fixture's whole value proposition is being indistinguishable from
the part, so it is fixed rather than documented as a limitation.

---

## 4. Mode-1 / bit-order / line-timing verification (what was actually checked)

Re-derived from scratch rather than trusting the file's own comments, because
this file previously shipped a mode-0 program labelled mode 1.

| Property | Verified? | Evidence |
|---|---|---|
| Responder samples MOSI on the **falling** edge | **Yes** | `.pio:136-138`: `wait 1 pin 31` (rising, *not* the sample point) → `wait 0 pin 31` (falling) → `in pins, 1`. Matches datasheet Table 5 CPOL=0 "Data bit latch — SCLK falling", and both masters' CPHA=1 config (`MAX31856.h`'s `MAX31856_SPI_MODE 1`; `SaftyFW/src/spi_owner.c:29`). The old mode-0 bug is genuinely gone. |
| Responder shifts MISO on the **rising** edge and holds through falling | **Yes** | `.pio:164-166` (bus A), `:192-194` (bus B): `wait 1` → `out pins, 1` → `wait 0`. Matches Table 5's "SDO next data bit shift — SCLK rising". |
| SCLK is the pin the `wait`s actually watch | **Yes** | `WAIT … PIN` indexes off the SM's IN base. RX IN base = MOSI, offset 31 ⇒ MOSI−1. TX IN base = CS0, offset 29 ⇒ CS0−3. Bus A GPIOs 6/7/8/9-11 and bus B 12/13/14/15 (`spi_emu_a.c:46-51`, `spi_emu_b.c:33-36`) satisfy both by construction. |
| RX byte order (MSB-first, low 8 bits of the FIFO word) | **Yes** | `sm_config_set_in_shift(&c, false, true, 8)` at `max31856_pio_engine.c:286` = shift **left**, autopush at 8 ⇒ byte in `word & 0xFF`, which is what `:113-114` reads. `mov isr, null` at `.pio:132` keeps the high bits clean. |
| TX byte order (MSB-first, left-justified) | **Yes** | `sm_config_set_out_shift(&tc, false, true, 8)` = shift left ⇒ OUT emits OSR bit 31; `tx_push_byte()` pushes `byte << 24` (`:31`). Consistent. |
| TX SM's *IN* shift direction | **Was wrong** | D1 above. This is the one that the `.pio` comments asserted and the C did not deliver. |
| MISO tri-state when no CS asserted | **Now yes** | Only after D1's fix; `set pindirs, 0` at `.pio:159`/`:171` was unreachable before. |
| CS polarity / `jmp pin` sense | **Yes** | `sm_config_set_jmp_pin(&c, cs_gpio[i])` (`:285`), active-low CS, `jmp pin` jumps when HIGH = deasserted (`.pio:134`, `:139`). Pull-ups on all inputs at `:241`. |
| **~DRDY line timing** | **NOT verifiable — no implementation exists** | There is no DRDY output anywhere in `firmware/SimFW/src/` (grep: the only hit is a comment in `max31856_regs.h:264`). See §6. |
| **~FAULT line timing** | **Partially.** The *level* logic exists and is correct (`max31856_regs.c:353-367`: maskable bits gated by MASK, TCRANGE/CJRANGE unmaskable, `spurious_fault_pin` override). No code drives a GPIO from it, and no assert/deassert latency spec is implemented. |

---

## 5. CS deassert between bytes, and long bursts with CS held low

**Does the responder require CS to deassert between bytes to resync?**
No — and it must not, because the part explicitly does not. Within one CS
assertion the RX SM stays in `bit_loop` and autopush delivers a byte every 8
sampled bits (`.pio:135-140`); byte framing comes from the 8-bit autopush
threshold, not from CS. CS deassert is a *reset to a clean byte boundary*
(`.pio:131-132`), used for aborted transactions and between transactions — not
a per-byte requirement.

**What happens if a master holds CS low across a long burst?** Correct
behaviour, up to the auto-increment address space: the register machine keeps
serving `addr++` (`max31856_regs.c:226`) with a coherency snapshot taken once
at CS assert (`:189-191`), so a 16-byte burst cannot tear. Past `0Fh` it now
serves `FFh` and wraps at `7Fh` (D4). There is no per-burst length limit in
the responder — nothing counts bytes, so an arbitrarily long CS-low window is
fine as long as the ISR keeps up. Neither master goes past 17 bytes anyway.

**Where CS *is* load-bearing:** the `tx_reset()` at CS rise (D2) is what keeps
consecutive transactions byte-aligned. A master that held CS low across two
logical operations would never get that reset — but as established in §0 no
master in this repo can do that, and the real part would not tolerate it
either (it has no concept of a second address byte inside one CS; those bytes
would just be more auto-incremented data).

**If CS never rises** (e.g. a wedged master), the responder stays in the open
transaction indefinitely: `max31856_pio_engine_channel_busy()` stays true and
`spi_emu_a.c:110` therefore skips that channel's conversion updates. It does
not hang or corrupt anything — it simply freezes that channel's registers
until CS rises. There is no timeout. Acceptable, but worth knowing.

---

## 6. Remaining gaps (not defects in what exists — things that do not exist)

1. **No ~DRDY output driver.** The real part asserts ~DRDY when a conversion
   completes and releases it when the master reads CJTH/LTCB — and *both*
   masters depend on that edge: KilnFW samples it before the burst
   specifically because the burst clears it (`MAX31856.c:1023-1029`), and
   SaftyFW's `thermo_task` does the same (`SaftyFW/src/max31856.c:187-188`).
   This is a **read-shape-dependent side effect** the register machine has no
   hook for: `max31856_regs_clock_read_byte()` does not notice that the read
   touched `0Ah..0Eh`. Whoever adds the DRDY GPIO must add that hook, or the
   DUTs' freshness logic will be tested against a lie. Not blocking the PIO
   freeze; blocking honest DRDY emulation.
2. **The `1.6 µs` first-byte budget in PLAN §3.2.1 is wrong by ~8×.** The
   address byte's value is only known after its *last* bit is latched (falling
   edge of clock 8). In mode 1 the responder must present the first response
   bit on the *rising* edge of clock 9. The real budget is therefore about
   **half an SCLK period — ~125 ns at 4 MHz, ~100 ns at 5 MHz — not one
   byte-time.** RP2040 IRQ entry plus the handler body cannot do that, so
   PLAN's Plan A (ISR staging) is very unlikely to work at either master's
   clock, and the failure mode is not "one late byte" but a whole burst
   shifted by however many clocks the ISR was late. Note the structural
   consequence: with Plan A the TX SM *always* stalls at the start of a read,
   which is precisely why D3's counter was useless. **This is a design
   decision for the coordinator, not something this audit changed.** A
   DMA-fed variant of Plan B (a control DMA channel reading the address byte
   out of the RX FIFO and rewriting a data DMA channel's `read_addr` to
   `&regs[addr]`, chained) is the obvious candidate — sub-200 ns, no CPU in
   the path — but it is a redesign, not a fix.
3. **Bus A's shared TX SM is single-threaded across three CS lines.** Correct
   for SPI (only one chip is ever selected), and `tx_reset()` now bounds any
   cross-transaction leakage. Two CS lines low simultaneously would be a
   master defect; the responder would interleave and the audit trail
   (`protocol_errors`) would not catch it. Not worth fixing.

---

## 7. What §11 item 4 and §0.1 should now say

*(Recorded here for the coordinator — `PLAN.md` is deliberately untouched.)*

**§11 item 4** → mark `[x]` **resolved 2026-08-20** by
`docs/SPI_ACCESS_AUDIT.md`: no write-then-read or any multi-phase transaction
occurs within one CS assertion on either master, structurally guaranteed by
both transports' one-CS-per-transfer design; six distinct transaction shapes
enumerated, all handled. Note that the audit found and fixed three responder
defects (TX SM IN shift direction, missing TX reset at CS deassert, useless
underrun counter) plus one register-map fidelity defect, and that it raises a
new question about §3.2.1's latency budget (§6 item 2 above) which should
become its own §11 entry rather than reopening item 4.

**§0.1** → the "audit both masters' drivers before freezing the PIO program"
item is done; add a new software-doable item for the ~DRDY read-clears hook
(§6 item 1) and a new M-A item for re-deciding Plan A vs a DMA Plan B in light
of the corrected latency budget.

---

## 8. Verification status of this document

* Every master-side and responder-side claim is cited to `file:line` and was
  read, not inferred.
* The transaction inventory in §1 is covered executably by
  `test/test_max31856_regs.c::test_real_master_transaction_shapes()`, which
  replays each shape byte-for-byte through the register machine.
* D4's fix is covered by the rewritten `test_auto_increment()`.
* Host tests: **5027/5027** (baseline was 5016; +11 new checks).
* `arm-none-eabi-gcc` build of `firmware/SimFW` clean under
  `-Wall -Wextra -Werror`.
* **D1, D2 and D3 are code-proved, not bench-proved.** They are argued from
  the RP2040 datasheet's documented `IN`/`SHIFTCTRL`/`FDEBUG`/`SM_RESTART`
  semantics; no Pico was attached. The Saleae capture M-A requires is still
  required, and now has three specific things to confirm: MISO goes Hi-Z
  between transactions, transaction *k+1* is not byte-shifted relative to
  transaction *k*, and MOSI is sampled on falling edges.

---

## 9. Resolution of §6 items 1 and 2 (implemented 2026-08-20)

Both gaps §6 raised have been closed. §6 is left as written — it is the
finding, not the fix — and this section records what was built against it.

### 9.1 Plan B, DMA-fed, is now what ships

`max31856_pio_engine.c` no longer stages response bytes from an ISR. The chain
is PIO + DMA only:

```
PIO RX SM ─ assembles each MOSI byte straight into a word-aligned POINTER
            into the channel's 256-entry response image
   │ RX-FIFO-not-empty DREQ
   ▼
DMA "sniff"  (per RX SM, transfer_count = 1) → bus->addr_capture, chains to
   ▼
DMA "load"   (per bus) → data channel's al3_read_addr_trig  (sets the read
   │                     address AND starts it), raises DMA_IRQ_0
   ▼
DMA "data"   (per bus) → PIO TX FIFO, paced by TX-FIFO-not-full, 512-byte
                         read-address ring
   ▼
PIO TX SM ─ MISO
```

The address byte never becomes a *value* anyone has to act on: the PIO
assembles it as `image_base | (addr << 2)` by preloading the ISR with
`base >> 10` and using a **10-bit** autopush threshold (8 sampled bits plus
`in null, 2` of word scaling). The 1024-byte alignment that makes that OR an
OR is checked at run time, not asserted in a comment.

Which word is the address byte is decided solely by the sniff channel's
transfer count of 1. The PIO program is deliberately *stateless* about it — it
preloads the base on every byte — so a missed CS edge cannot mis-frame a
transaction, only re-enter the byte loop with a correct pointer.

**Does it meet 125 ns? Honestly: not at the default 125 MHz sysclk.** Summing
RP2040 §2.5 semantics and published single-transfer DMA latencies, the path is
~19–27 sysclk cycles ≈ **150–215 ns at 125 MHz**. That misses the *design*
deadline (first response bit present at SCLK rising edge 9) but clears the
*hard* deadline (the master's own sample point at falling edge 9, i.e. a full
SCLK period minus setup: 250 ns at 4 MHz, 200 ns at 5 MHz) with margin at
4 MHz and little at 5 MHz. Raising sysclk to 200 MHz puts the path at
~95–135 ns and meets the design deadline at 4 MHz. **This is arithmetic from
the datasheet, not a measurement** — see §8. The recommendation for the
coordinator is: run the fixture at 200 MHz, or hold the masters to 4 MHz, and
treat the Saleae capture as the arbiter.

**Decided 2026-08-20 (`docs/PLAN.md` §3.2.1), superseding the "run at
200 MHz or hold to 4 MHz" recommendation above with the second option:** the
thermocouple SPI clock is now capped at 4 MHz on both real masters, not left
open. `KILNCTL_THERMO_SPI_CLOCK_HZ`'s Kconfig `range 100000 4000000` (KilnFW)
plus a `_Static_assert(SPI_OWNER_BAUDRATE_HZ <= 4000000u, ...)` on
`SaftyFW/src/spi_owner.c`'s `SPI_OWNER_BAUDRATE_HZ` enforce this at build
time on both sides, so the sysclk-bump path is no longer under
consideration. This document's own 5 MHz figures above are therefore
historical margin analysis, not a live target — they explain *why* 4 MHz was
chosen (comfortable hard-deadline margin) and *why* 5 MHz was rejected
(little to no margin), not an open question. M-A's exit criterion (§7, §8)
is a Saleae capture at 4 MHz, the masters' actual ceiling — not 5 MHz. Note
this is the **thermocouple** SPI clock only; the separate display SPI symbol
(`KILNCTL_DISPLAY_SPI_CLOCK_HZ`, 20 MHz) is unrelated and unaffected by this
cap.

Design notes on the three constraints that had to survive:

* **MISO tri-state (D1).** Unchanged and preserved: the TX programs and D1's
  `sm_config_set_in_shift(&tc, false, false, 32)` are untouched. `tx_reset()`
  still re-tri-states explicitly at every CS rise.
* **Image coherence.** Not the `sim_snapshot.h` seqlock — a DMA channel cannot
  retry a torn read, so optimistic-read-and-retry is unavailable to it. The
  same *shape* is used at the granularity the hardware can consume: each
  channel has **two response-image banks**, the owner task publishes into the
  bank the hardware is not pointed at, and hands over the new base with one
  atomic 32-bit `pio_sm_put()`. The RX SM adopts a new base only at its `idle`
  loop, i.e. only while CS is high, so a burst in flight always finishes out of
  the coherent bank it started on. The retired bank is not rewritten for
  another full 20 ms scan against a 34 µs worst-case transaction, and the
  publish is additionally gated on `channel_busy()`.
* **D2 (surplus TX bytes).** Still necessary — more so, since the data channel
  runs ahead by up to five words — and preserved: `tx_reset()` at CS rise,
  now explicitly ordered *after* stopping the data channel so the DMA cannot
  refill the FIFO that was just cleared.
* **D3 (`first_byte_late`).** Made structural rather than bookkept.
  `FDEBUG.TXSTALL` is no longer read at all, because the TX SM still stalls
  structurally at the start of every transaction. The counter is now derived
  from the data channel's own delivered-word count (`delivered < bytes the
  master clocked` ⇒ unambiguous starvation), plus one new genuinely observable
  case: the CS-rise handler finding the *next* transaction's address word
  already waiting when it re-arms the sniff channel. First-byte lateness
  *within* the 125 ns window remains unobservable in-band, as §6 noted.

Known, deliberate fidelity change: `corruption.bit_error_rate` is now applied
when the image is published rather than per byte clocked, so it re-rolls every
20 ms scan instead of every byte. `dead_mode` — the knob that matters for
fault injection — is exact either way. Recorded here rather than quietly
absorbed.

### 9.2 ~DRDY exists

Implemented in both halves §6 item 1 asked for.

* **Register machine** (`max31856_regs.c`, pure and host-tested):
  `drdy_asserted` is raised by `advance_conversion()` **only when the part
  would really have converted** — CR0.CMODE set, or a one-shot pending. It is
  released at `cs_deassert()` of a read transaction that clocked out any of
  LTCBH/LTCBM/LTCBL, or CJTH/CJTL **while CR0.CJ_DISABLE is clear** (the
  datasheet's "(if enabled)" qualifier, p15 and p24). Release is at completion,
  not per byte, per the datasheet's "completes". An SR-only poll (R2) does
  *not* release — which matters, because KilnFW issues R2 as a fault poll.
* **Pin**: open-drain emulation per PLAN §3.6 — the output latch is parked low
  forever and the *direction* is the control, so the fixture never drives the
  net high. Provisional GPIOs: 21/22/26 main-side, 27 safety-side.
