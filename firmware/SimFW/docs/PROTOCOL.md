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
| `0x02` | `SIMFW_CMD_STATUS_ERR_BAD_ARGS` | args failed to decode (short/overflowed payload) or a target (zone/channel/expander/pin/slot) was out of range |
| `0x03` | `SIMFW_CMD_STATUS_ERR_INTERNAL` | dispatch reached an inconsistent state (should not happen — see `cmd_task.c`'s dispatch fallthrough comment) |
| `0x04` | `SIMFW_CMD_STATUS_ERR_BUSY` | args were well-formed and in range, but the owning task could not apply the command right now (its internal command queue was full, or an immediate action such as `FAULT_FIRE_NOW` could not complete) — added this pass (section 5), see `cmd_ids.h`'s own comment on the constant |

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
| MODEL | `SIMFW_TASK_ID_MODEL` | 2 | **implemented** (section 5.1) |
| TC | `SIMFW_TASK_ID_TC` | 3 | **implemented** (section 5.2), incl. `TC_GET_MASTER_CONFIG` |
| CT | `SIMFW_TASK_ID_CT` | 4 | **implemented** (section 5.3) |
| RELAY | `SIMFW_TASK_ID_RELAY` | 5 | **implemented** (section 5.4) |
| IO | `SIMFW_TASK_ID_IO` | 6 | **implemented** (section 5.5) |
| FAULT | `SIMFW_TASK_ID_FAULT` | 7 | **implemented** (section 5.6) |
| EVT | `SIMFW_TASK_ID_EVT` | 8 | **TELEMETRY + EVT broadcast implemented** (`telemetry.c`) — unsolicited BROADCAST only (PLAN.md sec 5.3), no inbound request handling exists or ever will |

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

`feature_bitmask` bits (added this pass, `cmd_ids.h`'s `SIMFW_CAPS_FEATURE_BIT_*`):
"this group has a real implementation, not just a stub", per PLAN.md
section 5.1's own reasoning for `GET_CAPS` existing at all — so `kilnsim`
can refuse gracefully against a mismatched or partially-built firmware.

| Bit | Group | Notes |
|---|---|---|
| 0 | MODEL | |
| 1 | TC | fully implemented, incl. `TC_GET_MASTER_CONFIG` |
| 2 | CT | |
| 3 | RELAY | |
| 4 | IO | |
| 5 | FAULT | |

SYS has no bit (always fully present by construction — the link couldn't
work otherwise) and neither does EVT (not a request/reply group, section 6).
Today's value is `0x0000003F` (all six bits set).

### `RESET_SIM` / `SET_TIMESCALE` / `SET_SEED` (reserved)

Ids are allocated (`0x03`/`0x04`/`0x05`) so a future implementation doesn't
have to renumber anything else, but there is no handler yet — a request
today gets `[SIMFW_CMD_STATUS_ERR_NOT_IMPL]` back, same as any command in a
still-stub group.

## 5. MODEL / TC / CT / RELAY / IO / FAULT

All six groups below call only their owning task's public API
(`sim_engine.h`, `fault_sched.h`, `i2c_owner.h`, `wave_owner.h`,
`spi_emu_a.h`/`spi_emu_b.h`) — never another task's private state (PLAN.md
section 4.5, `cmd_task.c`'s own file header). Every setter is a decode-then-
call wrapper over that owner's existing queue-then-apply-next-tick command
surface; nothing here adds a new peripheral access path.

### 5.1 MODEL group (`SIMFW_TASK_ID_MODEL` = 2) — `sim_engine.h`

| `SIMFW_CMD_MODEL_*` | Value | Owner call |
|---|---|---|
| `SET_ZONE_PARAMS` | `0x01` | `sim_engine_set_zone_params()` |
| `GET_ZONE_PARAMS` | `0x02` | `sim_engine_get_zone_params()` |
| `SET_AMBIENT` | `0x03` | `sim_engine_set_ambient()` |
| `LOAD_PRESET` | `0x04` | `sim_engine_load_preset()` |
| `SET_TEMP` | `0x05` | `sim_engine_force_zone_temp()` / `sim_engine_clear_zone_manual()` |
| `SET_TC_LAG` | `0x06` | get + set `sim_engine_*_zone_params()` (read-modify-write, see below) |

`SET_ZONE_PARAMS` request (args after the `0x01` cmd_id byte, all little-endian):

```
u8  zone
f32 C
f32 k_loss
f32 k_couple[THERMAL_MODEL_MAX_ZONES]  (4 entries; diag ignored, thermal_model.h)
f32 R_element
f32 element_health
f32 tc_lag_s
f32 T0
```

41 bytes of args. Reply: `[status]`. `GET_ZONE_PARAMS` request is `[zone]`;
reply is `[status, zone, <same 40 bytes as above>]`.

`SET_AMBIENT` request: `[f32 ambient_c]`. `LOAD_PRESET` request:
`[u8 preset]` (`thermal_preset_id_t`: 0 `fast_test`, 1 `small_kiln`,
2 `three_zone`, 3 `stress`, PLAN.md 4.3).

`SET_TEMP` request: `[u8 zone, u8 mode, f32 temp_c]` — `mode` 0 returns the
zone to MODEL (`sim_engine_clear_zone_manual()`, `temp_c` ignored), `mode` 1
forces MANUAL at `temp_c` (`sim_engine_force_zone_temp()`). One command id
covers both halves of `sim_engine.h`'s MANUAL-mode pair.

`SET_TC_LAG` request: `[u8 zone, f32 tc_lag_s]`. `sim_engine.h` has no
standalone "set just `tc_lag_s`" setter — it is one field of
`thermal_zone_params_t`, and the only writer replaces the whole struct — so
this handler does a get-then-set internally (still only calling
`sim_engine.h`'s own public API twice, not touching any private state). Not
an owner-API gap, just a convenience that saves a client round trip and a
TOCTOU window.

### 5.2 TC group (`SIMFW_TASK_ID_TC` = 3) — `spi_emu_a.h`/`spi_emu_b.h` + `sim_engine.h` + `fault_sched.h`

Channel numbering matches `tc_fault_state.h`'s `tc_fault_channel_t`: `0`..`2`
= `MAIN_0`..`MAIN_2` (`spi_emu_a`, ESP bus, index-matches zone 0..2), `3` =
`SAFETY` (`spi_emu_b`).

| `SIMFW_CMD_TC_*` | Value | Owner call | Status |
|---|---|---|---|
| `GET_REGS` | `0x01` | `spi_emu_a/b_get_stats()` + `spi_emu_a/b_get_reg_image()` + `sim_snapshot_read()` + `tc_fault_state_read()` | implemented |
| `FORCE_TEMP` | `0x02` | `sim_engine_force_zone_temp()` (MAIN) / `sim_engine_force_safety_temp()` (SAFETY) | implemented, all channels |
| `SET_MODE` | `0x03` | `sim_engine_force_zone_temp()` / `sim_engine_clear_zone_manual()` (MAIN); `sim_engine_force_safety_temp()` / `sim_engine_clear_safety_manual()` (SAFETY) | implemented, all channels |
| `INJECT_FAULT` | `0x04` | `fault_sched_schedule()` + `fault_sched_fire_now()` | implemented |
| `CLEAR_FAULT` | `0x05` | `fault_sched_cancel()` | implemented |
| `GET_MASTER_CONFIG` | `0x06` | `spi_emu_a/b_get_reg_image()` + `spi_emu_a/b_channel_configured()` | implemented |

**Register-image getters (`spi_emu_a.h`/`spi_emu_b.h`):** a previous pass
left `TC_GET_REGS`'s register-image half and all of `TC_GET_MASTER_CONFIG`
unreachable because neither header exported an accessor for a channel's live
`max31856_channel_t.regs[]` (only `*_get_stats()` for instrumentation
counters). Both headers now export:

- `spi_emu_a_get_reg_image(channel, out_regs[16])` /
  `spi_emu_b_get_reg_image(channel, out_regs[16])` — copies the channel's
  current 16-byte register image. **Coherency guarantee:** never blocks
  (PLAN.md 4.5) and never returns a torn image. It reads
  `max31856_pio_engine_channel_busy()` (already tracked by the PIO engine as
  "CS is low right now") immediately before *and* after the `memcpy`, plus
  the channel's transaction counter before/after, and only reports success
  if both checks agree nothing could have mutated `regs[]` mid-copy — a
  bounded (4 attempts), non-blocking, seqlock-style optimistic read, the same
  pattern the zone snapshot already uses elsewhere in this repo. Returns
  `false` (image left untouched) if the channel stayed busy across every
  retry; the caller must treat that as "ask again," never as a torn read.
- `spi_emu_a_channel_configured(channel)` / `spi_emu_b_channel_configured(channel)`
  — `true` once the master has ever written any byte to this channel (any
  register address, including a write attempt at a read-only one).
  Backed by a new `max31856_channel_t.master_has_written` field
  (`src/sim/max31856_regs.h`, set in `apply_write_rule()`): a monotonic,
  single-byte flag, so it needs none of the busy/retry machinery above —
  a torn read of one bool by another core is not possible the way a
  16-byte struct copy could be.

**`GET_REGS`** request: `[u8 channel]`. Reply:

```
byte0      status
byte1      channel (echo)
byte2      flags: bit0 reg_image_valid, bit1 snapshot_valid,
           bit2 stuck_ltcb, bit3 spurious_fault_pin, bit4 force_sr_open,
           bit5 force_sr_ovuv
byte3-18   reg_image[16] -- the channel's live register image (CR0..SR,
           MAX31856_REG_* addressing). Only meaningful when reg_image_valid
           (bit0) is set; all-zero otherwise.
byte19-22  f32 shadow_true_tc_c       -- uncorrupted thermal-model truth
byte23-26  f32 shadow_reported_tc_c   -- post-TC-lag, pre-corruption signal
byte27     dead_mode (max31856_dead_mode_t: 0 NONE, 1 ALL_ZERO, 2 ALL_ONE, 3 HIGH_Z)
byte28-31  f32 noise_sigma_c
byte32-35  f32 bit_error_rate
byte36-39  u32 spi transactions   (spi_emu_a/b_get_stats().transactions)
byte40-43  u32 spi protocol_errors
byte44-47  u32 spi first_byte_late (TX FIFO underrun count)
```

Together, `byte3-18` (when valid) and `byte19-26` are PLAN.md 5.2's full "the
DUT was lied to, this is the truth" pair: the register image the DUT
actually reads over SPI, plus the shadow truth it was never shown.

**`reg_image_valid` (flags bit0) semantics:** clear means the getter found
the channel mid-transaction (CS low) across its whole bounded retry budget —
`byte3-18` is all zero, and this is not an error, just a transient "the DUT
was mid-read/write when you asked," the same as a real SPI collision would
produce. A client should simply ask again; at the fixture's task-loop scan
cadence (~20 ms) and real SPI transaction durations (microseconds), a busy
result on a genuinely idle channel should be rare.

**`GET_MASTER_CONFIG`** request: `[u8 channel]`. Reply:

```
byte0  status
byte1  channel (echo)
byte2  flags: bit0 configured (spi_emu_a/b_channel_configured() --
       "has the master EVER written anything to this channel", not
       "is it configured correctly"), bit1 reg_image_valid (same
       busy/retry semantics as GET_REGS's identically-named bit)
byte3  CR0 -- 0 if reg_image_valid is clear
byte4  CR1 -- TC TYPE[3:0] is bits[3:0], AVGSEL[2:0] is bits[6:4]
       (PLAN.md 3.2's CR1 row); 0 if reg_image_valid is clear
byte5  MASK -- 0 if reg_image_valid is clear
```

`configured` (bit0) is always trustworthy regardless of `reg_image_valid`
(bit1) — it is backed by `master_has_written`, a plain monotonic flag with
no busy/retry gate of its own (see the getter description above), not by the
register image. A channel can therefore be reported `configured` (bit0 set,
from a *past* write) while the *current* request happens to land
mid-transaction (bit1 clear, `byte3-5` all zero) — callers must check bit1
before trusting `byte3-5`, but bit0 alone already answers "did the DUT ever
configure this channel at all," independent of whether this particular poll
caught it mid-transaction. This is exactly the "configured wrong" vs. "never
configured" distinction PLAN.md 5.2 calls out as "a different and equally
important failure" from a bad TC-type value: `configured=0` means never
configured; `configured=1` with `reg_image_valid=1` and a wrong CR1 means
configured wrong.

**`FORCE_TEMP`** request: `[u8 channel, f32 temp_c]`. For `channel` `0`..`2`
(MAIN), calls `sim_engine_force_zone_temp(channel, temp_c)`. For `channel`
`3` (SAFETY), calls `sim_engine_force_safety_temp(temp_c)` — the safety-side
MANUAL override, independent of every zone's own MAIN-side override
(`sim_engine.h`'s blend/lag+MANUAL safety-TC API). `channel >= 4` returns
`ERR_BAD_ARGS`.

**`SET_MODE`** request: `[u8 channel, u8 mode, f32 manual_temp_c]` — `mode`
0/1 same meaning as MODEL's `SET_TEMP`. For MAIN channels, `mode` 0 calls
`sim_engine_clear_zone_manual(channel)` and `mode` 1 calls
`sim_engine_force_zone_temp(channel, manual_temp_c)`. For the SAFETY channel,
`mode` 0 calls `sim_engine_clear_safety_manual()` and `mode` 1 calls
`sim_engine_force_safety_temp(manual_temp_c)`, returning the safety channel
to blend/lag-driven reporting or pinning it, without touching any zone's
MAIN-side override. `channel >= 4` or `mode` outside `{0, 1}` returns
`ERR_BAD_ARGS`.

A `main_safety_skew`-style scenario (PLAN.md section 8, test 8) can now pin
the SAFETY channel's reported temperature independently of every zone's own
truth/MAIN-reported values via either command above — the owner-API gap this
section used to document here is closed (`sim_engine.h`'s
`sim_engine_force_safety_temp()` / `sim_engine_clear_safety_manual()`).

**`INJECT_FAULT`** request: `[u16 slot_id, u8 channel, u8 fault_kind, f32 param0]`
— `fault_kind` is `fault_sched_fault_type_t`'s TC-only subset (`0`
`TC_DISCONNECTED` .. `5` `TC_SPURIOUS_FAULT_PIN`). Deliberately routes
through `fault_sched_schedule()` (`MANUAL` trigger, `PERMANENT` duration,
`ONCE` repeat) + `fault_sched_fire_now()` rather than writing
`tc_fault_state_write()` directly: `tc_fault_state.h`'s own header names
`fault_sched.c` as the state's **sole writer**, which "recomputes it from
scratch every tick from the active fault slot set" — a direct write from
`cmd_task` would be silently clobbered on `fault_sched`'s very next
evaluation pass. Reply: `[status, u16 slot_id]` (echoed so the client can
`TC_CLEAR_FAULT` it later). **`CLEAR_FAULT`** request: `[u16 slot_id]` →
`fault_sched_cancel()`.

### 5.3 CT group (`SIMFW_TASK_ID_CT` = 4) — `wave_owner.h`

Channel is `0..CT_WAVE_NUM_CHANNELS-1` (3).

| `SIMFW_CMD_CT_*` | Value | Owner call |
|---|---|---|
| `SET_MODE` | `0x01` | `ct_wave_set_mode()` |
| `SET_AMPS` | `0x02` | `ct_wave_set_amps()` |
| `SET_DISTORTION` | `0x03` | `ct_wave_set_distortion()` |
| `GET_STATE` | `0x04` | `ct_wave_get_state()` |
| `SET_PHASE` | `0x05` | `ct_wave_set_phase()` — not in PLAN.md 5's original sketch; wired up because `wave_owner.h` exposes it as a first-class public setter |

`SET_MODE` request: `[u8 channel, u8 mode]` (`ct_wave_mode_t`: 0 MODEL, 1
MANUAL). `SET_AMPS`: `[u8 channel, f32 amps]`. `SET_PHASE`:
`[u8 channel, f32 phase_deg]`. `SET_DISTORTION`:
`[u8 channel, f32 dc_offset, f32 clip_fraction, u8 dropout_half_cycle,
u8 dropout_negative_half, u8 apply_immediately]`. `GET_STATE` request:
`[u8 channel]`; reply:

```
byte0      status
byte1      mode (ct_wave_mode_t)
byte2-5    f32 amps
byte6-9    f32 phase_deg
byte10-13  f32 distortion.dc_offset
byte14-17  f32 distortion.clip_fraction
byte18     distortion.dropout_half_cycle (0/1)
byte19     distortion.dropout_negative_half (0/1)
byte20     distortion.apply_immediately (0/1)
byte21-24  f32 last_pwm_scale
byte25     valid (0/1)
```

### 5.4 RELAY group (`SIMFW_TASK_ID_RELAY` = 5) — `i2c_owner.h`

| `SIMFW_CMD_RELAY_*` | Value | Owner call |
|---|---|---|
| `GET_STATES` | `0x01` | `i2c_owner_get_relay_states()` |
| `GET_EDGES` | `0x02` | `i2c_owner_get_relay_edges()` |

`GET_STATES` request: none. Reply:
`[status, k1_closed, k2_closed, k3_closed, k5_closed, k4_closed,
fault_line_asserted, u64 sample_time_us, valid]` (each flag a 0/1 byte).

`GET_EDGES` request: `[u32 since_seq, u8 max_count]` — `max_count` is
clamped server-side to `SIMFW_RELAY_EDGES_MAX_PER_REPLY` (8, so a full reply
of `status + count + 8 * 14-byte entries` = 114 bytes always fits the
128-byte frame). Reply: `[status, u8 returned_count, returned_count *
{u32 seq, u8 signal, u8 level, u64 time_us}]` — `signal` is
`i2c_owner_signal_t` (0 K1, 1 K2, 2 K3, 3 K5, 4 K4, 5 FAULT_LINE). A client
wanting more than 8 polls again with `since_seq` set to the last entry's
`seq` (`i2c_owner.h`'s own documented pagination contract).

**`RELAY_SET_CONTACT_FAULT`** (PLAN.md section 5's sketch) is **not
allocated**: `i2c_owner.h` exposes no such setter by design (relay sense is
read-only from this task's perspective; a "welded contact" is modeled at
`sim_engine`'s duty-override level). The FAULT group's `WELDED_RELAY` /
`STUCK_OPEN_RELAY` types (`FAULT_SCHEDULE`, section 5.6) are the real path.

### 5.5 IO group (`SIMFW_TASK_ID_IO` = 6) — `i2c_owner.h`

| `SIMFW_CMD_IO_*` | Value | Owner call |
|---|---|---|
| `SET_DIR` | `0x01` | `i2c_owner_io_set_dir()` |
| `WRITE` | `0x02` | `i2c_owner_io_write()` |
| `READ` | `0x03` | `i2c_owner_io_read()` |
| `ESTOP_SET` | `0x04` | `i2c_owner_set_estop()` |
| `FAULT_LINE_GET` | `0x05` | `i2c_owner_get_relay_states()` (`.fault_line_asserted`) |
| `DUT_POWER_SET` | `0x06` | `i2c_owner_set_dut_power()` |
| `ESTOP_GET` | `0x07` | `i2c_owner_get_estop_open()` |
| `DUT_POWER_GET` | `0x08` | `i2c_owner_get_dut_power_on()` |

`DUT_POWER_SET` is PLAN.md section 3.4's addition (the DUT 12 V power relay
was added to the plan after section 5's original command table was written)
— placed in the IO group per this pass's instructions. `ESTOP_GET`/
`DUT_POWER_GET` are not in PLAN.md 5's original sketch either, but
`i2c_owner.h` exposes both getters as first-class public API and a client
otherwise has no way to read back what it last commanded, so they are wired
up too.

`SET_DIR` request: `[u8 exp, u8 pin, u8 input, u8 pullup]` (`exp`: 0 =
`I2C_OWNER_EXP_1` (0x20), 1 = `I2C_OWNER_EXP_2` (0x21); `pin` 0..15, 0..7 =
port A, 8..15 = port B). `i2c_owner_io_set_dir()` returns `false` both for a
reserved fixed-role exp1 pin (relay sense / fault-line sense / E-stop drive /
DUT-power relay, pins 0..7 on `I2C_OWNER_EXP_1`) and for a transiently full
command queue — indistinguishable from the bool alone, so this handler
reports `ERR_BAD_ARGS` for both (the reserved-pin case is the far more likely
cause for a well-behaved client). `WRITE`: `[u8 exp, u8 pin, u8 level]`.
`READ`: `[u8 exp, u8 pin]`, reply `[status, level]`.

`ESTOP_SET`: `[u8 open]` (1 = loop opened/tripped). `ESTOP_GET`: no args,
reply `[status, open]`. `FAULT_LINE_GET`: no args, reply
`[status, asserted, u64 sample_time_us, valid]`. `DUT_POWER_SET`: `[u8 on]`.
`DUT_POWER_GET`: no args, reply `[status, on]`.

### 5.6 FAULT group (`SIMFW_TASK_ID_FAULT` = 7) — `fault_sched.h`

| `SIMFW_CMD_FAULT_*` | Value | Owner call |
|---|---|---|
| `SCHEDULE` | `0x01` | `fault_sched_schedule()` |
| `CANCEL` | `0x02` | `fault_sched_cancel()` |
| `LIST` | `0x03` | `fault_sched_list()` |
| `FIRE_NOW` | `0x04` | `fault_sched_fire_now()` |

**`SCHEDULE`** request is a compact re-encoding of `fault_engine.h`'s
`fault_trigger_t`/`fault_duration_t`/`fault_repeat_t` — **not** PLAN.md
5.2's original `{u8 kind, f32 a, f32 b, u8 zone/relay}` sketch, which
predates `fault_engine.h` and cannot address an `AFTER_FAULT` slot id (needs
16 bits, PLAN.md's sketch only offers 8) or carry `ON_EVENT`'s name string
at all:

```
u16      slot_id
u8       fault_type        (fault_sched_fault_type_t, 0..12)
u16      target             (zone / tc_fault_channel_t / system-target,
                             per fault_type -- fault_sched_schedule() itself
                             validates the pairing)
-- trigger (fault_trigger_kind_t, 0..6) --
u8       trigger_kind
f64      trigger_a          (AT_SIM_TIME: at_sim_time_s; AT_ZONE_TEMP: temp_c;
                             ON_RELAY_EDGE/ON_EVENT/AFTER_FAULT: delay_s;
                             RANDOM_IN: random_t0_s; else unused)
f64      trigger_b          (RANDOM_IN: random_t1_s; else unused)
u16      trigger_ref         (AT_ZONE_TEMP: zone; ON_RELAY_EDGE: relay;
                              AFTER_FAULT: after_fault_slot; else unused)
u8       trigger_edge         (AT_ZONE_TEMP: temp_edge 0=rising/1=falling;
                               ON_RELAY_EDGE: relay_edge 0=close/1=open; else 0)
char[24] event_name           (ON_EVENT only, NUL-padded ASCII; else ignored)
-- duration (fault_duration_kind_t) --
u8       duration_kind         (0 PERMANENT, 1 FOR -- 2 UNTIL_TRIGGER
                                REJECTED, see gap below)
f64      duration_for_s
-- repeat (fault_repeat_kind_t, 0..2) --
u8       repeat_kind
f64      repeat_period_s
f64      repeat_jitter_s
u16      repeat_n
-- params --
f32      param0, param1, param2, param3
```

94 bytes total (incl. the `0x01` cmd_id byte), comfortably under the
128-byte frame payload cap. Reply: `[status, u16 slot_id echo]`.

**GAP (wire-frame-size, not an owner-API gap):** PLAN.md section 7.2 lists
`UNTIL_TRIGGER` as a required duration kind, but it needs a second, full
nested `fault_trigger_t` — the same ~90-byte shape as the top-level trigger
above — which does not fit alongside everything else within
`BENCHPROTO_FRAME_MAX_PAYLOAD` (128 bytes). `duration_kind == 2` is rejected
with `ERR_BAD_ARGS`. Workaround: use `FOR` a generous duration, or arm a
`MANUAL`-triggered fault and `FAULT_CANCEL` it explicitly once a PC-side
scenario runner observes the clearing condition.

**`CANCEL`** request: `[u16 slot_id]` → `fault_sched_cancel()`. **`FIRE_NOW`**
request: `[u16 slot_id]` → `fault_sched_fire_now()`.

**`LIST`** request: `[u8 start_index, u8 max_count]` — pagination over the
fixed 32-slot pool (`fault_sched_list()` itself always returns from index 0);
`max_count` clamped to `SIMFW_FAULT_LIST_MAX_PER_REPLY` (8). Reply:
`[status, u8 returned_count, returned_count * {u16 slot_id, u8 state,
u16 fault_type, u16 target, u32 fire_count, f32 active_since_s}]` — `state`
is `fault_slot_state_t` (0 IDLE, 1 ARMED, 2 ACTIVE, 3 EXPIRED).

**Extending this file:** the same discipline section 4's closing note asked
of SYS applies here — when a new command id lands, add it to `cmd_ids.h`,
its handler + `commands[]` entry to `cmd_task.c`, and update the relevant
subsection above in the same commit.

## 6. EVT group (`SIMFW_TASK_ID_EVT` = 8) — TELEMETRY + EVT broadcast frames

PLAN.md section 5.3 describes two unsolicited frame shapes — a periodic
TELEMETRY frame and a per-event EVT frame — both driven by `telemetry.c`
(this pass's real body), both sent as `BENCHPROTO_MSG_BROADCAST` frames
(never ACKed, NACKed, or deduped, BENCHPROTO.md sec 4) with `src_task =
SIMFW_TASK_ID_EVT`, `dst_device = SIMFW_DEVICE_HOST`, `dst_task = 0` (no
specific registered receiver task for unsolicited traffic — see
`usb_owner_send_broadcast()`'s own doc comment, `usb_owner.h`). This is not
a request/reply command group and never will be: `SIMFW_TASK_ID_EVT` is
still registered as a `benchproto` task purely so a stray inbound frame
addressed to it gets a defined NACK (unroutable) rather than silently
vanishing.

Both frame shapes share one source task id and are told apart by **byte 0
of the payload**, `SIMFW_EVT_FRAME_KIND_*` (`cmd_ids.h`):

### TELEMETRY frame (`SIMFW_EVT_FRAME_KIND_TELEMETRY` = `0x01`)

Periodic, default `TELEMETRY_DEFAULT_RATE_HZ` = 2 Hz, rate settable via
`telemetry_set_rate_hz()` (`telemetry.h`) — no `SIMFW_CMD_*` sets it over
the wire yet (a future SYS or MODEL command calls that function once one
exists, per PLAN.md sec 5.3 "rate settable"). All multi-byte fields
little-endian (section 2's convention), `zone_count` copied verbatim from
`sim_snapshot_t` (`src/sim/sim_snapshot.h`, 1–`SIM_SNAPSHOT_MAX_ZONES`):

```
byte0        frame_kind = SIMFW_EVT_FRAME_KIND_TELEMETRY
byte1-8      sim_time_us, u64 LE          -- sim_snapshot_t.sim_time_us
byte9-12     timescale_x100, u32 LE       -- sim_snapshot_t.timescale_x100
byte13-16    seed, u32 LE                 -- always 0 today, see "Known gaps" below
byte17       zone_count, u8               -- sim_snapshot_t.zone_count
byte18..+16N per-zone, N = zone_count, 16 bytes each:
               f32 LE T_true_c
               f32 LE T_tc_reported_c
               f32 LE T_safety_reported_c
               f32 LE I_amps (current_a)
next+0..1    relay_mask, u16 LE           -- sim_snapshot_t.relay_mask
next+2       estop_open, u8 (0/1)         -- sim_snapshot_t.estop_open
next+3       fault_line_asserted, u8 (0/1)-- i2c_owner_get_relay_states().fault_line_asserted
next+4..5    active_fault_count, u16 LE   -- count of fault_sched_list() slots in FAULT_STATE_ACTIVE
next+6..9    spi_transactions_total, u32 LE -- sum of spi_emu_a/b_get_stats().transactions, all 4 channels
next+10..13  spi_underruns_total, u32 LE    -- sum of ...get_stats().first_byte_late, all 4 channels
next+14..17  evt_ring_high_water_mark, u32 LE -- telemetry_get_evt_ring_high_water_mark()
next+18..21  evt_seq_gap_count, u32 LE       -- telemetry_get_evt_seq_gap_count()
next+22..25  evt_send_drop_count, u32 LE     -- telemetry_get_send_drop_count()
```

Fixed overhead (everything but the per-zone blocks) is 44 bytes; at
`SIM_SNAPSHOT_MAX_ZONES` = 4 the frame is 108 bytes, comfortably under
`BENCHPROTO_FRAME_MAX_PAYLOAD` (128).

### EVT frame (`SIMFW_EVT_FRAME_KIND_EVENT` = `0x02`)

One frame per `sim_event_ring_drain()` entry (`src/sim/sim_snapshot.h`),
forwarded promptly — drained on every pass of `telemetry`'s internal poll
loop (`TELEMETRY_EVT_POLL_MS`, `telemetry.c`), independent of and much
faster than the TELEMETRY frame's own rate, so an event is never held back
behind a slow-rate telemetry period (PLAN.md sec 5.3: "unsolicited,
immediate"):

```
byte0        frame_kind = SIMFW_EVT_FRAME_KIND_EVENT
byte1-4      seq, u32 LE          -- sim_event_t.seq
byte5-12     sim_time_us, u64 LE  -- sim_event_t.sim_time_us
byte13       event_type, u8       -- sim_event_t.type verbatim (sim_event_type_t,
                                      sim_snapshot.h's own 1:1 wire-mapping guarantee)
byte14       a, u8                -- sim_event_t.a
byte15       b, u8                -- sim_event_t.b
byte16-19    f0, f32 LE           -- sim_event_t.f0
```

20 bytes total, fixed size.

### Loss visibility (PLAN.md sec 5.3: "the PC's report generator refuses to
certify a run with a sequence gap")

Every EVT frame's own `seq` is enough for a PC client to detect loss on its
own (a gap between consecutive received `seq` values) — the TELEMETRY
frame's `evt_seq_gap_count` and `evt_send_drop_count` fields above exist so
that visibility does not depend on a client reconstructing it from the EVT
stream alone:

- `evt_seq_gap_count` increments whenever `sim_event_ring_drain()` itself
  reports the ring wrapped past what `telemetry` last drained (the ring
  outran the drain loop) — a real, unrecoverable loss of event data.
- `evt_send_drop_count` increments whenever `usb_owner_send_broadcast()`
  fails for a TELEMETRY or EVT frame (CDC TX path busy/short) — the event
  data existed and was built into a frame, but the frame itself never made
  it onto the wire.

### Known gaps (report, not worked around — see this pass's own
instructions on sim_engine.h/fault_sched.h getters)

- **`seed` is always 0.** `sim_engine.h` now exposes `sim_engine_get_seed()`
  (added alongside the safety-TC MANUAL override this pass closed in section
  5.2), so the getter-side of this gap is closed, but `telemetry.c` — the
  file that builds this frame — has not been wired to call it yet, and is
  out of scope for this pass. Still harmless today since no
  `SIMFW_CMD_SYS_SET_SEED` handler exists either (section 4's reserved-ids
  table) — nothing can set a non-zero seed for this field to report
  regardless. Wire `sim_engine_get_seed()` into `telemetry.c` in the same
  pass that implements `SET_SEED`.

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
  it to yet); outbound BROADCAST (TELEMETRY/EVT, section 6, `telemetry.c`)
  is the only BROADCAST traffic this build actually sends or expects.
