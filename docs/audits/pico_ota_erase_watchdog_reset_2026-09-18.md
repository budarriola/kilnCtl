# Pico OTA relay: the destination-slot erase resets the safety processor

Audit date: 2026-09-18. Analysis only — no firmware source was modified, nothing
was built or flashed, and no hardware tool was invoked for this pass.

## 1. The observed failure

An ESP-driven OTA of the RP2040 safety processor was started with
`ota_update_pico()`. The ESP staged the image into its `pico_img` partition and
began the relay. `ota_status()` then reported
`phase='failed'`, `last_error='Pico did not confirm RECEIVING within 15000 ms of erase'`.
Immediately afterwards `safety_get_diag()` reported the Pico's boot reason as
`watchdog`, its state as `grace`, and its uptime as 24007 ms, against 2,552,017 ms
and `armed` moments before. Relays R1–R4 read 0 throughout, no trip latched, and
the configuration survived unchanged (`config_version` 162, `config_crc` 53177).

This document tests the hypothesis that the RP2040 hardware watchdog fired
during the destination-slot erase because nothing can feed it while
`hal_flash_safe_execute()` is running.

**Verdict: CONFIRMED.** The mechanism is established from code; the precise
per-block erase time on the specific flash die is the one inferred quantity, and
even the most favourable published figure leaves no margin.

## 2. What `hal_flash_safe_execute()` actually does on this target

The Pico backend is a thin pass-through to the SDK:

- `firmware/hwAbstraction/pico/flash/hal_flash_pico.c:242-261` — `hal_flash_safe_execute()`
  calls `flash_safe_execute(cb, arg, timeout_ms)` directly and maps
  `PICO_OK`/`PICO_ERROR_TIMEOUT`/`PICO_ERROR_NOT_PERMITTED` onto
  `HAL_OK`/`HAL_TIMEOUT`/`HAL_NOT_READY`.
- `firmware/hwAbstraction/pico/flash/hal_flash_pico.c:196-215` — `hal_flash_erase()`
  is a bounds check followed by a bare `flash_range_erase(abs_offset, len)`, with
  an explicit comment that it must be called from inside a safe-execute callback.

The SDK implementation (`C:/pico-tools/pico-sdk/src/rp2_common/pico_flash/flash.c`,
the path recorded in `firmware/SaftyFW/build/CMakeCache.txt:321`) decides the
lockout mechanism at compile time. SaftyFW builds the FreeRTOS-SMP variant:
`firmware/SaftyFW/FreeRTOSConfig.h:30-31` sets `configNUMBER_OF_CORES 2` and
`configUSE_CORE_AFFINITY 1`, `firmware/SaftyFW/CMakeLists.txt:258,326,493` links
`pico_flash`, and the SDK defaults `PICO_FLASH_SAFE_EXECUTE_SUPPORT_FREERTOS_SMP`
to 1 under FreeRTOS SMP (`pico/flash.h:104-107`). Neither
`PICO_FLASH_ASSUME_CORE0_SAFE` nor `PICO_FLASH_ASSUME_CORE1_SAFE` is defined
anywhere in SaftyFW's CMake, so both default to 0 (`pico/flash.h:94-101`) and
`use_irq_only()` returns false. That selects, verbatim:

- `flash.c:136-190` (`default_enter_safe_zone_timeout_ms`) — creates a
  `flash_lockout_task` pinned to the *other* core, raises it to
  `configMAX_PRIORITIES-1`, and waits for it to signal `LOCKEE_READY`.
- `flash.c:118-134` (`flash_lockout_task`) — the other core executes
  `save_and_disable_interrupts()` and then spins in `__wfe()` until released.
- `flash.c:181-185` — the calling core then does its own
  `save_and_disable_interrupts()`.
- `flash.c:78-87` (`flash_safe_execute`) — only after both of those does it call
  `func(param)`; the callback runs with interrupts disabled on both cores.

So during `flash_range_erase()` **both** RP2040 cores have interrupts disabled:
no SysTick, no FreeRTOS scheduling, no task of any kind runs. `grep -rn watchdog`
over the whole `pico_flash` directory returns nothing — the SDK's safe-execute
path has no watchdog interaction at all, neither feeding nor pausing.

## 3. How the watchdog is fed, and why the bracketing check-ins cannot help

The hardware watchdog is armed once, early in boot, at
`firmware/SaftyFW/src/main.c:320`: `watchdog_enable(SAFTYFW_WATCHDOG_TIMEOUT_MS, true)`
with `SAFTYFW_WATCHDOG_TIMEOUT_MS 1000` (`firmware/SaftyFW/src/main.c:57`).

There is exactly one feed site in the whole firmware. `grep -rn hal_wdt_feed`
over `firmware/SaftyFW/src` returns a single call:
`firmware/SaftyFW/src/tasks/watchdog_task.c:172`, `(void)hal_wdt_feed()`, reached
only inside the `all_ok` branch of `watchdog_task_fn()`'s loop
(`watchdog_task.c:134-183`). That loop is gated by
`vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAFTYFW_PERIOD_WATCHDOG_TASK_MS))`
with a 250 ms period (`firmware/SaftyFW/src/task_priorities.h:75`).
`hal_wdt_feed()` itself is a one-line `watchdog_update()`
(`firmware/hwAbstraction/pico/wdt/hal_wdt_pico.c:47-50`).

`watchdog_task` is a FreeRTOS task pinned to core 1
(`watchdog_task.c:265-271`, `vTaskCoreAffinitySet(s_task_handle, SAFTYFW_CORE_TRIP_PATH)`,
`SAFTYFW_CORE_TRIP_PATH` = `1u << 1`, `task_priorities.h:38`). `update_task` is
pinned to core 0 (`update_task.c:1374-1382`, `SAFTYFW_CORE_LINK_PATH` = `1u << 0`,
`task_priorities.h:39`). That separation does not help: the lockout task the SDK
spawns is deliberately created on the *other* core — i.e. exactly core 1 — at
`configMAX_PRIORITIES-1` (7 here, `FreeRTOSConfig.h:50`), above
`SAFTYFW_PRIO_WATCHDOG_TASK` (6, `task_priorities.h:44`) and above every other
SaftyFW task, and it disables interrupts there. `watchdog_task` therefore cannot
run, and `watchdog_update()` cannot be reached, for the entire duration of each
erase.

`watchdog_task_checkin()` (`watchdog_task.c:275`) writes only a per-task
last-seen tick into `s_last_checkin_tick[]`, read back by the gate at
`watchdog_task.c:146-162`. It does not touch hardware. The two check-ins
bracketing the erase (`update_task.c:490` and `update_task.c:493`) therefore
guarantee only that `update_task` will not be *blamed* by the software gate; they
do nothing whatsoever about the hardware timer. The code's own comment
(`update_task.c:462-476`) is candid that "nothing CAN run during the actual
erase" and that the block-at-a-time split was chosen so "the watchdog stays fed
across the whole operation" — that second half is the mistaken step. Splitting
the work bounds each stall, but the bound it achieves (one 64 KB block) is not
demonstrably below 1000 ms, and nothing in the loop feeds hardware between
blocks either.

`pause_on_debug` is passed `true` at `main.c:320` and forwarded verbatim to
`watchdog_enable()` by `hal_wdt_pico.c:36`. At the SDK level that maps to the
RP2040 `WATCHDOG.CTRL.PAUSE_DBG0/1` bits — the counter pauses only when a core
is halted by an attached debugger. There is no pause-on-flash bit in RP2040
hardware, and no such thing is configured. It is irrelevant to this failure: no
debugger was halting the board.

Two further points of corroboration:

- The timeout passed to the erase is `2000u` (`update_task.c:492`), literally
  twice the watchdog period. That argument is the SDK's *handshake entry/exit*
  timeout, not a limit on the erase itself (`flash.c:79-86` — `func(param)` is
  called unconditionally once entry succeeds and is never timed), so it neither
  caps nor detects an over-long erase. It does, however, show the code was
  written expecting stalls in that range.
- `spi_owner.c:41-58` already documents the house rule that blocking sections
  must be bounded below the 1 s watchdog. The erase loop violates it.

## 4. Realistic duration of one 64 KB block erase

The repo's own figure is `firmware/SaftyFW/docs/BOOTLOADER.md:280`: "**A 64 KB
block erase takes on the order of hundreds of milliseconds**, during which both
cores are effectively stopped. The watchdog must be fed around it, and the
guards are not running." `update_task.c:466-470` and
`firmware/KilnFW/App/drivers/net/ota_pico_relay.c:107-109` both cite that figure.

The repo does not name the flash die anywhere I could find. The only hardware
description is the schematic's module property, `hardware/mainBoard/SaftyProcessor.kicad_sch:1488`,
which describes a Raspberry Pi Pico module with "2MB QSPI flash"; the board is
built with `-DPICO_BOARD=pico` (`firmware/SaftyFW/CMakeLists.txt:19`). The
standard part on that module is a Winbond W25Q16JV-class device.
**Inferred, not verified from any file in this repo:** that family's datasheet
gives a 64 KB block erase (BE, `D8h`) of typically 150 ms with a maximum of
2000 ms, a 32 KB block erase of typically 120 ms / maximum 1600 ms, and a 4 KB
sector erase (SE, `20h`) of typically 45 ms / maximum 400 ms. Those maxima are
the end-of-life, worst-case-temperature numbers the part is specified to, and on
a safety processor the maximum is the number that has to fit, not the typical.

Against a 1000 ms watchdog:

- 64 KB block: typical 150 ms fits; **maximum 2000 ms does not** — a single block
  can exceed the timeout by 2x on its own.
- 32 KB block: maximum 1600 ms — still does not fit.
- 4 KB sector: maximum 400 ms — fits, with roughly 600 ms of headroom.

And even at the typical figure the whole-slot operation is ~13 x 150 ms ≈ 2 s of
almost-continuous stall, interrupted only by the microseconds between one
`flash_safe_execute()` returning and the next being entered. `watchdog_task`
only feeds at its own 250 ms `vTaskDelayUntil` boundary
(`watchdog_task.c:134-135`), and ticks are not delivered while interrupts are
disabled, so those sub-millisecond gaps are not a reliable opportunity to feed.
A reset partway through the erase is the expected outcome even when every
individual block completes in typical time — which matches the observed 24 s
uptime and `watchdog` boot reason precisely.

## 5. Does anything erase these slots successfully today?

Yes: the bootloader's recovery path, and it works for a reason that cannot be
copied. `firmware/SaftyFW/bootloader/recovery_update.c:138-151`
(`recovery_erase_slot()`) walks the same `BOOTLOADER_SLOT_FLASH_SIZE` in the same
64 KB units, wrapping each `flash_range_erase()` in
`save_and_disable_interrupts()`/`restore_interrupts()` rather than
`flash_safe_execute()`. Its own comment (`recovery_update.c:128-137`) states the
difference explicitly: it is bare-metal, "no FreeRTOS SMP here", and it keeps the
block-at-a-time shape "even though this bare-metal image has no watchdog to
starve". `grep -rn watchdog firmware/SaftyFW/bootloader/*.c` returns that comment
and nothing else — the bootloader never arms a watchdog, because
`watchdog_enable()` does not run until `main.c:320` in the application image.

So there is no existing working pattern for erasing a slot *under* an armed
watchdog. The bootloader succeeds by not having one.

The other in-application flash writer is not a counterexample either:
`config_store_flash.c:1176` uses a 1000 ms handshake timeout for a single-sector
config write, and `update_task.c:435` writes one ~256-byte metadata page — both
are single-sector operations whose stall is short, as
`update_task.c:415-419` itself notes.

## 6. Verdict

**CONFIRMED.** The bracketing `watchdog_task_checkin()` calls at
`update_task.c:490` and `:493` update software bookkeeping only; the one
hardware feed in the firmware (`watchdog_task.c:172`) lives in a task that
provably cannot be scheduled while `flash_safe_execute()` holds both cores with
interrupts disabled (`flash.c:118-134`, `:136-190`, `:181-185`). With
`SAFTYFW_WATCHDOG_TIMEOUT_MS` at 1000 ms (`main.c:57`, armed at `main.c:320`) and
a 64 KB block erase specified up to 2000 ms — and ~2 s of near-continuous stall
across 13 blocks even at typical times — the reset is the expected behaviour of
the code as written, not an anomaly. The ESP's
`'Pico did not confirm RECEIVING within 15000 ms of erase'` is the downstream
symptom: the Pico rebooted mid-erase and never advanced from `ERASING`
(`update_task.c:739-741`) to `RECEIVING` (`update_task.c:767`), so the ESP's wait
at `ota_pico_relay.c:443-451` timed out.

Everything above except the per-block erase millisecond figures is verified from
source. The erase durations are inferred from the datasheet family for the Pico
module's flash; the repo's own "hundreds of milliseconds"
(`BOOTLOADER.md:280`) independently supports the same conclusion without the
datasheet, since 13 x "hundreds of ms" back-to-back already exceeds 1000 ms many
times over.

The clean result of the failure — relays open, no trip latched, config intact,
board back to `armed` — is the fail-safe working as designed. Nothing here
suggests the watchdog is too aggressive; it suggests the erase path was written
as if check-in bookkeeping were a hardware feed.

## 7. Remedies, ranked

### R1 (recommended). Erase in 4 KB sectors and feed the hardware watchdog from the erase loop, through the existing check-in gate

Two changes, both inside `update_task_erase_slot()` (`update_task.c:478-505`):

1. Drop `UPDATE_TASK_ERASE_CHUNK_SIZE` from 64 KB to 4 KB
   (`update_task.c:154`). 4 KB is the smallest erase unit the part supports and
   matches `HAL_FLASH_ERASE_SIZE`, which `hal_flash_erase()` already enforces
   alignment against (`hal_flash_pico.c:204-206`). The `_Static_assert` at
   `update_task.c:169-172` continues to hold: 0xD0000 is 208 x 4 KB exactly.
   This caps a single uninterruptible stall at the part's 400 ms sector-erase
   maximum, ~600 ms inside the 1000 ms watchdog.
2. Between sectors, call the hardware feed directly rather than hoping
   `watchdog_task` gets scheduled — but only after re-evaluating the same
   deadline gate `watchdog_task` uses, so the *semantics* of the feed are
   unchanged and only its owner moves. `watchdog_gate_all_within_deadline()`
   (used at `watchdog_task.c:151-162`) is already a pure function over an
   `entries[]` array, so this is a shared-helper extraction, not a new policy.
   If the gate says a task is overdue, do not feed — the erase loop then lets
   the watchdog fire exactly as `watchdog_task.c`'s else-branch would.

Cost: 208 sectors at a typical 45 ms is roughly 9.4 s of wall clock, against
~2 s today; at datasheet maximum it is ~83 s. Sector erase is less efficient per
byte than block erase, so this trades total update time for the bounded stall
that makes the update survivable at all. That is the right trade on a safety
processor.

Safety property suspended: none. The watchdog stays armed at 1000 ms throughout,
and it still only gets fed when every registered task is within its own
deadline. What changes is that the feed can be issued from `update_task` while
core 1 is repeatedly locked out, closing a window in which a *correct* system was
being reset. A genuinely wedged task still starves the watchdog, because the
gate is still consulted.

Risk to review: `update_task` runs at priority 1 on core 0
(`task_priorities.h:70`, `update_task.c:1374-1382`). Feeding from there means a
low-priority task can extend the watchdog window. The gate check is what keeps
that honest, and it must not be skipped. This deserves a dedicated negative test
— sabotage a task's check-in during an erase and confirm the board still resets.

ESP side: **yes, `RELAY_ERASE_TIMEOUT_MS` must move.** At 15000 ms
(`ota_pico_relay.c:111`) it is already below the ~9.4 s typical / ~83 s worst
case this remedy implies, and it is arguably too tight even for today's 64 KB
path at datasheet maxima. Raise it to at least 120000 ms, and update the comment
at `ota_pico_relay.c:107-111`, which currently derives its budget from the 64 KB
figure. Note this is a pure timeout widening on the ESP's *waiting* side: it
delays the report of a genuinely stuck Pico, it does not weaken any guard.

### R2. Keep 64 KB blocks, add only the in-loop hardware feed

Cheaper and keeps the update fast (~2 s of erase), but it does not fix the
failure. A feed between blocks resets the counter to 1000 ms at the start of each
block; a single block that runs past 1000 ms still resets the chip, and the
datasheet maximum is 2000 ms. This makes the failure rarer and harder to
reproduce rather than removing it — the worst possible outcome for a safety
processor. Recommended only as a strict subset of R1, never as an alternative.

### R3. Raise `SAFTYFW_WATCHDOG_TIMEOUT_MS`

**Argue against.** `SAFTYFW_WATCHDOG_TIMEOUT_MS` (`main.c:57`) is the bound on
how long the safety processor may sit wedged while relays are energized. It is
already load-bearing elsewhere in the tree — `spi_owner.c:41-58` sizes its own
blocking bound against this constant and warns about drift, and
`watchdog_task.c:62` sizes the 250 ms feed cadence to leave room inside it.
Raising it to cover a 2000 ms erase would more than double the window in which a
wedged Pico holds the relays in whatever state they were left, permanently, for
the sake of an operation that happens only during a firmware update. That is
paying a continuous safety cost for an occasional convenience. Reject.

### R4. Temporarily relax the watchdog for the duration of the update

For completeness, since the task asked it to be considered. The shape would be:
before the erase, re-arm the watchdog at a longer timeout (`watchdog_enable()` is
re-callable); after the erase, restore 1000 ms. The property suspended would be
"a wedged safety processor reboots within 1 s", degraded to whatever the relaxed
figure is, for the duration of one slot erase (seconds). What would still protect
the kiln in that window: the Pico's ACTIVE slot is untouched by the update
(`update_task.c:741` erases only the *target* slot, and
`BOOTLOADER.md:277` states the previous slot is never erased until the new one is
`VALID`), so the running guard image is intact; relays are required to be open
before an update starts (`BOOTLOADER.md:280-282` names this as a second,
independent reason); and the ESP's own link-loss trip still fires if the Pico
stops answering.

Even so I do not recommend it over R1. It requires the relaxed state to be
restored on every exit path, including error paths and the `update_task_revert_target_slot()`
route, and a missed restore leaves the safety processor permanently under a weak
watchdog with nothing to announce it. R1 achieves the same outcome with no state
to restore. If R4 is ever adopted, `RELAY_ERASE_TIMEOUT_MS` would need no change
(the erase stays ~2 s), which is its only advantage.

## 8. Side observation (a): the two CRC32 values

The ESP's number is computed in `firmware/KilnFW/App/drivers/http/ota_http_pico.c`.
`ota_pico_do_stage()` seeds `uint32_t crc = 0xFFFFFFFFu` (`ota_http_pico.c:179`),
chains `crc = esp_rom_crc32_le(crc, s_ota_pico_chunk, ret)` over every 4096-byte
chunk of the HTTP request body as it writes it to `pico_img`
(`ota_http_pico.c:193`), and applies a final `crc ^= 0xFFFFFFFFu`
(`ota_http_pico.c:208`). The function's own comment (`ota_http_pico.c:57-69`)
states the intent: poly 0xEDB88320 reflected, init and final XOR 0xFFFFFFFF —
i.e. **standard CRC-32 (IEEE 802.3 / zlib)**, over the entire uploaded file with
no header, padding, or truncation. That value is passed straight to
`ota_pico_relay_start(..., crc, ...)` (`ota_http_pico.c:224`).

The Pico computes the comparison value with `bootloader_crc32()`
(`firmware/SaftyFW/bootloader/crc32.c:19-30`), which is byte-for-byte the same
algorithm: table built from poly 0xEDB88320 (`crc32.c:12`), init `0xFFFFFFFFu`
(`crc32.c:25`), final `crc ^ 0xFFFFFFFFu` (`crc32.c:29`).

So the two are **not** expected to differ, and this is not a variant mismatch.
The ESP's number is a plain CRC-32 of exactly the bytes that were uploaded.

It is also compared downstream, which makes the discrepancy worth chasing rather
than dismissing: `update_task.c:850` CRCs the slot read back from flash with
`bootloader_crc32()` and `update_task.c:863` fails the update with
`UPDATE_STATUS_ERR_CRC_MISMATCH` if it disagrees with the header's `crc32` — the
value that originated on the ESP. If the two algorithms had differed, every
update would fail CRC at the END frame. They do not differ, so the observed
`0xBA38A716` vs `0x83c472ef` cannot be explained by algorithm, seed, or final
XOR. From code alone the remaining explanations are all about *which bytes*:
the file the building agent hashed with objcopy was not byte-identical to the
file that was uploaded, or the upload was truncated/extended (the ESP CRCs
`written` bytes, which it takes from `Content-Length`, `ota_http_pico.c:172-176`).
The ESP logs both length and CRC together (`ota_http_pico.c:221-222`), so
comparing the logged `bytes` against the on-disk file size is the fastest next
step. **Not resolvable from code alone; flagged, not closed.**

## 9. Side observation (b): boot_id 199 to 176

Not a counter, and not suspicious. The Pico generates it once at link-task start:
`firmware/SaftyFW/src/tasks/link_task.c:3086`,
`s_boot_id = (uint8_t)(time_us_64() ^ (time_us_64() >> 8));` — a value derived
from the microsecond timer at the moment the link task starts, truncated to 8
bits. Nothing persists it across a reboot; there is no flash or scratch-register
backing for it anywhere in `link_task.c`. It is used only as a change-detector,
never as an ordering: `link_task.c:1260-1263` computes `boot_id_changed` by
inequality against the previous value, and `LINK_PROTOCOL.md` section 4, quoted
at `link_task.c:352` and `:1251`, describes it as invalidating correlation
windows on change.

199 to 176 across a reboot is therefore entirely expected. A decrease carries no
meaning, and so does an increase. (The one thing this design cannot detect is a
reboot that happens to reproduce the same byte — a 1-in-256 collision — but that
is an accepted property of a change-detector, not evidence of a defect here.)
