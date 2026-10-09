# cplval75 heating capture — ABORTED at 7 min. Two independent blockers.

**Date:** 2026-09-09, 10:31–10:41 local.
**Capture:** `logs/coupling/cplval75_20260909.jsonl` (5 s `/api/profile_exec` poll,
same `HH:MM:SS {json}` format as the other `logs/coupling/*.jsonl` captures) and
`logs/coupling/cplval75_20260909_ambient.tsv` (60 s ambient/safety/relay sidecar),
distilled for review into `logs/coupling/cplval75_20260909_observations.tsv` in the
style of `coupid6_dwell_observations.tsv`. The raw `.jsonl` stays local and
uncommitted — `.gitignore:100` excludes `logs/**/*.jsonl`, and every other coupling
capture follows the same convention; only the derived TSV/MD artifacts are tracked.
**Firmware running:** ESP `e8cfe344` built 2026-09-09 14:52:22Z (clean, `dirty=false`);
Pico `c13f8828`. **Nothing was flashed by this session.**

The run did **not** produce a settled hold point. Neither the coupling-matrix
prediction nor the `(T_hold, u_steady)` question is answered here. It got 397 s in
and stopped. Two separate things are wrong, and either alone is disqualifying.

## Blocker 1 — the `profile_executor` task panicked

At ~10:38:20 (≈400 s into segment 0) the ESP panicked and rebooted. The capture
shows two `POLL_ERROR` timeouts at 10:38:26/10:38:34 and then `state=idle` with
`last_run.present=false` — the executor state was gone, i.e. a reboot, not a stop.

```
GET /api/crash_report
  present=true acknowledged=false
  exc_task="profile_executo"  exc_cause_str="IllegalInstruction"
  exc_pc=0xfffffffd  exc_addr=0x00000000  backtrace_corrupted=true
  found_on_boot_reset_reason="PANIC"
GET /api/status  reset_reason="panic/exception"  uptime_s=128
```

`exc_pc=0xfffffffd` with `exc_addr=0x0` and a corrupted backtrace is the same
signature as the 2026-09-04 `safety_poll` panic recorded in CLAUDE.md, which was
**not** a real illegal instruction but a deliberate `abort()` from a FreeRTOS
`configASSERT`, reached through corrupted static storage. Treat `exc_addr 0x0` as
a red herring here too. There is nothing to symbolize from the frame itself.

**The crash report has deliberately been left UNACKNOWLEDGED** so
`capability_preflight` keeps refusing runs until someone reviews it.

Context that is probably relevant and needs a human to adjudicate: at the time of
this run the working tree carried another session's uncommitted modifications to
exactly the code that runs in this task —
`profile_executor_run.c`, `profile_executor_feedforward.c`,
`profile_executor_relay_io.c`, `zone_coupling_solve.c/.h`. Those edits are **not**
in the flashed image (`e8cfe344` is clean), so they did not cause this panic, but
they mean the panicking module is mid-rework by another pass and any fix must be
coordinated with it rather than applied on top.

## Blocker 2 — full duty, zero thermal response

Independently of the panic, **the kiln never heated.** Over the whole 397 s the
three zone thermocouples sat flat at ambient while the controller wound duty up
to saturation:

```
t=  3s  z0 31.78C d=0.011   z1 31.69C d=0.061   z2 31.64C d=0.127
t=373s  z0 31.96C d=0.561   z1 31.98C d=0.840   z2 31.82C d=1.000
t=397s  z0 32.03C d=0.601   z1 31.94C d=0.911   z2 31.80C d=1.000
```

Zone 2 held **duty 1.000 for the final ~90 s with no measurable rise at all.**
Enclosure temperature moved 32.03 → 32.19 C across the whole window, consistent
with nothing but the board's own idle self-heating.

This is not a control-tuning artifact. The relays **did** actuate — `relay_on` was
true in 18/28/36 of the 80 running samples for z0/z1/z2, and the 60 s sidecar
caught relay masks `0x02` and `0x06`. So the firmware asked for heat, the relay
outputs followed, and no heat arrived. The most likely explanations, in order:

1. Heater mains power is not present at the bench (breaker/plug/contactor open).
2. A break downstream of the SSR outputs — element or wiring.

For comparison, `coupid6` on this same rig reached 70 C, so this is a change of
bench state, not a standing limitation.

**The diagnostic that would have separated these is CT current.** `ct_counts` read
`16, 17, 60` before the run and `16, 17, 61` after, both with relays off, and the
sidecar did not sample `ct_counts` during the run — an omission worth fixing in the
logger before the retry. The summed CT on GPIO28 (see
`project_ct_sensor_on_gpio28`) should show current whenever a relay is on; if it
reads flat while `relay_on` is true, that confirms explanation 1 or 2 immediately.

## Pre-flight (all of it passed — the run was correctly authorized to start)

| Check | Result |
| --- | --- |
| Board reachable | yes, 192.168.1.156 |
| `get_heap_status` | clean, no crash banner, uptime 9336 s |
| `recovery_mode` | `false` |
| Prior crash report | `acknowledged=true` (a stale, already-reviewed record) |
| Safety link | up, `state armed`, `trip_mask 0x0000`, `warn_mask 0x0000` |
| Thermocouples (4) | 31.70 / 31.69 / 31.58 / 31.62 C, `status=0`, no faults |
| Relays | all four off; executor idle; autotune idle |
| ESP zone `max_temp_c` | 80.0 C on all three zones |
| Pico `abs_max_temp_c` | 80 C (S1 ARMED) — equal to the ESP, not tighter |

Ambient at start was ~31.6–31.7 C, not the ~29–30 C the task anticipated; the room
is simply warmer. The 75 C target against an 80 C ceiling on both processors is
the required 5 C of headroom, so the profile itself was correct to run.

## Profile used

Saved as user slot **#0 `cplval75`**, `zone_mask=0x7`, overwriting the superseded
`cplval70` in that slot. No builtin schedule was touched.

```
segment 0: target=62.0C ramp=60.0C/hr dwell=30min
segment 1: target=70.0C ramp=40.0C/hr dwell=30min
segment 2: target=75.0C ramp=40.0C/hr dwell=45min
```

The dwells were sized against the ~265 s zone time constant: 30 min ≈ 6.8 τ at the
intermediate plateaus and 45 min ≈ 10.2 τ at the 75 C hold, comfortably past the
8 τ the `dc_gain_factor_of_ten_2026-09-09` audit asks for, with the last 10 min
reserved for duty averaging. **The profile is fine and should be reused verbatim
once the two blockers are cleared.**

## Answers to the questions the run was supposed to answer

- **Settled hold point:** none reached. Zero plateaus attained; the run died on the
  first ramp having never left ambient.
- **Coupling-matrix prediction:** untested. No temperature excursion occurred, so
  there is no observed duty to compare against a prediction.
- **`ff_hold` infeasible:** **never**, in any of the 80 running samples, on any
  zone. `ff_hold_used_matrix` was true throughout. Consistent with
  `ff_hold_infeasible_above_62c` — the run never got near 62 C.
- **Ambient drift:** +0.25 C, enclosure 32.03 → 32.28 C over the ten minutes,
  recorded at both ends and every 60 s in between.

## State left behind

Firing stopped (`profiles_stop` returned ok), all four relays read **off** via
`/api/status` and `get_board_state`'s `io.relays=0`, executor idle, autotune idle,
safety armed with `trip_mask 0x0000`. No trip latched at any point and none was
cleared. Zones cooling back through 32.0 C. The capture logger exited on its own
idle-detect; no stray process is polling the board.

One unexplained observation for whoever picks this up: the Pico's
`safety_config_version` moved 131 → 132 and `config_crc` 13756 → 64098 during the
ten minutes of this run. This session wrote nothing to the safety processor, so
another session is commissioning it concurrently.

## Recommended order for the retry

1. Establish why no heat reached the kiln — check bench mains first, and add
   `ct_counts` to the capture sidecar so the next run answers this in seconds.
2. Review the `profile_executor` panic with the session that owns the in-flight
   edits to that module. Do not start another multi-hour firing on a build whose
   executor task aborts after seven minutes.
3. Re-run `cplval75` (slot #0) unchanged once both are resolved.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
