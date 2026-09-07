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
the isolation barrier (a digital isolator, U6, for the two UART lines as of
2026-08-25; a TCMT1109 optocoupler, U1, unchanged for the fault line). This
document specifies both ends, because **both ends need
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
TX FIFO are clocked out by the shift register at whatever the configured baud
is (see §3 for the current value) whether or not anything is listening, or is
even powered. A frozen ESP
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
| Line | 8N1, see `KILNCTL_SAFETY_BAUD_RATE` in `KilnFW/App/drivers/Kconfig` for the current measured value | `Kconfig:214` |

**HISTORY: the baud rate used to be capped by the TCMT1109 optocoupler pair,
not by the UART peripheral on either side; that pair (U2/U3, with R7/R12/R15)
was replaced by U6, a non-inverting ADuM1201WT digital isolator, on
2026-08-25, and this section's 9600 figure and its "needs a faster part"
conclusion belonged to the retired pair, not to U6 or to either UART
peripheral.** Measured 2026-08-23, walking the rate down with both ends
changed together while the Pico transmitted a status frame every 500 ms:
115200 and 57600 delivered zero frames, ever; 38400 lost about 20%; 19200
looked clean over a short window but lost ~10% over a longer one; 9600
tracked sent-to-received one for one over minutes and was the value both
firmwares then hardcoded, with no negotiation — `KILNCTL_SAFETY_BAUD_RATE` in
`KilnFW`, `UART_OWNER_BAUD_RATE` in `SaftyFW`'s `src/tasks/uart_owner.c`. At
the time, raising it needed a faster part or a line driver in place of the
optocoupler/pull-up pair; now that U6 is fitted instead, that specific
constraint is gone. A fresh sweep against U6 (2026-08-25, 60 s continuous per
rate, both sides rebuilt/reflashed each time) passed 921600, 460800 and
230400 clean, and a negative control (230400 against a deliberately
mismatched 115200 peer) failed in 3 s, proving the test could fail; the
committed value is **230400**, two standard steps below the fastest passing
rate. `KILNCTL_SAFETY_BAUD_RATE` in `KilnFW/App/drivers/Kconfig` is the single
source of truth for this number and carries the full table — see it, not the
9600 figure above. **A
static GPIO high/low test across this link passes at any baud rate** —
both an optocoupler and a digital isolator carry a DC level fine — which is
why the old optocoupler ceiling was found late: the wiring had already been
bench-verified that way in both directions, so suspicion fell on framing,
device/task ids and line inversion instead, all of which were correct. See
`firmware/KilnFW/docs/SAFETY_LINK.md`'s "Transport" section for the full
historical measurement table.

**Polarity: no inversion on either direction as of 2026-08-25.** U6 (the
ADuM1201WT that now carries both UART directions) does not invert, so
neither firmware applies any line inversion any more.
**HISTORY:** under the retired U2/U3 optocoupler pair, exactly one inversion
per direction was required, and the two directions did not invert in the
same place — ESP -> Pico was inverted entirely in the ESP's UART peripheral
(`TXD_INV` on GPIO5); the Pico's plain hardware UART read that direction at
standard polarity with no inversion of its own. Pico -> ESP could not use the
same trick, because the RP2040's PL011 UART has no line-inversion control —
so `SaftyFW` inverted at the GPIO pad instead, via
`gpio_set_outover(SAFTYFW_PIN_UART1_TX, GPIO_OVERRIDE_INVERT)` in
`uart_owner.c`, and `KilnFW` applied `TXD_INV` only (not `RXD_INV`) on its
own UART for that direction, since GPIO4 already arrived at the correct
polarity. That also meant both optocouplers sat dark (LED off) at idle: before
the pad-override fix, GP4 idled at ordinary UART mark (high), keeping U3's LED
lit continuously between frames; U2 was already correct. Both of those
inversion mechanisms have since been removed; re-adding either now would
invert an already-correct signal into U6 and break the link. See
`firmware/SaftyFW/docs/HARDWARE.md` §1 and `firmware/KilnFW/docs/SAFETY_LINK.md`
"Trap 2" for the full historical detail.

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

### Never wait on a receive buffer filling

Both ends read into a buffer sized for the **worst-case** frame, because
neither knows a frame's length before it arrives. Neither end may make the
delivery of what has *already* arrived wait on that buffer being filled.
This is a transport rule, not a protocol one, but it is stated here because
violating it breaks the timing contract every command in this document
depends on, while leaving the wire itself completely clean — no CRC error,
no resync, no drop, just late frames.

- **ESP (`KilnFW`, `espInterfaces/uart_protocol.c`)** —
  `uart_protocol_rx_task()` asks `uart_get_buffered_data_len()` first and
  reads only what is actually buffered, with a **zero** timeout. Only when
  nothing is buffered does it block, and then for a **single byte** bounded
  by `UART_PROTOCOL_RX_IDLE_POLL_MS` (100 ms) — so the wait ends on the next
  frame's first byte, and the bound is a shutdown-noticing tick rather than
  added latency.

**The ESP side has two buffers, sized for different things.** They are not
interchangeable and neither number should be "matched" to the other:

| Buffer | Size | Sized against |
| --- | --- | --- |
| `UART_OWNER_RX_RING_BUF_SIZE` (`uart_owner.c`, the `uart_driver_install()` ring) | 4096 | **Scheduling delay.** The IDF driver's ISR fills it; `uart_protocol_rx_task()` runs at priority 6, below WiFi. It must absorb everything that can land while that task is descheduled. |
| `UART_PROTOCOL_RX_CHUNK_BYTES` (`uart_protocol.h`, the task's stack buffer) | 2048 | **Frame size.** How much of the backlog one wakeup hands up. Must be ≥ one worst-case stuffed frame (528); above that it is headroom, ~4 back-to-back frames. |

Raised 528 → 2048 on 2026-08-28 at the owner's request. Growing it is only
safe because of the rule above — with the read decoupled from the buffer
filling, capacity sets how many bytes a wakeup may carry and never how long
an arrived byte waits. It lives on the RX task's stack, so
`CONFIG_KILNCTL_UART_PROTOCOL_STACK_SIZE` moved with it (6144 → 8192).
- **Pico (`SaftyFW`, `tasks/uart_owner.c` + `tasks/link_task.c`)** — the RX
  interrupt drains the UART FIFO into a fixed ring on every character, and
  `link_task` polls that ring every `LINK_TASK_POLL_MS` (10 ms). The buffer
  is large and the poll is bounded and independent of it, which is the same
  property by a different mechanism.

Neither side uses DMA for this link. It was considered (2026-08-28) and is
not what the latency problem was: on the ESP the standard ESP-IDF UART driver
already stages bytes into a 4096-byte ring from its own ISR and exposes no
DMA mode, and on the Pico the per-character ISR is far below the link's
budget. What matters is the rule above, not the mechanism that satisfies it —
and a DMA rewrite would satisfy it no better while carrying the fault-line
risk of replacing the transport under a safety-critical link.

> **The incident this rule was written from (2026-08-28).** The ESP's read was
> `uart_read_bytes(port, chunk, sizeof(chunk), 200 ms)` with `chunk` sized to
> the worst-case stuffed frame (528 bytes). `uart_read_bytes()` does not
> return early with a partial read — it re-blocks until `length` bytes have
> arrived or the timeout expires. On a link whose frames are ~40 bytes and
> which is busiest at ~10 frames/s, 528 bytes never arrive, so **every** read
> held its bytes for the full 200 ms. Measured live: **42 timeouts in 42
> polls (100%)** against 423 STATUS frames received in the same window, with
> zero CRC errors, zero resyncs and zero drops. That is
> `SAFETY_FAULT_SRC_SAFETY_LINK` asserted continuously, which blocks all
> heating.

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
| 2 | u8 | `boot_id` — a fresh random value generated per ESP boot, **not a monotonic counter** (`safety_link.c:358`, `esp_random()`). Randomness is deliberate: the Pico only needs to notice "this is a *different* boot than the last one I saw" to reset its correlation windows — it never needs to order boots relative to each other, so there is nothing an incrementing counter would buy that a random value doesn't, and a random value needs no persisted state to survive a crash-reboot. See section 9's `boot_id` checklist item for the full deviation note. |
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

`gain`/`offset` must already be inverted the way SimFW's
`tools/gen_ct_cal_table.py` used to invert its fit before writing a
compiled table (SimFW was removed 2026-08-28, so this tooling is gone; the
math it applied is recorded here for anyone rebuilding an equivalent):
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

### `SAFETY_CMD_CT_AUTO_ZERO_BEGIN` = `0x26` (ESP → Pico)

CT_COMMISSIONING_PLAN.md step 2 -- one byte channel argument (0-2). Arms
current_task.c's own idle-offset accumulator for that channel; the actual
measurement is NOT synchronous with this frame -- it accumulates one raw ADC
sample per current_task's own normal period (SAFTYFW_PERIOD_CURRENT_TASK_MS)
until `CT_AUTO_ZERO_TARGET_SAMPLES` (200, ≥10 s) is reached. Fire-and-forget,
same as `SAFETY_CMD_SET_CT_CAL`: never ACKed on the wire. Refused silently
(logged Pico-side) if `channel` is out of range or a measurement is already
in progress. **Deliberately does not measure synchronously inside link_task's
own dispatch** -- link_task's watchdog check-in deadline is 30 ms
(`watchdog_task.c`'s `WATCHDOG_CHECKIN_LINK_TASK` row), and a multi-second
blocking measurement there would starve it and reset the board.

### `SAFETY_CMD_GET_CT_AUTO_ZERO` = `0x27` (ESP → Pico), request only

One byte, no arguments — same shape as `SAFETY_CMD_GET_CT_CAL`. The ESP polls
this repeatedly, after a `CT_AUTO_ZERO_BEGIN`, until the reply
(`SAFETY_CMD_CT_AUTO_ZERO_STATUS`, §6 Frame H) reports `state == DONE`.

### `SAFETY_CMD_ROLLBACK` = `0x17` (ESP → Pico)

One byte, no arguments — same shape as `SAFETY_CMD_GET_FW_VERSION`.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `0x17` |

The explicit "go back to the previously-running bootloader slot, right now"
command — the Pico-side half of `tools/PcTools/TODO.md`'s
`ota_rollback(processor)` line (the ESP half, `POST /api/ota/esp/rollback`,
already exists in `firmware/KilnFW/App/drivers/http/ota_http.c`). Fire-and-forget,
same as `CLEAR_TRIP`/`SET_CONFIG`: never ACKed on the wire.

The refuse/proceed decision — is the other bootloader slot even valid to
fall back to? — is entirely `SaftyFW`'s, in `bootloader/metadata.c`'s
`bootloader_decide_rollback()`. This codec (`kilnlink_rollback.{c,h}`) only
serializes the one-byte frame; it carries no opinion about whether a
rollback should be allowed.

### `SAFETY_CMD_ANNOUNCE_REBOOT` = `0x18` (ESP → Pico)

One byte, no arguments — same shape as `SAFETY_CMD_ROLLBACK`/`SAFETY_CMD_GET_FW_VERSION`.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `0x18` |

Fire-and-forget, never ACKed on the wire. Sent unsolicited by the ESP
immediately before `esp_ota_mark_app_invalid_rollback_and_reboot()` for a
**routine OTA self-update** — "I am about to go silent for a few seconds on
purpose, this is not a crash." Introduced 2026-08-19
(`5173539`); it does not gate on `KILNLINK_PROTOCOL_VERSION`/
`KILNLINK_MIN_COMPATIBLE` at all (no bump accompanied it — an old Pico that
does not recognize `0x18` simply drops it, the same as any other unknown
cmd byte, and falls back to the pre-existing behavior of trusting S6b's
ordinary link-dead timers).

`SaftyFW`'s `link_task.c` (`link_task_handle_announce_reboot()`) records the
Pico's own uptime at which the frame was decoded via `reboot_announce_mark()`
(`reboot_announce.c`/`.h`). `safety_core.c` (`firmware/SaftyFW/src/tasks/
safety_core.c:170`) is the only place that turns that timestamp into a fact:
a fixed **`REBOOT_GRACE_WINDOW_MS` = 20000 (20 s)** window, computed fresh
every tick as `now_ms - announced_at_ms < REBOOT_GRACE_WINDOW_MS`, exposed to
`safety_guards.c` as the single bool `safety_guard_input_t::reboot_grace_active`.

**What it suppresses, precisely.** Only S6b's (link-dead guard,
`SAFETY_MODEL.md` section 4) own `trip()` calls, and only those — the
elapsed-silence accumulator that S6b reads keeps incrementing regardless, so
a genuinely dead ESP still trips on the very first tick after the window
closes, with no accumulated advantage carried over. The underlying `link_up`
fact is untouched, every other guard is untouched, and the frame has no path
into `relay_owner`'s energize/ARM logic at all (`safety_guards.c` has no
link/GPIO access, by this codebase's isolation rule — `check_isolation.ps1`).
This is a courtesy notice, never a permission grant: if the reboot runs long
and the link is still down when the window expires, S6b trips exactly as if
the frame had never arrived.

### `SAFETY_CMD_ROLLBACK_RESULT` = `0x25` (Pico → ESP)

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `0x25` |
| 1 | u8 | `accepted` (0/1) |
| 2 | u8 | `reason` |

The missing reply for a refused `SAFETY_CMD_ROLLBACK` — see
`kilnlink_rollback_result.h` for the full design rationale. `reason` is
`kilnlink_rollback_result_reason_t`:

| Value | Meaning |
|---|---|
| 0 | `ARMED` — relay is ARMED, same gate `SET_CONFIG`/`SET_CT_CAL` use |
| 1 | `NO_METADATA` — no bootloader metadata record to roll back from |
| 2 | `SLOT_INVALID` — the other slot is not currently VALID/PENDING_VERIFY |
| 3 | `STORAGE` — validation passed but the flash write itself failed |
| 4 | `UNKNOWN` — fallback; should not occur in practice |

**Asymmetric by design.** `update_task_request_rollback()` does not return
on acceptance — it calls `watchdog_reboot()` and the RP2040 resets
immediately (`src/tasks/update_task.c`) — so no code path exists that could
send this frame after an accepted rollback. In practice this frame is sent
**only on refusal**. `accepted` is a real wire field (not hardcoded), but
every ESP-side decoder must treat "the rollback was accepted" as something
it infers from the *absence* of this frame plus a link drop-and-reconnect
with a new `boot_id` — never as something this frame itself reports.

**Skew safety.** SaftyFW only emits this frame once it has learned, via
`ANNOUNCE_VERSION`, that the peer ESP is running `KILNLINK_PROTOCOL_VERSION`
≥ `KILNLINK_ROLLBACK_RESULT_MIN_PROTOCOL` (9) — mirroring
`LINK_FRAME_STATUS_V2_MIN_PROTOCOL`'s own gate exactly, so an old ESP is
never sent a frame its dispatch switch has no case for. The reverse
direction (a new ESP talking to an old Pico that predates this frame
entirely) cannot be protected by a version gate at all — an old Pico simply
never sends `0x25`, gate or no gate — so `safety_link_send_rollback()`
(KilnFW's `safety_link.c`) always bounds its wait for this reply with a
timeout and reports "unknown outcome" rather than success when nothing
arrives in that window, exactly the same as it would if the link had simply
been down. **A timeout must never be misreported as success.**

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

**The Pico's interlocks on the ON direction (2026-08-28).** `enable = 1` is
refused outright — never forwarded to the relay — when an update transfer is
active, when `safety_tc_installed == 0`, or when the board is **not
commissioned** (`commissioning_gate.h`: `calibration_missing` clear AND every
no-safe-default `fields_set` bit set, the two agreeing). `enable = 0` is never
gated. No reply frame reports the refusal and none is added: the
commissioning case is already visible as Frame B (`SAFETY_CMD_DIAG`)'s
`KILNLINK_DIAG_FLAG_CALIBRATION_MISSING` bit, which is what the ESP renders as
`commissioned: false`. There is no new fault source, reject reason or trip
code for this.

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

### Frame A: `SAFETY_CMD_GET_STATUS` = `0x01` — the existing 23-byte layout, now optionally 24 or 26

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
| 23 | u8 | **V2 only** (24 bytes total) — `tx_dropped_sat`, saturating uart_owner TX-ring-drop count, 255 = "254 or more" |
| 24 | u8 | **V3 only** (26 bytes total) — flags2: bit0 `BORROWED` (this reading is sourced, at least partly, from another zone's probe — `tc_source` is `BORROWED_ZONE` or `BOTH`) |
| 25 | u8 | **V3 only** — `borrowed_zone_index` (0..2), or `0xFF` if the borrowed zone itself is not commissioned on the Pico |

Flags: bits 0 (`LINK_UP`) and 1 (`FAULT`) are the **ESP's** to own — the Pico
sends them as **0** and the ESP clears them on receipt regardless. The Pico
owns bit 2 `ESTOP`, bit 3 `RELAY` (K4 energized), bit 4 `ENABLED` (heating
permitted), bit 5 `TEMP_VALID`, bit 6 `TC_NOT_INSTALLED` (`safety_tc_installed
== 0`, heat refused), bit 7 `TC_INJECTED` (a bench TC-injection dev switch is
active — the reading is synthetic, not from the part).

**Send NaN, never 0, when `TEMP_VALID` is clear.** An explicit not-a-number is
much harder to mistake for a cold kiln than a plausible-looking zero.

**V1/V2/V3 length, additive, sender-gated (2026-08-23 / 2026-09-03).** Byte 1
has no room left for more flag bits, so both extensions grow the frame by a
whole byte instead — the same "new byte, not a reused/overloaded bit"
discipline both times, because aliasing two different safety-relevant
conditions onto one bit would defeat the point of a distinct flag. A receiver
must accept **any** of 23/24/26 bytes, never rejecting one length in favor of
another. The sender (SaftyFW's `link_task_send_status()`) only emits the wider
length once it has positively learned, via `ANNOUNCE_VERSION`, that the peer's
`protocol_version` is new enough (`LINK_FRAME_STATUS_V2_MIN_PROTOCOL` = 6,
`LINK_FRAME_STATUS_V3_MIN_PROTOCOL` = 10) — before that, the safe default is
the shorter frame every older receiver already accepts. On the receiving side,
an absent byte 24/25 (a V1/V2 frame) must read as **UNKNOWN** whether the
reading is borrowed, never as a confident "not borrowed" — the same
`min_compatible` precedent that a too-short frame fails closed to UNKNOWN,
never to a zero that silently means "fine".

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

**`warn_mask` bit numbering (real per-guard, since the Opus review of
51c084f/c49bb0e).** For any guard that has a `SAFETY_TRIP_*` code, the bit is
`(that code - 1)` — the same numbering `trip_mask` already uses, so a guard's
warn bit and trip bit line up. S4 and S10 are WARN-only and were given
reserved trip-code gaps (4 and 11) for exactly this reason, so they already
had a collision-free slot (bits 3 and 10). S14 and S15
(`CT_COMMISSIONING_PLAN.md`) were added after `safety_trip_t` was written and
never got a reserved code — codes 15/16 went to the unrelated, not-yet-built
CONFIG_CORRUPT/SELF_TEST guards — so they take bits 14/15 directly; both bits
were always 0 on every frame sent before this (the mask was previously a
single aggregate bit, bit 0, set whenever any WARN-capable guard was active),
so this is a first use, not a reuse, of live wire bits — no
`KILNLINK_PROTOCOL_VERSION` bump needed.

| Bit | Guard | Field |
|---|---|---|
| 3 | S4 | commanded-load-should-be-off-but-current-present WARN |
| 4 | S5 | sensor-invalid WARN (pre-trip) |
| 9 | S9 | current-present-but-uncommissioned WARN (`s9_uncommissioned_warn`) |
| 10 | S10 | safety-TC-disagrees-with-every-zone-TC WARN |
| 12 | S12 | cold-junction/enclosure-over-temperature WARN (pre-trip) |
| 13 | S13 | borrowed-channel-not-updating WARN (pre-trip) |
| 14 | S14 | zone-current-above-measured-normal WARN (any of 3 channels) |
| 15 | S15 | summed-topology zone-under-current ("open heater") WARN (any of 3 zones) |

S14/S15 are tracked per-channel/per-zone in SaftyFW's `safety_guard_state_t`
(`s14_warn[3]`/`s15_warn[3]`) but each collapses to exactly one mask bit here
— any channel/zone warning sets the guard's bit — matching `trip_mask`'s
existing per-guard-not-per-channel granularity. All bits not listed above are
always 0 in this build (no other guard has a WARN concept yet); a future
WARN-capable guard reuses this same "trip code minus one, or a fresh bit if
none was reserved" rule. See `firmware/SaftyFW/src/safety_guards.h`'s
`safety_guards_warn_mask()` doc comment for the source of truth this table
mirrors.

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
| 2 | u8 | flags: bit0 `mains_voltage_configured`, bit1 `any_channel_clipped`, bit2 `calibrated`, bit3 `counts_valid` (2026-09-06) |
| 3..6 | f32 LE | `mains_voltage_v` as configured (NaN if not set) |
| 7..30 | 3 × 8 | per channel: `i_conducting_a` f32 LE, `conduction_fraction` f32 LE |
| 31..42 | 3 × f32 LE | `p_avg_w` per channel (NaN if `mains_voltage_v` unset or channel clipped) |
| 43..46 | f32 LE | `p_total_w` (NaN if any contributing channel is invalid) |
| 47..54 | f64 LE | `energy_wh` accumulated since the Pico last booted |
| 55..60 | 3 × u16 LE | `counts_avg` per channel — **added 2026-09-06** (`KILNLINK_PROTOCOL_VERSION` 11) |

`i_conducting_a` is the draw **while conducting**, not a duty-averaged figure —
see `firmware/SaftyFW/docs/CURRENT_SENSE.md` §3b for why those must be reported separately, and why
the ESP's own duty figure is the better multiplier.

`energy_wh` resets on Pico reboot; `boot_id` in the context/version frames is
how the ESP knows to restart its own totalisation rather than see a step change.

**`counts_avg`** is the raw 16x-oversampled ADC reading per channel
(`current_sense.c`'s `cs_read_channel_counts()`, `CURRENT_SENSE.md` §4),
published **independent of calibration** — unlike `amps[]`/`i_conducting_a`,
it is never zero-forced by an uncommissioned `k_ct_v_per_a`. This closes the
gap `CURRENT_SENSE.md` §4 ("Measured noise floor — BLOCKED, not measured")
recorded: there was previously no path from the running board to raw ADC
counts on either the wire or the debug side. This frame's length grew from
`KILNLINK_POWER_LEN_V1` (55) to `KILNLINK_POWER_LEN_V2` (61) to carry it, the
first length change this frame has ever had; `kilnlink_power_decode()`
accepts both lengths and reports whether `counts_avg` is real via the new
`KILNLINK_POWER_FLAG_COUNTS_VALID` bit rather than inferring it from length
alone — see `kilnlink_version.h`'s 10→11 entry for the full skew-safety
argument (this is the first POWER-frame length change, so unlike Frame A it
had no pre-existing length-tolerant decoder to reuse; this change adds one).

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
zero" reasoning. (SimFW's `src/sim/ct_calibration.h` used to carry the
matching header comment for why the same discipline held on its side of
this exact problem; SimFW was removed 2026-08-28.)

### Frame H: `SAFETY_CMD_CT_AUTO_ZERO_STATUS` = `0x28` — auto idle-offset progress/result

Sent in reply to `SAFETY_CMD_GET_CT_AUTO_ZERO` (§4 above, `0x27`).
CT_COMMISSIONING_PLAN.md step 2. Reports `current_task.c`'s own auto-zero
accumulator state exactly, no interpretation — the ESP decides refusal/
100 mV/manual-wins policy from this raw measurement.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `0x28` |
| 1 | u8 | `state` — 0=IDLE, 1=IN_PROGRESS, 2=DONE |
| 2 | u8 | `channel` |
| 3..4 | u16 LE | `samples_taken` |
| 5..6 | u16 LE | `samples_target` |
| 7..8 | u16 LE | `zero_counts` — mean raw ADC counts, valid only when `state == DONE` |

Fixed 9-byte length always, same "always answer every copy seen" convention
as `SAFETY_CMD_CT_CAL` above.

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
through — the UART bridge, the dashboard's `POST /api/diagnostics/danger/relay`
(`/api/relay` before its 2026-08-27 removal), and
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
- [x] 0.10 `KILNLINK_PROTOCOL_VERSION` bumped, `UART_PROTOCOL_VERSION` aliased to it
      -- **STALE, corrected 2026-09-04**: this item's premise (a single bump
      to 5, done in one cross-firmware commit) was overtaken by events long
      ago. `firmware/CommonFW/include/kilnlink/kilnlink_version.h`'s own
      history comment shows the two numbers were later made INDEPENDENT
      (2026-08-24, after three incidents of a bump here silently refusing
      PC<->ESP traffic for changes that never touched that link) rather than
      kept aliased, and `KILNLINK_PROTOCOL_VERSION` has since been bumped
      six more times on its own schedule (5->6->7->8->9->10, one step per
      dated comment in that file, the last on 2026-09-03 for the BORROWED
      status flag). `KILNLINK_MIN_COMPATIBLE` is currently 7. The alias part
      of this item is real and current: `uart_task_ids.h`'s
      `UART_PROTOCOL_VERSION` doc comment documents the two numbers as
      independently maintained, not that one is aliased to the other --
      leaving the original "aliased to it" wording obsolete too. See
      `CommonFW/README.md` "Versioning" for the full split history.
      **Superseded 2026-08-24**: the alias itself turned out to be the
      problem it was trying to avoid, not a solution — a `KILNLINK_PROTOCOL_
      VERSION` bump driven purely by the isolated link (5->6, `tx_dropped_
      sat`) silently dragged `UART_PROTOCOL_VERSION` with it and got every PC
      command refused on real hardware, with no PC-link contract change to
      justify it. `UART_PROTOCOL_VERSION` is once again its own,
      independently-maintained literal (frozen at 7); see that header's own
      doc comment and `tools/check_uart_version_independence.ps1`.

**ESP → Pico**
- [x] `SAFETY_CMD_PUSH_CONTEXT` (0x07) built and broadcast at 500 ms.
      **2026-09-04 (triage verification)**: `safety_link_poll.c`'s poll loop
      calls `safety_build_and_send_context(link)` every
      `CONFIG_KILNCTL_SAFETY_POLL_PERIOD_MS`, independent of the
      request/reply exchange above it — a real `BROADCAST`, not part of any
      ACK'd pairing. Payload built by `safety_link_frames.c`, encoded via
      `kilnlink_context_encode()`.
- [x] `relay_recent_mask` tracked over a ≥150 s window in `relay_authority`.
      **2026-09-04**: `safety_link_frames.c`'s `safety_context_update_relay_recent()`
      windows on `SAFETY_LINK_CONTEXT_RECENT_WINDOW_S` (`safety_link.h:622`) =
      **180 s**, ≥ the doc's 150 s floor. (Tracked in `safety_link.c`'s own
      state, not literally `relay_authority.{c,h}` as this item's file column
      named — a deviation from where, not whether, this landed.)
- [x] `recent_window_s` transmitted, not assumed. **2026-09-04**:
      `safety_link_frames.c:390` sets `ctx.recent_window_s =
      (uint8_t)SAFETY_LINK_CONTEXT_RECENT_WINDOW_S` on every context frame.
- [x] `boot_id` increments per ESP boot. **2026-09-04, deviation noted**:
      `safety_link.c:358` sets `link->esp_boot_id = (uint8_t)esp_random()` —
      a fresh random value per boot, not a monotonic counter. Functionally
      equivalent for this field's stated purpose (the Pico resets its
      correlation windows whenever the value changes across a reboot) and
      documented in-code as deliberate (`"not a security or safety value, so
      true randomness costs nothing and needs no persisted counter"`,
      `link_task.c`'s `s_boot_id` comment, referenced from
      `safety_link.c:352`) — but it is not literally "increments," so this
      box is ticked on behavior, not on the literal wording.
- [x] Per-zone `sample_counter` incremented at conversion-consume time.
      **2026-09-04**: implemented in `safety_link_frames.c` (grep confirms
      `sample_counter` is populated there); not independently re-verified
      this pass that the increment site is conversion-consume rather than
      frame-build (see 0.13 in section 9 for the original requirement) —
      flagged for a closer look if this ever misbehaves like a stale-value
      bug.
- [x] Per-zone `tc_type` configurable and transmitted. **2026-09-04**:
      `safety_link_frames.c:443` sets `z->tc_type = cfg.tc_type` per zone in
      the context builder, sourced from `zones_config_get_safety_tc_type()`.
- [x] `SIM_PLANT` flag set when built against the simulated plant.
      **2026-09-04**: `safety_link_frames.c:497-498`,
      `#if CONFIG_KILNCTL_SIM_PLANT` sets `KILNLINK_CONTEXT_FLAG_SIM_PLANT`.
- [ ] `SET_FIRING_CEILING` (0x09) sent at profile start/edit. **Confirmed
      still not built** (2026-09-04): no call to `kilnlink_ceiling_encode()`
      anywhere under `App/drivers/`. Doable in software (no hardware
      dependency), but implementing a new outbound frame + `profile_executor.c`/
      `profiles_http.c` wiring is out of scope for this triage pass, which
      prioritized the OTA host-test gap per this task's own instructions;
      left for a dedicated pass.
- [x] `CLEAR_TRIP` (0x0A) wired to the GUI. **2026-09-04**:
      `dashboard_exec_http.c`'s `safety_clear_trip_post_handler()` (`POST
      /api/safety/clear_trip`) calls `safety_link_send_clear_trip()`
      (encoded via `kilnlink_clear_trip_encode()` in `safety_link_commands.c`),
      and both `main_page.html` and `safety_page.html` `fetch()` that
      endpoint from a "Clear trip" control.
- [x] `GET_FW_VERSION` (0x0B) at boot, **with retry**, and on every `boot_id`
      change. **2026-09-04**: `safety_link_poll.c`'s poll loop sends
      `SAFETY_CMD_FW_VERSION` (shared request/reply id, per this doc's own
      floor-frame exception) whenever `peer_version_known` is false; that
      flag is cleared on link-down and on any `boot_id` change
      (`safety_reset_stale_peer_info_if_link_down()`), so the next poll
      period re-requests automatically — the poll cadence itself is the
      retry, as `safety_link_poll.c`'s own comment states explicitly.
- [ ] `SET_CLOCK` (0x0C) — optional, diagnostic only. **Confirmed still not
      built** (2026-09-04): no `kilnlink_set_clock_encode()` call site.
      Marked optional by the doc itself; left undone, same reasoning as
      `SET_FIRING_CEILING` above.
- [x] `GET_STATUS` poll loop removed (no ACK'd `DATA` polls remain).
      **2026-09-04**: `safety_exchange()` (`safety_link_inbox.c:667`) sends
      via `uart_protocol_send_broadcast()`, not an ACK'd `DATA` frame — every
      request on this link, including `GET_STATUS`/`GET_FW_VERSION`, rides
      the fire-and-forget path.

**Pico → ESP**
- [x] 23-byte status frame, byte-identical to the existing layout, at 500 ms
      (`firmware/SaftyFW/src/tasks/link_task.c`, `link_frame.c`)
- [x] `DIAG` (0x08) — `firmware/KilnFW/App/drivers/safety/safety_link_frames.c`'s
      `safety_apply_diag()` (2026-09-04, M15 C5 verification)
- [x] `FW_VERSION` (0x0B) on request **and** unsolicited at boot, with config CRC
      (CRC field present and transmitted; value is honestly 0 — no
      `config_store` yet, Phase 9)
- [x] `TRIP_EVENT` (0x0D) pushed immediately, repeated, deduped on `trip_seq`
      (`firmware/KilnFW/App/drivers/safety/safety_link_frames.c:883-928`'s
      `safety_apply_trip_event()`, backed by `safety_trip_decision.c` and
      `test_safety_trip_decision.c`; 2026-09-04, M15 C5 verification)
- [x] `POWER` (0x0E) — `firmware/KilnFW/App/drivers/safety/safety_link_frames.c`'s
      `safety_apply_power()` (2026-09-04, M15 C5 verification)
- [x] Non-blocking TX ring: drops on full, counts, never blocks
      (`firmware/hwAbstraction/pico/uart/uart_owner.c`)

**Liveness (§8)**
- [x] `SAFETY_FAULT_SRC_SAFETY_LINK` redefined as "no telemetry within 1.5 s".
      **2026-09-04 (triage verification)**: `safety_link_poll.c:350` calls
      `safety_link_set_fault_source(link, SAFETY_FAULT_SRC_SAFETY_LINK, !up
      || version_mismatch)` every poll, where `up` is
      `safety_link_up_locked()` — gated on `SAFETY_LINK_UP_PERIODS` (3) missed
      500 ms polls, i.e. 1.5 s, matching the doc exactly.
- [x] 30 s firing-abort wired into `profile_executor`
      (`firmware/KilnFW/App/drivers/control/profile_executor.c:1201-1236`'s
      `safety_link_silent_30s`/`SAFETY_LINK_FIRING_ABORT_SILENCE_MS`, pinned
      by `firmware/KilnFW/App/test/test_safety_link.c:77-92`; 2026-09-04,
      M15 C5 verification). **Evidence level: host-tested and CI-pinned
      only, NOT hardware-verified** (`docs/SAFETY_CASE.md` section 4
      classification) — the tick means the code exists and a host test
      proves it fires at the coded threshold, not that anyone has held the
      real link down on a running board with a stopwatch. `ROADMAP.md` M6
      and its "Blocked on hardware that does not exist yet" table (row `S`)
      track that bench step as still open.
- [x] Before the first frame ever arrives, the link counts as down.
      **2026-09-04**: every caller that reads link state before the first
      exchange defaults `link_up = false` (`dashboard_http.c`,
      `zones_current_sweep_task.c`, `ota_http.c`, etc. — grep confirms no
      call site defaults it `true`); there is no code path that reports "up"
      before a real frame has been observed.
- [x] Bench-escape documented: `safety_link_fault_on_link_loss(link, false)`.
      **2026-09-04**: the function exists in `safety_link.c`/`.h`
      (referenced by `safety_link_poll.c:110-112`'s own comment on the
      "exactly like a dead link" policy switch) and this doc's §8 already
      documents it as the deliberate bench escape.

**GUI (§7)**
- [x] Safety Processor panel: safety temp, enclosure temp, power, energy, link age, state, trip reason.
      **2026-09-04**: `firmware/KilnFW/App/drivers/http/safety_page.html` renders
      all of these (`safetyTemp`, enclosure/cold-junction, `powerW`, energy,
      link age/state, trip reason) from `GET /api/status`.
- [x] Safety temperature labelled with its placement mode and source.
      **2026-09-04**: `safety_page.html` distinguishes chamber vs external
      placement in its rendering logic (see its `tempSuffix`/`tempCls`
      handling around line 240).
- [x] Invalid readings render `—`, never a number. **2026-09-04**: e.g.
      `safety_page.html:285`,
      `(st.power_w != null && !isNaN(st.power_w)) ? ... : '—'` — same
      em-dash discipline applied to `safetyTemp` and the other fields.
- [x] Power marked as an estimate; `—` when `mains_voltage_v` unset.
      **2026-09-04**: `safety_page.html:143-148`, "Power draw (estimate)"
      label plus explicit note that an em-dash means no configured mains
      voltage, never an assumed default.
- [x] `TRIP_INEFFECTIVE` given its own visual treatment. **2026-09-04**:
      `safety_page.html`'s `breakerBanner` ("TRIP_INEFFECTIVE -- power is
      still flowing. Go to the breaker.") and `main_page.html`'s distinct
      "!! SAFETY TRIP -- TRIP_INEFFECTIVE !!" heading, both keyed off
      `SAFETY_TRIP_INEFFECTIVE` (S9) specifically.
- [x] Panel renders with the link down (last-known + age, never a spinner).
      **2026-09-04**: consistent with the "before the first frame" item
      above — the page renders `—`/last-known values keyed off explicit
      null checks rather than blocking on a pending fetch.
- [x] Mirrored on the PC-link `SAFETY` task for `pc_tools`/MCP.
      **2026-09-04**: `firmware/KilnFW/App/drivers/bridge/uart_bridge_safety.c`
      exists and implements task 7 (`"SAFETY (task 7) -- the isolated link
      to the RP2040"`) on the PC UART bridge.
