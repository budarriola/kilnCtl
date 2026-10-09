# CommonFW — shared control-interface code

> **Status:** planning · **Last reviewed:** 2026-08-24
> **Keep this file current.** If you change anything this document describes,
> update it in the same commit. If it disagrees with the code, **the code
> wins** — fix this file and say so in the commit message.

The wire between the ESP32-S3 main controller ([`../KilnFW`](../KilnFW)) and
the RP2040 safety processor ([`../SaftyFW`](../SaftyFW)), implemented **once**
and linked into both.

`CommonFW` also hosts a second, independent protocol family here:
**`benchproto`** ([`docs/BENCHPROTO.md`](docs/BENCHPROTO.md),
`include/benchproto/`, `src/benchproto_*.c`) — a hardened, addressed
request/reply protocol (framing, CRC16, retry/dedup, task registration) for
bench/instrument firmwares talking to a PC, extracted from
`firmware/UnitTestFw`'s UART prototype for SimFW's USB-CDC link (SimFW
was removed 2026-08-28, `UnitTestFw` restored in its place; the design
rationale that lived in SimFW's `docs/DESIGN_NOTES.md` sec 4.4/12 went with
it). It shares no code with `kilnlink`
— see `docs/BENCHPROTO.md` sec 1 for why the two look alike but are kept
apart — and is host-tested the same way (`test_benchproto_frame.c`,
`test_benchproto_link.c`, both wired into this file's `CMakeLists.txt`).

**Status: the framing layer exists and is host-tested.** `kilnlink_frame.{c,h}`
+ `kilnlink_crc.c` are written, byte-exact cross-checked against
`tools/PcTools/src/kilnctrl/protocol.py` (the second, already-proven
implementation of this exact envelope) via `test/vectors/frame_vectors.json`,
consumed by both `test/test_frame.c` (built and passing with MSVC via CMake +
Ninja, verified 2026-08-16) and `tools/PcTools/selfcheck.py`. **Framing is now
integrated into both firmwares** (as of 2026-08-18): `KilnFW`'s
`firmware/hwAbstraction/esp/uart/uart_protocol.c` calls `kilnlink_crc16_ccitt_false`/
`kilnlink_stuff` instead of its own copies, proven byte-identical against the
pre-migration implementation (`test/test_uart_protocol_delegate.c`); `SaftyFW`
now has a real CMake project (`firmware/SaftyFW/CMakeLists.txt`,
`add_subdirectory(../CommonFW kilnlink)`) linking `kilnlink` into all three of
its executables, and `link_task.c` calls into several of the payload codecs
directly (see below). Payload-codec migration is still partial — see
"2026-08-19" below and the Completion checklist's Migration section.

**2026-08-19: every command/frame `docs/LINK_PROTOCOL.md` sections 4 and 6 give
a concrete byte layout for now has a codec.** ESP→Pico (sec 4):
`kilnlink_context.{c,h}` (`SAFETY_CMD_PUSH_CONTEXT` 0x07, including
`relay_recent_mask` and the per-zone block), `kilnlink_announce.{c,h}`
(`SAFETY_CMD_ANNOUNCE_VERSION` 0x0F, the compatibility-floor layout),
`kilnlink_ceiling.{c,h}` (`SAFETY_CMD_SET_FIRING_CEILING` 0x09),
`kilnlink_clear_trip.{c,h}` (`SAFETY_CMD_CLEAR_TRIP` 0x0A),
`kilnlink_get_fw_version.{c,h}` (`SAFETY_CMD_GET_FW_VERSION` 0x0B, one byte,
no fields), `kilnlink_set_clock.{c,h}` (`SAFETY_CMD_SET_CLOCK` 0x0C, optional).
Pico→ESP (sec 6): `kilnlink_status.{c,h}` (Frame A, `SAFETY_CMD_GET_STATUS`
0x01, the existing 23-byte layout), `kilnlink_diag.{c,h}` (Frame B,
`SAFETY_CMD_DIAG` 0x08), `kilnlink_trip.{c,h}` (Frame D,
`SAFETY_CMD_TRIP_EVENT` 0x0D), `kilnlink_power.{c,h}` (Frame E,
`SAFETY_CMD_POWER` 0x0E). All ten are host-tested (one `test_<name>.c` each,
MSVC+CMake+Ninja, all passing as of 2026-08-19 -- 13 host test binaries
total including `test_frame`/`test_fuzz`/`test_uart_protocol_delegate`) with
byte-exact vectors in `test/vectors/`, and as of 2026-08-19 every one of
those vector manifests is also consumed by `tools/PcTools/selfcheck.py`
(`commonfw_payload_vector_checks()`, against the new pure-Python encoder/decoder pair that lives there
(`kilnctrl/kilnlink_codec.py`) -- not just `frame_vectors.json` as before.
`kilnlink_codec.py` is encode-only: nothing in `pc_tools` decodes these
frames yet, so there is no real caller to justify a decode side (see that
module's own docstring).

**2026-08-20: three more codecs close the CT-calibration gap** `firmware/
SimFW/tools/ct_calibration/README.md` documented: `kilnlink_set_ct_cal.{c,h}`
(`SAFETY_CMD_SET_CT_CAL` 0x19, ESP→Pico, sets one channel's CT amps
gain/offset), `kilnlink_get_ct_cal.{c,h}` (`SAFETY_CMD_GET_CT_CAL` 0x1A,
ESP→Pico, one byte, no fields) and `kilnlink_ct_cal.{c,h}` (`SAFETY_CMD_
CT_CAL`, same 0x1A id as the request, Pico→ESP reply, 28 bytes, all three
channels). Host-tested (`test_set_ct_cal.c`/`test_get_ct_cal.c`/
`test_ct_cal.c`, hand-computed byte-exact vectors inline rather than in
`test/vectors/` -- no `tools/PcTools/selfcheck.py` cross-check exists yet
for these three; that PC-side integration, and the PC-side sender for
`SET_CT_CAL` itself, are still open. The consumer is `firmware/SaftyFW/src/
tasks/link_task.c` (`link_task_handle_set_ct_cal()`/`_get_ct_cal()`/
`send_ct_cal()`) against `firmware/SaftyFW/src/config_store.{c,h}`'s `ct_cal`
record and `firmware/SaftyFW/src/ct_amps_cal.{c,h}`'s pure apply logic.

**2026-08-21: five codecs for `docs/COMMISSIONING.md` sec 2's commissioning
family (ids 0x1B-0x1F).** Codecs and host tests only -- no consumer wiring in
either firmware yet, same as `SET_CT_CAL` when it landed.
`kilnlink_set_log_level.{c,h}` (`SAFETY_CMD_SET_LOG_LEVEL` 0x1B, ESP→Pico, 2
bytes, opaque `LOG_LEVEL_*` byte). `kilnlink_set_param.{c,h}`
(`SAFETY_CMD_SET_PARAM` 0x1C, ESP→Pico, `param_id` u16 + type tag + a
variable-length value -- stages only, nothing reaches flash from this frame
alone). `kilnlink_commit_config.{c,h}` (`SAFETY_CMD_COMMIT_CONFIG` 0x1D,
ESP→Pico, one byte, no fields -- validates the staged set as a whole and
writes it; that validation is `SaftyFW`'s, not this codec's).
`kilnlink_get_param.{c,h}` / `kilnlink_param.{c,h}` (`SAFETY_CMD_GET_PARAM`/
`PARAM`, both 0x1E, request/reply sharing an id per the `GET_CT_CAL`/`CT_CAL`
convention; the reply carries an explicit `found` byte so an id one side
doesn't recognise is named in the reply rather than failing the whole
exchange). `kilnlink_get_config_page.{c,h}` / `kilnlink_config_page.{c,h}`
(`SAFETY_CMD_GET_CONFIG_PAGE`/`CONFIG_PAGE`, both 0x1F, bulk read of packed
`(id, type, value)` triples, one page per frame).

**2026-08-24: the three shared request/reply ids above were split apart**
(`KILNLINK_PROTOCOL_VERSION` 6 -> 7): `GET_CT_CAL` moved to `0x22`,
`GET_PARAM` to `0x23`, `GET_CONFIG_PAGE` to `0x24` -- their replies
(`CT_CAL`/`PARAM`/`CONFIG_PAGE`) keep `0x1A`/`0x1E`/`0x1F` unchanged. A
shared id structurally blocks a length-different refusal reply (neither a
bare request nor the fixed-size success reply), which is exactly what was
blocking `firmware/KilnFW/App/drivers/bridge/uart_bridge.c`'s SAFETY task from ever
telling the PC "driver error" for `GET_CT_CAL` -- see
`docs/LINK_PROTOCOL.md`'s "Request/reply ids must never be shared" rule for
the reusable lesson. `GET_FW_VERSION`/Frame C still shares its id
deliberately -- it sits in the frozen `0x00`-`0x0F` floor, where a refusal
is never needed.

All five share a new type-tag helper, `kilnlink_param_value.{c,h}`
(`KILNLINK_PARAM_TYPE_BOOL/U8/U16/F32`, 1/1/2/4 bytes) -- a small closed tag
set rather than a length-prefixed blob, so a value whose declared type this
build doesn't recognise is a decode `ERR_BAD_TYPE`, never a guess at how many
bytes follow. `kilnlink_config_page_pack()` greedily packs `(id, value)`
pairs into a page until the next one would not fit in the 253-byte payload
cap, then reports `more = 1` if entries remain -- the caller re-invokes with
the leftover slice and the next page index. Host-tested (`test_set_log_
level.c`, `test_param_value.c`, `test_set_param.c`, `test_commit_config.c`,
`test_get_param.c`, `test_param.c`, `test_get_config_page.c`,
`test_config_page.c`): round trips for every type tag, byte-exact vectors,
and the hostile-input set (short/over-long/truncated-mid-value/bad-type-tag
frames, plus `CONFIG_PAGE`'s entry_count-too-large and trailing-garbage
cases and length sweeps that assert no other length ever decodes OK). No
`tools/PcTools/selfcheck.py` cross-check exists yet for these five, same gap
`SET_CT_CAL`'s entry above notes.

**Wiring status, same day (2026-08-19):** most of
these codecs are still codec-only, not called from either firmware's real
send/receive dispatch -- see Migration below -- but two are now genuine
exceptions. `ANNOUNCE_VERSION` (0x0F) is wired on both ends:
`SaftyFW`'s `link_task.c` decodes inbound frames via
`kilnlink_announce_decode()`, and `KilnFW`'s `safety_link.c` builds outbound
ones via `kilnlink_announce_encode()` (its `components/kilnlink/CMakeLists.txt`
now compiles `kilnlink_announce.c` to make that possible -- still a subset of
what `SaftyFW`'s CMake links, see the ESP-IDF component wrapper item below).
`CLEAR_TRIP` (0x0A) is wired on both ends: `SaftyFW`'s
`link_task.c` decodes it via `kilnlink_clear_trip_decode()` and enforces the
refusal rules `docs/LINK_PROTOCOL.md` section 4 describes (refuses if nothing is
currently tripped, refuses if the wire `trip_mask` doesn't match the
latched one); `KilnFW`'s `safety_link_commands.c` builds it via
`kilnlink_clear_trip_encode()`.

## Why this exists

The link is a contract between two independently-built firmwares. Implemented
twice, it drifts — and it drifts silently, because a framing or field-offset
mismatch does not fail loudly, it delivers a plausible wrong number.

This project already has evidence of exactly that failure mode: the isolated
link's wire direction was documented backwards in `firmware/KilnFW/docs/SAFETY_LINK.md`
for months, because the description lived inside one of the two implementations
and read as an implementation note rather than as a contract with a second party
(`firmware/SaftyFW/docs/HARDWARE.md` §1).

So: **one definition of the frame layouts, one encoder, one decoder, one set of
test vectors, and the protocol document lives here** rather than inside either
firmware.

## Layout

```
firmware/CommonFW/
├─ README.md                       ← this file
├─ docs/
│  ├─ LINK_PROTOCOL.md             ← THE kilnlink contract. Owned here, not by either firmware
│  └─ BENCHPROTO.md                ← THE benchproto spec (separate protocol family, see "Layout" below)
├─ include/benchproto/
│  ├─ benchproto_version.h         ← BENCHPROTO_PROTOCOL_VERSION, the single source
│  ├─ benchproto_frame.h           ← delimiter/stuffing/CRC framing (independent of kilnlink_frame.h)
│  └─ benchproto_link.h            ← retry/dedup + addressable-task-registration model
├─ include/kilnlink/
│  ├─ kilnlink_version.h           ← KILNLINK_PROTOCOL_VERSION, the single source
│  ├─ kilnlink_bytes.h             ← shared LE encode/decode helpers
│  ├─ kilnlink_frame.h             ← delimiter/stuffing/CRC framing
│  ├─ kilnlink_context.h           ← ESP → Pico, SAFETY_CMD_PUSH_CONTEXT (0x07)
│  ├─ kilnlink_announce.h          ← ESP → Pico, SAFETY_CMD_ANNOUNCE_VERSION (0x0F)
│  ├─ kilnlink_ceiling.h           ← ESP → Pico, SAFETY_CMD_SET_FIRING_CEILING (0x09)
│  ├─ kilnlink_clear_trip.h        ← ESP → Pico, SAFETY_CMD_CLEAR_TRIP (0x0A)
│  ├─ kilnlink_get_fw_version.h    ← ESP → Pico, SAFETY_CMD_GET_FW_VERSION (0x0B)
│  ├─ kilnlink_set_clock.h         ← ESP → Pico, SAFETY_CMD_SET_CLOCK (0x0C)
│  ├─ kilnlink_status.h            ← Pico → ESP, Frame A, SAFETY_CMD_GET_STATUS (0x01)
│  ├─ kilnlink_diag.h              ← Pico → ESP, Frame B, SAFETY_CMD_DIAG (0x08)
│  ├─ kilnlink_trip.h              ← Pico → ESP, Frame D, SAFETY_CMD_TRIP_EVENT (0x0D)
│  ├─ kilnlink_power.h             ← Pico → ESP, Frame E, SAFETY_CMD_POWER (0x0E)
│  ├─ kilnlink_set_config.h        ← ESP → Pico, SAFETY_CMD_SET_CONFIG (0x16)
│  ├─ kilnlink_set_ct_cal.h        ← ESP → Pico, SAFETY_CMD_SET_CT_CAL (0x19)
│  ├─ kilnlink_get_ct_cal.h        ← ESP → Pico, SAFETY_CMD_GET_CT_CAL (0x22, own id since v7)
│  ├─ kilnlink_ct_cal.h            ← Pico → ESP, SAFETY_CMD_CT_CAL (0x1A, reply)
│  ├─ kilnlink_set_log_level.h     ← ESP → Pico, SAFETY_CMD_SET_LOG_LEVEL (0x1B)
│  ├─ kilnlink_param_value.h       ← shared type-tag + value codec (BOOL/U8/U16/F32)
│  ├─ kilnlink_set_param.h         ← ESP → Pico, SAFETY_CMD_SET_PARAM (0x1C)
│  ├─ kilnlink_commit_config.h     ← ESP → Pico, SAFETY_CMD_COMMIT_CONFIG (0x1D)
│  ├─ kilnlink_get_param.h         ← ESP → Pico, SAFETY_CMD_GET_PARAM (0x23, own id since v7)
│  ├─ kilnlink_param.h             ← Pico → ESP, SAFETY_CMD_PARAM (0x1E, reply)
│  ├─ kilnlink_get_config_page.h   ← ESP → Pico, SAFETY_CMD_GET_CONFIG_PAGE (0x24, own id since v7)
│  ├─ kilnlink_config_page.h       ← Pico → ESP, SAFETY_CMD_CONFIG_PAGE (0x1F, reply)
│  └─ kilnlink_fw_version.h        ← Pico → ESP, Frame C, SAFETY_CMD_FW_VERSION (0x0B, reply)
│  (no `kilnlink_ids.h` or `kilnlink_port.h` yet — see the Completion checklist)
├─ src/
│  ├─ kilnlink_crc.c    kilnlink_frame.c     kilnlink_context.c
│  ├─ kilnlink_announce.c  kilnlink_ceiling.c  kilnlink_clear_trip.c
│  ├─ kilnlink_get_fw_version.c  kilnlink_set_clock.c
│  ├─ kilnlink_status.c  kilnlink_diag.c  kilnlink_trip.c  kilnlink_power.c
│  ├─ kilnlink_set_config.c  kilnlink_set_ct_cal.c  kilnlink_get_ct_cal.c  kilnlink_ct_cal.c
│  └─ kilnlink_set_log_level.c  kilnlink_param_value.c  kilnlink_set_param.c
│     kilnlink_commit_config.c  kilnlink_get_param.c  kilnlink_param.c
│     kilnlink_get_config_page.c  kilnlink_config_page.c  kilnlink_fw_version.c
├─ src/benchproto_crc.c  src/benchproto_frame.c  src/benchproto_link.c
├─ test/
│  ├─ test_frame.c  test_fuzz.c  test_uart_protocol_delegate.c
│  ├─ test_context.c  test_announce.c  test_ceiling.c  test_clear_trip.c
│  ├─ test_get_fw_version.c  test_set_clock.c
│  ├─ test_status.c  test_diag.c  test_trip.c  test_power.c
│  ├─ test_set_config.c  test_set_ct_cal.c  test_get_ct_cal.c  test_ct_cal.c
│  ├─ test_set_log_level.c  test_param_value.c  test_set_param.c  test_commit_config.c
│  ├─ test_get_param.c  test_param.c  test_get_config_page.c  test_config_page.c
│  ├─ test_fw_version.c
│  ├─ test_benchproto_frame.c  test_benchproto_link.c
│  └─ vectors/                     ← shared byte-exact test vectors, see below
│     (kilnlink's `*_vectors.json` plus benchproto_frame_vectors.json)
└─ CMakeLists.txt                  ← builds both the `kilnlink` and `benchproto` targets
```

## Rules for code in here

These are what make the same source usable from ESP-IDF, pico-sdk and a host
test binary. **A violation of any of them is a build break, not a style note.**

1. **Freestanding C11.** `stdint.h`, `stdbool.h`, `stddef.h`, `string.h`. Nothing
   else — no ESP-IDF, no pico-sdk, no FreeRTOS, no POSIX.
2. **No allocation, ever.** Every function writes into a caller-supplied buffer
   with a caller-supplied length.
3. **No I/O and no time.** Encoders and decoders are pure functions. Timestamps
   arrive as arguments. This is what makes them host-testable and what keeps the
   safety processor's guard path free of anything that can block.
4. **No global mutable state.** Every function is reentrant; both firmwares call
   these from more than one task.
5. **Explicit byte packing.** No `__attribute__((packed))` structs cast onto the
   wire, no `memcpy` of a struct, no bitfields. Serialize field by field, little
   endian, by hand. Both targets happen to be little-endian; relying on that
   makes the host tests unable to catch a layout mistake.
6. **Every decoder is bounds-checked and returns a status.** A payload is
   untrusted input from another processor across an isolated link. A decoder
   that can be made to read past its buffer by a corrupt length byte is a bug in
   the component that must not have bugs.
7. **`-Wall -Wextra -Werror`, and it must compile clean under all three
   toolchains** — xtensa-gcc, arm-none-eabi-gcc, MSVC.
8. **No `assert()` that survives into release.** A decoder returns an error; it
   does not abort the safety processor.

## What is *not* shared

| Stays in `KilnFW` | Why |
|---|---|
| `uart_owner.c` | ESP-IDF UART driver, ring buffers, event task |
| `uart_protocol.c`'s transport half | ACK/NACK, retry, dedup, task registry, FreeRTOS queues. The Pico deliberately has none of this (`docs/LINK_PROTOCOL.md` §2) |
| `uart_task_ids.h`'s PC-link half | THERMO/IO/DISPLAY/CONTROL/PROFILES/AUTOTUNE/WIFI are ESP↔PC only and never cross the barrier |
| `uart_log_bridge.c` | ESP-only |

| Stays in `SaftyFW` | Why |
|---|---|
| `uart_owner.c` | pico-sdk UART, non-blocking TX ring |
| `link_task.c` | Task plumbing, snapshot publication |
| Everything in `safety_guards.c` | Policy, not protocol |

**The split is transport vs. contract.** Bytes-on-a-wire and how they are moved
are each firmware's own problem; what the bytes *mean* is shared.

## Integration

### `SaftyFW` (pico-sdk + CMake)

```cmake
add_subdirectory(../CommonFW kilnlink)
target_link_libraries(SaftyFW PRIVATE kilnlink)
```

### `KilnFW` (ESP-IDF component)

Add a thin `components/kilnlink/CMakeLists.txt` wrapper:

```cmake
idf_component_register(SRCS ${KILNLINK_SRCS}
                       INCLUDE_DIRS ${KILNLINK_DIR}/include)
```

pointing at `../../CommonFW`. **`uart_protocol.c` then deletes its own CRC and
byte-stuffing and calls `kilnlink_frame_*` instead.** This is a refactor of
working, hardware-verified code, so it needs the round-trip tests below passing
against the *existing* implementation's output before the old code is removed.

### `pc_tools` — the third implementation

`tools/PcTools/src/kilnctrl/protocol.py` implements this same framing in
Python and cannot link C. It is the reason for `test/vectors/`.

## Test vectors are the mechanism that keeps three implementations honest

`test/vectors/` holds byte-exact encode/decode pairs — a JSON manifest plus the
raw frames — that **all three** implementations run against:

- The C tests decode each vector and assert the decoded fields, then re-encode
  and assert byte equality.
- `pc_tools`' test suite loads the same manifest.
- A vector is added for **every** frame type and for every edge case that has
  ever caused a bug.

Include hostile vectors, not just valid ones: truncated frames, a length byte
larger than the payload, an unterminated escape at the end of a buffer, a
delimiter inside a payload, wrong CRC, `zone_count` beyond the array, NaN and
infinity in every float field.

This is cheaper than a code generator and catches the failure that actually
happens — one implementation updated, the other two not.

## Versioning

`KILNLINK_PROTOCOL_VERSION` lives in `kilnlink_version.h` and is the source of
truth for the **ESP<->Pico isolated safety link only**. `KilnFW`'s
`UART_PROTOCOL_VERSION` (`App/drivers/common/uart_task_ids.h`) is the **PC<->ESP
link**'s own, independent version — the two links are different contracts and
version separately.

From 2026-08-17 to 2026-08-24 `UART_PROTOCOL_VERSION` was a plain alias of
`KILNLINK_PROTOCOL_VERSION` rather than a second number. That alias bit real
hardware three times: a bump driven purely by the isolated link's own contract
(most notably 2026-08-23's Frame A `tx_dropped_sat` addition, which touched
nothing on the PC link) silently dragged the PC link's version along with it,
and `devices.FirmwareVersion.compatible` in `pc_tools` — a hard equality gate —
then refused every PC command against a board that, from the PC link's own
point of view, had not changed at all. The alias is gone; the two constants
are independent literals again and must stay that way.
`tools/check_uart_version_independence.ps1` (repo root) is the CI grep that
enforces it.

Bump each one when **its own** link's contract would break a peer running the
old value: renumbering an id, changing a payload layout or length, or changing
the envelope. Do not bump for comments or internal refactors, and do not bump
one because the other moved. It is a human judgement call, deliberately not a
hash of the file — the reasoning in
`firmware/KilnFW/App/drivers/common/uart_task_ids.h:8-20` applies unchanged and should
be carried over with each constant.

---

## Completion checklist

Tick these as they land. Phase numbers refer to [`../SaftyFW/TODO.md`](../SaftyFW/TODO.md).

**Structure**
- [x] `firmware/CommonFW/` created with the layout above (2026-08-16; `docs/`,
      `include/kilnlink/`, `src/`, `test/`, `test/vectors/` all exist —
      `kilnlink_context.h`/`kilnlink_status.h` added 2026-08-18, see Codecs.
      `kilnlink_ids.h` deliberately not created — see Contract, "Won't do")
- [x] `CMakeLists.txt` producing a `kilnlink` target consumable by pico-sdk
      (standard `add_library` + `target_include_directories`). **Now actually
      linked into a pico-sdk build**: `firmware/SaftyFW/CMakeLists.txt` has
      `add_subdirectory(../CommonFW kilnlink)` and links it into all three of
      `SaftyFW`'s executables (`SaftyFW`/`SaftyFW_slotA`/`SaftyFW_slotB`)
- [x] ESP-IDF component wrapper in `firmware/KilnFW/components/kilnlink/`
      (2026-08-16). Auto-discovered under `firmware/KilnFW/components/` —
      ESP-IDF's default search path, no `EXTRA_COMPONENT_DIRS` edit needed.
      `App/drivers/CMakeLists.txt` lists it in `REQUIRES` so `idf.py build`
      compiles it. Framing is now called from `uart_protocol.c` and
      `kilnlink_announce` from `safety_link.c` (see Migration); it still
      compiles only a subset of the codecs `SaftyFW`'s CMake links
      (`kilnlink_crc.c`, `kilnlink_frame.c`, `kilnlink_context.c`,
      `kilnlink_announce.c` — added 2026-08-19 — not the rest)
- [x] Builds clean under xtensa-gcc, arm-none-eabi-gcc and MSVC at `-Wall -Wextra -Werror`.
      **MSVC verified** (cl.exe via CMake+Ninja, `/W4 /WX`), **xtensa-gcc
      verified** (`idf.py -C firmware/KilnFW build`, real callers now exist so
      nothing is stripped by `--gc-sections`), and **arm-none-eabi-gcc
      verified**: `SaftyFW`'s real CMake project (`add_subdirectory(../CommonFW
      kilnlink)`) builds `kilnlink` clean into all three of its executables

**Contract**
- [x] `docs/LINK_PROTOCOL.md` moved here from `firmware/SaftyFW/docs/` and cross-links updated
      (already done before this session — confirmed 2026-08-16, no stale
      duplicate remains under `firmware/SaftyFW/docs/`)
- [x] `kilnlink_version.h` created (2026-08-16). `KilnFW`'s `UART_PROTOCOL_VERSION`
      (`App/drivers/common/uart_task_ids.h`) was a real alias of `KILNLINK_PROTOCOL_VERSION`
      from 2026-08-17 to 2026-08-24; that alias is now gone (see "Versioning"
      above) — the two are independent literals again, `UART_PROTOCOL_VERSION`
      frozen at 7, `KILNLINK_PROTOCOL_VERSION`/`KILNLINK_MIN_COMPATIBLE` at 7/5.
      This line used to describe the alias as the intended end state; it
      wasn't, and this entry was stale until 2026-08-24.
- [x] **Won't do** — `kilnlink_ids.h` as a shared-ids header consumed by
      `uart_task_ids.h`. Investigated 2026-08-24 (see `SaftyFW/TODO.md`'s
      matching entry): every `SAFETY_CMD_*` in `uart_task_ids.h` that matches
      a `KILNLINK_*_CMD` value is a *documented, deliberate* literal mirror,
      not an oversight —
        - Several of them are genuinely dual-purpose: `uart_bridge.c` dispatches
          real PC→ESP commands (`SAFETY_CMD_CLEAR_TRIP`, `SET_CONFIG`,
          `ROLLBACK`, `SET_CT_CAL`, `GET_CT_CAL`, …) on these exact values,
          "doubling additively" as PC-link subcommands on
          `UART_TASK_ID_SAFETY` — they are not "PC-link ids left behind" vs.
          "shared ids," they are both at once by design.
        - The rest (`SAFETY_CMD_PUSH_CONTEXT`, `SET_LOG_LEVEL`, `SET_PARAM`,
          `COMMIT_CONFIG`, `GET_PARAM`/`PARAM`, `GET_CONFIG_PAGE`/`CONFIG_PAGE`,
          `COMMIT_CONFIG_REJECTED`, `ANNOUNCE_REBOOT`) are redeclared purely so
          `uart_task_ids.h` stays "the one place every `SAFETY_CMD_*`
          subcommand on this wire is enumerated" (its own doc comments, e.g.
          line ~916) — the codec header's `KILNLINK_*_CMD` is named in the
          same comment as the actual authority.
        - `SaftyFW/src/tasks/link_frame.h` makes the identical choice for the
          identical reason: `LINK_FRAME_CLEAR_TRIP_CMD`/`SET_CONFIG_CMD`/
          `ROLLBACK_CMD`/`SET_CT_CAL_CMD` are redeclared as local dispatch
          literals "rather than pulling the kilnlink codec header into this
          file's own namespace," even though `link_task.c` in the same
          firmware already includes and calls the kilnlink codecs directly.
          This is an established, repo-wide convention — a dispatch/enumeration
          header states its own ids as literals with a cross-reference comment
          to the codec header that owns the value, instead of `#include`-ing
          the codec just to reach one macro.
      Collapsing these into a shared `kilnlink_ids.h` would reverse a
      convention both firmwares already apply on purpose, without fixing any
      actual drift (nothing has ever gone out of sync — SaftyFW's own
      duplicate is unmoved evidence this pattern is stable), and does not
      cleanly separate into "shared" vs. "PC-link" as the item's title
      assumed. No code changed for this item; `check_uart_version_independence.ps1`
      still passes (it is unaffected either way — it only ever checked
      `UART_PROTOCOL_VERSION`, not these subcommand ids).

**Codecs**
- [x] `kilnlink_frame.{c,h}` — delimiter, stuffing, CRC16/CCITT-FALSE. Done 2026-08-16
- [x] `kilnlink_context.{c,h}` — ESP → Pico, `SAFETY_CMD_PUSH_CONTEXT` (0x07,
      `LINK_PROTOCOL.md` sec 4). 2026-08-18
- [x] `kilnlink_announce.{c,h}` — ESP → Pico, `SAFETY_CMD_ANNOUNCE_VERSION`
      (0x0F, sec 4, compatibility-floor layout). 2026-08-18
- [x] `kilnlink_ceiling.{c,h}` — ESP → Pico, `SAFETY_CMD_SET_FIRING_CEILING`
      (0x09, sec 4). 2026-08-19
- [x] `kilnlink_clear_trip.{c,h}` — ESP → Pico, `SAFETY_CMD_CLEAR_TRIP`
      (0x0A, sec 4). 2026-08-19
- [x] `kilnlink_get_fw_version.{c,h}` — ESP → Pico, `SAFETY_CMD_GET_FW_VERSION`
      (0x0B, sec 4, one byte, no fields). 2026-08-19
- [x] `kilnlink_set_clock.{c,h}` — ESP → Pico, `SAFETY_CMD_SET_CLOCK` (0x0C,
      sec 4, optional). 2026-08-19
- [x] `kilnlink_status.{c,h}` — Pico → ESP Frame A, `SAFETY_CMD_GET_STATUS`
      (0x01, sec 6, the existing 23-byte layout). 2026-08-18. **2026-08-24:**
      updated for protocol 5→6's optional 24th byte (`tx_dropped_sat`,
      `KILNLINK_STATUS_LEN_V2`) — decode accepts either 23 or 24 bytes,
      encode emits 24 iff the caller sets `has_tx_dropped`, mirroring
      `link_frame_pack_status()`'s own negotiation. `has_tx_dropped` is
      explicit on `kilnlink_status_t` so "peer never sent this" is never
      confused with a real `tx_dropped_sat == 0`. Still latent: nothing
      calls `kilnlink_status_decode`/`_encode` yet (see Migration)
- [x] `kilnlink_inject_tc.{c,h}` — ESP → Pico, `SAFETY_CMD_INJECT_TC`
      (0x21, sec 4, synthetic thermocouple injection gated by
      `safety_tc_installed == 0` on the receiving side). Codec existed
      already; it was the only payload codec with no host test until
      2026-08-24 (`test_inject_tc.c`, 9 cases including a valid=0 "codec
      does not editorialize" check and bit-exact NaN/Inf round trips)
- [x] `kilnlink_diag.{c,h}` — Pico → ESP Frame B, `SAFETY_CMD_DIAG` (0x08, sec 6). 2026-08-18
- [x] `kilnlink_trip.{c,h}` — Pico → ESP Frame D, `SAFETY_CMD_TRIP_EVENT` (0x0D, sec 6). 2026-08-18
- [x] `kilnlink_power.{c,h}` — Pico → ESP Frame E, `SAFETY_CMD_POWER` (0x0E, sec 6). 2026-08-18
- [x] Every `docs/LINK_PROTOCOL.md` sec 4/6 frame with a concrete byte layout
      now has a codec (2026-08-19) -- all ten listed above, plus framing.
      Nothing is wired into either firmware's real send/receive dispatch
      (`uart_protocol.c` on `KilnFW`, `link_task.c` on `SaftyFW`) -- that
      remains separate Migration work, matching how `kilnlink_diag`/
      `kilnlink_trip` already landed codec-only
- [x] Every decoder bounds-checked and returning a status (`kilnlink_frame_decode`/
      `kilnlink_unstuff` and every `kilnlink_<name>_decode` above)
- [x] No allocation, no I/O, no globals — verified by review, not assumed

**Tests**
- [x] Host test binary, all frame types, round-trip byte equality
      (`test/test_frame.c`, MSVC + CMake + Ninja + CTest, 14 checks, all
      passing 2026-08-16)
- [x] Hostile vectors: truncated, over-long length, bad CRC, out-of-range
      counts. **Unterminated escape**: kilnlink itself checks it
      (`KILNLINK_FRAME_ERR_UNTERMINATED_ESC`); `pc_tools`' one-shot `unstuff()`
      does not by design (see the vector's own note in the manifest) — a real,
      documented difference, not an oversight. **NaN/Inf floats**: covered by
      `kilnlink_context`/`kilnlink_status`/`kilnlink_power`'s own vectors
- [x] `test/vectors/` manifest created for every codec above (one
      `<name>_vectors.json` each, `frame_vectors.json` for framing) --
      13 host test binaries total as of 2026-08-19 (`ctest`, all passing)
- [x] `pc_tools` test suite consuming every manifest above, not just
      `frame_vectors.json` (2026-08-19, `selfcheck.py`'s
      `commonfw_vector_checks()` for the framing layer plus the new
      `commonfw_payload_vector_checks()` -- against the new pure-Python
      `tools/PcTools/src/kilnctrl/kilnlink_codec.py` -- for every payload
      codec; fixed a pre-existing invalid-JSON bug in `status_vectors.json`'s
      `too_short` hostile vector found while wiring this up, a stray Python
      slice expression `[:-2]` left in the JSON literal rather than the
      sliced string)
- [x] Fuzz harness over the decoders. Done 2026-08-16: `test/test_fuzz.c`,
      seeded xorshift32 (deterministic/reproducible), both uniform-random and
      structurally-biased garbage (real delimiter/escape/type bytes dropped
      at random positions), every length 0 through past the largest legal
      frame, ~800K inputs against both `kilnlink_frame_decode` and
      `kilnlink_unstuff` -- asserts only "did not crash or hang", passing.
      Not coverage-guided (no libFuzzer/AFL) -- good enough to catch an
      out-of-bounds read or infinite loop, not a substitute for one if this
      code ever needs deeper scrutiny.
      Extended 2026-09-04 (`test/test_fuzz_payloads.c`) to the other 27
      decoders -- every payload codec one layer above the framing pair
      above (context/status/power/announce/diag/trip/ceiling/clear_trip/
      set_config/rollback/get_fw_version/set_clock/announce_reboot/
      set_ct_cal/get_ct_cal/ct_cal/set_log_level/set_param/commit_config/
      commit_config_rejected/inject_tc/get_param/param/get_config_page/
      config_page/rollback_result/fw_version). Same fixed-seed-xorshift32
      determinism, plus a real fixed corpus per decoder (built from that
      decoder's own `_encode()`/`_pack()`: a valid frame bit-flipped at
      every byte, truncated at every offset, and padded with unaccounted
      trailing bytes) and an assertion stronger than "did not crash": a
      truncated or length-lied-about copy of a valid frame must never
      decode OK. Sanitizers confirmed available on this MSVC install
      (`/fsanitize=address` via `test/run_fuzz_payloads_asan.ps1`, opt-in,
      ASan-clean) though not the CI default; canary-guarded output structs
      stand in as the explicit-bounds-assertion fallback for the default
      `cl` build. Negative-tested by deliberately dropping
      `kilnlink_announce_decode()`'s final length check -- the harness
      failed loud, named the decoder, and named the exact truncation that
      broke it; reverted clean. Wired into
      `firmware/SaftyFW/test/build_host_tests.ps1` as CI. Full writeup:
      `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`'s "Fuzz over every
      decoder" row.

**Migration**
- [x] `KilnFW`'s `uart_protocol.c` delegating framing/CRC to `kilnlink_frame`
      (2026-08-18): `firmware/hwAbstraction/esp/uart/uart_protocol.c` calls
      `kilnlink_crc16_ccitt_false`/`kilnlink_stuff` instead of its own copies.
      `firmware/UnitTestFw`'s fork was a stale mirror, never migrated; moot
      now, since `UnitTestFw` was decommissioned and deleted wholesale
      (SimFW is its replacement — see `firmware/SimFW/docs/DESIGN_NOTES.md`
      sec 12), so its allowlist entry below is gone with it
- [x] Round-trip proven against the *pre-refactor* implementation's output before old code is deleted
      (`test/test_uart_protocol_delegate.c`, known vectors + 528 fuzz cases)
- [x] `SaftyFW` linking `kilnlink` with no duplicated protocol code —
      `firmware/SaftyFW/CMakeLists.txt` has a real
      `add_subdirectory(../CommonFW kilnlink)` and links `kilnlink` into all
      three of its executables (`SaftyFW`/`SaftyFW_slotA`/`SaftyFW_slotB`);
      `link_task.c` calls into the framing layer plus the
      `diag`/`trip`/`power`/`clear_trip`/`announce` codecs
- [x] `grep` check in CI: no CRC or stuffing implementation outside `CommonFW`
      (`tools/check_no_duplicate_crc.ps1`, 2026-08-18) — not registered in any
      CI pipeline yet (none exists for it to join), and scoped to an explicit
      allowlist of the one pre-migration copy still in the tree (`pc_tools`'
      `protocol.py` mirror). `KilnFW`'s `uart_protocol.c` mirror under
      `UnitTestFw` dropped off the allowlist when `UnitTestFw` was deleted
      wholesale (2026-08-23)
- [ ] Payload codecs (`context`/`ceiling`/`get_fw_version`/`set_clock`/
      `status`/`diag`/`trip`/`power`) wired into either firmware's real
      send/receive dispatch — most are still codec-only. `announce` and
      `clear_trip` are exceptions: both are wired both ways
      (`clear_trip` send side: `KilnFW`'s `safety_link_commands.c`). See the 2026-08-19 note above for the exact
      state of each
- [x] **Frame C (`SAFETY_CMD_FW_VERSION`, §6) now has a `CommonFW` codec**
      (2026-09-01): `kilnlink_fw_version.{c,h}` — same layout as
      `kilnlink_announce.{c,h}` (both are the compatibility-floor's mirror
      pair, sharing wire id `0x0B` deliberately, see §4's "Request/reply ids
      must never be shared" rule for why this one pair is exempt), extended
      past `boot_id` with `config_version` and `config_crc`. Host-tested
      (`test_fw_version.c`, 12 checks: round trip, byte-exact vectors in
      `test/vectors/fw_version_vectors.json`, and the hostile-input set —
      too-short, wrong command byte, commit/datetime length fields that
      claim more bytes than the buffer actually holds, a declared string
      length past this codec's own cap, and a regression guard against the
      exact failure class commit ca472fb fixed elsewhere in this repo: a
      decoder that reads `min(declared_len, cap)` out of a buffer shorter
      than that). Every one of those hostile cases was proven to fail
      red by mutation (removing the length-prefix bounds check, flipping
      `protocol_version`'s encode to big-endian, and dropping `config_crc`
      from the encoder each turned green tests red before the mutation was
      reverted) — this is not a codec that merely runs, it is one whose
      tests can prove a defect.

      **Drift now pinned by host tests (2026-10-07):** both sides are byte-compared
      against this codec -- `SaftyFW/test/test_link_frame_wire.c`
      (`test_fw_version_pack_matches_commonfw_codec`: `link_frame_pack_fw_version()`
      vs `kilnlink_fw_version_encode()`, byte-identical over empty, 1-byte,
      max-length (64+32) and endianness-probe inputs, plus the same undersized-buffer
      refusal) and `KilnFW/App/test/test_safety_link_compile.c`
      (`test_fw_version_parse_matches_commonfw_codec`: `safety_parse_fw_version()`
      recovers every field from the codec's encoder output). A one-byte change to
      either hand-rolled side now fails a test; the hand-rolled code itself is
      still not replaced.

      **Still open, and deliberately not done in this pass:** `SaftyFW`'s
      `link_frame.c` (`link_frame_pack_fw_version()`) and `KilnFW`'s
      `safety_link.c` still hand-roll Frame C independently against
      `LINK_PROTOCOL.md` §6's offset table — this codec is not yet wired
      into either firmware's real send/receive dispatch, the same
      codec-only state most of the other payload codecs in this checklist
      shipped in first (see "Migration" below). Audited again 2026-08-24
      (before this pass) and unaudited since: the two hand-rolled
      implementations were still in agreement as of that date, so this is
      not a known live bug, but LINK_PROTOCOL.md's own words on this frame
      still apply — migrating both sides is "not safe to do piecemeal from
      one side of the link alone" and needs a dedicated coordinated commit,
      which is out of scope for adding the shared codec itself.

## Related

- [`docs/LINK_PROTOCOL.md`](docs/LINK_PROTOCOL.md) — the wire, both ends.
- [`docs/UPDATE_PROTOCOL.md`](docs/UPDATE_PROTOCOL.md) — firmware updates for
  both processors: interlocks, authentication, ESP OTA partitioning, and the
  frames that carry an image to the Pico over the isolated link. The update
  frame codecs belong in `kilnlink` for the same reason the rest do — one
  implementation, shared test vectors.
- [`docs/BENCHPROTO.md`](docs/BENCHPROTO.md) — the separate `benchproto`
  protocol family's spec: framing, CRC, reliability, and task-registration,
  hardware-agnostic. First (and, since SimFW's 2026-08-28 removal, only
  historical) consumer was SimFW; shares no code with
  `kilnlink` (see that document's section 1 for why).
