# Isolated Link Protocol

> **Status:** partially implemented · **Last reviewed:** 2026-08-18
> **Keep this file current.** This document is the *contract* between two
> independently-built firmwares — it is owned by `CommonFW`, not by either side.
> If you change a frame layout, an id, or a timing rule, update it in the same
> commit as the code and bump `KILNLINK_PROTOCOL_VERSION` if a peer would break.
> If it disagrees with the code, **the code wins** — fix this file and say so.
> A completion checklist is at the bottom.
>
> **2026-08-19: every command in secs 4 and 6 with a concrete byte layout now
> has a codec** — `kilnlink_context`, `kilnlink_announce`, `kilnlink_ceiling`,
> `kilnlink_clear_trip`, `kilnlink_get_fw_version`, `kilnlink_set_clock`
> (ESP→Pico), and `kilnlink_status`, `kilnlink_diag`, `kilnlink_trip`,
> `kilnlink_power` (Pico→ESP). See `firmware/CommonFW/README.md`'s Completion
> checklist for the file-by-file detail.
>
> **Some of these are now actually wired into runtime dispatch, not just
> codec-only:** `SaftyFW`'s `link_task.c` decodes and acts on inbound
> `ANNOUNCE_VERSION` (0x0F, via `kilnlink_announce_decode()`) and `CLEAR_TRIP`
> (0x0A, via `kilnlink_clear_trip_decode()`, refusing when nothing is tripped
> or when the wire `trip_mask` doesn't match the currently-latched one —
> exactly the refusal logic this document's §4 `CLEAR_TRIP` entry describes).
> `KilnFW`'s `safety_link.c` sends `ANNOUNCE_VERSION` via
> `kilnlink_announce_encode()`. `SaftyFW`'s `link_task.c` also dispatches the
> `UPDATE_BEGIN`/`UPDATE_DATA`/`UPDATE_END`/`UPDATE_ABORT` frames from
> `UPDATE_PROTOCOL.md` §4 to `update_task.c`. **Still not wired anywhere:**
> `SET_FIRING_CEILING`, `GET_FW_VERSION`, `SET_CLOCK` on the ESP send side,
> and `KilnFW` has no `CLEAR_TRIP` send path yet (no GUI trigger built) — see
> `firmware/CommonFW/README.md`'s checklist and this document's own §10 for
> the itemised state of each frame.

Implemented **once**, in [`../`](../README.md), and linked into both
[`../../KilnFW`](../../KilnFW) and [`../../SaftyFW`](../../SaftyFW). Do not
reimplement any of it in either firmware — see `firmware/CommonFW/README.md` for the
transport-vs-contract split.

The wire between the ESP32-S3 (`KilnFW`) and the RP2040 (`SaftyFW`), across
three optocouplers. This document specifies both ends, because **both ends need
changing** — neither firmware currently implements what the system needs.

---

## 1. The governing constraint

> **A freeze of the main processor's UART or core must under no circumstance
> cause the safety processor to hang.**

This is a hard requirement, and it is the reason the protocol below is not
simply "the existing `uart_protocol`, with the Pico as a peer".

### Why the existing protocol cannot be used as-is

`KilnFW`'s `uart_protocol` is a **stop-and-wait ACK protocol**. From
`uart_protocol.h:75-79`, the only frame types are:

```c
UART_PROTO_MSG_DATA = 0x01,
UART_PROTO_MSG_ACK  = 0x02,
UART_PROTO_MSG_NACK = 0x03,
```

Every `DATA` frame blocks its sender waiting for an `ACK` and retransmits up to
`UART_PROTO_MAX_RETRIES` (10) times. **There is no unacknowledged frame type.**

That has two fatal consequences for a one-way link:

- **If the Pico does not ACK**, every ESP context push costs
  10 × `SAFETY_LINK_ACK_TIMEOUT_MS` (50 ms) = **~500 ms of blocking**, on every
  push, forever, and the link reports itself permanently down.
- **If the Pico does ACK**, it now has a mandatory obligation to transmit in
  response to the ESP — which is the coupling the constraint above forbids.

So the ESP side needs one small additive change, and it is the single most
important item in this document.

### The change: an unacknowledged broadcast frame

```c
UART_PROTO_MSG_BROADCAST = 0x04,   /* fire-and-forget: no ACK, no retry, no dedup */
```

Same delimiter, same byte-stuffing, same CRC, same `(device, task_id)`
addressing, same RX assembly path. The *only* differences are that the sender
returns as soon as the bytes are handed to the UART, and the receiver never
replies.

This is additive and wire-compatible: a peer that has never heard of type
`0x04` sees an unrecognised type byte and discards the frame, exactly as it
would discard noise. It does not need a `UART_PROTOCOL_VERSION` bump under the
policy in `uart_task_ids.h:8-20` — but **bump it to 5 anyway**, for the same
reason v4 was bumped: an old peer that silently ignores the context broadcast
would look "compatible" while receiving none of the safety context, which is
precisely the silent skew the version gate exists to catch.

`LOG` (task_id 5) is already documented as "fire-and-forget, no reply expected"
in `uart_task_ids.h:429-443`, so the *concept* already exists in this codebase —
it just has no frame type of its own yet, and currently rides on `DATA`.
Migrating `LOG` onto `BROADCAST` is an obvious follow-on and is listed in
`TODO.md` as optional cleanup, not a prerequisite.

---

## 2. Direction policy

| Direction | Mechanism | Blocking? |
|---|---|---|
| **ESP → Pico** (context, heartbeat) | `BROADCAST`, every 500 ms | ESP: no. Pico: no |
| **ESP → Pico** (fault) | GPIO6 → U1 → GPIO10, out of band | neither |
| **ESP → Pico** (version request) | `BROADCAST`, at ESP boot, ESP retries | ESP: no. Pico: no |
| **Pico → ESP** (telemetry) | `BROADCAST`, unsolicited, every 500 ms | **never** |
| **Pico → ESP** (version reply, boot push) | `BROADCAST`, unsolicited | **never** |

### The Pico never waits for the ESP. Ever.

Concretely, and these are testable rules, not aspirations:

1. **The Pico never sends an ACK, and never expects one.**
2. **The Pico never retransmits.** A lost frame is lost; the next one is
   500 ms away.
3. **The Pico never blocks on TX.** Transmission is DMA- or IRQ-driven into a
   ring buffer; **if the ring is full, the frame is dropped**, a counter is
   incremented, and the caller returns immediately.
4. **`safety_core_task` never calls into the link at all** — not to read, not
   to write. It reads a snapshot struct that `link_task` publishes. The
   dependency is one-way, and it is enforced by `safety_core.c` not including
   the link header.
5. **The link task runs at a priority *below* the guard and relay tasks**, so
   even a link task spinning at 100 % CPU cannot delay a trip.

### Why the Pico can transmit and still never hang

The constraint is about **waiting**, not about transmitting, and the hardware is
why. Only three signals cross the barrier — `DataToSafty`, `DataFromSafty`,
`Fault`. **There is no RTS/CTS, no flow control of any kind.** A UART
transmitter with no flow control cannot be backpressured: bytes written to the
TX FIFO are clocked out by the shift register at the configured baud (9600 —
see §3) whether or not anything is listening, or is even powered. A frozen ESP
is *physically unable* to stall the Pico's transmitter.

The hang risk is real, but it lives entirely in the software above the
peripheral — waiting for an ACK, blocking on a full buffer, sharing a lock with
something that blocks. Rules 1–5 eliminate all three, and they hold whether the
Pico is silent or streaming.

### No "firing" / "not firing" mode split

The relaxation for the idle case is not needed, and the plan deliberately does
not take it. Because rules 1–5 make the Pico non-blocking *unconditionally*, it
can answer a version request and stream telemetry during a firing just as
safely as when idle — so there is nothing for a mode switch to buy.

Adding one would cost real safety: two behavioural modes double the state space
of the component that has no backup, and introduce a mode-transition bug class
in exactly the code that must not have one. **One behaviour, always, is both
simpler and safer here.** If a future requirement genuinely needs the Pico to
block on something, that is the point to revisit this — and the answer will
still probably be "don't".

### Retries live entirely on the ESP side

The boot-time version exchange (§4.3) is a one-shot that matters, so it needs
retry. That retry belongs to the **ESP**, which is allowed to block:

- The ESP re-sends the request until it gets an answer or gives up.
- The Pico answers **every** request it sees, idempotently, and never tracks
  whether its answer arrived.

Requests are cheap, stateless and repeatable, so "retry the request" is a
complete substitute for "acknowledge the response" — and it keeps every timeout
on the side of the link that can afford one.

---

## 3. Framing

Unchanged from `uart_protocol.c`, and **implemented once in
`firmware/CommonFW/src/kilnlink_frame.c`** — neither firmware reimplements it. The values
are tabulated here because they are the contract; the code is the only place
they are written down twice, and CI should assert there is no second CRC or
byte-stuffing implementation anywhere in the tree.

| Element | Value | Source |
|---|---|---|
| Delimiter (start **and** end) | `0x7E` | `uart_protocol.c:10` |
| Escape byte | `0x7D` | `uart_protocol.c:11` |
| Escape rule | if byte is `0x7E` or `0x7D`, emit `0x7D` then `byte ^ 0x20` | `uart_protocol.c:43-45` |
| CRC | **CRC16/CCITT-FALSE** — poly `0x1021`, init `0xFFFF`, MSB-first, no reflection, no final XOR | `uart_protocol.c:24-30` |
| CRC covers | header + payload (not the delimiters, not the stuffing) | `uart_protocol.c:110` |
| Max payload | 253 | `uart_protocol.h:48` |
| Device ids | ESP = 0, HOST = 1, **SAFETY = 2** | `uart_protocol.h:70-72` |
| Task id | **7** (`UART_TASK_ID_SAFETY`) | `uart_task_ids.h:60` |
| Line | 8N1, **9600** baud | `Kconfig:214` |

**The baud rate is capped by the TCMT1109 optocouplers, not by the UART
peripheral on either side.** Measured 2026-08-23, walking the rate down with
both ends changed together while the Pico transmitted a status frame every
500 ms: 115200 and 57600 delivered zero frames, ever; 38400 lost about 20%;
19200 looked clean over a short window but lost ~10% over a longer one; 9600
tracked sent-to-received one for one over minutes and is the value both
firmwares now hardcode, with no negotiation — `KILNCTL_SAFETY_BAUD_RATE` in
`KilnFW`, `UART_OWNER_BAUD_RATE` in `SaftyFW`'s `src/tasks/uart_owner.c`.
Raising it again needs a faster part or a line driver in place of the
optocoupler/pull-up pair, not a config change. **A static GPIO high/low test
across this link passes at any baud rate** — an optocoupler carries a DC level
fine — which is why this ceiling was found late: the wiring had already been
bench-verified that way in both directions, so suspicion fell on framing,
device/task ids and line inversion instead, all of which were correct. See
`firmware/KilnFW/docs/SAFETY_LINK.md`'s "Transport" section for the full
measurement table.

**Polarity: exactly one inversion per direction, but the two directions do not
invert in the same place.** ESP -> Pico is inverted entirely in the ESP's
UART peripheral (`TXD_INV` on GPIO5); the Pico's plain hardware UART reads
that direction at standard polarity with no inversion of its own. Pico -> ESP
cannot use the same trick, because the RP2040's PL011 UART has no
line-inversion control — so `SaftyFW` inverts at the GPIO pad instead, via
`gpio_set_outover(SAFTYFW_PIN_UART1_TX, GPIO_OVERRIDE_INVERT)` in
`uart_owner.c`, and `KilnFW` applies `TXD_INV` only (not `RXD_INV`) on its
own UART for that direction, since GPIO4 already arrives at the correct
polarity. This also means both optocouplers now sit dark (LED off) at idle:
before the pad-override fix, GP4 idled at ordinary UART mark (high), keeping
U3's LED lit continuously between frames; U2 was already correct. Adding a
second inversion anywhere in either direction cancels the optocoupler's own
inversion and the link goes dead. See `firmware/SaftyFW/docs/HARDWARE.md` §1
and `firmware/KilnFW/docs/SAFETY_LINK.md` "Trap 2" for the full detail.

**Pins: `PicoTx` = GP4, `PicoRx` = GP5** (`firmware/SaftyFW/docs/HARDWARE.md` §2). On the ESP
side these land on **GPIO4 = ESP RX** and **GPIO5 = ESP TX**, matching
`KilnFW`'s `Kconfig` defaults (`TX_IO = 5`, `RX_IO = 4`). Measured on the bench
2026-08-23; the schematic-derived assignment used before that was inverted, and
so were the defaults.

### Receiver robustness — required, not optional

The Pico's frame assembler is exposed to a link that will, at various times,
carry noise, half-frames from an ESP mid-reset, and a continuous break. It must:

- **Resynchronise on `0x7E` unconditionally**, from any state. A delimiter is
  always a frame boundary.
- **Bound every buffer.** A length byte is a byte from an untrusted wire.
- **Treat a break as "peer not up"**, not as an error to latch. Count it, do
  not act on it.
- **Never allocate.** Fixed buffers, sized at compile time.
- **Never block the RX path on downstream processing.** Parse into a
  double-buffered snapshot; if the consumer has not read the last one, overwrite
  it. Newer context is strictly more useful than older context.

---

## 4. ESP → Pico: the context broadcast

`BROADCAST`, `(device=SAFETY, task_id=7)`, sent every
`CONFIG_KILNCTL_SAFETY_POLL_PERIOD_MS` (default 500 ms). This is what the
safety processor needs from the main controller, and it is new work on the ESP
side — **`KilnFW` currently sends no such thing.** Confirmed: the only two
ESP→Pico payloads today are `GET_STATUS` (1 byte) and `REQUEST_ENABLE`
(2 bytes), and `safety_link.h:44-47` states the rest never cross the barrier.

### `SAFETY_CMD_PUSH_CONTEXT` = `0x07`

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `0x07` |
| 1 | u8 | flags — see below |
| 2 | u8 | `boot_id` — increments on every ESP boot |
| 3..6 | u32 LE | `seq` — increments every frame, never resets except on boot |
| 7..10 | u32 LE | `uptime_ms` |
| 11 | u8 | `relay_now_mask` — bits 0–3, relays 1–4 **as actually commanded** (post-refusal) |
| 12 | u8 | `relay_recent_mask` — relays commanded on at **any** point in the last `recent_window_s` |
| 13 | u8 | `recent_window_s` — the window the mask above covers |
| 14 | u8 | `zone_count` (N, 0–3) |
| 15.. | N × 10 | per-zone block |

Per-zone block (**14 bytes**):

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `zone_index` |
| 1 | u8 | zone flags: bit0 `measured_valid`, bit1 `active`, bit2 `relay_on`, bit3 `guard_tripped` |
| 2..5 | f32 LE | `setpoint_c` |
| 6..9 | f32 LE | `measured_c` — **raw**, uncalibrated |
| 10 | u8 | **`sample_counter`** — increments on every *fresh conversion* read from this channel, wrapping. See below |
| 11 | u8 | **`tc_type`** — the thermocouple type this channel is configured for (`THERMO_TC_*`) |
| 12 | u8 | `tc_fault` — that channel's MAX31856 SR bits |
| 13 | u8 | reserved, 0 |

### `sample_counter` is what makes a borrowed thermocouple safe to use

The safety processor may be configured to use one of the **main board's**
thermocouples as its safety sensor (`firmware/SaftyFW/docs/THERMOCOUPLE.md` §3). When it
does, the reading arrives through this frame rather than from its own MAX31856 —
and that creates a failure mode the Pico must be able to see.

A frozen MAX31856 on the main board returns the same value on every read. The
ESP forwards it faithfully, at 500 ms, forever. **From the Pico's side that is
indistinguishable from a genuinely stable temperature** — which is exactly the
condition under which a kiln at setpoint sits for hours.

`sample_counter` breaks the ambiguity. It increments **only when a new
conversion was actually read** from that channel — driven by `~DRDY` (or the
elapsed-time fallback `firmware/KilnFW/docs/MAX31856.md` documents), never by the mere act
of building this frame. So:

| `sample_counter` | `measured_c` | Meaning |
|---|---|---|
| advancing | changing | healthy |
| advancing | identical for a long time | **stable kiln** — fine, unless heat is going in (`S11`) |
| **not advancing** | anything | **the channel has stopped converting** — `S13` |

Without this field the Pico would have to infer staleness from value changes
alone, which is precisely the inference that fails on a kiln holding a soak.

**Implementation note for `KilnFW`:** the counter must be incremented at the
point a conversion is *consumed*, not where the frame is built. Incrementing it
per-frame makes it a constant-rate counter that proves nothing.

### `tc_type` per zone

Each thermocouple in the system may be a different type — the wire already
supports this (`THERMO_CMD_CONFIG_CHANNEL`'s `byte2 = tc_type`), but `KilnFW`
currently configures all three channels identically.

Carrying it here lets the Pico (a) sanity-check that a borrowed channel is the
type it was told to expect, and (b) apply the right plausibility range. A
mismatch between the configured type and the thermocouple physically fitted is
otherwise **silent and reads low** — see `firmware/SaftyFW/docs/THERMOCOUPLE.md` §2.

Top-level flags (byte 1):

| Bit | Name | Meaning |
|---|---|---|
| 0 | `PROFILE_RUNNING` | a profile is executing |
| 1 | `ANY_ZONE_FAULTED` | at least one zone is in a guard trip |
| 2 | `HEAT_REQUESTED` | the ESP wants heating permitted |
| 3 | `CONTEXT_VALID` | the ESP believes these numbers are meaningful |
| 4 | `SIM_PLANT` | **built with `KILNCTL_SIM_PLANT`** — temperatures are fabricated |

Total: 15 + 3 × 14 = **57 bytes** at 3 zones. Comfortably inside the 253 cap.

### Design notes on the fields

**`relay_recent_mask` is the field that makes correlation possible, and it is
the reason this frame exists at all.** `KilnFW` renders duty as a **60-second**
time-proportioned window (`HEATER_WINDOW_MS`, `profile_executor.c:27`). At 20 %
duty a relay is on for 12 s and off for 48 s, and the current front end's
peak-hold decays in ~4 s — so for most of every minute a perfectly healthy zone
reads zero current. Correlating against `relay_now_mask` would trip guard S4 on
every low-duty firing.

The ESP already knows its own window phase, so computing "did I command this
relay on at any point in the last N seconds" is trivial there and impossible
here. `recent_window_s` should be **≥ 150 s** (two heater windows plus decay
margin); it is transmitted rather than assumed so the Pico does not have to
hard-code a constant that lives in another repository.

**`boot_id` invalidates history.** When it changes, the Pico resets every
correlation window. A relay history from before a reboot describes a different
program's intentions.

**`seq` detects loss without needing an ACK.** A gap means frames were dropped;
a large gap or a stall means the link is degrading. This is how the Pico
measures link health with no return path.

**`measured_c` is the raw reading**, matching `thermal_guard.h:80-84`'s rule
that a calibration offset must never be able to hide an out-of-range sensor.

**`SIM_PLANT` is not decoration.** `KilnFW` can be built against a simulated
plant that reports fabricated temperatures (`firmware/KilnFW/README.md`). A safety
processor that silently correlated against simulated data would be actively
dangerous. On seeing this bit set, `SaftyFW` **disables every context-consuming
guard (S2, S3, S4) and raises a persistent warning.** S1 and S5–S12 are unaffected,
because they never look at context.

### `SAFETY_CMD_SET_FIRING_CEILING` = `0x09` (ESP → Pico)

Sent when a profile starts, when it is edited, and repeated in every context
frame's shadow (see below). Carries the **highest target temperature this
firing will ever ask for**.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `0x09` |
| 1..4 | f32 LE | `firing_max_c` — 0 or NaN = "no firing / no ceiling known" |

This is small and worth more than it looks. Without it, S1's absolute limit has
to be a single fixed number set high enough for the *hottest firing the kiln
will ever do* — 1300 °C — which means a 900 °C bisque firing runs with 400 °C of
unprotected headroom. With it, the Pico can enforce

```
effective_ceiling = min(abs_max_temp_c,  firing_max_c + firing_margin_c)
```

so every firing is protected to *its own* envelope, and the protection tightens
automatically without anyone editing a threshold. `firing_margin_c` defaults to
**100 °C** — generous enough that no legitimate overshoot reaches it.

The ceiling only ever **tightens** S1; it can never raise it above
`abs_max_temp_c`. A hostile or buggy ESP sending `firing_max_c = 5000` gets
clamped, not obeyed. **The main controller is allowed to ask for more
protection, never for less.** That asymmetry is what makes it safe to accept
this field from the component under suspicion at all.

### `SAFETY_CMD_CLEAR_TRIP` = `0x0A` (ESP → Pico)

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `0x0A` |
| 1..2 | u16 LE | `trip_mask` being acknowledged — must match the current one |

Refused, with the reason reported in the next diagnostic frame, if the
tripping condition is still true or if `trip_mask` does not match. Echoing the
mask back prevents a stale "clear" queued before a *second*, different trip
from clearing that one too.

This is the GUI's path to acknowledging a trip. The E-stop assert/release cycle
remains available as the physical alternative (`firmware/SaftyFW/docs/SAFETY_MODEL.md` §6).

### `SAFETY_CMD_SET_CONFIG` = `0x16` (ESP → Pico)

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `0x16` |
| 1 | u8 | `tc_type` — one of `MAX31856_TC_TYPE_*` (`SaftyFW/src/max31856.h`) |

The GUI's path to commissioning the safety processor's `config_store.h`
record (`firmware/SaftyFW/TODO.md` Phase 9). Fire-and-forget, same as
`CLEAR_TRIP`: never ACKed on the wire, and the PC observes the outcome via
the next status/diag poll rather than a reply to this frame.

Refused, with the reason logged on the Pico side, if:
- the relay is currently `ARMED` (`config_store_decide_write()` — "config
  writes are refused while ARMED" applies unconditionally, no per-field
  carve-out), or
- `tc_type` is not a value `SaftyFW` recognises as a `MAX31856_TC_TYPE_*`.

This codec (`kilnlink_set_config.{c,h}`) only serializes the byte — like
`CLEAR_TRIP`'s `trip_mask`, it does not know what a valid `tc_type` is; that
validation, and the write itself, are `SaftyFW`'s (`link_task.c` calling
`config_store_write()`).

### `SAFETY_CMD_SET_CT_CAL` = `0x19` (ESP → Pico)

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `0x19` |
| 1 | u8 | `channel` (0..2) |
| 2 | u8 | `calibrated` (0/1) |
| 3..6 | f32 LE | `gain` |
| 7..10 | f32 LE | `offset` |

The bench-tool's path to actually pushing CT amps calibration constants into
`SaftyFW`'s own flash — closes the gap `firmware/SimFW/tools/ct_calibration/`
's README documented: a calibration run could compute per-channel
constants but had no wire command to write them anywhere. One channel per
frame (a bench run sweeps and fits one CT channel at a time); setting one
channel must never disturb another's stored constants — `link_task.c`
read-modify-writes the other two channels' current values back unchanged.

Fire-and-forget, same as `SET_CONFIG`: never ACKed on the wire, refused (with
the reason logged on the Pico side) if the relay is currently `ARMED`, or if
`channel` is out of range. Unlike `SET_CONFIG`, does **not** touch
`calibration_missing` — thermocouple commissioning and CT calibration are
separate concerns.

`gain`/`offset` must already be inverted the way
`firmware/SimFW/tools/gen_ct_cal_table.py` inverts its fit before writing a
compiled table: `firmware/SimFW/tools/ct_calibration/calibrate_ct.py` fits
`measured_a = fit_gain * commanded + fit_offset` (`commanded` = the
fixture's known true amps, `measured_a` = `SaftyFW`'s own reported
`current_a`, read back via `SAFETY_CMD_GET_STATUS` over the existing
kilnctrl link). The sender must invert before transmitting: `gain = 1 /
fit_gain`, `offset = -fit_offset / fit_gain`, so that the Pico's
`corrected = gain * raw + offset` undoes the measured error. This codec has
no way to enforce that inversion; see `firmware/SaftyFW/src/config_store.h`'s
header comment on `config_store_ct_channel_cal_t`.

### `SAFETY_CMD_GET_CT_CAL` = `0x22` (ESP → Pico), request only

One byte, no arguments — same shape as `SAFETY_CMD_GET_FW_VERSION`. The
Pico answers every copy it sees, with `SAFETY_CMD_CT_CAL` (§6 Frame G).

**Through `KILNLINK_PROTOCOL_VERSION` 6** this request shared its wire id
with the reply (both `0x1A`), distinguished only by direction and length.
Version 7 split the request onto its own id (`0x22`) — see this file's
"Request/reply ids must never be shared" rule below for why. `0x1A` is now
used ONLY by the reply; `0x22` is burned for this request going forward.

### `SAFETY_CMD_ROLLBACK` = `0x17` (ESP → Pico)

One byte, no arguments — same shape as `SAFETY_CMD_GET_FW_VERSION`.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `0x17` |

The explicit "go back to the previously-running bootloader slot, right now"
command — the Pico-side half of `tools/PcTools/TODO.md`'s
`ota_rollback(processor)` line (the ESP half, `POST /api/ota/esp/rollback`,
already exists in `firmware/KilnFW/App/drivers/ota_http.c`). Fire-and-forget,
same as `CLEAR_TRIP`/`SET_CONFIG`: never ACKed on the wire.

The refuse/proceed decision — is the other bootloader slot even valid to
fall back to? — is entirely `SaftyFW`'s, in `bootloader/metadata.c`'s
`bootloader_decide_rollback()`. This codec (`kilnlink_rollback.{c,h}`) only
serializes the one-byte frame; it carries no opinion about whether a
rollback should be allowed.

### `SAFETY_CMD_GET_FW_VERSION` = `0x0B` (ESP → Pico)

One byte, no arguments. Sent by the ESP at boot and whenever the Pico's
`boot_id` changes. **The ESP retries this**; the Pico answers every copy it
sees and never tracks whether its answer arrived (§2).

### `SAFETY_CMD_ANNOUNCE_VERSION` = `0x0F` (ESP → Pico)

**The version check is mutual. Each side checks the other, and both must agree
before either trusts anything the other says.**

The original design had the ESP request the Pico's version and the Pico answer.
That is half a handshake: it lets the ESP detect a mismatch, and leaves the
safety processor parsing context frames from a main controller whose format it
has never verified. Since the context frame is where setpoints, the relay mask
and a borrowed thermocouple reading arrive, the safety processor is precisely the
side that must not be guessing.

So the ESP announces itself too, unprompted, in the same layout as Frame C:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `0x0F` |
| 1..2 | u16 LE | **`KILNLINK_PROTOCOL_VERSION`** — fixed offset, never moves |
| 3..4 | u16 LE | **`KILNLINK_MIN_COMPATIBLE`** — the oldest peer this build will talk to |
| 5 | u8 | `dirty` (0 = clean, 1 = dirty **or unknown**) |
| 6 | u8 | `commit_len` (N1) |
| 7.. | N1 × u8 | git commit hash, ASCII |
| … | u8 | `datetime_len` (N2) |
| … | N2 × u8 | build date+time, ASCII |
| … | u8 | `boot_id` |

Sent as `BROADCAST` at ESP boot, repeated a few times against loss, and re-sent
whenever the Pico's `boot_id` changes — a Pico that has just rebooted has
forgotten everything, including who it is talking to. Frame C gains the same
`min_compatible` field at the matching offset.

#### What "compatible" means

Compatibility is decided on the **protocol** version, never the firmware
version. Two builds months apart that speak the same protocol are fine together;
that is the whole point of versioning the wire separately from the code.

```
compatible  ==  peer.protocol >= self.min_compatible
            &&  self.protocol >= peer.min_compatible
```

Both directions, because "I can read you" and "you can read me" are different
claims. Bumping `KILNLINK_MIN_COMPATIBLE` is the deliberate act of dropping
support for older peers, and it belongs in the same commit as whatever change
made them incompatible.

#### What each side does about a mismatch

**The ESP:** treats it exactly like a dead link — `SAFETY_FAULT_SRC_SAFETY_LINK`
asserts, every heater-on is blocked, a running firing aborts. A safety processor
it cannot parse is not a safety processor. The GUI must show **both** versions
and which one is older, because "incompatible" without saying which side to
update is a message that generates a support question rather than answering one.

**The Pico:** enters `DEGRADED_NO_CONTEXT` and **does not latch a trip.**

That last part matters and is easy to get wrong. A version mismatch is not
evidence of a dangerous kiln; it is evidence that the two processors were flashed
out of step, which happens constantly during development. Tripping on it would
be a nuisance trip in the exact style §2 of `SAFETY_MODEL.md` forbids — and it
would be pointless, because the ESP is already refusing to heat.

In `DEGRADED_NO_CONTEXT` the Pico:

- **discards every context frame unparsed.** It does not attempt a best-effort
  decode of a layout it does not recognise. Guessing at the offset of a setpoint
  is worse than having no setpoint.
- **keeps running every guard that needs no context** — absolute over-temp,
  sensor invalid, E-stop, frozen reading, cold junction. These are the guards
  that matter with an unknown main controller, and they still command the relay.
- **disables every guard that needs context** — sustained-over-setpoint,
  load-active correlation, thermocouple disagreement, borrowed-channel staleness
  — and reports them as disabled rather than passing.
- **keeps sending telemetry**, including its own version, so the ESP can display
  the mismatch and so an update can be pushed to fix it.

#### The compatibility floor: why a mismatch can never brick the link

There is a trap here worth naming, because walking into it costs a bench visit
with a debug probe:

> If two incompatible peers refuse to talk, and the only way to update the Pico
> is *through* the ESP over that same link, then a mismatch makes the field
> update path unusable.

The fix is to carve out a subset that is **frozen at version 1 and never
changes**: the framing itself, `ANNOUNCE_VERSION`, `FW_VERSION`, and the
`UPDATE_*` frames (`../docs/UPDATE_PROTOCOL.md` §4). Both sides must honour
those regardless of protocol version, and a mismatch must never disable them.

- Frame ids `0x00`–`0x0F` are reserved for the floor. Nothing above `0x0F` may
  be required to establish contact or to carry an update.
- The floor's layouts may gain **appended** fields, never reordered or resized
  ones. A peer reads the prefix it understands and ignores the tail.
- `KILNLINK_MIN_COMPATIBLE` does not apply to the floor. That is what makes it a
  floor.

This is also why the ESP must **refuse to push a Pico image whose declared
protocol version its own build cannot talk to**, unless explicitly overridden:
that single action is the one that creates the lockout. When both processors
need updating, the ESP goes first, and the GUI should say so.

### Rule: request/reply ids must never be shared

`GET_CT_CAL`/`CT_CAL`, `GET_PARAM`/`PARAM`, and `GET_CONFIG_PAGE`/`CONFIG_PAGE`
were all originally specified as one command byte shared between a request
and its own reply, distinguished only by direction and length (the same
convention `GET_FW_VERSION`/Frame C above still uses, deliberately left
alone — see that entry's own note on why). **This has bitten once already
and is now a documented rule, not just a pattern:** a shared id structurally
blocks a length-different refusal reply. A driver-error refusal frame
(`{subcmd, ok=0, reason...}`, `uart_bridge.c`'s `bridge_reply_reject()`) is
essentially never exactly 1 byte (a bare request) or exactly the successful
reply's fixed length, so under the shared-id scheme a bridge task has no way
to tell the peer "I refuse" without that refusal being misread as a
truncated or malformed successful reply. `GET_CT_CAL`, `GET_PARAM`, and
`GET_CONFIG_PAGE` were all split onto their own ids at
`KILNLINK_PROTOCOL_VERSION` 7 (`0x22`/`0x23`/`0x24` respectively) for exactly
this reason — see each one's own entry for the before/after id.

**When adding a new request/reply pair on this link: give them different ids
from the start.** The only length-shared exception on this link is
`GET_FW_VERSION`/Frame C, and only because it sits in the frozen `0x00`-`0x0F`
floor above, where a refusal reply is never needed (a floor frame is never
refused — see that section). Anything above the floor gets two ids.

### `SAFETY_CMD_SET_CLOCK` = `0x0C` (ESP → Pico), optional

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `0x0C` |
| 1..8 | u64 LE | Unix epoch milliseconds |

The Pico has no RTC. Without this, every trip is timestamped only in
milliseconds-since-boot, which makes correlating a trip against a firing log
needlessly hard. Purely diagnostic — **no guard may ever read this clock**, or
a bad time from the ESP becomes a safety input.

### `SAFETY_CMD_REQUEST_ENABLE` = `0x02` (existing)

Kept, converted to `BROADCAST`. Advisory only — the Pico's interlocks always
win, which `firmware/KilnFW/docs/SAFETY_LINK.md` already documents and the ESP already
handles. In practice it is now redundant with `HEAT_REQUESTED` in the context
frame; keep it for one release, then retire it.

### `SAFETY_CMD_GET_STATUS` = `0x01` (existing) — no longer a poll

The Pico pushes status unsolicited every 500 ms (§6), so the ESP does not need
to ask. The ESP must **stop sending it as an ACK'd `DATA` poll**; otherwise
every poll burns 10 retries × 50 ms against a peer that never ACKs.

It may still be sent as a `BROADCAST` to request an immediate extra push —
useful behind a "refresh" button in the GUI — but nothing depends on it, and
the 500 ms cadence is the real mechanism.

### `SAFETY_CMD_INJECT_TC` = `0x21` (ESP → Pico)

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `0x21` |
| 1 | u8 | `valid` (0/1) |
| 2..5 | f32 LE | `tc_c` |
| 6..9 | f32 LE | `cj_c` |
| 10 | u8 | `fault_bits` — `SAFETY_THERMO_FAULT_*` bits (`safety_guards.h`) |

Feeds a synthetic thermocouple reading into `SaftyFW`'s guard chain
(`thermo_task.c`'s published snapshot), so the whole S1/S5/S11/S12 guard
chain can be exercised on real hardware before the physical safety
MAX31856 exists. `valid` mirrors `thermo_snapshot_t`'s own valid flag; when
0, the receiver ignores `tc_c`/`cj_c` and substitutes NaN itself (per that
struct's own "NaN when `!valid`, never 0, never stale" contract) — this
frame still carries real bytes for both regardless of `valid`, rather than
omitting them, keeping it the same fixed size as every other command in
this family.

**Honoured only while `safety_tc_installed == 0`.** The moment `SaftyFW`
has a real safety thermocouple wired and commissioned, every
`INJECT_TC` frame is refused — this is not a mode flag the ESP can leave
set; it is a structural gate inside `thermo_task_inject_reading()` itself,
the one function that can act on this command, and it is never persisted.
A reader of this document must not assume `INJECT_TC` is unconditionally
honoured just because a well-formed frame was sent. This codec
(`kilnlink_inject_tc.{c,h}`) only serializes the payload bytes; it has no
opinion about, and cannot see, whether the gate is currently open.

---

## 5. The fault line (out of band)

Unchanged, and worth keeping precisely *because* it is out of band: it is the
one ESP→Pico signal that works with the UART completely dead.

`Fault` (ESP GPIO6) → R11 → U1 LED → Pico `mainFault` (GPIO10), R8 1k pull-up.
**Active low at the Pico.** Asserted by the ESP whenever any
`SAFETY_FAULT_SRC_*` bit is set (`safety_link.h:151-166`):

| Source | Value |
|---|---|
| `MANUAL` | 0x01 |
| `PC_LINK` | 0x02 |
| `THERMO` | 0x04 |
| `SAFETY_LINK` | 0x08 |
| `APP` | 0x10 |
| `THERMAL_SANITY` | 0x20 |

> **This line cannot detect a dead ESP.** Unpowered main board ⇒ LED dark ⇒
> R8 pulls GPIO10 high ⇒ reads *healthy*. Guard S6 treats the fault line and
> the UART timeout as independent evidence for exactly this reason. See
> `firmware/SaftyFW/docs/HARDWARE.md` §4.

**`SAFETY_FAULT_SRC_SAFETY_LINK` needs re-examining on the ESP side.** It is
currently asserted when the Pico fails to answer a poll, with
`fault_on_link_loss` defaulting true. With no return path there are no polls to
miss, so as written it would assert permanently. **Redefined in §8** as "no
telemetry frame within 1.5 s", which is what makes *the safety processor must be
alive to heat* enforceable. `firmware/SaftyFW/TODO.md` item 0.6.

---

## 6. Pico → ESP: telemetry — **required**

All `BROADCAST`, `(device=ESP, task_id=7)`, unsolicited, **never retried,
dropped on a full TX ring, never waited on.**

This direction is **mandatory**, not optional: the main controller must be able
to prove the safety processor is alive before it fires, and must be able to show
the operator what the safety processor sees. See §8.

**Nothing in the Pico's own safety function depends on this direction.** The
guards trip identically whether or not anything is listening — the host tests
assert that by running the guard suite with the TX path stubbed out. What
depends on it is the *ESP's* permission to heat, which is the point.

### Frame A: `SAFETY_CMD_GET_STATUS` = `0x01` — the existing 23-byte layout

Byte-for-byte the frame `KilnFW` already parses (`safety_link.h:102`,
`SAFETY_LINK_STATUS_FRAME_LEN = 23`). Emitting the *existing* layout unchanged
means **the Pico can be brought up against today's unmodified `KilnFW`**, which
is what makes phase 2 of `TODO.md` possible before any ESP work lands.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `0x01` |
| 1 | u8 | flags (`SAFETY_FLAG_*`) |
| 2..5 | f32 LE | safety TC °C — **NaN if invalid** |
| 6..9 | f32 LE | cold junction °C — **NaN if invalid** |
| 10 | u8 | TC fault bits (`THERMO_FAULT_*`) |
| 11..14 | f32 LE | current 1, A |
| 15..18 | f32 LE | current 2, A |
| 19..22 | f32 LE | current 3, A |

Flags: bits 0 (`LINK_UP`) and 1 (`FAULT`) are the **ESP's** to own — the Pico
sends them as **0** and the ESP clears them on receipt regardless. The Pico
owns bit 2 `ESTOP`, bit 3 `RELAY` (K4 energized), bit 4 `ENABLED` (heating
permitted), bit 5 `TEMP_VALID`.

**Send NaN, never 0, when `TEMP_VALID` is clear.** An explicit not-a-number is
much harder to mistake for a cold kiln than a plausible-looking zero.

### Frame B: `SAFETY_CMD_DIAG` = `0x08` — new, additive

Everything the 23-byte frame has no room for. A `KilnFW` that has never heard of
`0x08` ignores it, so this can ship before the ESP knows what to do with it.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `0x08` |
| 1 | u8 | `trip_reason` (`SAFETY_TRIP_*`, 0 = none) |
| 2..3 | u16 LE | `warn_mask` — one bit per guard currently warning |
| 4..5 | u16 LE | `trip_mask` — one bit per guard currently tripped |
| 6..9 | u32 LE | Pico `uptime_ms` |
| 10 | u8 | `boot_reason` — bit0 power-on, bit1 **watchdog**, bit2 brownout |
| 11 | u8 | `context_age_100ms` (255 = never received) |
| 12..15 | u32 LE | `context_frames_ok` |
| 16..19 | u32 LE | `context_frames_bad` (CRC/framing/length) |
| 20..23 | u32 LE | `tx_frames_dropped` (ring full) |
| 24 | u8 | `state` (0 init, 1 grace, 2 armed, 3 warn, 4 **tripped**) |
| 25 | u8 | flags: bit0 `sim_context_seen`, bit1 `calibration_missing`, bit2 `estop_unwired_suspect` |

`boot_reason` bit 1 is the one to watch on a bench: a safety processor that is
silently watchdog-resetting in a loop presents as a working system with an
occasional inexplicable trip.

### Frame C: `SAFETY_CMD_FW_VERSION` = `0x0B` — build identity

Sent in reply to a request, **and pushed unsolicited once at boot**. The boot
push is what tells the ESP "the safety processor just restarted" without
polling for it — the same trick `KilnFW`'s own INFO task uses
(`uart_task_ids.h:520-528`).

The layout deliberately mirrors `INFO_CMD_GET_FW_VERSION` so the PC-side
parser can be reused rather than rewritten:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `0x0B` |
| 1..2 | u16 LE | **`KILNLINK_PROTOCOL_VERSION`** — fixed offset, never moves |
| 3..4 | u16 LE | **`KILNLINK_MIN_COMPATIBLE`** — oldest peer this build will talk to |
| 5 | u8 | `dirty` (0 = clean, 1 = dirty **or unknown**) |
| 6 | u8 | `commit_len` (N1) |
| 7.. | N1 × u8 | git commit hash, ASCII, not null-terminated |
| … | u8 | `datetime_len` (N2) |
| … | N2 × u8 | build date+time, ASCII `YYYY-MM-DD HH:MM:SSZ` |
| … | u8 | `boot_id` |
| … | u8 | `config_version` |
| … | u16 LE | `config_crc` — CRC of the active threshold/calibration set |

**Read bytes 1–4 first and decide compatibility before parsing anything after
them**, same rule as the ESP's `ANNOUNCE_VERSION` frame. A mismatched protocol
version means the rest of this frame may not mean what you think — the two
version fields and the frame id are the only parts whose position is guaranteed.

This frame is part of the **compatibility floor** described under
`ANNOUNCE_VERSION` above: it must be emitted and parseable regardless of whether
the two sides agree on anything else, because it is how they find out that they
do not.

The last two fields are the ones worth having beyond a build stamp: **the GUI
should display the safety processor's active config CRC**, so "which thresholds
is the safety processor actually enforcing right now" has an answer that does
not require trusting a separate record of what was uploaded. A config CRC of
zero means running on compiled-in defaults that were never commissioned.

Generate this the way `KilnFW` does — a `gen_build_info.cmake` equivalent that
regenerates on every build, so a dirty tree cannot silently report itself clean.
`dirty = 1` must also be the value used when git is unavailable at build time:
**"unknown" and "dirty" must map to the same value**, because the one thing that
must never happen is an uncommitted build reporting itself as a clean release.

### Frame D: `SAFETY_CMD_TRIP_EVENT` = `0x0D` — pushed immediately on trip

Not on the 500 ms cadence — pushed **the moment a trip latches**, and repeated
a few times over the next second in case the first copy is lost (there is no
ACK to tell us). Idempotent: the ESP dedups on `trip_seq`.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `0x0D` |
| 1 | u8 | `trip_seq` — increments per trip event |
| 2 | u8 | `trip_reason` (`SAFETY_TRIP_*`) |
| 3..6 | u32 LE | `uptime_ms` at trip |
| 7..10 | f32 LE | safety TC °C at trip |
| 11..14 | f32 LE | the deciding threshold value |
| 15..18 | f32 LE | current channel 1 A at trip |
| 19..22 | f32 LE | current channel 2 A at trip |
| 23..26 | f32 LE | current channel 3 A at trip |
| 27 | u8 | `relay_recent_mask` last received |
| 28 | u8 | `context_age_100ms` at trip |

**Capturing the deciding values at the instant of the trip is the whole point.**
Five hundred milliseconds later the temperature has changed, the peak-hold has
decayed, and the evidence is gone. "Why did the kiln stop?" is the first
question anyone asks, and this frame is the only place the answer is preserved.

### Frame E: `SAFETY_CMD_POWER` = `0x0E` — power estimate, for the GUI

**No guard reads any of this.** It exists to be displayed.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `0x0E` |
| 1 | u8 | `power_window_s` — the window the fractions cover (default 120) |
| 2 | u8 | flags: bit0 `mains_voltage_configured`, bit1 `any_channel_clipped`, bit2 `calibrated` |
| 3..6 | f32 LE | `mains_voltage_v` as configured (NaN if not set) |
| 7..30 | 3 × 8 | per channel: `i_conducting_a` f32 LE, `conduction_fraction` f32 LE |
| 31..42 | 3 × f32 LE | `p_avg_w` per channel (NaN if `mains_voltage_v` unset or channel clipped) |
| 43..46 | f32 LE | `p_total_w` (NaN if any contributing channel is invalid) |
| 47..54 | f64 LE | `energy_wh` accumulated since the Pico last booted |

`i_conducting_a` is the draw **while conducting**, not a duty-averaged figure —
see `firmware/SaftyFW/docs/CURRENT_SENSE.md` §3b for why those must be reported separately, and why
the ESP's own duty figure is the better multiplier.

`energy_wh` resets on Pico reboot; `boot_id` in the context/version frames is
how the ESP knows to restart its own totalisation rather than see a step change.

### Frame F: LOG relay — the Pico's console, task id **5**

Not a new frame type: the Pico emits ordinary `BROADCAST` frames addressed to
`(device=ESP, task_id=5)` with the **existing** `LOG` payload shape from
`uart_task_ids.h:429-443` — byte 0 = level, the rest ASCII "TAG: message", not
null-terminated, truncated rather than split.

The ESP already forwards every `LOG` frame it receives to the PC. So the Pico's
console reaches the GUI with **no new task id, no new payload format, and no
extra cable** — it is the same mechanism `KilnFW` already uses for its own log
lines, with a different source device.

Two rules, both non-negotiable:

- **Best-effort and droppable.** Dropped locally when the TX ring is full,
  never blocking the task that logged. This is rule 3 of §2, not a nicety —
  a log line must never be able to stall the safety processor.
- **`log_task` is the lowest priority in the system**
  (`firmware/SaftyFW/docs/ARCHITECTURE.md` §4), so a chatty log cannot delay a trip.

The PC distinguishes the two streams by the frame's source device, and should
mark the transport on each line: a log line relayed through the ESP and one read
over RTT mean very different things when the link is what is under
investigation. See `tools/PcTools/TODO.md`, "Logging and consoles".

### Frame G: `SAFETY_CMD_CT_CAL` = `0x1A` — CT amps calibration, in reply to `SAFETY_CMD_GET_CT_CAL`

Sent in reply to `SAFETY_CMD_GET_CT_CAL` (§4 above, now `0x22`). Through
`KILNLINK_PROTOCOL_VERSION` 6 this reply shared its wire id with the request
(both `0x1A`), distinguished only by direction and length; version 7 split
the request onto its own id — see the "Request/reply ids must never be
shared" rule above. `0x1A` is unchanged and is used ONLY by this reply now;
the request is always 1 byte, this reply is always 28 bytes. Reports the
three current-sense channels' stored CT amps calibration exactly as
`config_store.c` holds it — the GUI's way to show
what the safety processor is actually correcting current with right now,
same motivation as Frame C's `config_crc` field.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `0x1A` |
| 1 | u8 | channel 0 `calibrated` (0/1) |
| 2..5 | f32 LE | channel 0 `gain` |
| 6..9 | f32 LE | channel 0 `offset` |
| 10..18 | — | channel 1: same 9-byte layout |
| 19..27 | — | channel 2: same 9-byte layout |

`calibrated` is carried explicitly per channel, never inferred from
gain/offset — an uncalibrated channel's gain/offset are meaningless (the
Pico's own `ct_amps_cal.c` never reads them for that channel) and must not
be displayed or trusted as if they were a real correction. See
`firmware/SaftyFW/src/config_store.h`'s header comment on
`config_store_ct_channel_cal_t` for the full "uncalibrated is explicit, not
zero" reasoning, and `firmware/SimFW/src/sim/ct_calibration.h`'s header
comment for why the same discipline holds on the SimFW side of this exact
problem.

---

## 7. What the ESP web GUI should show

The safety processor is the second half of a two-processor safety system, and
right now the operator has no window onto it at all. Everything below is
already on the wire from §6 — this section is about `KilnFW`'s dashboard
(`dashboard_http.c`, `WEB_UI.md`) presenting it.

### A "Safety Processor" panel, always visible

| Shown | Source | Notes |
|---|---|---|
| **Safety temperature** | 23-byte status frame, bytes 2..5 | **Label it with its placement mode** — "Chamber (safety)" vs "External overheat" (`firmware/SaftyFW/docs/SAFETY_MODEL.md` §3). Shown as `—` when `TEMP_VALID` is clear, never as a number |
| **Enclosure temperature** | status frame bytes 6..9 (MAX31856 cold junction) | This is the electronics enclosure, not the kiln. Colour it against S12's `cj_warn_c` (60 °C) and `cj_max_c` (85 °C) |
| **Power, total and per channel** | `SAFETY_CMD_POWER` | Plus `i_conducting_a` per channel — the number that drifts as elements age |
| **Energy this firing** | integrate `p_total_w`, reset on profile start | The figure people actually want |
| **Link state + age** | frame arrival time | Green / amber (>1.5 s, heat blocked) / red (>30 s, firing aborted) |
| **Armed / grace / tripped** | `DIAG` byte 24 | |
| **Trip reason + deciding values** | `TRIP_EVENT` | The answer to "why did the kiln stop" |
| **Build identity + config CRC** | `FW_VERSION` | On a diagnostics page, not the main view |

### Presentation rules that matter

**Never render an invalid reading as a number.** `TEMP_VALID` clear means NaN on
the wire and `—` on screen. A greyed-out dash is unmistakable; a stale "247 °C"
is not, and the whole NaN-not-zero discipline in the protocol is wasted if the
GUI helpfully renders it as 0.

**Label the safety temperature with its placement.** In `EXTERNAL_OVERHEAT` mode
the number will not track the zone temperatures and *should not* — an unlabelled
reading sitting 600 °C away from the zone readings looks like a broken sensor and
will generate support questions forever.

**Mark the power figure as an estimate.** No voltage is measured anywhere;
`mains_voltage_v` is a nominal and elements are resistive, so a 5 % supply sag is
a 10 % power error (`firmware/SaftyFW/docs/CURRENT_SENSE.md` §3b). Show it to three significant
figures at most, and show `—` when `mains_voltage_configured` is clear rather
than silently assuming 240 V.

**Give `TRIP_INEFFECTIVE` its own treatment.** Every other trip means
"investigate the kiln". S9 means **"power is still flowing — go to the
breaker"** (`firmware/SaftyFW/docs/SAFETY_MODEL.md` S9). Different required action, so it must not
look like the others.

**Show the safety processor's own build identity, not just the ESP's.** Two
processors means two firmwares that can be out of sync, and a config CRC of zero
means the safety processor is running compiled-in defaults nobody commissioned.

**The panel must render with the link down.** Last-known values with an explicit
age, or dashes — never a blank panel or a spinner, which read as "loading"
rather than "the safety processor is not answering".

Mirror all of it on the PC-link `SAFETY` task (task 7) as well, so the GUI in
`pc_tools` and the MCP server see the same data without Wi-Fi — the same
reasoning `uart_task_ids.h` gives for tasks 8–11 existing at all.

---

## 8. Liveness: the safety processor must be alive to heat

A firing may not start, and may not continue, unless the ESP has recent
evidence that the safety processor is running.

The mechanism already exists on the ESP and needs only to be pointed at the
telemetry stream: `safety_link_set_fault_source(SAFETY_FAULT_SRC_SAFETY_LINK)`
feeds `relay_authority_on_blocked()`, which every relay-on path already goes
through — the UART bridge, the dashboard's `POST /api/relay`, and
`profile_executor` (`firmware/KilnFW/docs/SAFETY_MODEL.md` §1). So asserting that one bit
blocks **every** heater-on event on the board, with no new gate to write.

**Two timeouts, not one**, for the same reason `firmware/SaftyFW/docs/SAFETY_MODEL.md` §2 gives:
blocking new heat is cheap and instantly reversible, aborting a firing is
neither.

| Silence | Action | Rationale |
|---|---|---|
| > **1.5 s** (3 × 500 ms, matching the existing `SAFETY_LINK_UP_PERIODS = 3`) | Assert `SAFETY_FAULT_SRC_SAFETY_LINK`. **All relay-on refused.** Clears automatically on the next good frame | Cheap, reversible, and self-healing. One dropped frame costs nothing |
| > **30 s** | **Abort the firing** — `profile_executor` faults, relays dropped and retried until the write succeeds | Half a minute of silence is a dead safety processor, not a glitch |

A single dropped telemetry frame must not abort a twelve-hour firing. Thirty
seconds of silence must not be survivable.

**At boot the ESP must have heard from the Pico before it will heat at all.**
Before the first frame ever arrives, the link counts as down — the same
"absence is not proof of safety" rule `KilnFW` already applies to the PC link
(`firmware/KilnFW/docs/SAFETY_MODEL.md` §3). An unpopulated A1, an unprogrammed Pico and
a crashed one all correctly refuse to let the kiln heat.

**This makes `fault_on_link_loss` load-bearing rather than vestigial**, and
resolves the open question in §5: `SAFETY_FAULT_SRC_SAFETY_LINK` now means "no
telemetry frame within 1.5 s", which is a real, checkable condition.

> ⚠️ **This is also the one change most likely to make the board unusable if the
> Pico is not fitted.** With it in place, a main board with no safety processor
> cannot turn on a relay at all. That is correct behaviour and it is the point —
> but bench work on the main board alone will need
> `safety_link_fault_on_link_loss(link, false)`, which already exists, already
> logs a warning, and is already documented as a deliberate removal of a safety
> behaviour rather than a configuration detail.

---

## 9. Changes required in `KilnFW`

Neither firmware is complete; these are the ESP-side items. Sequenced in
`firmware/SaftyFW/TODO.md` phase 0, and **items 0.1 and 0.2 block all bring-up.**

| # | Change | Files |
|---|---|---|
| 0.1 | **`KILNCTL_SAFETY_TX_IO` = 5, `KILNCTL_SAFETY_RX_IO` = 4**, internal pull-up on the RX pin (GPIO4, alongside external R15). Link is dead if these are swapped. Set the other way round on 2026-08-16 from a schematic trace; **corrected 2026-08-23 against a bench measurement.** | `App/drivers/Kconfig`, `sdkconfig`, `safety_link.c` |
| 0.2 | Add `UART_PROTO_MSG_BROADCAST = 0x04` — send without waiting, receive without ACKing. **Done 2026-08-16**: the frame type and `uart_protocol_send_broadcast()` exist; nothing in `safety_link.c` calls it yet, that is 0.3 below | `espInterfaces/uart_protocol.{c,h}` |
| 0.3 | Replace the `GET_STATUS` poll loop with a 500 ms context broadcast; build the frame in §4 | `safety_link.c` |
| 0.4 | Track `relay_recent_mask` over a ≥150 s window. `relay_authority` already sees every relay command, so this belongs there | `relay_authority.{c,h}` |
| 0.5 | Accept unsolicited status/diag/version/trip frames with no outstanding request (partly present — the poll cache already accepts unsolicited status) | `safety_link.c` |
| 0.6 | **Redefine `SAFETY_FAULT_SRC_SAFETY_LINK` as "no telemetry within 1.5 s"** and wire the 30 s firing-abort. §7 | `safety_link.{c,h}`, `profile_executor.c` |
| 0.7 | Request the Pico's build identity at boot, **with retry**, and on every `boot_id` change. Reuse the `INFO_CMD_GET_FW_VERSION` parser | `safety_link.c`, `pc_tools/` |
| 0.8 | Send `SET_FIRING_CEILING` at profile start/edit — the highest target the profile will ask for | `profile_executor.c`, `profiles_http.c` |
| 0.9 | Surface `DIAG`, `FW_VERSION` (incl. **config CRC**) and `TRIP_EVENT` on the dashboard and over the PC link | `dashboard_http.c`, `uart_bridge.c` |
| 0.10 | Bump `UART_PROTOCOL_VERSION` 4 → 5 with a note explaining the broadcast type | `uart_task_ids.h:52` |
| 0.11 | Correct `docs/SAFETY_LINK.md` "Trap 1" and the R15 boot-artefact note; correct `docs/HARDWARE.md`'s optocoupler table. **Done 2026-08-16.** | `firmware/KilnFW/docs/` |
| 0.12 | Update `docs/SAFETY_MODEL.md`'s summary table — the "safety processor reports E-stop/fault" row flips from **No** to **Yes** once 0.6 lands | `firmware/KilnFW/docs/` |

Item 0.11 matters more than it looks. Those two documents currently describe the
link direction backwards, and they are the first thing anyone reads before
touching this code. Leaving them wrong guarantees the swap in 0.1 gets
"corrected" back by the next person.

Item 0.6 is the one that changes main-board behaviour most: it is what makes
"the safety processor must be alive to fire" true, and it will stop an
unaccompanied main board from heating at all. See the warning at the end of §8.

Two more, driven by the borrowed-thermocouple option:

| # | Change | Files |
|---|---|---|
| 0.13 | **Per-zone `sample_counter`**, incremented where a conversion is *consumed* — not where this frame is built | `MAX31856.c`, `profile_executor.c`, `safety_link.c` |
| 0.14 | **Per-zone `tc_type` configuration** in `zones_cfg_t` and the zones page, carried in the context frame. The wire command already supports it; only the config does not | `zones_http.{c,h}`, `uart_task_ids.h` |

---

## 10. Completion checklist

**Blocking (`firmware/SaftyFW/TODO.md` phase 0)**
- [x] 0.1 Safety-UART pins in `KilnFW` measured and set (TX→5, RX→4), pull-up on GPIO4
- [x] 0.2 `UART_PROTO_MSG_BROADCAST = 0x04` added (send + receive path; not yet called from `safety_link.c`)
- [x] 0.3 `firmware/KilnFW/docs/SAFETY_LINK.md` and `HARDWARE.md` optocoupler direction corrected
- [~] 0.10 `KILNLINK_PROTOCOL_VERSION` bumped to 5, `UART_PROTOCOL_VERSION` aliased to it
      -- **half done (2026-08-17)**: the alias is real,
      `firmware/KilnFW/App/drivers/uart_task_ids.h`'s `UART_PROTOCOL_VERSION`
      is now `((uint16_t)KILNLINK_PROTOCOL_VERSION)`, not a second number.
      The bump to 5 was deliberately **not** done this pass: it is a
      cross-firmware wire-compatibility decision (both `KilnFW` and `SaftyFW`
      must agree on the same number in the same commit) and `SaftyFW` was
      being worked on in parallel by a different pass when this one landed --
      bumping the shared `CommonFW/include/kilnlink/kilnlink_version.h`
      constant unilaterally risked stepping on that work. Revisit together,
      in one commit touching both firmwares, per this file's own header
      guidance ("bump `KILNLINK_PROTOCOL_VERSION` if a peer would break").

**ESP → Pico**
- [ ] `SAFETY_CMD_PUSH_CONTEXT` (0x07) built and broadcast at 500 ms
- [ ] `relay_recent_mask` tracked over a ≥150 s window in `relay_authority`
- [ ] `recent_window_s` transmitted, not assumed
- [ ] `boot_id` increments per ESP boot
- [ ] Per-zone `sample_counter` incremented at conversion-consume time
- [ ] Per-zone `tc_type` configurable and transmitted
- [ ] `SIM_PLANT` flag set when built against the simulated plant
- [ ] `SET_FIRING_CEILING` (0x09) sent at profile start/edit
- [ ] `CLEAR_TRIP` (0x0A) wired to the GUI
- [ ] `GET_FW_VERSION` (0x0B) at boot, **with retry**, and on every `boot_id` change
- [ ] `SET_CLOCK` (0x0C) — optional, diagnostic only
- [ ] `GET_STATUS` poll loop removed (no ACK'd `DATA` polls remain)

**Pico → ESP**
- [x] 23-byte status frame, byte-identical to the existing layout, at 500 ms
      (`firmware/SaftyFW/src/tasks/link_task.c`, `link_frame.c`)
- [ ] `DIAG` (0x08) — not this pass, explicitly lower priority than Frame A
- [x] `FW_VERSION` (0x0B) on request **and** unsolicited at boot, with config CRC
      (CRC field present and transmitted; value is honestly 0 — no
      `config_store` yet, Phase 9)
- [ ] `TRIP_EVENT` (0x0D) pushed immediately, repeated, deduped on `trip_seq`
- [ ] `POWER` (0x0E)
- [x] Non-blocking TX ring: drops on full, counts, never blocks
      (`firmware/SaftyFW/src/tasks/uart_owner.c`)

**Liveness (§8)**
- [ ] `SAFETY_FAULT_SRC_SAFETY_LINK` redefined as "no telemetry within 1.5 s"
- [ ] 30 s firing-abort wired into `profile_executor`
- [ ] Before the first frame ever arrives, the link counts as down
- [ ] Bench-escape documented: `safety_link_fault_on_link_loss(link, false)`

**GUI (§7)**
- [ ] Safety Processor panel: safety temp, enclosure temp, power, energy, link age, state, trip reason
- [ ] Safety temperature labelled with its placement mode and source
- [ ] Invalid readings render `—`, never a number
- [ ] Power marked as an estimate; `—` when `mains_voltage_v` unset
- [ ] `TRIP_INEFFECTIVE` given its own visual treatment
- [ ] Panel renders with the link down (last-known + age, never a spinner)
- [ ] Mirrored on the PC-link `SAFETY` task for `pc_tools`/MCP
