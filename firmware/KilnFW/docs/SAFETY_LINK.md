# Safety Processor Link

The ESP32-S3 main controller and the RP2040 safety processor (A1) live in two
electrically separate ground domains. This document covers the isolation
barrier between them, the wire protocol the RP2040 firmware has to implement,
the C API on the ESP side (`App/drivers/safety_link.h`), the UART subcommands
the PC can drive it with, and how to exercise the whole thing with no Pico
attached.

The RP2040 firmware (`firmware/SaftyFW`) now exists and the link has been run
end to end on the bench for the first time (2026-08-23): `safety_get_status()`
returns real telemetry — link up, safety thermocouple invalid (no sensor
fitted on that run), all three currents 0.00 A, the frame a few hundred
milliseconds old. Getting there needed a baud-rate correction (see
"Transport" below) on top of the pin/inversion fixes in the traps below.

With no Pico attached, or before it is flashed, the link still comes up
cleanly on the ESP side alone: polls go out, every one times out, and
`GET_STATUS` reports `link_up = 0` with `age = 65535`. That is a designed-for
state, not a bug, and it is what the "How to test this without a Pico"
section below still exercises. Nothing is faked and nothing aborts startup
either way.

## The isolation barrier

**2026-08-25 update: the UART pair no longer crosses through an optocoupler.**
U2 and U3 (the TCMT1109 pair that used to carry `DataToSafty`/`DataFromSafty`,
along with R7/R12/R15) have been removed and replaced by **U6, a single
ADuM1201WT digital isolator** with one non-inverting channel per direction.
The fault line is untouched and still crosses through its own TCMT1109
optocoupler, U1. Traps 1 and 3 below still describe the current board; Trap 2
(inversion) and the baud-rate section after it describe the *retired*
optocoupler pair and no longer apply to the fitted hardware — each is marked
where it stops applying.

| Part | Driven by | Output | Direction |
|------|---------------|--------------------|-----------|
| U6 (VIB/VOB) | Pico `PicoTx` (GP4, safety side) | ESP GPIO4, net `DataFromSafty` | Pico -> ESP data |
| U6 (VIA/VOA) | ESP GPIO5, net `DataToSafty` | Pico `PicoRx` (GP5), R9 1k pull-up | ESP -> Pico data |
| U1 (opto) | ESP GPIO6 via R11 390R, net `Fault` | Pico `mainFault` (GPIO10), R8 1k pull-up | ESP -> Pico fault |

There are three traps here, and getting any of them wrong produces a link that
comes up cleanly and silently never works.

### Trap 1: this was got wrong twice by tracing, and is now settled by measurement

`DataToSafty` (**GPIO5**) is the ESP's **TX** and `DataFromSafty` (**GPIO4**)
is the ESP's **RX**. `Kconfig` follows this: `KILNCTL_SAFETY_TX_IO` defaults
to 5, `KILNCTL_SAFETY_RX_IO` to 4.

Two earlier revisions of this document argued the opposite assignment, each
from a different chain of schematic reasoning, and both were wrong. The
history is worth keeping because the failure mode repeats:

- The **first** version said the net names were "backwards" — that
  `DataToSafty` was the ESP's RX — reasoning from U3's symbol as if it were
  drawn the same way round as U1 and U2. U3 *is* mirrored, so that reasoning
  was faulty, and the conclusion was wrong.
- The **second** version corrected the mirroring but landed on GPIO4=TX /
  GPIO5=RX, resting on what looked like an independent confirmation: R15, a 1k
  pull-up, sat on GPIO5, and a pull-up only belongs on an open collector. The
  premise was true of the schematic and false of the intent — **R15 was
  connected to the wrong net**, and the trace inherited the error. The same
  reasoning also missed that the top-level sheet crossed the two hierarchical
  pins, so `DataToSafty` (main sheet) and `DataFromSafty` (safety sheet) were
  a single net that KiCad happened to name after the main sheet.

Both schematic errors were fixed on 2026-08-22/23 (R15 moved to the GPIO4 net,
sheet pins uncrossed, labels renamed), and the pin map is now a **bench
measurement** rather than a trace — see
[`HARDWARE.md`](HARDWARE.md) §"How this was measured". That measurement was
taken against U2/U3, the optocoupler pair since replaced by U6 (each
optocoupler channel is unidirectional by which pin carries the LED anode vs.
the collector; each of U6's channels is unidirectional too, by which pin is a
VIx input vs. a VOx output) — the direction of each net is unchanged by the
2026-08-25 rework, only the mechanism that fixes it in place is different.

### Trap 2 (HISTORY — applied to U2/U3, retired 2026-08-25): each direction needed exactly one inversion, and both ends had to supply theirs correctly

**This entire trap describes the optocoupler pair that has been removed. As
of 2026-08-25 the barrier is U6, an ADuM1201WT digital isolator, which does
NOT invert either direction; both firmwares' inversion (`UART_SIGNAL_TXD_INV`
in `KilnFW`'s `safety_link.c`, `gpio_set_outover(..., GPIO_OVERRIDE_INVERT)`
in `SaftyFW`'s `uart_owner.c`) has been removed to match, and re-adding either
now would invert an already-correct signal and break the link.** The
narrative below is kept as the record of why the old parts needed exactly one
inversion per direction, and remains useful if this barrier is ever put back
on optocouplers.

An optocoupler is not a wire. The driving side's high lights the LED, the
phototransistor conducts, and the receiving side's collector is pulled **low**.
Left uncorrected, an idle-high UART line would arrive idle-**low**, in both
directions — and a continuously-lit LED between frames besides.

**ESP -> Pico (GPIO5, through U2)** is fixed entirely on the ESP side, in the
UART peripheral:

```c
uart_set_line_inverse(SAFETY_UART_PORT_NUM, UART_SIGNAL_TXD_INV);
```

It must be called *after* `uart_param_config()` (which happens inside
`uart_owner_init()`), because that rewrites the same register block. `TXD_INV`
drives an idle logical 1 as a physical low on GPIO5, so U2's LED is **off** at
idle, so R9 holds the Pico's RX **high** — a correct, standard-polarity idle
that the Pico's plain hardware UART reads with no changes on its side at all.
A start bit (logical 0) drives GPIO5 high, lights the LED, and pulls the
Pico's RX low, exactly like an ordinary UART start bit. Both halves are
measured, not asserted: driving GPIO5 low reads GP5 high at the Pico, driving
it high reads GP5 low.

**Pico -> ESP (GP4, through U3)** cannot be fixed the same way, because the
RP2040's PL011 UART peripheral has no line-inversion control the way the
ESP's does. For a long time this direction was simply left uncorrected:
`SaftyFW` drove GP4 straight from its hardware UART TX, which idles high
(UART mark), so U3's LED sat lit continuously between frames — burning power
across the barrier the whole time the link was idle, not just while
transmitting. The fix, landed and verified 2026-08-23, uses the RP2040's GPIO
block instead of its UART block: `SaftyFW`'s `uart_owner.c` now calls

```c
gpio_set_outover(SAFTYFW_PIN_UART1_TX, GPIO_OVERRIDE_INVERT);
```

which inverts the signal at the pad after the UART peripheral has already
produced it, independent of the peripheral's own polarity control (which
doesn't exist for this purpose on this part). Idle mark now reaches the GP4
pad as low, so U3's LED is dark at idle, and on the ESP side R15's 1k
pull-up and the internal pull-up hold GPIO4 **high** — standard mark, exactly
what a UART receiver expects. Because the inversion already happened on the
Pico's pad, `KilnFW`'s `safety_link.c` applies `UART_SIGNAL_TXD_INV` only to
its UART peripheral; it does **not** apply `RXD_INV`. GPIO4 needs no software
inversion on the ESP side any more — the signal arriving at the pad is
already correct polarity.

Summarizing both directions:

| Direction | Where the inversion happens | ESP `uart_set_line_inverse` flags |
|-----------|------------------------------|-------------------------------------|
| ESP -> Pico | ESP UART peripheral (`TXD_INV`) | `UART_SIGNAL_TXD_INV` |
| Pico -> ESP | Pico GPIO pad override (`gpio_set_outover(..., GPIO_OVERRIDE_INVERT)`) | none — GPIO4 needs no `RXD_INV` |

The corollary to remember is unchanged in spirit but sharper now: **exactly
one inversion per direction, applied once.** Before 2026-08-23 the firmware
compensated for the missing Pico-side fix by also setting `RXD_INV` on the
ESP, which happened to make the *data* correct (two inversions — U3's and
`RXD_INV`'s — cancelling back to the right logic levels) while leaving U3's
LED lit at idle the whole time; that combination worked for framing but was
wrong for the optocoupler's operating point, see below. Do **not** add
`RXD_INV` back now that the Pico-side fix is in place — the signal at GPIO4
is already correct, and re-inverting it breaks the link.

Two boot-time artefacts follow from this and are worth designing the Pico
firmware around:

- While the ESP is in reset or before `safety_link_start()` runs, GPIO5 (TX,
  `DataToSafty`) is high-impedance and nothing on the main board pulls it —
  R15 sits on the *RX* net, not this one. U2's LED is dark, and R9 holds the
  Pico's RX **high**, a clean idle, not a break. The Pico still must tolerate
  a break on its RX (a half-driven line during ESP boot can produce one), but
  it is not the guaranteed steady-state condition through an ESP reset.
- The reverse case is covered under "Side effect worth knowing about" below.

`GPIO4` is the bare collector of U3. R15 (1k to 3.3V_Main) already sits on
this net — the ESP's internal pull-up is belt-and-braces, not load-bearing,
but `safety_link_start()` still sets it explicitly rather than relying on an
external part it does not control.

### Trap 3: the fault line is an ESP *output*

`GPIO6` is driven by the ESP. High lights U1's LED, which pulls the Pico's
`mainFault` input low. There is no hardware path for the Pico to signal the
ESP — everything coming back does so over the isolated UART. Configuring
GPIO6 as an input, or reading it expecting the Pico's opinion, gets you
nothing.

The line is configured as an output driven **low (de-asserted)** before the
UART is even touched, because the pin powers up floating and a floating gate
on U1 is an undefined fault state at the safety processor.

Measured 2026-08-23: with the fault asserted, ESP `GPIO_OUT_REG` bit 6 is set
and the Pico's `mainFault` (GP10) reads **low**; with the ESP held in reset,
GP10 reads **high**. Note what the second reading means — **this line fails
de-asserted.** A dead, unpowered or held-in-reset main controller leaves U1's
LED dark and R8 holding `mainFault` high, which the Pico reads as "the main
controller is fine". The safety processor must infer a dead main controller
from UART silence; the fault line cannot tell it.

### Side effect worth knowing about (HISTORY, U2/U3-era; recheck against U6)

With no Pico attached and U2/U3 fitted, U3's LED was never lit, the
phototransistor never conducted, and R15 plus the internal pull-up held
GPIO4 high. Since GPIO4 no longer carried `RXD_INV` (see Trap 2's history),
that high reached the UART peripheral as an ordinary idle mark, not as a
permanent break — the absent safety processor simply looked like silence,
which is what `link_up = 0` / `age = 65535` already expects. Before the
2026-08-23 fix, with `RXD_INV` still applied, the same idle-high GPIO4 was
inverted into a continuously-low internal RX line — a permanent break
condition — and the UART event task in `uart_owner.c` counted those, so
`uart_owner_get_rx_error_count()` for UART1 (and therefore the CRC/framing
counter in `GET_LINK_STATS`) climbed on a board with no safety processor
fitted.

With U6 now fitted and no software inversion anywhere on this link, GPIO4's
idle level with no Pico attached depends on how the ADuM1201 behaves with its
Pico-side supply unpowered (VOB may float rather than idle high the way U3's
open collector plus R15 did) — this has not yet been re-measured against U6
and should be confirmed on the bench rather than assumed to match the
optocoupler-era behaviour above.

## ESP <-> Pico contract

Transport: ESP32-S3 **UART1**, 8N1, `CONFIG_KILNCTL_SAFETY_BAUD_RATE`,
**230400** as of the 2026-08-25 sweep against U6 (see `KilnFW/App/drivers/Kconfig`
for the full table and the current value), no line inversion on either end
any more (the ADuM1201 in U6 does not invert; see Trap 2's history for the
inversion this link used to need under the retired optocoupler pair). The
payload framing is
the **same `uart_protocol` stack the PC link uses** — 0x7E-delimited,
byte-stuffed, CRC16/CCITT-FALSE, indexed DATA frames with ACK/NACK, retried and
de-duplicated by the protocol layer. See `docs/UART_PROTOCOL.md` for the
envelope; there is deliberately no bespoke framing on this link.

### HISTORY: the baud rate used to be capped by the optocouplers, not by the UART

**This section documents the retired U2/U3 optocoupler pair. U6, the digital
isolator that replaced it on 2026-08-25, is not subject to this ceiling; do
not read the 9600 figure below as current.** The TCMT1109s and R15's 1k
pull-up could not switch fast enough for a 115200 bit (8.7 us). Measured
2026-08-23, walking the rate down with both sides changed together, RP2040
transmitting a status frame every 500 ms:

| Baud | Result |
|-----:|--------|
| 115200 | zero frames received, ever |
| 57600 | zero frames received, ever |
| 38400 | ~80% received (53 of 66), errors climbing |
| 19200 | clean over a short window (20 of 20), but ~10% lost over a longer one (107/118, then 117/134) |
| 9600 | received tracks sent one for one over minutes (48/52, then 72/75) — the committed value for as long as the optocoupler pair was fitted |

At the time, `CONFIG_KILNCTL_SAFETY_BAUD_RATE` on this side and
`UART_OWNER_BAUD_RATE` in `SaftyFW`'s `src/tasks/uart_owner.c` were both
hardcoded to 9600 for this reason; there is still no negotiation, so a change
to one without the other is a dead link regardless of what number is chosen.
Raising the rate used to need a faster optocoupler or a real line driver in
place of the TCMT1109/R15 pair; now that U6 (an ADuM1201WT) is fitted instead,
that constraint no longer applies.

### The 2026-08-25 sweep against U6

Walking the rate down again, 60 seconds continuous traffic per rate, both
sides rebuilt and reflashed for each rate (RP2040 status pushes at the
500 ms poll period). A rate passes only if none of four counters —
crc/framing errors, length mismatch, crc mismatch, resync — gain a single
count during the window, and status replies keep answering every poll.
`timeouts` and `config_page` are excluded from the verdict: both are
dominated by the separate open `safety_cfg_store_refetch` defect (page 0
failed / `ESP_ERR_TIMEOUT`), which climbs identically at every baud rate
regardless of whether the link itself is healthy.

| Baud | Result |
|-----:|--------|
| 921600 | PASS — 0 new crc/framing, 0 length mismatch, 0 crc mismatch, 0 resync; status +124 in 62 s (2.00/s) |
| 460800 | PASS — 0 new crc/framing, 0 length mismatch, 0 crc mismatch, 0 resync; status +123 in 62 s (2.00/s) |
| 230400 | PASS — 0 new crc/framing, 0 length mismatch, 0 crc mismatch, 0 resync; status +124 in 62 s (2.01/s) |

**Negative control**, proving the test can fail: ESP at 230400 against a Pico
deliberately left at 115200 failed in 3 s — crc/framing errors rose
941 -> 1062 (+121), length mismatch 58 -> 69.

**Chosen value: 230400** (the committed default). Rule used: the fastest
passing rate was 921600; one standard step down from that is 460800; the
chosen value is the slower of that step and the requested 230400 — so
230400, two full standard steps below a rate that soaks clean.

Two methodology points this sweep depends on, because a first attempt at it
got both wrong:

1. **`sent` and `received` are not a loss metric.** `sent` counts outbound
   requests of several kinds; `received` counts status replies only, and
   equals the histogram's `status` count exactly. Comparing the two shows
   ~50% "loss" at every rate, including the deliberately mismatched
   negative control — that is the metric being wrong, not the link.
2. **Flashing resets the Pico while the ESP keeps running**, and that reset
   throws a genuine burst of framing errors (plus `break condition on
   uart1` / `frame length mismatch` log lines). The measurement window must
   start after a settle delay following any flash, or every rate fails on
   reset residue rather than its own behavior. The first sweep attempt
   misread exactly this residue as a 921600 failure; 921600 in fact passes
   cleanly once measured past the reset.

See `KILNCTL_SAFETY_BAUD_RATE` in `KilnFW/App/drivers/Kconfig` for this same
table kept next to the default it justifies.

### Open defect found after the sweep: `SAFETY_INBOX_LEN` overflows at 230400, and the 60 s soak did not catch it (2026-08-25)

The sweep above passed at 230400 because it only measured the four
physical-layer counters and status cadence, and those stayed clean for 60 s.
Left running for three minutes after an ESP reset, the link itself came
apart: `link_up` flapped (up at 80 s, down 100-140 s, up again at 160 s)
while `broadcast dropped` climbed continuously at about 2.8/s (78 -> 450 over
160 s) — with crc/framing, length mismatch, crc mismatch and resync all
staying at zero new counts the entire time. The bytes are arriving intact;
something above the wire is failing to keep up with them.

Mechanism: `SAFETY_INBOX_LEN` is 4 (`safety_link.c:83`). `safety_poll_task`
blocks for seconds at a time inside the failing `safety_cfg_store_refetch()`
calls documented above (263 `config_page` requests observed, all timing out —
this is that same pre-existing defect, not a new one). Four inbox slots
overflow long before the task returns to drain them. At 9600 baud the peer's
broadcast rate was slow enough that four slots were plenty; at 230400 it
is not — raising the baud converted a latent, wire-throttled bug into an
active one that destabilises the link.

**This is why the 60 s soak windows above did not catch it**: they measure
physical-layer counters and status rate, and both still look healthy inside
one minute. Anyone re-running this sweep needs a longer window (multiple
minutes) if they want to see this failure mode.

Candidate directions, not yet implemented: raise `SAFETY_INBOX_LEN` (buys
headroom, changes no timing), or stop the failing refetch from monopolising
the transaction lock. **Warning:** two previous attempts to fix the
`config_page` refetch defect panicked `safety_poll_task` and were reverted —
that path must not be changed casually.

**A static GPIO high/low test across this link passes at any baud rate**,
because both an optocoupler and a digital isolator carry a DC level perfectly
well — only a bit that switches fast enough to matter exposes any real
limit. The wiring here had already been bench-verified in both directions
that way, fault line included, which is exactly why the old optocoupler-era
baud ceiling took so long to find: the wire looked proven, so
suspicion fell on framing, device/task ids and line inversion instead, all of
which were in fact already correct.

Addressing:

- ESP identifies as `UART_PROTO_DEVICE_ESP` (0).
- Pico must identify as `UART_PROTO_DEVICE_SAFETY` (2).
- Both ends register task_id `UART_TASK_ID_SAFETY` (7). Everything below is
  that task's payload.

### ESP -> Pico requests

| byte0 | Name | Args | Reply |
|-------|------|------|-------|
| 0x01 | `SAFETY_CMD_GET_STATUS` | none | the status frame below |
| 0x02 | `SAFETY_CMD_REQUEST_ENABLE` | byte1 = enable (0/1) | protocol ACK only |

`REQUEST_ENABLE` is **advisory**: the Pico may refuse, and its own interlocks
always win. The ESP learns the outcome from `SAFETY_FLAG_ENABLED` in the next
status. A status frame pushed unsolicited right after a `REQUEST_ENABLE` is
accepted and refreshes the cache, so a Pico that wants to answer immediately
may.

**Confirmed on the bench with no Pico present (2026-08-25):** RP2040 halted
over SWD to simulate an absent safety processor, then `safety_request_enable(True)`
called via the MCP tool (`tools/PcTools/src/kilnctrl/mcp_server.py:1746`).
It returned `ok - requested enable=True` — nothing was there to grant it.
This is the transport-ACK-means-queued-not-done gap the docstring above
already calls out; this is that gap actually observed, not a new mechanism.
Everything else about the absent-processor case behaved correctly: `link_up`
went false, the reported status age grew to its 65534 ms saturation, the
isolated fault line correctly asserted (`fault line asserted (by us)`, GPIO6
high), and the ESP's own PC link stayed alive throughout.

There is no `PING` on this wire: `safety_link_ping()` sends a `GET_STATUS`
right away instead of waiting for the next poll tick. The other `SAFETY_CMD_*`
values (`PING`, `GET_LINK_STATS`, `SET_POLL_PERIOD`, `SET_FAULT_OUT`) are
PC->ESP only and never cross the barrier.

### Pico -> ESP status frame (23 bytes)

Sent as the reply to `GET_STATUS`, addressed to (`UART_PROTO_DEVICE_ESP`,
task 7). May also be pushed unsolicited at any time — the ESP accepts either.

| Offset | Type | Field |
|--------|------|-------|
| 0 | u8 | `0x01` (`SAFETY_CMD_GET_STATUS`) |
| 1 | u8 | flags, see below |
| 2..5 | f32 LE | safety thermocouple temperature, degC |
| 6..9 | f32 LE | safety cold-junction temperature, degC |
| 10 | u8 | safety thermocouple fault status (`THERMO_FAULT_*` bits) |
| 11..14 | f32 LE | current sense 1, amps |
| 15..18 | f32 LE | current sense 2, amps |
| 19..22 | f32 LE | current sense 3, amps |

Flags (`SAFETY_FLAG_*` in `uart_task_ids.h`):

| Bit | Name | Owner |
|-----|------|-------|
| 0 | `LINK_UP` | **ESP** — Pico must send 0 |
| 1 | `FAULT` | **ESP** — Pico must send 0 |
| 2 | `ESTOP` | Pico: estop input asserted |
| 3 | `RELAY` | Pico: safety relay K4 energized |
| 4 | `ENABLED` | Pico: heating currently permitted |
| 5 | `TEMP_VALID` | Pico: the two temperatures are real readings |

Bits 0 and 1 are cleared by the ESP driver on receipt whatever the Pico sends.
They describe the ESP's view of the link and the fault line the ESP is itself
driving; a peer reporting them would only be reporting them back at us.

When `TEMP_VALID` is clear, send **NaN** for the two temperatures, not 0. An
explicit not-a-number is much harder to mistake for a cold kiln than a
plausible-looking zero.

The **age** field the PC sees (bytes 23..24 of the PC-facing `GET_STATUS`
response) is *not* on this wire. The ESP measures it from when the frame
arrived; the Pico has no clock the ESP trusts.

## C API

From `App/drivers/safety_link.h`. One `SafetyLinkClass` instance owns a second
`uart_owner_t` + `uart_protocol_t` on UART1, entirely separate from the PC link
on UART0.

```c
esp_err_t safety_link_start(SafetyLinkClass *link);
esp_err_t safety_link_stop(SafetyLinkClass *link);

esp_err_t safety_link_get_status(SafetyLinkClass *link, safety_link_status_t *out);
esp_err_t safety_link_request_enable(SafetyLinkClass *link, bool enable);
esp_err_t safety_link_ping(SafetyLinkClass *link);
esp_err_t safety_link_set_poll_period(SafetyLinkClass *link, uint16_t period_ms);
esp_err_t safety_link_get_stats(SafetyLinkClass *link, safety_link_stats_t *out);

esp_err_t safety_link_set_fault(SafetyLinkClass *link, bool assert_fault);
bool      safety_link_get_fault(SafetyLinkClass *link);
esp_err_t safety_link_set_fault_source(SafetyLinkClass *link, uint32_t source_mask,
                                       bool assert_fault);
uint32_t  safety_link_get_fault_sources(SafetyLinkClass *link);
esp_err_t safety_link_fault_on_link_loss(SafetyLinkClass *link, bool enable);
bool      safety_link_get_fault_on_link_loss(SafetyLinkClass *link);

size_t safety_link_build_status_payload(SafetyLinkClass *link, uint8_t *out); /* 25 bytes */
size_t safety_link_build_stats_payload(SafetyLinkClass *link, uint8_t *out);  /* 19 bytes */
```

`safety_link_start()` brings up UART1 on `SAFETY_TX_IO`/`SAFETY_RX_IO` at
`SAFETY_UART_BAUD_RATE` with no line inversion (the ADuM1201 in U6 does not
invert), enables the RX pull-up, drives `SAFETY_FAULT_IO` low, attaches the
protocol stack, registers
`UART_TASK_ID_SAFETY` and starts the poll task. It returns `ESP_OK` once the
*local* side is up; it cannot tell you whether anything is listening. That only
shows up as `link_up`.

### Polling and staleness

The poll task sends `GET_STATUS` every `poll_period_ms` (default
`CONFIG_KILNCTL_SAFETY_POLL_PERIOD_MS`, 500) and caches the last good answer
with a timestamp. `safety_link_get_status()` copies that cache and computes an
age — it **never** talks to the far side, so a dead safety processor shows up as
stale data, never as a hung call:

```c
typedef struct {
    bool     link_up;        /* a valid status within 3 poll periods */
    uint16_t age_ms;         /* 65535 (SAFETY_LINK_AGE_NEVER) = never received */
    uint8_t  flags;          /* SAFETY_FLAG_* as they go to the PC */
    float    tc_temp_c, cj_temp_c;
    uint8_t  tc_fault;
    float    current_a[3];
    bool     fault_asserted; /* what this firmware drives on GPIO6 */
} safety_link_status_t;
```

`link_up` is "a valid reply within **3** poll periods", so one dropped poll
does not flap it. `age_ms` saturates at 65534; 65535 is reserved for "nothing
has ever arrived" — a link that died a minute ago and a peer that has never
spoken are genuinely different situations. Before the first reply the cached
temperatures and currents are `NaN`, never 0.

Timeout budget per request: `uart_protocol_send` retries up to
`UART_PROTO_MAX_RETRIES` (10) times, so the ACK timeout is deliberately short
here (`SAFETY_LINK_ACK_TIMEOUT_MS` = 50 ms, vs the PC link's 200) — a dead peer
costs ~500 ms per poll instead of ~2 s. The status frame gets a further 250 ms
after the ACK, covering the Pico actually reading its thermocouple and three
ADC channels. Only the poll task ever waits that out.

A down link is logged **once** on the transition, then at most once per 60 s
(`SAFETY_LINK_DOWN_LOG_PERIOD_MS`) — not once per poll, which at the default
period would be two warnings a second. Recovery logs once at INFO.

`safety_link_request_enable()` and `safety_link_ping()` do block for the
exchange (worst case ~500 ms with no peer), so call them from a bridge or app
task. `safety_link_get_status()` and `safety_link_get_stats()` do not.

### Thread safety

Two mutexes. `state_lock` guards the cache, stats, poll period and fault mask,
and is only ever held for the duration of a field read/update — never across a
UART transaction, which is what keeps `get_status()` non-blocking. `xact_lock`
serializes whole request/reply exchanges: replies land in one shared inbox, so
two tasks exchanging at once could each consume the other's answer. `xact_lock`
is always taken *outside* `state_lock`.

### The fault line and its policy

The isolated fault line is driven from a **source mask**, not a single boolean.
The line is high whenever any source is set, so clearing one reason can never
clear another's assertion:

| Source | Meaning |
|--------|---------|
| `SAFETY_FAULT_SRC_MANUAL` | the PC's `SET_FAULT_OUT` (what `safety_link_set_fault()` maps onto) |
| `SAFETY_FAULT_SRC_PC_LINK` | PC control link lost |
| `SAFETY_FAULT_SRC_THERMO` | a main-board thermocouple faulted |
| `SAFETY_FAULT_SRC_SAFETY_LINK` | this link itself went stale |
| `SAFETY_FAULT_SRC_APP` | whatever else the app decides |

Only the last of these does the driver raise by itself, and only when
`fault_on_link_loss` is enabled. Everything else — "the PC link dropped", "a
thermocouple faulted" — is the caller's policy call; the driver supplies the
mechanism (`safety_link_set_fault_source()`) and stays out of it.

**`fault_on_link_loss` defaults to true, i.e. fail-safe.** The safety
processor's entire job is to cut heat when the main controller is
untrustworthy, and a main controller that cannot even be reached is the
clearest possible case of that. The failure mode we must never have is a dead
ESP leaving the kiln heating because nobody told the Pico; the failure mode the
default risks is a nuisance shutdown, which is recoverable. The cost today is
that with no Pico firmware the line sits asserted from the first missed poll —
correct, and harmless, since the only thing reading it is the processor that
isn't there. On a bring-up board or one built without the safety domain
populated, call `safety_link_fault_on_link_loss(link, false)`; it logs a
warning, because it is a deliberate removal of a safety behaviour rather than a
configuration detail.

`safety_link_stop()` deliberately leaves the fault line in whatever state it
was in. Tearing the link down is not evidence that the controller is healthy.

## UART subcommands (PC <-> ESP)

`task_id = UART_TASK_ID_SAFETY` (7) on the PC link. Full text in
`App/drivers/uart_task_ids.h`; examples here are payload bytes, hex, as handed
to `uart_protocol_send`.

| Subcommand | Payload example | Meaning |
|------------|-----------------|---------|
| `0x01 GET_STATUS` | `01` | query — 25-byte reply below |
| `0x02 REQUEST_ENABLE` | `02 01` | ask the Pico to permit heating |
| | `02 00` | ask it to drop heating |
| `0x03 PING` | `03` | poll the far side now |
| `0x04 GET_LINK_STATS` | `04` | query — 19-byte reply below |
| `0x05 SET_POLL_PERIOD` | `05 F4 01` | 500 ms (u16 LE) |
| | `05 00 00` | stop polling |
| `0x06 SET_FAULT_OUT` | `06 01` | assert the isolated fault line |
| | `06 00` | release the manual assertion |

`GET_STATUS` reply, no peer present (what you get with no Pico flashed or
attached) — 25 bytes:

```
01 00 00 00 C0 7F 00 00 C0 7F 00 00 00 C0 7F 00 00 C0 7F 00 00 C0 7F FF FF
^  ^  \__________/ \__________/ ^  \__________/ \__________/ \__________/ \___/
|  |   tc = NaN     cj = NaN    |   I1 = NaN     I2 = NaN     I3 = NaN    age
|  flags = 0 (link_up clear)    tc_fault = 0                        = 65535
subcmd                                                          (never seen)
```

`GET_STATUS` reply, live peer at 31.5 degC / cold junction 25.0 / 12.0 A on
channel 1, estop clear, relay on, enabled, temperature valid, link up, no
fault, 120 ms old:

```
01 39 00 00 FC 41 00 00 C8 41 00 00 00 40 41 00 00 00 00 00 00 00 00 78 00
^  ^  \__________/ \__________/ ^  \__________/ \__________/ \__________/ \___/
|  |    31.5 degC    25.0 degC  |    12.0 A        0.0 A        0.0 A     120 ms
|  flags = 0x39                 tc_fault = 0
subcmd
```

flags `0x39` = `LINK_UP`(0x01) | `RELAY`(0x08) | `ENABLED`(0x10) |
`TEMP_VALID`(0x20). They are a plain OR of the `SAFETY_FLAG_*` values —
compute them rather than copying a literal.

`GET_LINK_STATS` reply — 19 bytes: `04`, then frames sent u32 LE, frames
received u32 LE, CRC/framing errors u32 LE, timeouts u32 LE, poll period u16
LE. With no peer, sent climbs by one per poll, received stays 0, timeouts
tracks sent, and the error count follows UART1's line errors (see "side effect"
above).

Counter semantics: `frames_sent` counts requests handed to `uart_protocol_send`
— retransmissions happen inside that call and are not counted again.
`frames_received` counts well-formed status frames accepted. `timeouts` counts
requests that produced no usable answer, which includes a NACK (the peer's
protocol layer answered but has no task 7 registered) and an ACK with no status
frame behind it. `frame_errors` is this driver's rejected payloads plus
`uart_owner`'s line-error count for UART1.

## How to test this without a Pico

A Pico running `SaftyFW` is no longer required to get a real reply — see the
"live peer" example below. Nothing in this section needs it, though, and it
remains the right way to bring up the ESP side alone (bench work without the
safety domain populated, or CI).

### 1. The no-peer path (zero hardware)

Boot the board and watch the log. Expect exactly one
`no reply from the safety processor (never seen one)` warning, then silence for
a minute at a time — not one per poll. `GET_STATUS` must return `link_up = 0`,
`age = 65535`, NaN temperatures. `GET_LINK_STATS` must show `frames_sent`
climbing and `frames_received` pinned at 0. If the warning repeats twice a
second, the rate limiter is broken; if `age` comes back as anything other than
65535, something is faking data.

### 2. Loopback (wire only)

**A direct GPIO5-to-GPIO4 jumper on the ESP side is now a clean loopback.**
As of the 2026-08-25 rework, neither GPIO5 nor GPIO4 carries any software
inversion (Trap 2's history describes why that was not true under the
retired optocoupler pair, where GPIO5 carried `TXD_INV` and GPIO4 did not,
and a bare jumper would have fed an inverted transmit into an uninverted
receiver). With both ends non-inverting, tying GPIO5 to GPIO4 directly loops
the ESP's own frames back at standard polarity: no external inverter, and no
`gpio_set_outover()` on a spare pin, is needed any more. Every `GET_STATUS`
the poll task sends therefore arrives at the ESP's own protocol RX task,
addressed to `(UART_PROTO_DEVICE_SAFETY, task 7)` — a device this end is not,
so it is dropped rather than answered, and the poll still times out. What
this proves is the physical path and the baud rate; watch
`uart_owner_get_rx_error_count()` stay flat and the RX task stop reporting
framing errors.

A loopback across the barrier on the safety side (a wire jumped from U6's
VOA output, `PicoRx`, back into U6's VIB input, `PicoTx`) needs no inverter
either, since U6 does not invert in either channel. This additionally proves
the part, but needs the safety-domain 3.3 V rail powered.

For a loopback that actually answers, temporarily register task 7 with
`own_device` swapped — i.e. bring up a second `uart_protocol_t` with
`UART_PROTO_DEVICE_SAFETY` — and have it reply with a canned 23-byte status
frame. That exercises the parse, the cache, the age computation and `link_up`
end to end without any second processor.

### 3. PC-side stub on the safety UART

The most useful option once a USB-TTL adapter is to hand. Connect the adapter
to GPIO4/GPIO5 (adapter TX -> ESP RX GPIO4, adapter RX -> ESP TX GPIO5) with
the isolator out of circuit; neither GPIO4 nor GPIO5 carries any software
inversion any more (see Trap 2's history for the asymmetric inversion this
used to require under the retired optocoupler pair), so the adapter should
run at ordinary UART mark, standard polarity, on both legs.

Then speak the same `uart_protocol` framing the PC tools already implement
(`tools/PcTools/src/kilnctrl/protocol.py`), with `own_device = UART_PROTO_DEVICE_SAFETY`,
registering task 7, and answer each `GET_STATUS` with the 23-byte frame above.
This is the closest thing to the real Pico and is the recommended way to
develop the RP2040 firmware's protocol layer before there is any RP2040
involved.

Things worth deliberately testing with the stub: stop answering and confirm
`link_up` drops after three poll periods (and the fault line asserts, if the
policy is on); answer with a 22-byte frame and confirm it is rejected and
counted rather than partially applied; answer with bits 0/1 set in flags and
confirm the ESP clears them; push an unsolicited status and confirm the cache
picks it up without a request.
