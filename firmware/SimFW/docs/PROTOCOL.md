# SimFW USB control protocol reference

SimFW's own command reference for the native USB CDC link, built on top of
`benchproto` (`firmware/CommonFW/include/benchproto/`, spec in
[`../../CommonFW/docs/BENCHPROTO.md`](../../CommonFW/docs/BENCHPROTO.md)).
This document is SimFW-specific numbering and payload layouts *layered on
top of* that library, exactly the split BENCHPROTO.md section 7 describes:
"`SimFW`'s own `PROTOCOL.md` ... is where its SYS/MODEL/TC/CT/RELAY/IO/
FAULT/EVT payload layouts belong."

Written with the code (docs/PLAN.md section 9): the numeric source of truth
is `src/tasks/cmd_ids.h`; keep the two in sync in the same commit, same
discipline `task_priorities.h`'s header comment asks of PLAN.md section
4.1's task table.

## 1. Transport

Native USB CDC (TinyUSB device stack), one interface, full speed only. No
pico-sdk `stdio_usb` — the CDC interface belongs entirely to `usb_owner`
(`src/tasks/usb_owner.c`), the single owner of the USB peripheral (PLAN.md
section 4's opening paragraph). VID/PID `0xCafe`/`0x4001`
(`src/tasks/usb_descriptors.c`) — a placeholder pair (this is a one-off bench
fixture, never mass produced), same one TinyUSB's own examples use, chosen
for the same reason: every OS's generic CDC-ACM driver binds to it without a
custom `.inf`/driver install. The USB serial-number string descriptor is the
RP2040's own 64-bit flash unique id (16 hex chars), so `kilnsim`'s PC-side
auto-detect can tell two SimFW fixtures apart on the same host.

## 2. Framing, addressing, reliability

Identical to `benchproto`'s own spec — see BENCHPROTO.md sections 2-4 for
the full detail (SLIP-style byte stuffing, CRC-16/CCITT-FALSE, DATA/ACK/
NACK/BROADCAST, sequence numbers, retry/dedup). This document only adds
SimFW's own numbers on top:

| Constant | Value | Source |
|---|---|---|
| `SIMFW_DEVICE_HOST` | 0 | `cmd_ids.h` — the PC / `kilnsim` side |
| `SIMFW_DEVICE_TARGET` | 1 | `cmd_ids.h` — this firmware |

### Reply convention (SimFW-specific, not part of `benchproto` itself)

Every command reply's payload starts with a **status byte**, byte 0:

| Value | Name | Meaning |
|---|---|---|
| `0x00` | `SIMFW_CMD_STATUS_OK` | command handled, remaining bytes (if any) are the answer |
| `0x01` | `SIMFW_CMD_STATUS_ERR_NOT_IMPL` | recognised group, but this command id has no handler yet (a reserved id, or the whole group is still a stub) |
| `0x02` | `SIMFW_CMD_STATUS_ERR_BAD_ARGS` | reserved for a future handler that validates its own args |
| `0x03` | `SIMFW_CMD_STATUS_ERR_INTERNAL` | dispatch reached an inconsistent state (should not happen — see `cmd_task.c`'s dispatch fallthrough comment) |

This is deliberately a separate layer from `benchproto`'s own ACK/NACK:
ACK/NACK say only whether the frame was *delivered* to a registered task
(BENCHPROTO.md section 4/5) — a command can be delivered perfectly well and
still fail at the application level (not implemented, bad arguments). This
build also piggybacks the actual reply payload directly in the ACK frame
itself, rather than `UnitTestFw`'s older two-frame "ACK for delivery, then a
separate DATA frame with the answer" INFO pattern
(`firmware/UnitTestFw/UnitTest/docs/UART_PROTOCOL.md`'s INFO section) —
simpler, and nothing in BENCHPROTO.md's request/reply description requires
the two-frame split. A command's request payload is `[cmd_id, args...]`
(byte 0 = subcommand, matching that same ancestor document's convention);
multi-byte fields inside a payload are little-endian (BENCHPROTO.md section
3's endianness rule).

## 3. Command groups and task ids

Each row registers as an addressable `benchproto` task (`benchproto_link_register_task()`,
done once at boot by `cmd_task_start()`) — PLAN.md section 5.1's task-registration model.

| Group | `SIMFW_TASK_ID_*` | Value | Status |
|---|---|---|---|
| SYS | `SIMFW_TASK_ID_SYS` | 1 | **PING/GET_VERSION/GET_CAPS implemented**, rest reserved |
| MODEL | `SIMFW_TASK_ID_MODEL` | 2 | stub — every command replies `ERR_NOT_IMPL` |
| TC | `SIMFW_TASK_ID_TC` | 3 | stub |
| CT | `SIMFW_TASK_ID_CT` | 4 | stub |
| RELAY | `SIMFW_TASK_ID_RELAY` | 5 | stub |
| IO | `SIMFW_TASK_ID_IO` | 6 | stub |
| FAULT | `SIMFW_TASK_ID_FAULT` | 7 | stub |
| EVT | `SIMFW_TASK_ID_EVT` | 8 | reserved — unsolicited BROADCAST only (PLAN.md sec 5.3), no inbound request handling exists |

A "stub" group *is* registered (so a request to it gets a real ACK with
`ERR_NOT_IMPL`, never a silent drop or a NACK) — only its dispatch table
(`src/tasks/cmd_task.c`'s `s_groups[]`) has no `cmd_id` entries yet. Filling
one in is exactly `s_groups[]`'s point: add rows to that group's own
`commands` array (see `s_sys_commands[]` for the shape); `cmd_task.c`'s
two-level lookup (group by `task_id`, then command by `cmd_id`) never needs
to change shape for this.

## 4. SYS group (`SIMFW_TASK_ID_SYS` = 1)

| `SIMFW_CMD_SYS_*` | Value | Status |
|---|---|---|
| `PING` | `0x01` | **implemented** |
| `GET_VERSION` | `0x02` | **implemented** |
| `RESET_SIM` | `0x03` | reserved (PLAN.md sec 5) |
| `SET_TIMESCALE` | `0x04` | reserved |
| `SET_SEED` | `0x05` | reserved |
| `GET_CAPS` | `0x06` | **implemented** |

### `PING` (request: `[0x01]`, no args)

Reply: `[status]` — no data, just confirms the round trip and that `cmd_task`
is alive and dispatching.

### `GET_VERSION` (request: `[0x02]`, no args)

Reply — the "version block" shared with `GET_CAPS` below:

```
byte0        status
byte1-2      BENCHPROTO_PROTOCOL_VERSION, u16 LE (benchproto_version.h)
byte3-4      BENCHPROTO_MIN_COMPATIBLE, u16 LE
byte5        SIMFW_FW_VERSION_MAJOR (version.h)
byte6        SIMFW_FW_VERSION_MINOR
byte7        SIMFW_FW_VERSION_PATCH
byte8        dirty flag (0=clean, 1=dirty/unknown) -- SIMFW_FW_GIT_DIRTY, build_info.h
byte9        git_hash_len (N)
N bytes      git commit, ASCII, not null-terminated -- SIMFW_FW_GIT_COMMIT, build_info.h
```

`build_info.h` is regenerated on every build (`gen_build_info.cmake`, wired
into `CMakeLists.txt` as an always-run custom target — not just on a CMake
reconfigure), same mechanism `KilnFW/App/drivers/gen_build_info.cmake` uses,
so it can never be stale relative to what's actually flashed. Always read
and compare `BENCHPROTO_PROTOCOL_VERSION` first, before trusting anything
else in the payload — same discipline `UART_PROTOCOL.md`'s own
`GET_FW_VERSION` section documents for its `UART_PROTOCOL_VERSION` field.

### `GET_CAPS` (request: `[0x06]`, no args)

Reply — the version block above, followed by capability fields (PLAN.md
section 5.1: "protocol version, SimFW version + git hash, zone count
limits, channel counts, and a feature bitmask"):

```
byte0..(9+N)   version block, identical layout to GET_VERSION above
byte(10+N)     zone_count_min      -- SIMFW_CAPS_ZONE_COUNT_MIN (1)
byte(11+N)     zone_count_max      -- SIMFW_CAPS_ZONE_COUNT_MAX (4)
byte(12+N)     zone_count_default  -- SIMFW_CAPS_ZONE_COUNT_DEFAULT (3), PLAN.md sec 4.3
byte(13+N)     tc_main_channels    -- SIMFW_CAPS_TC_MAIN_CHANNELS (3), spi_emu_a
byte(14+N)     tc_safety_channels  -- SIMFW_CAPS_TC_SAFETY_CHANNELS (1), spi_emu_b
byte(15+N)     ct_channels         -- SIMFW_CAPS_CT_CHANNELS (3), wave_owner
byte(16+N)     relay_channels      -- SIMFW_CAPS_RELAY_CHANNELS (3)
byte(17+N)..+3 feature_bitmask, u32 LE -- SIMFW_CAPS_FEATURE_BITMASK, 0 today
```

`feature_bitmask` is reserved (all zero) until MODEL/TC/CT/RELAY/IO/FAULT
gain real handlers — a future pass assigns one bit per group ("this group
has a real implementation, not just a stub") so `kilnsim` can refuse
gracefully against a mismatched or partially-built firmware, per PLAN.md
section 5.1's own reasoning for `GET_CAPS` existing at all. No bits are
assigned yet; do not assume bit 0 means SYS (SYS is always fully present by
construction — the link couldn't work otherwise).

### `RESET_SIM` / `SET_TIMESCALE` / `SET_SEED` (reserved)

Ids are allocated (`0x03`/`0x04`/`0x05`) so a future implementation doesn't
have to renumber anything else, but there is no handler yet — a request
today gets `[SIMFW_CMD_STATUS_ERR_NOT_IMPL]` back, same as any command in a
still-stub group.

## 5. MODEL / TC / CT / RELAY / IO / FAULT (reserved, not yet implemented)

PLAN.md section 5's table lists each group's planned commands
(`SET_ZONE_PARAMS`, `TC_GET_REGS`, `CT_SET_AMPS`, `RELAY_GET_EDGES`,
`IO_WRITE`, `FAULT_SCHEDULE`, ...) — none have a `SIMFW_CMD_*` id or a wire
layout defined in this file yet. That happens when each group's owning task
(`spi_emu_a/b`, `wave_owner`, `i2c_owner`, `fault_sched` — separate,
in-progress work) is far enough along to define real payloads against it.
Until then, every request to one of these five groups' task ids gets
`[SIMFW_CMD_STATUS_ERR_NOT_IMPL]` back (a real, delivered ACK — not a NACK,
not a timeout), so a `kilnsim` client can tell "this group exists on this
firmware build but isn't wired up yet" apart from "this firmware doesn't
know about this group at all" (which would NACK, since the task id itself
wouldn't be registered).

**Extending this file:** when a group's first real command lands, add its
`SIMFW_CMD_<GROUP>_*` ids to `cmd_ids.h`, its handler(s) + a `commands[]`
table entry to `cmd_task.c` (mirroring `s_sys_commands[]`'s shape), and
replace that group's row above with the same kind of "implemented" table
SYS has in section 4 — do not create a second protocol-reference document.

## 6. EVT group (`SIMFW_TASK_ID_EVT` = 8, reserved)

PLAN.md section 5.3 describes EVT as unsolicited BROADCAST traffic
(`{u32 seq, u64 sim_time_us, u8 event_type, payload}` for relay edges, fault
fired/cleared, threshold crossings, mode changes) driven by `telemetry.c`
draining the event ring — not a request/reply command group at all. It is
registered as a `benchproto` task id today only so a stray inbound frame
addressed to it gets a defined NACK (unroutable, since no request-style
handler exists) rather than silently vanishing; the actual outbound
BROADCAST path is `telemetry`'s own work, out of scope here.

## 7. Known simplifications (this pass)

- **Reply-in-ACK, one outstanding request at a time.** `usb_owner` enqueues
  a delivered `DATA` frame to `cmd_task`'s inbox and returns immediately
  (it does not block waiting for the reply) — `cmd_task` calls
  `usb_owner_send_reply()` once it has an answer, from its own task
  context. There is no per-request matching beyond `cmd_task`'s inbox being
  a plain FIFO: since every SYS handler today is a small, synchronous,
  non-blocking computation, replies are for all practical purposes
  processed in the order requests arrive. A future group with a
  slower/blocking handler must preserve that property (PLAN.md section
  4.5's "nothing ever blocks" rule extends to `cmd_task`'s own handlers) or
  this ordering assumption needs revisiting.
- **Duplicate-ACK cache is one slot per task id**, not per outstanding
  request. If a single task id ever needs more than one request in flight
  at a time (it doesn't today — the host's own single-outstanding-per-link
  discipline, BENCHPROTO.md section 3, already limits this in practice), a
  `BENCHPROTO_LINK_ACTION_DUPLICATE_REACK` could resend the wrong cached
  answer. Documented in `usb_owner.c`'s own comment on
  `USB_OWNER_ACK_CACHE_SLOTS`.
- **No inbound BROADCAST consumer.** `usb_owner_handle_deliver()` drops any
  BROADCAST frame delivered to a registered task (there is nothing to hand
  it to yet); only outbound BROADCAST (telemetry/EVT, PLAN.md sec 5.3, not
  built yet) is expected to matter in practice.
