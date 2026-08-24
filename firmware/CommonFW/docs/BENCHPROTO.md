# benchproto — hardened, addressed protocol for bench/instrument firmwares

`benchproto` is a reliable, addressed message protocol for a firmware talking
to a PC over a serial-shaped link. It provides framing, a CRC, a
request/reply layer with sequence numbers and retry/dedup, and an
addressable-task-registration model. It is hardware- and transport-agnostic:
nothing in it assumes UART, USB CDC, or any particular byte pipe — a caller
feeds it bytes (or already-decoded frames) and drives its retry timing
itself.

**First consumer:** `SimFW` (the bench test-fixture Pico, see
[`../../SimFW/docs/DESIGN_NOTES.md`](../../SimFW/docs/DESIGN_NOTES.md) sections 4.4, 5, 5.1
(and §12 for the `UnitTestFw` extraction history — decommission complete,
`firmware/UnitTestFw` deleted 2026-08-23), speaking it over native USB CDC to its PC-side `kilnsim` tools.
Framing, CRC, reliability, and task registration are implemented once here;
`SimFW`'s command groups (`SYS`/`MODEL`/`TC`/`CT`/`RELAY`/`IO`/`FAULT`/`EVT`,
DESIGN_NOTES.md sec 5) register as addressable tasks the same way this document's
section 4 describes.

## 1. Relationship to `kilnlink`

This is a **separate protocol family** from `kilnlink`
([`kilnlink/`](../include/kilnlink/), documented in
[`LINK_PROTOCOL.md`](LINK_PROTOCOL.md) and the top-level
[`README.md`](../README.md)) — the ESP32-S3 ⟷ RP2040 safety link. The two
exist for unrelated purposes, are used by unrelated devices, and share **no
code**: no shared frame codec, no shared CRC function, no shared constant.
They happen to look alike — SLIP-style byte-stuffed framing, CRC16/CCITT-FALSE,
a small fixed header — because that shape is a proven, reasonable design for
this kind of link, and reusing a proven shape is good engineering, not
because one is secretly a rename of the other. Two further, deliberate
differences from `kilnlink`:

- `kilnlink` intentionally keeps its transport half — ACK/NACK, retry,
  dedup, task registry — **out** of `CommonFW` (see that project's README,
  "What is *not* shared": `uart_protocol.c`'s transport half stays in
  `KilnFW`, because the RP2040 safety side deliberately has none of it —
  `docs/LINK_PROTOCOL.md` sec 2). `benchproto` does the opposite on
  purpose: its reliability/dedup/task-registration logic **is** the shared,
  host-tested part (`benchproto_link.{c,h}`), because unlike the safety
  link, every side of a `benchproto` link is expected to run the same
  request/reply discipline (see `firmware/UnitTestFw`'s extraction history
  in `firmware/SimFW/docs/DESIGN_NOTES.md` sec 12 for why this extraction
  exists at all).
- `kilnlink`'s frame envelope is a closed contract between two specific,
  already-shipping firmwares (`KILNLINK_PROTOCOL_VERSION`, currently 5,
  tracks `KilnFW`'s existing PC-link version because the two have not
  diverged). `benchproto` starts its own version counter
  (`BENCHPROTO_PROTOCOL_VERSION`, `benchproto_version.h`) at 1 — it is a new
  protocol, not a fork of an existing shipped one, even though its origin
  (`UART_PROTOCOL.md`) was a UART-specific prototype in
  `firmware/UnitTestFw`.

Do not "deduplicate" these two into one shared frame codec later; see
`kilnlink`'s own README for why one link is one implementation, and apply
the same reasoning symmetrically — one *family* of protocol is not the same
thing as one link.

## 2. Framing (SLIP-style byte stuffing)

```
DELIM ( ...stuffed bytes... ) DELIM
```

- `DELIM = 0x7E` marks both the start and end of a frame. Back-to-back
  delimiters are empty-frame noise and are ignored, which is also how a
  receiver resyncs after garbage on the line: any byte before the first
  `DELIM` is discarded.
- `ESC = 0x7D`, `ESC_XOR = 0x20`. Any raw byte equal to `DELIM` or `ESC` is
  stuffed as `ESC (byte ^ ESC_XOR)`.

This matches `firmware/UnitTestFw/UnitTest/docs/UART_PROTOCOL.md`'s framing
section verbatim in shape (that document is `benchproto`'s direct ancestor);
only the surrounding language has been generalized away from "UART" to
"link", since `SimFW` carries the identical envelope over USB CDC.

## 3. Raw (unstuffed) frame layout

All **header** multi-byte fields are big-endian.

| Offset | Field | Size | Notes |
|---|---|---|---|
| 0 | `MSG_TYPE` | 1 | `DATA=0x01`, `ACK=0x02`, `NACK=0x03`, `BROADCAST=0x04` |
| 1–2 | `MSG_INDEX` | 2 (BE) | sender-assigned; reused verbatim on retransmit |
| 3 | `SRC_DEVICE` | 1 | link-defined; `SimFW` uses `HOST=0`, `TARGET=1` |
| 4 | `SRC_TASK` | 1 | |
| 5 | `DST_DEVICE` | 1 | |
| 6 | `DST_TASK` | 1 | |
| 7 | `LENGTH` | 1 | payload length, 0–`BENCHPROTO_FRAME_MAX_PAYLOAD` (128) |
| 8..8+LEN-1 | `PAYLOAD` | LEN | |
| 8+LEN..+1 | `CRC16` | 2 (BE) | CRC-16/CCITT-FALSE over bytes `[0, 8+LEN)` |

CRC-16/CCITT-FALSE: poly `0x1021`, init `0xFFFF`, no reflection, no xorout.
Check value: `crc16(b"123456789") == 0x29B1`. Chosen for consistency with
`kilnlink`'s own choice (a well-understood, table-free, easy-to-verify
variant) — see section 1 for why that consistency is a design choice, not
shared code: `benchproto_crc16_ccitt_false()` is its own implementation and
never calls `kilnlink_crc16_ccitt_false()`.

`BENCHPROTO_FRAME_MAX_PAYLOAD` is 128 (not `kilnlink`'s or `KilnFW`'s
current 253) — a deliberately generous, round default for the request/reply
payloads `SimFW`'s command groups need (DESIGN_NOTES.md sec 5.2's representative
payloads are all comfortably under half of it); raise it, up to the 255 hard
ceiling the one-byte `LENGTH` field allows, the same way `KilnFW` raised its
own copy of this constant if a future payload genuinely needs the headroom.

As with `UART_PROTOCOL.md`'s original design: header fields are big-endian,
but the multi-byte fields *inside* command payloads are conventionally
little-endian — the natural layout for a `memcpy` into a `float`/`double` on
either an RP2040 or a Python `struct.pack("<...")`. This is a convention for
`benchproto`'s consumers to follow in their own payload codecs (SimFW's
command groups); the envelope itself, `benchproto_frame.{c,h}`, does not
interpret payload bytes at all.

## 4. Reliability

- A sender picks the next `MSG_INDEX` (`benchproto_link_next_msg_index()`),
  transmits a `DATA` frame, and waits for a reply (`ACK`/`NACK`) whose
  `SRC_DEVICE`/`SRC_TASK` matches the `DST_DEVICE`/`DST_TASK` it sent to,
  and whose `MSG_INDEX` matches (`benchproto_link_on_frame()` with a
  `benchproto_pending_request_t` tracking that outstanding send).
- On timeout it retransmits the **same** `MSG_INDEX`
  (`benchproto_pending_note_retry()`), up to `BENCHPROTO_MAX_RETRIES = 10`
  total attempts before the caller must report `TIMEOUT`.
- Only one outstanding send-and-await-reply cycle is allowed per
  `benchproto_pending_request_t`, mirroring `UART_PROTOCOL.md`'s original
  one-outstanding-cycle-per-link design (there, enforced by a FreeRTOS
  mutex around a single `uart_protocol_t`). A caller wanting concurrent
  outstanding requests runs multiple `benchproto_pending_request_t`
  instances, one per allowed concurrency slot.
- The receiver dedups: a retransmitted `DATA` frame (the receiver's own
  prior `ACK` was lost) is recognized via a small ring
  (`BENCHPROTO_DEDUP_DEPTH = 4`) of recently-delivered
  `(src_device, src_task, msg_index)` tuples per registered task
  (`benchproto_link_mark_delivered()` / the `DUPLICATE_REACK` action from
  `benchproto_link_on_frame()`), and gets re-ACKed **without** being
  delivered to the application a second time. Like the original design,
  this ring is shallow and stateless across sessions: it has no notion of
  the peer having restarted, and an old, evicted `msg_index` reused far
  enough in the future is (rarely, harmlessly) redelivered — see
  `test/test_benchproto_link.c`'s `test_dedup_ring_depth` for the exact
  boundary this produces.
- If the destination `task_id` isn't registered, the receiver's caller
  should reply `NACK` ("undeliverable") — `benchproto_link_on_frame()`
  returns `BENCHPROTO_LINK_ACTION_NACK_UNROUTABLE` for a `DATA` frame to an
  unregistered task, instead of a silent drop.
- If a registered task's own inbox (owned entirely by the caller — see
  section 5) is full, the receiver should withhold the `ACK` so the
  sender's retry gives the task time to drain its queue; this doubles as
  natural backpressure, same as `UART_PROTOCOL.md`'s original rule. This
  library enforces the mechanism that makes that safe (only mark a message
  delivered, and thus dedup-eligible, *after* a caller confirms the
  hand-off succeeded — see `benchproto_link_mark_delivered()`'s doc
  comment) but does not itself own an inbox to observe as full or not.
- `BROADCAST` frames are the unsolicited, fire-and-forget case (`SimFW`'s
  TELEMETRY/EVT frames, DESIGN_NOTES.md sec 5.3): never ACKed, never NACKed, never
  deduped. A `BROADCAST` to an unregistered task is silently dropped
  (`IGNORE`, not `NACK_UNROUTABLE`) — the receiver is never obliged to
  reply to a broadcast either way, matching `KilnFW`'s own broadcast
  extension to this same protocol family
  (`App/drivers/espInterfaces/uart_protocol.c`, added for the safety link's
  PC-link cousin).

Return values callers can expect from a send (mapped from
`benchproto_link_action_t` plus the caller's own retry loop):

| Result | Meaning |
|---|---|
| `OK` | `ACK_MATCHED` — delivered to the destination task's inbox |
| `UNDELIVERABLE` | `NACK_MATCHED` — destination task not registered there |
| `TIMEOUT` | `benchproto_pending_note_retry()` returned false after all retries — link or peer down |

## 5. What is pure and what is each consumer's own job

Same transport-vs-contract split `CommonFW`'s top-level README draws for
`kilnlink`, applied to this file's own set of "what is shared":

| Lives in `benchproto` (`CommonFW`, host-tested) | Lives in each consumer (e.g. `SimFW`, `kilnsim`) |
|---|---|
| Frame envelope: delimiting, byte-stuffing, CRC16, encode/decode (`benchproto_frame.{c,h}`) | The actual byte pipe: UART/USB-CDC driver, ring buffers, an RX task assembling a byte stream into frames |
| Sequence-number issuance, retry-attempt counting, receiver dedup, task-registration bookkeeping, and the pure classification of a decoded frame into "what do I do next" (`benchproto_link.{c,h}`) | The retry *timer* (an RTOS `vTaskDelay`/semaphore-timeout, a host event loop, whatever the runtime provides) and the actual inbox queue a registered task's messages land in |
| Nothing about payload *contents* — command IDs, floats, structs inside a payload are entirely a consumer concern | Every command group's payload codec (`SYS`/`MODEL`/`TC`/... for `SimFW`) |

This is why `benchproto_link_on_frame()` takes an already-decoded,
already-validated frame and returns an enum rather than doing any I/O
itself, and why `benchproto_pending_note_retry()` takes no clock argument —
CommonFW/README.md's freestanding rules ("no I/O, no time, no globals, no
allocation") apply to this file exactly as they do to `kilnlink`'s codecs;
see that document's rules 1–6 and `benchproto_link.h`'s file comment for how
each rule maps onto this specific state machine.

## 6. Task registration

Any task on either side of a `benchproto` link can register a `task_id`
(1 byte, `benchproto_link_register_task()`) to become a valid destination.
Task IDs only need to be unique within their own device — exactly
`UART_PROTOCOL.md`'s original convention, ported unchanged:

> Task IDs only need to be unique within their own device (ESP task 1 and
> HOST task 1 are unrelated).

`firmware/UnitTestFw`'s prototype registered `DAC`/`AD9833`/`INFO`/`OLED`/
`LOG`/`SYSTEM`/`PCF8575` this way; `SimFW`'s own task table (its command
groups, DESIGN_NOTES.md sec 5) is defined by `SimFW` itself, not by this library —
`benchproto` only provides the registration mechanism
(`benchproto_link_register_task()` / `_unregister_task()` /
`_is_registered()`), not any fixed numbering. `BENCHPROTO_MAX_TASKS = 16`
task slots are available per `benchproto_link_t` (matching `KilnFW`'s own
post-growth cap, chosen for the same reason: the original `8` proved too
small once a real firmware's task count grew past it — see
`benchproto_link.h`'s constant comment).

## 7. What this document deliberately leaves open

- **Payload command layouts.** `UART_PROTOCOL.md`'s DAC/AD9833/OLED/PCF8575/
  INFO/LOG/SYSTEM payload sections do not have a `benchproto` equivalent
  here — those were `UnitTestFw`-specific device commands, out of scope for
  a hardware-agnostic library. `SimFW`'s own `PROTOCOL.md`
  (`firmware/SimFW/docs/PROTOCOL.md`, "written with the code" per DESIGN_NOTES.md
  sec 9) is where its `SYS`/`MODEL`/`TC`/`CT`/`RELAY`/`IO`/`FAULT`/`EVT`
  payload layouts belong, following this document's conventions (section 3's
  endianness rule, section 6's task-registration model).
- **A second, independent implementation for cross-checking.** `kilnlink`'s
  test vectors (`test/vectors/*.json`) are checked against three
  implementations: the C library, `pc_tools`' Python codec, and each
  firmware's (now-migrated) local copy. `benchproto` currently has only the
  one C implementation plus its host tests
  (`test/vectors/benchproto_frame_vectors.json`'s vectors are computed by an
  independent Python re-implementation of the spec, inlined in
  `test/test_benchproto_frame.c`, but that Python code does not live
  anywhere as a real, reusable module yet). `SimFW`'s PC-side `kilnsim` link
  layer (DESIGN_NOTES.md sec 6, "the extraction's independent second
  implementation — the same prove-it-twice pattern the kilnlink codecs
  used") is expected to become that second implementation and should be
  checked against this same manifest once it exists.
- **Version/capability negotiation.** `UART_PROTOCOL.md`'s `GET_FW_VERSION`/
  boot-time version push and `SimFW`'s planned `GET_CAPS` (DESIGN_NOTES.md sec 5.1)
  are payload-level conventions layered *on top of* `benchproto`, not part
  of the envelope or reliability layer itself — `benchproto_version.h`
  supplies the version *number* both ends should compare, not the exchange
  that compares it.
