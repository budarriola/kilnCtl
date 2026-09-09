# Dual-processor flash + commissioning attempt, 2026-09-09

## Summary

Both processors were built from a clean detached worktree and flashed. The
ESP flash verified fully. The Pico flash uncovered and required fixing a
genuine, previously-unknown FreeRTOS stack-overflow deadlock (see below) that
would otherwise have bricked the safety processor's link on every future
flash of `current_task.c`/`discrete_task.c`'s current code. After that fix
the deadlock is gone and the Pico runs, but a **second, separate** defect
was found: the Pico's periodic Frame A (`SAFETY_CMD_GET_STATUS`, sent
unsolicited every 500 ms by `link_task_send_status()`) never successfully
decodes on the ESP side, so `safety_link_up_locked()` never returns true and
full commissioning (Stage 5) could not proceed. **Commissioning was NOT
completed.** The board was left safe and idle throughout: relays off, not
enabled, no heat commanded, no firing/autotune/profile started at any point.

## Stage 1 -- clean build

Built from a detached worktree at `C:\wt\kilnctl_flash`, HEAD `316967b7`
(current shared-tree HEAD moved on to `bd2ad739` by another session during
this pass; unrelated to this work).

- `git worktree add --detach C:/wt/kilnctl_flash 316967b7`
- `git submodule update --init firmware/KilnFW/components/lvgl`
- Copied the shared tree's `firmware/KilnFW/sdkconfig` in and diffed --
  identical.
- SaftyFW: `cmake -S . -B build -G Ninja -DPICO_SDK_PATH=C:/pico-tools/pico-sdk
  -DFREERTOS_KERNEL_PATH=C:/pico-tools/FreeRTOS-Kernel -DPICO_BOARD=pico`,
  then `ninja -j 24` -- clean build, 462/462.
- KilnFW: ESP-IDF PowerShell profile, `idf.py -C firmware/KilnFW build` --
  clean build, `KilnCtrl.bin` 0x223360 bytes (29% partition free).

Both HEAD builds verified clean before any flashing.

## Stage 2 -- pre-flight (before touching anything)

- `get_fw_version`: ESP running `e8cfe344`, 42 commits behind HEAD (as
  expected, that's why we're flashing).
- `safety_fw_version`: SaftyFW `c13f8828`, protocol 12, config_version 132,
  config_crc 64098, boot_id 29.
- `trip_mask`/`warn_mask`: link up, armed, not tripped; `fault_status=0`.
- Thermocouples: CH0 37.59/CH1 37.65/CH2 37.49 C, all status 0, all valid.
- `relays: 0` (off).
- `get_heap_status`: **unacknowledged crash report** --
  `exc_task='profile_executo' exc_cause_str='IllegalInstruction'
  reset_reason='PANIC'`, `uptime_s=14304`. This is the same panic
  `379f3fe6` (today's `profile_executor` stack-overflow fix, part of this
  flash) addresses. **Not acknowledged** -- it did not block flashing, and
  the task brief said to acknowledge only if it did.

## Stage 3 -- flash the Pico, and a real stack-overflow deadlock

`debug_program(peer="pico", elf_path=".../build/SaftyFW.elf", confirm=true)`
from the clean worktree reported "programmed OK, reset and running" -- but
the link never came up. Diagnosis via SWD (`debug_read_registers`/
`debug_read_memory`, not the UART link):

- **core1 permanently parked** in `vApplicationStackOverflowHook`
  (`main.c:141`, `cpsid i; b .`), r1 pointing at the FreeRTOS task-name
  string of the overflowing task: `current_task`.
- **core0 permanently deadlocked** inside `xQueueGenericSend`
  (`queue.c:965`) spinning forever in `spin_lock_unsafe_blocking`
  (`hardware/sync/spin_lock.h:265`) waiting for a hardware spinlock that
  `current_task`'s overflow (which corrupted state mid-critical-section on
  core1) never released.

Confirmed reproducible: re-flashing the same unmodified ELF reproduced the
identical hang (`current_task` overflow observed twice).

**Root cause:** `current_task.c`'s `CURRENT_TASK_STACK_WORDS` was left at
bare `configMINIMAL_STACK_SIZE` while `CT_COMMISSIONING_PLAN.md` step 2's
auto idle-offset calibration state and the
`current_task_reload_cal()`/`current_sense_sample()`/`get_snapshot()`/
`get_power()` call chain grew the task's real stack depth past that margin.

**Fix applied** (`firmware/SaftyFW/src/tasks/current_task.c`):
`CURRENT_TASK_STACK_WORDS` raised from `configMINIMAL_STACK_SIZE` to
`configMINIMAL_STACK_SIZE * 6` (matching `link_task.c`/`safety_core.c`'s own
`*6` treatment for the same class of problem).

Rebuilt and reflashed. The deadlock changed shape: core1 now overflowed a
**different** task, `discrete_task`, at the identical
`vApplicationStackOverflowHook` location, with core0 deadlocked the same
way. **Fix applied** (`firmware/SaftyFW/src/tasks/discrete_task.c`):
`DISCRETE_TASK_STACK_WORDS` raised from `configMINIMAL_STACK_SIZE` to
`configMINIMAL_STACK_SIZE * 4`.

An intermediate attempt raised `configMINIMAL_STACK_SIZE` itself (256->512
words) globally in `FreeRTOSConfig.h` rather than patching tasks
individually. **This was wrong and was reverted**: every `*N`-multiplied
task stack (`link_task`/`safety_core`/`current_task`'s `*6`,
`update_task`'s `*3`, `log_task`'s `*2`) scales off that same constant, so
the global bump inflated total static stack allocation by roughly 29 KB, not
the ~5 KB intended, and traded the stack overflow for `pvPortMalloc()`
failing during startup -- core0 parked forever in
`vApplicationMallocFailedHook` instead. Reverted `configMINIMAL_STACK_SIZE`
back to 256 and fixed only the two tasks that actually needed it. Also
raised `configTOTAL_HEAP_SIZE` 32K -> 40K for headroom given the two
per-task bumps (RP2040 has 264 KB SRAM total; ample margin).

After the per-task fix + rebuild + reflash: SWD register reads on both
cores show normal, changing PC values across repeated samples (not parked
in either hook) -- **the deadlock is confirmed fixed.**

Files touched (both the shared tree and the build worktree, kept in sync):
- `firmware/SaftyFW/src/tasks/current_task.c`
- `firmware/SaftyFW/src/tasks/discrete_task.c`
- `firmware/SaftyFW/FreeRTOSConfig.h`

## Stage 4 -- flash the ESP

`flash_firmware(kiln_fw_root="C:/wt/kilnctl_flash/firmware/KilnFW",
verify=True)`: **flashed and verified OK** (bootloader + partition table +
app), provenance HEAD `316967b7` tree clean (via the `kiln_fw_root`
override), post-flash verification confirmed the board is running
`factory` with the matching build. `get_fw_version` afterward: board
running HEAD `316967b7`, `board/HEAD comparison: OK`.

## Stage 4.5 -- the second, unresolved defect: Frame A never decodes

With the Pico deadlock fixed, the link partially recovered:
`cmd_fw_version_count` and `cmd_trip_event_count` climb steadily (FW_VERSION
request/reply and the Pico's TRIP_EVENT push both work), but
**`cmd_status_count` stayed at 0 for the entire remainder of the session**
(hundreds of poll cycles, `crc_errors` and `timeouts` climbing throughout).
`link_task_send_status()` (SaftyFW `link_task.c`) broadcasts Frame A
unsolicited every 500 ms regardless of request/reply state, so it should be
arriving continuously -- it is not being successfully decoded on the ESP
side. Consequently `safety_link_up_locked()` (`safety_link.c:259`, gated on
`link->ever_received` being set by a **Frame A** reply specifically) never
returns true, `safety_get_status`/`safety_get_diag` report "never received"
throughout, and the board never reaches `link_up=1`.

This is a **separate defect from the stack overflow**, not investigated
further given the scope/time budget of this pass and the risk of further
speculative changes to safety-processor link code without being able to
observe the actual wire bytes (no logic analyzer session was available --
the `saleae` MCP server was not connected this session). It should be
picked up as its own investigation; a Saleae capture of the Pico TX line
during the STATUS broadcast window is the natural next diagnostic step.

**One thing this session's Frame B (TRIP_EVENT) traffic does let us
confirm safely:** the Pico's `SAFTYFW_ESP_LOGW` trip log decoded
`trip_reason 0x06` on every occurrence, every time, across ~189 TRIP_EVENT
frames received (`cmd_trip_event_count`). `SAFETY_TRIP_MAIN_FAULT = 6`
(`safety_guards.h`) -- this is an **enum value**, not the bitmask
`trip_mask` the full V2 status frame would report (where the same fault is
bit 6, `0x0040`). Every trip observed decoded to exactly this one value,
consistent with the expected, benign S6a mainFault from the dual-processor
reset racing the link handshake -- **no evidence of any other guard
tripping.** However, because the full status frame (which carries the real
`trip_mask`/`warn_mask` bitfields) never decoded, this could not be
independently corroborated the way the task brief asked, and
**`safety_clear_trip()` was deliberately not called** -- the tool's own
guidance is explicit that clearing a trip before `link_up` is confirmed
"will just re-trip if the link isn't actually up yet," and `link_up` never
went true this session.

## Stage 5 -- commissioning: NOT ATTEMPTED

Blocked by Stage 4.5: with `link_up` never true and the full status frame
never decoding, there is no reliable way to read back a written config
parameter to confirm it landed (the read-back discipline this repo requires
for exactly this reason -- see `project_safety_calls_logging_unchecked_
success` class of bug). No `SAFETY_CMD_SET_CONFIG` calls were made.
`commissioned`/`calibration_missing` state is therefore **unchanged** from
before this session.

## Advisory notes (from a concurrent opus review, relayed mid-task; not
## caused by and not fixed in this pass)

1. **ESP-side commissioning mirror was not updated by `b5cb83a4`.**
   `readiness_http.h:136`'s `readiness_param_required_for_commissioning()`
   still keys only on `ct_installed`, and `readiness_http.c:615` passes
   only that -- no `ct_topology` check. So even once the Pico reports
   `commissioned=true`, the ESP's own readiness item 10a will still count
   `ct_channel_map[0..2]` plus `i_normal_a[0..2]` as six
   applicable-and-unset parameters. This is advisory only -- the firing
   interlock (`d5170d54`) gates on `safety_trip`/`recovery_mode`/
   `crash_report`/`estop_verified`, not on item 10a, so it does not block
   heat by itself. Moot for this session since Stage 5 was not reached, but
   worth recording: **do not treat a future Pico `commissioned=true` as the
   whole picture** -- check the ESP's own readiness view too.

2. **The seqlock in `config_store_flash.c:230-283` has a confirmed
   defect.** Its fallback snapshot `s_last_good_record` is written
   unsynchronized by every successful read, and readers run on both cores,
   so a trip-path reader that exhausts its retries can copy a struct
   another core is mid-write on. Still a net improvement over the plain
   unsynchronized assignment it replaced -- flashing this HEAD was fine --
   but this is **not fully race-free**, and a fix is being dispatched
   separately (expect another Pico flash after this one).

## Board state at end of session

- ESP: running HEAD `316967b7`, verified. Relays off (`relays: 0`).
  `cfg` LittleFS partition confirmed **mounted, not reformatted**
  (524288 B total, 77824 B used, 446464 B free, 6 files:
  `display_power.dat`, `ramp_assist.dat`, `relay_cycles.dat`,
  `relay_names.dat`, `unit_pref.dat`, `zones.json`). Thermocouples all
  valid (~37.2-37.3 C, ambient). No firing, no autotune, no profile ever
  started this session. The pre-existing `profile_executor` panic crash
  report remains **unacknowledged** (informational, predates this session,
  did not block flashing).
- Pico: running the two-commit-worth stack fix above, protocol 12,
  `config_version 132`, `config_crc 64098` (unchanged -- no config writes
  were made). Link is **not** up (`link_up=0`); `fault line asserted (by
  us)` throughout, consistent with the expected S6a trip that was never
  cleared because link-up could not be confirmed.
- `commissioned`/`calibration_missing`: **unchanged, commissioning not
  attempted.**
- No relay was energized, no heat was commanded, K4 was never tested, at
  any point in this session.

## What was stopped on, explicitly

Stopped at Stage 4.5/5: the Pico's Frame A status broadcast does not
successfully decode on the ESP, so `link_up` never becomes true, so neither
the trip-clear step nor any commissioning config write (which requires
read-back verification) could be performed safely. This is a genuine,
newly-discovered firmware defect distinct from the stack-overflow deadlock
that was fixed and verified in this same session, and it needs its own
investigation (ideally with a logic-analyzer capture of the Pico's UART TX
during the STATUS broadcast window) before commissioning can be completed.
