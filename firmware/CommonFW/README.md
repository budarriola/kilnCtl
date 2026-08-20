# CommonFW — shared control-interface code

> **Status:** planning · **Last reviewed:** 2026-08-19
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
`firmware/UnitTestFw`'s UART prototype for `SimFW`'s USB-CDC link (see
`firmware/SimFW/docs/PLAN.md` sec 4.4/12). It shares no code with `kilnlink`
— see `docs/BENCHPROTO.md` sec 1 for why the two look alike but are kept
apart — and is host-tested the same way (`test_benchproto_frame.c`,
`test_benchproto_link.c`, both wired into this file's `CMakeLists.txt`).

**Status: the framing layer exists and is host-tested.** `kilnlink_frame.{c,h}`
+ `kilnlink_crc.c` are written, byte-exact cross-checked against
`pc_tools/src/kilnctrl/protocol.py` (the second, already-proven
implementation of this exact envelope) via `test/vectors/frame_vectors.json`,
consumed by both `test/test_frame.c` (built and passing with MSVC via CMake +
Ninja, verified 2026-08-16) and `tools/PcTools/selfcheck.py`. **Framing is now
integrated into both firmwares** (as of 2026-08-18): `KilnFW`'s
`App/drivers/espInterfaces/uart_protocol.c` calls `kilnlink_crc16_ccitt_false`/
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
(`commonfw_payload_vector_checks()`, against the new pure-Python
`kilnctrl/kilnlink_codec.py`) -- not just `frame_vectors.json` as before.
`kilnlink_codec.py` is encode-only: nothing in `pc_tools` decodes these
frames yet, so there is no real caller to justify a decode side (see that
module's own docstring). **Wiring status, same day (2026-08-19):** most of
these codecs are still codec-only, not called from either firmware's real
send/receive dispatch -- see Migration below -- but two are now genuine
exceptions. `ANNOUNCE_VERSION` (0x0F) is wired on both ends:
`SaftyFW`'s `link_task.c` decodes inbound frames via
`kilnlink_announce_decode()`, and `KilnFW`'s `safety_link.c` builds outbound
ones via `kilnlink_announce_encode()` (its `components/kilnlink/CMakeLists.txt`
now compiles `kilnlink_announce.c` to make that possible -- still a subset of
what `SaftyFW`'s CMake links, see the ESP-IDF component wrapper item below).
`CLEAR_TRIP` (0x0A) is wired on the receive side only: `SaftyFW`'s
`link_task.c` decodes it via `kilnlink_clear_trip_decode()` and enforces the
refusal rules `docs/LINK_PROTOCOL.md` §4 describes (refuses if nothing is
currently tripped, refuses if the wire `trip_mask` doesn't match the
latched one); `KilnFW` has no send side for it yet -- no GUI trigger exists
to call `kilnlink_clear_trip_encode()` from.

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
│  └─ kilnlink_power.h             ← Pico → ESP, Frame E, SAFETY_CMD_POWER (0x0E)
│  (no `kilnlink_ids.h` or `kilnlink_port.h` yet — see the Completion checklist)
├─ src/
│  ├─ kilnlink_crc.c    kilnlink_frame.c     kilnlink_context.c
│  ├─ kilnlink_announce.c  kilnlink_ceiling.c  kilnlink_clear_trip.c
│  ├─ kilnlink_get_fw_version.c  kilnlink_set_clock.c
│  └─ kilnlink_status.c  kilnlink_diag.c  kilnlink_trip.c  kilnlink_power.c
├─ src/benchproto_crc.c  src/benchproto_frame.c  src/benchproto_link.c
├─ test/
│  ├─ test_frame.c  test_fuzz.c  test_uart_protocol_delegate.c
│  ├─ test_context.c  test_announce.c  test_ceiling.c  test_clear_trip.c
│  ├─ test_get_fw_version.c  test_set_clock.c
│  ├─ test_status.c  test_diag.c  test_trip.c  test_power.c
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

`KILNLINK_PROTOCOL_VERSION` lives in `kilnlink_version.h` and is **the** source
of truth. `KilnFW`'s `UART_PROTOCOL_VERSION` becomes an alias of it rather than
a second number.

Bump it when a change would break a peer running the old value: renumbering an
id, changing a payload layout or length, or changing the envelope. Do not bump
for comments or internal refactors. It is a human judgement call, deliberately
not a hash of the file — the reasoning in `firmware/KilnFW/App/drivers/uart_task_ids.h:8-20`
applies unchanged and should be carried over with the constant.

---

## Completion checklist

Tick these as they land. Phase numbers refer to [`../SaftyFW/TODO.md`](../SaftyFW/TODO.md).

**Structure**
- [x] `firmware/CommonFW/` created with the layout above (2026-08-16; `docs/`,
      `include/kilnlink/`, `src/`, `test/`, `test/vectors/` all exist —
      `kilnlink_context.h`/`kilnlink_status.h` added 2026-08-18, see Codecs.
      `kilnlink_ids.h` still not created, see Contract)
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
- [x] `kilnlink_version.h` created (2026-08-16); `KilnFW`'s `UART_PROTOCOL_VERSION`
      (`App/drivers/uart_task_ids.h`) is now `((uint16_t)KILNLINK_PROTOCOL_VERSION)`,
      a real alias rather than a second number — both it and
      `KILNLINK_MIN_COMPATIBLE` are 5
- [ ] `kilnlink_ids.h` — shared ids split out of `uart_task_ids.h`, PC-link ids left behind

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
      (0x01, sec 6, the existing 23-byte layout). 2026-08-18
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
      code ever needs deeper scrutiny

**Migration**
- [x] `KilnFW`'s `uart_protocol.c` delegating framing/CRC to `kilnlink_frame`
      (2026-08-18): `App/drivers/espInterfaces/uart_protocol.c` calls
      `kilnlink_crc16_ccitt_false`/`kilnlink_stuff` instead of its own copies.
      `firmware/UnitTestFw`'s fork is a stale mirror, not migrated, and stays
      on the CI grep's allowlist below
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
      allowlist of the pre-migration copies still in the tree (`KilnFW`'s
      `uart_protocol.c` mirror under `UnitTestFw`, `pc_tools`' `protocol.py`
      mirror)
- [ ] Payload codecs (`context`/`ceiling`/`get_fw_version`/`set_clock`/
      `status`/`diag`/`trip`/`power`) wired into either firmware's real
      send/receive dispatch — most are still codec-only. `announce` and
      `clear_trip` are partial exceptions: `announce` is wired both ways,
      `clear_trip` is wired receive-only (`SaftyFW`'s `link_task.c`) with no
      `KilnFW` send side yet. See the 2026-08-19 note above for the exact
      state of each

## Related

- [`docs/LINK_PROTOCOL.md`](docs/LINK_PROTOCOL.md) — the wire, both ends.
- [`docs/UPDATE_PROTOCOL.md`](docs/UPDATE_PROTOCOL.md) — firmware updates for
  both processors: interlocks, authentication, ESP OTA partitioning, and the
  frames that carry an image to the Pico over the isolated link. The update
  frame codecs belong in `kilnlink` for the same reason the rest do — one
  implementation, shared test vectors.
- [`docs/BENCHPROTO.md`](docs/BENCHPROTO.md) — the separate `benchproto`
  protocol family's spec: framing, CRC, reliability, and task-registration,
  hardware-agnostic. First consumer is `SimFW`
  ([`../SimFW/docs/PLAN.md`](../SimFW/docs/PLAN.md)); shares no code with
  `kilnlink` (see that document's section 1 for why).
