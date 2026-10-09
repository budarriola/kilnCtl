# Reset-one-side-of-a-pair sweep — 2026-09-07

Scope: `firmware/KilnFW/App`, `firmware/SaftyFW`, `firmware/CommonFW`,
`tools/PcTools/src`. Method per CLAUDE.md: no mechanical check (rejected,
no unifying syntactic shape) — enumerate `*_reset`/`*_clear`/`*_init` sites
and explicit `= 0`/`memset` on counters, cursors, windows, timestamps,
seeds, then ask by hand: does another module / the other processor / the
PC hold a copy or a derived expectation of this value?

## Sites examined and classification

| Site | State reset | Other holder? | Class |
|---|---|---|---|
| `tools/PcTools/src/kilnctrl/serial_link.py` `UartLink.connect()` / reconnect, `_next_tx_index` | outgoing msg_index | firmware per-(device,task) dedup ring, boot-lifetime | **PAIRED-AND-HANDLED** — `_next_tx_index = _random_msg_index()` (not a restart-at-0); this is the fix for the originally-confirmed msg_index instance. Verified still in place. |
| `firmware/SaftyFW/src/tasks/link_task.c` `link_task_start()`: `s_msg_index=0`, `s_boot_id=<fresh random>`, `s_last_context_boot_id=0`, `s_trip_last_seq_seen=0` | Pico's own per-boot link state | ESP's `safety_link_frames.c` link struct, keyed off `pico_boot_id` | PAIRED-AND-HANDLED — all Pico-side counters restart together at boot alongside a fresh `s_boot_id`; ESP side detects the boot-id change and clears its mirrors (see next row). This is the producer side of the already-fixed instance. |
| `firmware/KilnFW/App/drivers/safety/safety_link_frames.c` `boot_id_changed` block (~line 249-311) | `link->cached.trip_last_seq`, `link->cached.trip_event_ever_received`, `link->safety_relay_state_known` | Pico's `s_trip_seq` (safety_core.c) restarts at 0 on Pico reboot; K4 relay state predates the reboot | **PAIRED-AND-HANDLED** (already fixed, 2026-08-27 audit per CLAUDE.md). Confirmed both `trip_last_seq` and `safety_relay_state_known` are cleared together on `boot_id_changed`, with an explicit comment invoking this exact bug class for both. No regression found. |
| `firmware/SaftyFW/src/tasks/update_task.c` `update_task_process_begin()`: `update_received_ranges_reset(&s_ranges, hdr.length)`, `s_gap_cursor=0`, `s_retransmit_round_count=0`, `s_pass_had_gap=false` | Pico-side OTA receive-window state | PC side (`ota_http`/kilnctrl uploader) has no local cursor into this — it only reacts to whatever gap report the Pico last sent | UNPAIRED (correct) — both counters that describe the *same* window (`s_ranges`, `s_gap_cursor`) are reset together in the one function that starts a new transfer; nothing external derives an expectation from the old value. |
| `firmware/SaftyFW/src/clear_trip_diag.c` / `watchdog_overdue_diag.c` `_clear()` | one hal_scratch register each | cached copy is refreshed by the same module's own `_read()`, single-writer-at-boot | UNPAIRED (correct) — self-contained diag registers, no cross-module or cross-processor consumer. |
| `firmware/KilnFW/App/drivers/control/profile_executor_run.c` `s_exec.segment_index = 0` (start) / `= plan.entry_segment_index` (warm-start) | current segment cursor | `profile_executor_firing_stats.c` reads `s_exec.segment_index` directly (same module, same lock), `run_state.c` snapshots it into `boot.segment_index` at persist/restore time | UNPAIRED (correct) — single owner, readers take a snapshot rather than keeping an independent derived cursor; warm-start path explicitly re-seeds `segment_index` alongside `dwelling`/`segment_elapsed_s`/`baseline_target_c` together (comment names this deliberately). |
| `firmware/KilnFW/App/drivers/control/zones_current_sweep_task.c` `s_sweep.zone_index = 0` | sweep cursor | no other module tracks sweep progress independently | UNPAIRED (correct). |
| `firmware/CommonFW/src/benchproto_link.c` various `_reset` | benchproto per-connection state | see feedback note "PC-side msg_index restarting per connection" — this is the **benchproto** analog of the same fixed class, already covered by the `serial_link.py` random-index fix above (benchproto rides the same `UartLink`) | PAIRED-AND-HANDLED (shared fix). |
| fault_sched.c / SimFW `apply_reset()` (seed / ring sequence instances) | — | — | **N/A — module deleted.** SimFW and `kilnsim` were removed 2026-08-28 (see project memory); the seed and telemetry-cursor instances no longer exist in the tree to regress. |

## Result

No new PAIRED-AND-BROKEN instance found. All four originally-confirmed
instances remain fixed on inspection of current source (two of the four —
SimFW's cursor and fault_sched's seed — are moot: the module was deleted
outright). The candidate CLAUDE.md flagged as "mid-edit elsewhere"
(`thermal_guard.c` window state vs. PWM-chopping behaviour) was left
untouched per the task's explicit instruction not to collide with that
other session's pass.

No fixes were needed this pass, so no new host tests were added for this
sweep specifically (the existing host-test suite already exercises the
boot_id-changed dedup-clear path in `test_safety_link_compile.c` /
`test_closed_loop.c`, and the msg-index randomization in the PC-side
serial_link tests).

## Narrow mirror-drift check: not proposed

CLAUDE.md's own reasoning holds up on this pass: every pair found is either
(a) already handled with a comment calling out the exact hazard, (b) fully
self-contained within one module/owner, or (c) moot (module deleted). The
one live open candidate (`thermal_guard.c` vs PWM-chopping) is explicitly
mid-edit in another session, so writing a narrow check against it now would
either duplicate or fight that fix — deferred, as instructed, to whoever
finishes that pass.

## Verification

`tools/run_all_checks.ps1` run (bypass execution policy) to confirm the
inspection-only pass left the tree green — no source changes were made.
