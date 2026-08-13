# Safety Processor Link

The ESP32-S3 main controller and the RP2040 safety processor (A1) live in two
electrically separate ground domains. This document covers the isolation
barrier between them, the wire protocol the RP2040 firmware has to implement,
the C API on the ESP side (`App/drivers/safety_link.h`), the UART subcommands
the PC can drive it with, and how to exercise the whole thing with no Pico
attached.

The RP2040 firmware does not exist yet. That is a designed-for state, not a
bug: the link comes up, polls go out, every one times out, and `GET_STATUS`
reports `link_up = 0` with `age = 65535`. Nothing is faked and nothing aborts
startup.

## The isolation barrier

Three TCMT1109 optocouplers, and nothing else, cross between `GND_Main` and
`GND_Safty`:

| Part | LED driven by | Collector (output) | Direction |
|------|---------------|--------------------|-----------|
| U3 | Pico `PicoTx` (GPIO4) via R7 390R | ESP GPIO4, net `DataToSafty` | Pico -> ESP data |
| U2 | ESP GPIO5 via R12 390R, net `DataFromSafty` | Pico `PicoRx` (GPIO5), R9 1k pull-up | ESP -> Pico data |
| U1 | ESP GPIO6 via R11 390R, net `Fault` | Pico `mainFault` (GPIO10), R8 1k pull-up | ESP -> Pico fault |

There are three traps here, and getting any of them wrong produces a link that
comes up cleanly and silently never works.

### Trap 1: the data net names are backwards

`DataToSafty` is the ESP's **RX** and `DataFromSafty` is the ESP's **TX**. The
names read as if they described the ESP's direction; they do not. Trace U2/U3
in the table above whenever this looks wrong — the silicon is the authority,
and both `settings.h` and `Kconfig` already follow it (`KILNCTL_SAFETY_TX_IO`
defaults to GPIO5, `KILNCTL_SAFETY_RX_IO` to GPIO4).

### Trap 2: both data directions are logically inverted

An optocoupler is not a wire. The driving side's high lights the LED, the
phototransistor conducts, and the receiving side's collector is pulled **low**.
So an idle-high UART line arrives idle-**low**, in both directions.

The firmware fixes this in the UART peripheral, not in software:

```c
uart_set_line_inverse(SAFETY_UART_PORT_NUM, UART_SIGNAL_TXD_INV | UART_SIGNAL_RXD_INV);
```

It must be called *after* `uart_param_config()` (which happens inside
`uart_owner_init()`), because that rewrites the same register block.

**The Pico does not need to do anything.** Inverting on this side is not half
a fix that the far end has to match — it is the whole fix, because each
optocoupler is itself an inverter and two inversions in series cancel:

| Direction | ESP pin | Optocoupler | What the far pin sees |
|-----------|---------|-------------|-----------------------|
| ESP -> Pico | `TXD_INV` drives `NOT L` | U2 inverts again | `L` — standard polarity at the Pico's RX |
| Pico -> ESP | — | U3 inverts `L` | `NOT L` at GPIO4, which `RXD_INV` turns back into `L` |

Concretely, transmitting: an idle logical 1 becomes a physical low on GPIO5,
so U2's LED is **off**, so R9 holds the Pico's RX **high** — a correct idle. A
start bit (logical 0) drives GPIO5 high, lights the LED, and pulls the Pico's
RX low. That is exactly what an ordinary UART start bit looks like.

So the RP2040 can use its plain hardware UART, unmodified, at standard
polarity. No PIO UART, no external inverter. The inversion is entirely an
ESP-side concern and the barrier is transparent to the far end.

The corollary is the one to remember: **exactly one end inverts.** If the Pico
side were also inverted (PIO, or an inverter fitted at U2's collector), the two
inversions would cancel the optocoupler's and *that* is when nothing works.

Two boot-time artefacts follow from this and are worth designing the Pico
firmware around:

- While the ESP is in reset or before `safety_link_start()` runs, GPIO5 is
  high-impedance and R15 (1k to 3.3V_Main) pulls the TX net high — which lights
  U2's LED and holds the Pico's RX **low**, i.e. a continuous break. The Pico
  should tolerate a break on its RX as "main controller not up yet" rather than
  latching an error. (On a future board revision, a pull-*down* there would idle
  the LED off and give the Pico a clean idle-high line through an ESP reset.)
- The reverse case is covered under "Side effect worth knowing about" below.

`GPIO4` is the bare collector of U3. R15 (1k to 3.3V_Main) sits on the *TX*
net, not this one, and nothing else is on the RX net — so the ESP's internal
pull-up is what defines the LED-off level. Without it the pin floats and the
link has no defined idle at all, which is why `safety_link_start()` sets it
explicitly rather than relying on `uart_set_pin()` doing it incidentally.

### Trap 3: the fault line is an ESP *output*

`GPIO6` is driven by the ESP. High lights U1's LED, which pulls the Pico's
`mainFault` input low. There is no hardware path for the Pico to signal the
ESP — everything coming back does so over the isolated UART. Configuring
GPIO6 as an input, or reading it expecting the Pico's opinion, gets you
nothing.

The line is configured as an output driven **low (de-asserted)** before the
UART is even touched, because the pin powers up floating and a floating gate
on U1 is an undefined fault state at the safety processor.

### Side effect worth knowing about

With no Pico attached, U3's LED is never lit, the phototransistor never
conducts, the internal pull-up holds GPIO4 high, and `RXD_INV` turns that into
a continuously-low internal RX line — i.e. a permanent break condition. The
UART event task in `uart_owner.c` counts those, so `uart_owner_get_rx_error_count()`
for UART1 (and therefore the CRC/framing counter in `GET_LINK_STATS`) can climb
on a board with no safety processor fitted. That is the wiring reporting itself
accurately, not a driver fault.

## ESP <-> Pico contract

Transport: ESP32-S3 **UART1**, 8N1, `CONFIG_KILNCTL_SAFETY_BAUD_RATE` (115200
default), both signals inverted as above. The payload framing is the **same
`uart_protocol` stack the PC link uses** — 0x7E-delimited, byte-stuffed,
CRC16/CCITT-FALSE, indexed DATA frames with ACK/NACK, retried and de-duplicated
by the protocol layer. See `docs/UART_PROTOCOL.md` for the envelope; there is
deliberately no bespoke framing on this link.

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
`SAFETY_UART_BAUD_RATE`, applies both line inversions, enables the RX pull-up,
drives `SAFETY_FAULT_IO` low, attaches the protocol stack, registers
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

`GET_STATUS` reply, no peer present (what you get today) — 25 bytes:

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

Nothing below needs the RP2040 firmware to exist.

### 1. The no-peer path (zero hardware)

Boot the board and watch the log. Expect exactly one
`no reply from the safety processor (never seen one)` warning, then silence for
a minute at a time — not one per poll. `GET_STATUS` must return `link_up = 0`,
`age = 65535`, NaN temperatures. `GET_LINK_STATS` must show `frames_sent`
climbing and `frames_received` pinned at 0. If the warning repeats twice a
second, the rate limiter is broken; if `age` comes back as anything other than
65535, something is faking data.

### 2. Loopback with inversion (wire only)

Tie **GPIO5 to GPIO4** directly (main-board TX to main-board RX, bypassing both
optocouplers). Because both signals are inverted in the peripheral, an
inverted transmit sampled by an inverted receiver is self-consistent: the ESP
sees its own frames back, correctly framed. Every `GET_STATUS` the poll task
sends therefore arrives at the ESP's own protocol RX task, addressed to
`(UART_PROTO_DEVICE_SAFETY, task 7)` — a device this end is not, so it is
dropped rather than answered, and the poll still times out. What this proves is
the physical path, the baud rate and the inversion pair; watch
`uart_owner_get_rx_error_count()` stay flat and the RX task stop reporting
framing errors. A loopback across the *optocouplers* (GPIO5 to a wire jumped
from U2's collector back into U3's LED) additionally proves the parts, but
needs the safety-domain 3.3 V rail powered.

For a loopback that actually answers, temporarily register task 7 with
`own_device` swapped — i.e. bring up a second `uart_protocol_t` with
`UART_PROTO_DEVICE_SAFETY` — and have it reply with a canned 23-byte status
frame. That exercises the parse, the cache, the age computation and `link_up`
end to end without any second processor.

### 3. PC-side stub on the safety UART

The most useful option once a USB-TTL adapter is to hand. Connect the adapter
to GPIO4/GPIO5 (adapter TX -> ESP RX GPIO4, adapter RX -> ESP TX GPIO5) with
the optocouplers out of circuit, and **invert on the PC side**: either an
adapter that supports inverted signalling, or a small inverter, or accept that
you must invert in software (in which case you are decoding the line yourself
and the framing layer will not help you).

Then speak the same `uart_protocol` framing the PC tools already implement
(`pc_tools/src/kilnctrl/protocol.py`), with `own_device = UART_PROTO_DEVICE_SAFETY`,
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
