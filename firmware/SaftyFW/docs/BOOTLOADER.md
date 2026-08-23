# RP2040 Bootloader — field updates over the isolated link

> **Status:** flash layout and metadata format design frozen (§2); bootloader
> boot/CRC/fallback logic and recovery mode's full minimal frame subset
> (`UPDATE_BEGIN`/`UPDATE_DATA`/`UPDATE_END`/`UPDATE_ABORT`/`UPDATE_STATUS`)
> are built and host-build-verified (`bootloader/main.c`,
> `bootloader/recovery_update.c`, `bootloader/persist.c`, `metadata.h`/`.c`,
> `flash_layout.h`). §4's recovery-mode frame handling reuses the
> application's own pure `src/update/{image_header,received_ranges,
> update_receiver}.h`/`.c` and CommonFW's `kilnlink` framing/CRC library, not
> a second implementation of either — see §4 below for exactly what is and
> is not exercised. The reserved signature/key-space fields in §2's tables
> are now reflected in code too: `bootloader_slot_meta_t` declares
> `signature[64]`/`sig_required` (packed/unpacked, host-test-verified, never
> consulted by any boot decision) and `flash_layout.h` declares
> `BOOTLOADER_PUBKEY_FLASH_OFFSET`/`_SIZE` as a 768 B reservation — both still
> reserved/unused/no-op by design, only the "not yet in metadata.h/
> flash_layout.h" gap is closed. No signing algorithm has been picked and no
> verification code exists (§6 remains not planned for the first version).
> Nothing has been flashed to physical hardware or verified over SWD. See
> `../TODO.md` phase 10 for the itemised status of every checklist entry.
> **Last reviewed:** 2026-08-19
>
> **Keep this file current.** The flash layout here is a commitment: once a
> bootloader is written to a board over SWD it is not going to be changed in the
> field, so the offsets and the metadata format have to be right before the
> first unit is programmed. Edit this file in the same change as any layout
> decision.

The wire protocol, the interlocks and the authentication live in
[`../../CommonFW/docs/UPDATE_PROTOCOL.md`](../../CommonFW/docs/UPDATE_PROTOCOL.md).
This file is the RP2040 half: what runs, where it lives in flash, and how a bad
image gets undone.

---

## 1. The fact that shapes everything

**The RP2040 mask ROM has no UART bootloader.** Its boot paths are USB
(PICOBOOT / UF2 mass storage) and executing the second-stage loader from flash.
There is no vendor-supported way to push an image in over a serial line.

So updating the safety processor over the isolated link means writing a
bootloader. That is a meaningful amount of new code in the one component whose
entire design argument is that every line in it can fail with nothing to catch
it — which is why the layout below spends its complexity budget on exactly one
property:

> **The bootloader is written once over SWD and never updates itself.**

It is the recovery path. A recovery path that can be overwritten by the thing it
recovers from is not a recovery path. If the bootloader itself ever needs
changing, that is a debug-probe operation on a bench, not a field update.

---

## 2. Flash layout

The Pico module has 2 MB of flash, memory-mapped for execute-in-place at
`0x10000000`.

```
0x10000000  +--------------------------------+
            | second stage (boot2)      256 B|  vendor, from the SDK
0x10000100  +--------------------------------+
            | bootloader                ~64 K|  written once over SWD, never
            |   .text/.data          ~63.25K|  updated in the field
            |   pubkey reservation      768 B|  unused today (signing off);
            |                                |  see §6 — a fixed offset near
            |                                |  the end of the region so the
            |                                |  bootloader's own code size can
            |                                |  grow without moving it
0x10010000  +--------------------------------+
            | metadata sector             4 K|  two copies, A/B, CRC'd
0x10011000  +--------------------------------+
            | slot A                     ~832K|  application
0x100E1000  +--------------------------------+
            | slot B                     ~832K|  application
0x101B1000  +--------------------------------+
            | config                      64 K|  runtime configuration
0x101C1000  +--------------------------------+
            | reserved                   ~252K|  headroom
0x10200000  +--------------------------------+
```

- [ ] **Confirm the module's actual flash size before committing to this.**
      2 MB is the standard Pico; a Pico clone or a W variant may differ, and the
      layout is baked in at first programming.
- [ ] Slot sizes are a guess until there is an image to measure. A FreeRTOS SMP
      application with a MAX31856 driver, ADC sampling and the link is likely
      150–250 KB, so 832 KB is generous. Size the slots once, generously, and
      stop moving them.
- [ ] Both slots must be erase-block aligned (4 KB sectors, 64 KB blocks).
- [ ] The config partition stays **outside** both slots, so an update never
      touches the kiln's safety configuration. A config format change is then a
      migration problem, not an update problem.

### Metadata

Two copies in the metadata sector, each CRC'd, written alternately so a power
loss during a metadata write always leaves one valid copy:

| Field | Notes |
|---|---|
| `magic`, `format_version` | Refuse anything unrecognised rather than guessing |
| `active_slot` | A or B |
| `slot[2].state` | `EMPTY` / `STAGED` / `VALID` / `PENDING_VERIFY` / `BAD` |
| `slot[2].length`, `slot[2].crc32` | Verified before every boot, not just after an update |
| `slot[2].version[16]`, `build_commit[20]`, `build_epoch` | Reported over the link |
| `slot[2].signature[64]` | **Reserved, unused.** Space for an Ed25519 (or similar) signature over `[0, length)` of the slot, so turning on signing later (§6) is a format-version bump inside the *same* record layout, not a new field that has to be squeezed in around existing data. All-zero means "no signature present" and is what every image ships with until signing is turned on. |
| `boot_attempts` | Incremented before jumping, cleared on check-in |
| `sig_required` | **Reserved, unused, defaults to 0.** A future format version could set this per-record to require a valid `signature` before a slot is considered bootable; until then the bootloader must treat it as always-false regardless of its stored value, so a stray nonzero byte in an old record can never accidentally start enforcing a check the current bootloader build cannot perform. |
| `crc32` | Over the whole record |

The implementation's record is `BOOTLOADER_METADATA_RECORD_LEN` = 256 bytes
(`metadata.h`), unchanged. `signature[64]` and `sig_required` now live inside
each slot's 117-byte on-flash block (`metadata.c`'s `SLOT_BLOCK_LEN`, up from
52 bytes) — two slot blocks plus the fixed 16-byte header and trailing 4-byte
CRC use 250 of the 256 bytes, with a compile-time check
(`bootloader_metadata_record_budget_check` in `metadata.c`) that fails the
build if a future field addition ever overruns the record. The metadata
sector's own offset/size in `flash_layout.h` did not change.
`bootloader_slot_meta_t` declares both fields, `bootloader_metadata_pack()`/
`_unpack()` roundtrip them byte-for-byte, and `test_bootloader_metadata.c`
covers the roundtrip (including non-zero values) and confirms
`bootloader_decide_boot()` treats a nonzero `sig_required` as a no-op — landed
before any board is programmed, per this section's own discipline that the
record's on-flash byte layout is exactly as permanent as the offsets around
it.

---

## 3. What the bootloader does

On every reset, in this order:

1. **Drive GPIO6 low.** First statement, before anything else, exactly as the
   application does. The relay must be open before any other decision is taken —
   a bootloader that leaves the safety relay in an unknown state for even a few
   milliseconds of flash-CRC time is not acceptable.
2. Read and validate metadata. If both copies are bad, enter recovery.
3. Check `boot_attempts` on the active slot. Above the limit (3), mark the slot
   `BAD`, switch to the other slot if it is `VALID`, and record why.
4. CRC the active slot against its recorded length and CRC. **Every boot**, not
   just the first after an update — this is what catches flash degradation and a
   partially-erased slot.
5. Increment `boot_attempts`, write metadata, jump to the application.

If no slot is bootable, enter recovery instead of looping.

### What it must never do

- Never write to its own region.
- Never touch the config partition.
- Never enable interrupts it does not need, bring up the ADC, or initialise the
  thermocouple SPI. It is not a safety processor; it is a loader with the relay
  pinned low.
- Never wait indefinitely on the link. Every recovery wait is bounded.

---

## 4. Recovery mode

Entered when no slot is bootable, or on an explicit request latched in a
watchdog scratch register before a deliberate reboot.

In recovery the bootloader brings up **only** UART1 at the link's baud rate and
speaks a minimal subset of the update protocol: `UPDATE_BEGIN`, `UPDATE_DATA`,
`UPDATE_END`, `UPDATE_ABORT`, `UPDATE_STATUS`. It does not implement telemetry,
context frames or anything else.

- GPIO6 stays low the entire time.
- It emits `UPDATE_STATUS` on a ~1s timer so the ESP can tell "sitting in
  recovery" from "dead", and the GUI can say so.
- There is no timeout out of recovery. There is nothing safe to time out *into*.

This is what makes the whole scheme defensible: a failed update lands in a state
that can be updated again over the same link, without a probe.

**Built, TODO.md item 10.4** (`bootloader/recovery_update.c`/`.h`,
`bootloader/persist.c`/`.h`): `main.c`'s `enter_recovery()` brings up UART1
then calls `recovery_update_run()`, a bare `for(;;)` polling loop (no
RTOS — this is a bare-metal executable) that parses `kilnlink`-framed bytes
off UART1 and dispatches `UPDATE_BEGIN`/`UPDATE_DATA`/`UPDATE_END`/
`UPDATE_ABORT` to handlers built on the same pure decision modules
`src/tasks/update_task.c` uses (`src/update/image_header.h`,
`received_ranges.h`, `update_receiver.h`), compiled directly into this
executable rather than pulled in via `src/tasks/update_task.c` itself (that
file is FreeRTOS/queue-coupled and cannot link here). Framing/CRC/stuffing
comes from CommonFW's `kilnlink` library, linked into the bootloader for the
first time this pass (`add_subdirectory(../../CommonFW)`, freestanding C11
with no allocation/I/O/globals, so no RTOS dependency is pulled in) — the
same library `src/tasks/link_task.c` uses on the application side, not a
second framing implementation. `UPDATE_STATUS`'s wire layout is
byte-identical to `update_task.c`'s own (a fresh, small, standalone packer,
since that file's own builder is not linkable here either).

Two deliberate departures from the application-side receiver, both
documented in `recovery_update.h`'s header comment:
- **No live preconditions.** `update_receiver_handle_begin()`'s
  `update_preconditions_t` argument is passed as all-satisfied
  (`relay_open`/`no_trip_pending`/`temp_known_and_low` all `true`) rather
  than gathered from `safety_core`/`thermo_task`, which do not exist in a
  bare-metal recovery image — recovery mode is entered specifically because
  the application, and everything it would have started, never ran. The
  image header itself is still fully, independently validated.
- **No reboot after `UPDATE_END`.** The target slot is marked
  `PENDING_VERIFY` (not `VALID`) and `active_slot` flipped to it, matching
  `update_task_process_end()`'s own choice — the application still has to
  earn `VALID` via §5's confirmation gate. The operator/ESP is expected to
  reset the board afterward, at which point the boot path above picks up
  the newly-staged slot.

**Verified this pass**: `bootloader/` builds clean under
`-Wall -Wextra -Werror` (arm-none-eabi-gcc 14.2.1, pico-sdk 2.1.1) at 12812 B
of flash `text` (~20% of the ~64K budget); the application's
`SaftyFW`/`SaftyFW_slotA`/`SaftyFW_slotB` targets were rebuilt clean
afterward to confirm the shared `src/update/*` files were not broken; all
459 existing host-test checks still pass unchanged (no new pure decision
logic was added — everything new here is flash/UART I/O glue around
already-host-tested pure modules).

**Not verified, honestly**: no real ESP/PC peer has sent real frames over a
live opto-isolated UART1 link to this code; no real flash-erase/program
timing has been exercised under this bootloader's specific
interrupts-disabled window; and recovery mode has never actually been
entered from a genuinely bad/corrupted application image on real hardware.
No RP2040 or debug probe is attached to the machine this was built on.

---

## 5. Rollback: the application must earn its slot

A new image boots with its slot marked `PENDING_VERIFY`. It becomes `VALID` only
when the application calls the equivalent of "I am actually working" — and, as
with the ESP, that call must not be at the end of `main()`.

The safety processor's bar for a working image:

- [ ] Configuration loaded and its CRC verified — **wired but permanently
      false**: `src/tasks/update_task.c`'s `update_task_confirm_tick()` calls
      `update_confirm_missing()` (`src/update/confirm.h`) with real evidence
      for every item below except this one, which is hardcoded `false` since
      no `config_store` exists yet (Phase 9) — `confirm.h`'s own discipline
      is "a caller with nothing to check should pass false, never true", so
      this item is the one thing standing between the gate and actually
      reaching `VALID` today. Not a bug; an honest, documented gap.
- [x] Thermocouple front end returning a plausible reading —
      `thermo_task_get_snapshot()`'s `valid` field, real.
- [x] ADC sampling — proxied by `current_task_get_snapshot()`'s timestamp
      freshness (`current_task.h` has no direct "is sampling running"
      boolean; see `update_task.c`'s own comment on this choice), real.
- [x] All tasks checked in with the watchdog at least once —
      `watchdog_task_all_checked_in_since_boot()` (new this pass, a
      cumulative-since-boot bitmask distinct from `watchdog_task`'s own
      periodic feed-window mask), real.
- [x] At least one telemetry frame acknowledged by the ESP — substituted
      with "sent and not dropped" (`link_task_get_status_tx_ok_count()`),
      per `confirm.h`'s own header comment: this link's design has no ACK
      for the Pico to wait on at all, so "acknowledged" was never buildable
      as literally written.

An image that boots but cannot read its thermocouple is worse than the old one,
and it would sail through any check that just proves `main()` ran.

If the application does not confirm within a bounded time, the watchdog resets,
`boot_attempts` exceeds its limit, and the bootloader falls back to the previous
slot. The previous slot is never erased until the new one is `VALID`.

### Writing flash while running from flash

The application receives the image and writes it, so the standard RP2040 rules
apply and they are not optional here:

- The flash write routine and every ISR that can fire during it must be in RAM
  (`__not_in_flash_func`), or the core will fetch from a flash that is mid-erase.
- Core 1 must be parked for the duration — `flash_safe_execute()` with the
  multicore lockout, already required for config writes in
  [`ARCHITECTURE.md`](ARCHITECTURE.md) §8.
- **A 64 KB block erase takes on the order of hundreds of milliseconds**, during
  which both cores are effectively stopped. The watchdog must be fed around it,
  and the guards are not running. This is a second, independent reason the
  relay must already be open before an update starts.
- [ ] Decide whether to erase the whole staging slot up front or block-by-block
      as data arrives. Up front is simpler and keeps the stalls out of the
      streaming path; it also means a longer window before the first byte lands.

---

## 6. Optional: image signing

`UPDATE_PROTOCOL.md` §2 explains the gap — the ESP authorises Pico updates, so a
compromised ESP can flash the safety processor with anything.

The answer, if it is ever judged worth the cost, is a public key in the
bootloader, which is written once over SWD and never updated. The bootloader
then refuses any image without a valid signature, and a compromised ESP can only
deliver images the developer signed.

This is **not** planned for the first version, because it needs a signing key
that has to be kept somewhere and a build step that uses it, and getting key
management wrong produces a confident false sense of security. But it must be
possible to turn on later without a flash-layout change:

- [x] Reserve space in the bootloader region for a public key, populated or
      not — **layout frozen** (§2's flash map: a 768 B pubkey reservation at
      a fixed offset near the end of the ~64K bootloader region), **now in
      `flash_layout.h`** as `BOOTLOADER_PUBKEY_FLASH_OFFSET`/
      `BOOTLOADER_PUBKEY_FLASH_SIZE` (0x0000FD00, 768 B, computed from
      `BOOTLOADER_FLASH_OFFSET + BOOTLOADER_FLASH_SIZE - 768`). 768 B is
      sized against Ed25519 (32 B public key) with generous headroom for a
      larger scheme (e.g. an RSA-2048 key at 256 B, or room for more than
      one key if key rotation is ever wanted) — **open question, not
      decided**: which signature algorithm, and whether more than one key
      should be reservable, is deliberately left unpicked until signing is
      actually built, since picking wrong now costs nothing (the space is
      reserved either way) and picking early would just be a guess. This is
      a reservation only — no code reads or writes this region.
- [x] Reserve a signature field in the image header from the start — **format
      frozen** (§2's metadata table: `slot[2].signature[64]`, all-zero =
      "none present"), **now in `metadata.h`** as
      `bootloader_slot_meta_t.signature[64]`, packed/unpacked byte-for-byte
      by `metadata.c` and covered by `test_bootloader_metadata.c`'s roundtrip
      tests. 64 B fits an Ed25519 signature (64 B) with no headroom to
      spare; if a larger-signature scheme is ever chosen this field is
      undersized and would need a `format_version` bump to widen — flagged
      here rather than over-allocating against an algorithm that has not
      been chosen.
- [x] Reserve a metadata flag for "signature required", defaulting to off —
      **format frozen** (§2's metadata table: `sig_required`), **now in
      `metadata.h`** as `bootloader_slot_meta_t.sig_required`. The bootloader
      ignores this flag entirely today — `bootloader_decide_boot()` and
      `bootloader_decide_after_crc_fail()` never read it, and
      `test_bootloader_metadata.c` has an explicit test asserting a nonzero
      `sig_required` with an all-zero signature does not block boot — so a
      stray nonzero byte in an old, pre-signing record can never be misread
      as "verification required" by a bootloader that has no verifier to
      run. No signature verification is implemented; this remains reserved
      and unused.

Reserving the space costs nothing now. Not reserving it means the upgrade path
is a bench visit to every board.

---

## 7. Completion checklist

**Before the first board is programmed** — these are effectively permanent
- [ ] Module flash size confirmed on the actual hardware
- [ ] Layout fixed: bootloader, metadata, two slots, config, all block-aligned
- [ ] Metadata format frozen, with a `format_version` that can refuse the unknown
- [ ] Signature field and public-key space reserved even though signing is off
- [ ] DEBUG header fitted — the recovery path underneath the recovery path

**Bootloader**
- [x] GPIO6 driven low as the first statement — `bootloader/main.c`'s `main()`,
      literal first three statements, before the flash-capacity check or any
      metadata read.
- [x] Metadata double-buffered and CRC'd; survives power loss mid-write — via
      the append-only log scheme (`bootloader/metadata.h`'s header comment
      supersedes this section's original "two copies... written alternately"
      wording; same offset/size, power-loss-safe by construction since a torn
      write only corrupts the in-progress slot, never a previously-written
      one). `main.c`'s `persist_metadata()` calls the frozen
      `bootloader_metadata_pack()`/`_next_write_slot()`/
      `_next_write_needs_erase()` API exactly as designed.
- [x] Active slot CRC'd on **every** boot — `main.c`'s step 5, `bootloader_crc32()`
      over the chosen slot's `[0, length)`, every boot, not just after an update.
- [x] `boot_attempts` limit falls back to the other slot — via
      `bootloader_decide_boot()` (frozen, host-tested, unmodified).
- [x] Never writes its own region or the config partition — `main.c` only ever
      calls `flash_range_erase()`/`flash_range_program()` against
      `BOOTLOADER_METADATA_FLASH_OFFSET`/`_SIZE`.
- [x] Recovery mode: UART1 only, GPIO6 low, no timeout out, full minimal frame
      subset — `enter_recovery()` brings up UART1 at 9600 8N1, matching the
      application's `UART_OWNER_BAUD_RATE`: the TCMT1109 optocouplers cannot
      switch fast enough for 115200 (`docs/HARDWARE.md` §1), and recovery mode
      shares the same physical UART1/optocoupler path, so it was updated to
      9600 along with everything else on this link — confirmed against
      `bootloader/main.c`'s `enter_recovery()`, which now says so explicitly —
      then calls
      `recovery_update.c`'s `recovery_update_run()`, which parses
      `kilnlink`-framed `UPDATE_BEGIN`/`UPDATE_DATA`/`UPDATE_END`/
      `UPDATE_ABORT` and emits a real framed `UPDATE_STATUS` every ~1s,
      forever, reusing the application's own pure `src/update/*` decision
      modules and CommonFW's `kilnlink` library rather than a second
      implementation of either. See section 4 above for exactly what is
      built/host-tested vs. not yet exercised on real hardware.
- [ ] Bootloader-only build flashed over SWD, verified independently of any
      app — **not done**; build-verified only (see TODO.md Phase 10 item
      10.5), no hardware/probe available to this pass.

**Application side**
- [x] Staged writes to the inactive slot only — `src/tasks/update_task.c`'s
      `update_task_process_begin()` always targets `update_receiver_handle_begin()`'s
      `target_slot` ("whichever slot is not currently active", computed
      independently of anything the ESP claims), erases and programs only that
      slot's `BOOTLOADER_SLOT_A/_B_FLASH_OFFSET` range.
- [x] Flash routines and interruptible ISRs in RAM; core 1 parked —
      `flash_safe_execute()` (pico-sdk's FreeRTOS-SMP helper, verified against
      the vendored SDK source this pass, see `update_task.c`'s header comment)
      disables interrupts on BOTH cores for the whole duration of every erase/
      program/metadata-write callback and parks core 1 via its own high-
      priority lockout task — a stronger guarantee than "the ISRs are in RAM",
      and the reason none of `update_task.c`'s own callbacks need
      `__not_in_flash_func()` themselves (see that file's header comment for
      the full reasoning, cited against the real `pico/flash.c` source).
- [x] Watchdog handled across multi-hundred-millisecond erases —
      `update_task_erase_slot()` erases the 832K target slot in
      `FLASH_BLOCK_SIZE` (64K) units, one `flash_safe_execute()` call per
      block, with `watchdog_task_checkin()` immediately before AND after each
      block (never during — nothing can run during the erase itself).
- [x] Whole-slot CRC verified by reading **back from flash** —
      `update_task_process_end()` (item 10.7): `bootloader_crc32()` over the
      XIP-mapped target slot's `[0, length)`, compared against the BEGIN
      header's `crc32`, exactly the "catches a write that reported success
      and did not land" check this section calls for.
- [~] `PENDING_VERIFY` cleared only after config, thermocouple, ADC, watchdog
      check-in and one acknowledged telemetry frame — the confirmation gate
      itself is fully wired (`update_task_confirm_tick()`, real
      `thermo_task`/`current_task`/`watchdog_task`/`link_task` evidence for
      four of the five items), but `config_crc_ok` is **permanently false**
      in this build (no `config_store`, Phase 9) and `confirm.h`'s own
      discipline forbids faking it, so `update_confirm_missing()` can never
      reach 0 and no slot can be marked `VALID` until Phase 9 lands. "One
      acknowledged telemetry frame" is also honestly substituted with "one
      telemetry frame handed to the TX ring and not dropped" —
      `src/update/confirm.h`'s own header comment explains why: this link's
      design has no ACK for the Pico to wait on at all.

**Verification**
- [ ] Power cut during erase, during streaming, and during the metadata write —
      old image still boots in all three
- [ ] Deliberately corrupted slot rejected at boot
- [ ] Image that boots but fails a bring-up check is rolled back automatically
- [ ] Both slots deliberately invalidated: lands in recovery and can be updated
      over the link with no probe attached
- [ ] Update refused while the relay is closed
