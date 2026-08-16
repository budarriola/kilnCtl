# CommonFW — shared control-interface code

> **Status:** planning · **Last reviewed:** 2026-08-16
> **Keep this file current.** If you change anything this document describes,
> update it in the same commit. If it disagrees with the code, **the code
> wins** — fix this file and say so in the commit message.

The wire between the ESP32-S3 main controller ([`../KilnFW`](../KilnFW)) and
the RP2040 safety processor ([`../SaftyFW`](../SaftyFW)), implemented **once**
and linked into both.

**Status: planning only. No code exists yet.**

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
│  └─ LINK_PROTOCOL.md             ← THE contract. Owned here, not by either firmware
├─ include/kilnlink/
│  ├─ kilnlink_version.h           ← KILNLINK_PROTOCOL_VERSION, the single source
│  ├─ kilnlink_ids.h               ← device ids, task ids, command ids, flag bits
│  ├─ kilnlink_frame.h             ← delimiter/stuffing/CRC framing
│  ├─ kilnlink_context.h           ← ESP → Pico context, ceiling, clear, version req, clock
│  ├─ kilnlink_status.h            ← Pico → ESP status, diag, power, version, trip event
│  └─ kilnlink_port.h              ← the (very small) set of things a port must supply
├─ src/
│  ├─ kilnlink_crc.c
│  ├─ kilnlink_frame.c
│  ├─ kilnlink_context.c
│  └─ kilnlink_status.c
├─ test/
│  ├─ test_frame.c  test_context.c  test_status.c  test_fuzz.c
│  └─ vectors/                     ← shared byte-exact test vectors, see below
└─ CMakeLists.txt
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
- [ ] `firmware/CommonFW/` created with the layout above
- [ ] `CMakeLists.txt` producing a `kilnlink` target consumable by pico-sdk
- [ ] ESP-IDF component wrapper in `firmware/KilnFW/components/kilnlink/`
- [ ] Builds clean under xtensa-gcc, arm-none-eabi-gcc and MSVC at `-Wall -Wextra -Werror`

**Contract**
- [ ] `docs/LINK_PROTOCOL.md` moved here from `firmware/SaftyFW/docs/` and cross-links updated
- [ ] `kilnlink_version.h` created; `KilnFW`'s `UART_PROTOCOL_VERSION` aliased to it
- [ ] `kilnlink_ids.h` — shared ids split out of `uart_task_ids.h`, PC-link ids left behind

**Codecs**
- [ ] `kilnlink_frame.{c,h}` — delimiter, stuffing, CRC16/CCITT-FALSE
- [ ] `kilnlink_context.{c,h}` — ESP → Pico encoders/decoders
- [ ] `kilnlink_status.{c,h}` — Pico → ESP encoders/decoders
- [ ] Every decoder bounds-checked and returning a status
- [ ] No allocation, no I/O, no globals — verified by review, not assumed

**Tests**
- [ ] Host test binary, all frame types, round-trip byte equality
- [ ] Hostile vectors: truncated, over-long length, bad CRC, unterminated escape, out-of-range counts, NaN/Inf floats
- [ ] `test/vectors/` manifest created
- [ ] `pc_tools` test suite consuming the same manifest
- [ ] Fuzz harness over the decoders

**Migration**
- [ ] `KilnFW`'s `uart_protocol.c` delegating framing/CRC to `kilnlink_frame`
- [ ] Round-trip proven against the *pre-refactor* implementation's output before old code is deleted
- [ ] `SaftyFW` linking `kilnlink` with no duplicated protocol code
- [ ] `grep` check in CI: no CRC or stuffing implementation outside `CommonFW`

## Related

- [`docs/LINK_PROTOCOL.md`](docs/LINK_PROTOCOL.md) — the wire, both ends.
- [`docs/UPDATE_PROTOCOL.md`](docs/UPDATE_PROTOCOL.md) — firmware updates for
  both processors: interlocks, authentication, ESP OTA partitioning, and the
  frames that carry an image to the Pico over the isolated link. The update
  frame codecs belong in `kilnlink` for the same reason the rest do — one
  implementation, shared test vectors.
