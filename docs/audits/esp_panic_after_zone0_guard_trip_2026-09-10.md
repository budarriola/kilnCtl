# ESP panic immediately after a per-zone thermal-guard trip (2026-09-10)

Follow-up to `docs/audits/pico_double_reboot_investigation_2026-09-10.md`
(finding 3): during that session's reproduction attempt on `cplval45`-shaped
profile #7, the Pico did not reboot, but the **ESP panicked**
(`esp_reset_reason=4`) immediately after zone 0's per-zone thermal guard
tripped and abandoned the firing. That was out of scope there; this document
covers it.

## Stage 1 — evidence preserved before flashing

Board on connect: ESP `commit=0dddd435`, tree clean, built
`2026-09-10 00:09:57Z` — **35 commits behind HEAD** (`3120cca5` at the time
of connection; the repo's `main` advanced further during this session).

- `get_heap_status`: `reset_reason='panic/exception' (unclean boot)`,
  `uptime_s=422`. No "UNACKNOWLEDGED CRASH REPORT" banner was raised by this
  call (same non-firing observed by the prior session on this build).
- `get_device_log(n=200)` recovered the causal sequence directly from the
  ESP's own console log:
  ```
  profile_executor: zone 0 feedforward ON: K_dc 39.25 C/duty, tau 263.8s
  profile_executor: zone 1 feedforward ON: K_dc 31.97 C/duty, tau 269.8s
  profile_executor: zone 2 feedforward ON: K_dc 31.68 C/duty, tau 270.9s
  profile_executor: profile 'cplval45b' (id 7, zone_mask 0x07) running
  ... (~2 min of heartbeats) ...
  profile_executor: zone 0's heat is blocked: sources 0x20
  profile_executor: zone 0 thermal guard tripped (per-zone): heating but
    rose only -0.21C in 1.0min (need >=0.50C)
  heat_enable: heat-enable released: safety processor asked to drop heating (K4)
  profile_executor: zone 0 per-zone trip abandoned the whole firing
    (continue_on_zone_trip is off)
  app_main: esp_reset_reason=4 (PANIC); free_heap=8567220 free_spiram=8347448
  uart_log_bridge: 18 log line(s) dropped (queue full)
  app_main: internal heap: total_free=251803 ...
  app_main: COREDUMP PRESENT in flash from a previous crash -- decode with
    espcoredump.py info_corefile/dbg_corefile against build/KilnCtrl.elf
  ```
  The panic landed in the same log-flush window as `uart_log_bridge`
  dropping 18 lines to a full queue — the actual panic backtrace/register
  dump (which normally prints via the same path) was lost. This is a real
  evidence gap, not a decoding failure.
- `debug_check_partition_table`: on-chip table matches `partitions.csv`.
- **No matching ELF exists to symbolize the coredump.** Every file in
  `firmware/KilnFW/build/elf_archive/` and the current
  `firmware/KilnFW/build/KilnCtrl.elf` were grepped for the running build's
  commit string `0dddd435`; none matched. Per this repo's standing rule
  (never symbolize against an ELF that doesn't match the running build's
  embedded timestamp/commit), **the coredump was not decoded.** The
  causal chain above comes entirely from the plain-text ESP_LOG lines,
  which is sufficient to identify the code path without the coredump.
- `safety_get_status` / `safety_get_diag`: link up, armed, `trip_mask
  0x0000`, `trip_reason 0` — the safety processor was never involved; this
  is purely an ESP-side event, consistent with the prior session's
  observation that the Pico stayed healthy throughout.
- `profiles_get_exec_status`: `state=0` idle, no active run — the board
  was already left safe from the prior session's stop.

## Stage 2 — why the panic happened, and why the guard tripped

### The panic path

`profile_executor.c`'s `escalate_guard_trip()` → per-zone branch (
`firmware/KilnFW/App/drivers/control/profile_executor_relay_io.c:696-798`)
sets `s_exec.state = PROFILE_EXEC_FAULTED` when `continue_on_zone_trip` is
off (the default). The executor's main loop
(`profile_executor.c:367-381`) reacts to `DONE`/`FAULTED` by calling, in
order, `firing_stats_maybe_finalize()`, then (outside the lock)
`firing_stats_persist()` and `adaptive_tune_run_end()` — **exactly** the
run-end sequence that a 2026-09-09 panic
(`docs/audits/executor_panic_stack_overflow_2026-09-09.md`, fixed by commit
`379f3fe6`, "Fix profile_executor stack overflow; add its missing budget
check") diagnosed as overflowing the executor task's 4096 B stack.

**This is NOT that same defect resurfacing on a build that never got the
fix.** Checked directly:

```
git merge-base --is-ancestor 379f3fe6 0dddd435   # true
git log --oneline 0dddd435..<HEAD> -- '**/profile_executor*' '**/adaptive_tune*' '**/firing_stats*'
# (empty -- no further changes to these files between the board's commit and HEAD)
```

The board's running commit `0dddd435` (built `2026-09-09 17:04:07 -0700`,
i.e. `2026-09-10 00:09:57Z`) **already contains** `379f3fe6` (committed
`2026-09-09 13:53:58 -0700`, earlier the same day) — `firing_stats_persist()`
already heap-allocates its blob, and no profile_executor/adaptive_tune/
firing_stats file changed again between the board's commit and the current
tip. So this run-end sequence on this exact build was already exercised by
yesterday's fix and, per that fix's own accounting, its deepest path (this
one) should sit at 1168 B, well under budget.

**Conclusion: this is either a new, different overflow (a still-unbudgeted
task, or a new deep call added elsewhere) or a non-stack panic entirely** —
not a resurfacing of the fixed defect. Without the coredump (no matching
ELF) and without the dropped panic backtrace, the exact panic cause
(exception type, PC, faulting task) could not be determined this session.
Flashing to current HEAD and deliberately reproducing, with the log queue
watched live so the backtrace isn't lost to the drop this time, is the next
diagnostic step (see Stage 3).

### Why the thermal guard tripped

`thermal_guard.c`'s guard 1 ("climbing") branch
(`firmware/KilnFW/App/drivers/control/thermal_guard.c:232-283`) requires a
zone that is below setpoint by more than `progress_band_c` to rise at least
`rate_cfg * elapsed_min` within a window `window_s`. That window defaults to
300 s (`PROGRESS_WINDOW_S`), but an operator-configurable
`wrong_dir_window_s` **now overrides it for both the climbing and falling
branches** (a deliberate 2026-08-29/09-07 fix, already on this board's
build, that closed a gap where a configured "how long may heat be commanded
without a response" setting only applied to the falling-while-heating case).
The trip fired at **~2.06 min into the firing** with the message "in
1.0min" — i.e. `wrong_dir_window_s` is configured to ~60 s for this
zone/profile, not the 300 s default.

Zone 0's own feedforward parameters, logged at firing start, give
`tau ≈ 264 s`. Demanding a confirmed 0.5 °C rise within a 60 s window on a
plant with a ~4.4-minute time constant, starting from cold with feedforward
still ramping duty up, is a substantially tighter ask than guard 1's
original 300 s design point — and the actual reading was a **drop** of
0.21 °C (sensor/CJ noise on a nearly-flat true trajectory this early, not a
real temperature fall). This reads as the guard doing exactly what it was
configured to do, on a configuration that is too aggressive for this zone's
actual thermal time constant: a 60 s "wrong direction" window is a
reasonable ceiling for catching a genuinely dead element quickly, but it is
being applied here to guard 1's *rise* requirement as well, on a zone whose
honest early-firing slope is well below 0.5 °C in any 60 s window regardless
of health. **This is worth flagging as a live-config issue independent of
the panic**: either `wrong_dir_window_s` should not be this small for a
~264 s-tau zone, or guard 1's climbing branch should keep its own longer
window when an operator's `wrong_dir_window_s` was sized for the
falling-while-heating case instead. This was not fixed this session (out of
scope — it is a live commissioning value, not a code defect), only
identified.

## Stage 3 — flashing the ESP to HEAD: BLOCKED

Built from a clean detached worktree (`C:\wt\espfix`, `git worktree add
--detach` against `origin/main`, submodules initialized, `sdkconfig` copied
from the main tree and diff-confirmed identical) via the PowerShell
ESP-IDF profile + `idf.py build` (per this repo's documented invocation,
never plain Bash/idf.py, which silently no-ops here).

**`origin/main` does not currently build.** Two separate compile failures
were hit in succession, each corresponding to a commit whose matching
header/companion change had not yet landed:

1. At `origin/main` `6d7454e8`: `safety_cfg_store.c` referenced
   `safety_cfg_diff_entry_t`/`SAFETY_CFG_STORE_DIFF_MAX`, undeclared in
   `safety_cfg_store.h` at that commit. A subsequent fetch
   (`origin/main` advanced to `9f5fe0d1`, a merge commit) resolved this.
2. At `origin/main` `9f5fe0d1`: `safety_cfg_http.c:1879` references
   `s8_rate_guard_zone_input_t.coupling_provenance_ok`, which does not
   exist in the `s8_rate_guard_zone_input_t` struct
   (`s8_rate_guard_estimate.h`) at this commit. The main tree's own
   uncommitted working copy of `s8_rate_guard_estimate.h`/`.c` (320 lines
   of diff across the pair, `zones_current_sweep_engine.c`/`_task.c` also
   touched) already carries this field and a substantial amount of related
   logic — this is an in-progress feature from another active session,
   pushed in a state where the consuming commit (`safety_cfg_http.c`)
   landed on `origin/main` ahead of the producing header/impl change that
   is still local, uncommitted WIP on the shared main tree.

Per standing practice (never appropriate another session's uncommitted WIP,
never hand-patch around an in-flight multi-file feature), this was **not**
patched around. Repeated `git fetch origin` over the course of this session
did not turn up a fix landing. **Flashing to HEAD, the reproduction attempt
on current firmware, and the 45 °C plateau measurement (Stage 4) are
therefore blocked on `origin/main` regaining a green build**, and were not
attempted against the stale `0dddd435` build already on the board (that
build is the one already implicated in the panic under investigation, and
flashing anything other than a genuinely current, building HEAD would not
advance this task's goal of landing the 35 outstanding commits).

## Stage 4 — not attempted (blocked by Stage 3)

The pre-registered hypotheses from
`docs/audits/cplval45_scale_vs_offset_aborted_2026-09-10.md` still stand and
are restated here for the next session that picks this up:

- if the coupling-matrix error is a per-row **SCALE** error: z2 forward
  residual ≈ **−2.9 °C** at the 45 °C plateau
- if it is a constant **OFFSET** error: z2 forward residual ≈ **−8.4 °C**

No plateau data was collected this session.

## Board state left at end of session

Unchanged from the prior session's end state (this session made no live
firing attempts, only diagnostic reads): `profiles_get_exec_status`
`state=0` idle, no active run; safety link up, `trip_mask=0x0000`,
`trip_reason 0`; no relay/profile action taken. **The board was left exactly
as found: safe, idle, no trip latched, still running the stale `0dddd435`
build** (35+ commits behind, pending a future flash once `origin/main`
builds again).

## Next steps

1. Wait for (or help land, in a separate, scoped session) whichever
   in-progress commit completes the `coupling_provenance_ok` /
   `s8_rate_guard_zone_input_t` feature, then confirm `origin/main` builds
   clean before retrying.
2. Flash to that HEAD via `flash_firmware(kiln_fw_root=...)`, verify the
   expected S6a-only trip on reset, confirm `cfg` LittleFS still mounts and
   Pico commissioning (S1/S8=20 °C/min) is intact.
3. Deliberately reproduce the per-zone-trip → panic sequence with
   `get_device_log` polled tightly around the trip so the backtrace is not
   lost to the log-bridge queue drop this time; if it recurs on current
   HEAD, that rules out "already fixed upstream" and this becomes its own
   priority defect.
4. Separately, review `wrong_dir_window_s` on zone 0 (and the profile that
   set it) — a 60 s window against a ~264 s-tau zone is a plausible source
   of spurious guard-1 trips independent of anything found in the panic
   itself.
5. Once the board is confirmed stable on current HEAD, retry the 45 °C
   plateau measurement per the aborted document's protocol.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
