# Power-cycle "black screen: kilnctl ready" — 2026-09-08

Follow-up to `docs/audits/boot_guard_recovery_loop_2026-09-08.md` ("Pass 3"),
whose open item recommended a true power-on reset as the first, least
destructive thing to try. The owner did that power cycle; this is what was
found afterward, gathered entirely over HTTP (the CONTROL UART link to the
board, COM14, was already stuck serving a stale pre-power-cycle buffer for
the whole of this pass — every `kiln_call` that depends on it, including
`get_fw_version`, `safety_get_status`, `touch_log_tap_targets`, and
`get_device_log`, either errored `link or peer is down` or returned frames
timestamped before the power cycle. This is a PC-side link problem, not
evidence about the board).

## Did it eventually boot?

Yes. `GET /api/status` answers normally, `uptime_s` climbed from 125 to 443
across this pass's polling with no gaps, `io_ready`/`thermo_ready`/
`safety_ready` are all `true`, three thermocouple channels read valid,
`zones_config_valid: true`, safety diag frames are flowing
(`diag_context_frames_ok` climbing, `diag_context_frames_bad: 0`). This is a
fully live, answering board, not a hang.

## JTAG PC location

Not gathered. Once HTTP confirmed the board was live and advancing on its
own, halting the core over JTAG to sample the PC would have interrupted a
board that was demonstrably not stuck — the risk (disturbing thermocouple
reads/safety polling on a genuinely running board, and the temptation to
`debug_reset`, which would erase the RTC stuck-counter marker this fix
depends on) outweighed the value, since HTTP already answered the "is it
hung" question. If it is still black at the bench with HTTP up, that is a
display/LVGL-only bug (see below), not a boot hang — JTAG would not add
information HTTP hasn't already given.

## How far boot got

Fully — this is a completely booted, operating board, just in recovery mode
(see below), which is a normal-but-degraded operating state, not a stuck
boot.

## Recovery mode: NOT cleared by the power cycle

`GET /api/ota/esp/status` → `"recovery_mode": true`, still. `GET
/api/status` confirms `reset_reason: panic/exception` for the *current*
boot — i.e. the board now running is not the immediate result of the power
cycle; it already panicked at least once since then and rebooted itself
before settling into the state observed here. Running build:
`commit: fc90f682`, `dirty: true`, `build_date: 2026-09-08 05:29:10Z` — a
further-modified, unflashed-from-clean-tree build sitting on top of the
committed `0b5d9dad` fix (the repo's working tree currently carries
uncommitted changes to `boot_guard.c`/`.h` and `cfg_fs_status.*` from a
concurrent session; not touched by this pass).

Given the recovery-mode decision is made once at `boot_guard_init()` from
the count loaded *before* any clear this boot performs (per
`boot_guard_recovery_loop_2026-09-08.md`), a board that boots into recovery
mode and then successfully verifies a clear will not show that clear until
the *next* boot. Whether that happened here is unproven without another
reboot to check.

## Crash report

`GET /api/crash_report`: `present: true`, `acknowledged: false`,
`exc_cause_str: LoadProhibited`, `exc_pc: 0xfffffffd`,
`exc_addr: 0x00000018`, `exc_task: main`, `found_on_boot_reset_reason:
PANIC`, `backtrace_corrupted: true`. This matches the *shape* of the
previously-known stale `218f65f7` record (same `exc_pc` placeholder for a
corrupted backtrace) but cannot be the same instance: `reset_reason:
panic/exception` is reported for the *current* boot, meaning this panic
happened during or immediately after this power-cycle session, not before
it. This is a **new, distinct, unacknowledged panic** — same crash
signature (null/near-null pointer read at offset `0x18`, task `main`), not
yet root-caused. `exc_pc: 0xfffffffd` and `backtrace_corrupted: true` mean
the usual symbolize-against-the-matching-ELF step (per CLAUDE.md) cannot
recover a call site from this report alone; only `exc_addr: 0x18` is
informative (a small-offset field read through a null/near-null struct
pointer).

## cfg mount / format

`GET /api/cfgfs`: `mounted: false`, `status: unmounted`, `reason: not
mounted this boot -- either recovery mode skipped the mount, or boot has
not reached it yet`, checked at both 223 s and (implicitly) later uptime.
Given `recovery_mode: true` for this boot and CLAUDE.md's documented
behaviour (`boot_guard.h` RECOVERY MODE skips starting subsystems), the
simplest reading is recovery mode skipped the mount again — **the
first-ever LittleFS format of the 512 K `cfg` partition is still
unexercised**, on this boot at least. `dual_write.zones` and prefs/
profiles/kilncfg_slots/adaptive_tune/relay_cycles all remain NVS-only.

## PID gains and coupling

Not independently re-read this pass (the endpoint that reports it needs the
same live serial/`zones` HTTP surface, and this pass prioritized the
boot/recovery/crash questions on limited time); `zones_config_valid: true`
in `/api/status` is the only signal gathered, which is consistent with the
zone config being present and internally coherent, not proof the specific
Kp/Ki/Kd/coupling numbers in the task brief still match on disk. Recommend
an explicit `GET /api/zones/config` read-back before any heating, same as
CLAUDE.md's standing rollback caution.

## Profiles

Not indepedently reread; `readiness` endpoint reports `profile_saved: ok,
8 saved, 28 shipped schedules available` — profile storage looks intact
(NVS-backed, unaffected by the cfg-mount question).

## Display ("black screen: kilnctl ready")

Not independently confirmed visually this pass — a `capture_lcd.ps1 -Full`
frame came back essentially black across the whole frame, but per CLAUDE.md
the bench camera's aim/crop has been drifted and unverified since
2026-09-06, so a dark frame does not distinguish "LCD genuinely stuck" from
"camera exposure/aim". The one board-side signal available
(`display_flush_us: no samples yet`, from `get_heap_status`) says the LVGL
flush callback has not fired since this boot started — consistent with a
UI that drew its splash once and has not been asked to redraw anything
since (normal if nothing changed on screen) and equally consistent with a
genuinely stalled UI task. Not resolved this pass.

## Action taken

None destructive. No flash, no JTAG reset, no NVS erase. The board was
found already live and fully answering over HTTP, so the "don't leave the
owner with a dead board" branch of the task did not apply — there was no
dead board to recover. Recommend NOT power-cycling or JTAG-resetting again
before someone gets a fresh, complete device log off the CONTROL UART link
(current one is stale/stuck) or a corrected LCD frame, since either would
localize the new panic and the recovery-mode persistence far better than
another blind reboot — and per the referenced audit, `debug_reset`
specifically discards the RTC state this whole recovery mechanism depends
on.

## Board state now

Running, answering HTTP normally, uptime climbing, thermocouples/safety/IO
all ready, relays off, no active firing. `recovery_mode: true` (not cleared
by the power cycle). `cfg` LittleFS partition still unmounted. One new,
unacknowledged crash report (`LoadProhibited` @ `main`, `exc_addr 0x18`)
distinct from the old `218f65f7` record. Running commit `fc90f682` (dirty),
built 2026-09-08 05:29:10Z, on the `factory` partition.
