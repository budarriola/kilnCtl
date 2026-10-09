# Short heat proof run — completed, 2026-09-09

**Two owner decisions executed in order, then a ~13 min closed-loop heat proof.**
Capture: `logs/coupling/cplval75_proof_20260909.jsonl` (5 s poll of
`/api/profile_exec` + `/api/status` + `/api/control`, `http_capture_log.py`
envelope shape), distilled to
`logs/coupling/cplval75_proof_20260909_observations.tsv`. The raw `.jsonl`
stays local per `.gitignore:100`; only the TSV is tracked.

## Stage 1 — Pico reflash

Built SaftyFW from a clean detached worktree at HEAD (`a87672ab`,
`C:\wt\saftyfw_flash`, configured fresh with `cmake -G Ninja` against
`C:/pico-tools/pico-sdk` + `C:/pico-tools/FreeRTOS-Kernel`, then
`cmake --build`). Flashed via `debug_program(peer="pico", confirm=true)` to
COM14 (probe E66540F0A36C6E21). This carries the seqlock/ABA fallback fix
(`cb1ba325`/`b202fe56`/`985e41b4`), the fatal-fault diagnosability work
(`31741ece`), and S8's floor/ceiling bound (`f392bcf0`) — all ancestors of
`a87672ab`.

Post-flash: `safety_get_diag()` reported `trip_reason 6`
(`SAFETY_TRIP_MAIN_FAULT`, i.e. S6a) with `warn_mask 0x0000` — the expected
handshake-window trip. **Note for CLAUDE.md:** the observed `trip_mask` was
`0x0020`, not `0x0040` as documented. `link_frame.h`'s own comment says the
mask is `1 << (reason - 1)`, so reason 6 -> bit 5 (`0x20`), not bit 6
(`0x40`). The `reason` field itself unambiguously named S6a, so this is a
doc/formula mismatch, not evidence of an unexpected guard.

Cleared with `safety_clear_trip()`; confirmed `state armed`, `trip_reason 0`.
`cmd_status_count` (the `status` field of `safety_get_link_stats()`'s cmd
histogram) climbed 18888 -> 18908 over 7 s with link error counters flat.
Commissioning survived the reset: `commissioned=True`, S1 `abs_max_temp_c=80C`
ARMED, **S8 still `max_rate_c_per_min=20C/min` ARMED** (no reversion).
One oddity: `safety_get_diag()` reported `boot reason: watchdog` for what was
a deliberate OpenOCD/SWD reset, not a power-on or an actual watchdog event —
worth a look at `boot_reason.h`'s classification, though nothing downstream
was affected.

## Stage 2 — crash report preserved, then acknowledged

Before acknowledging, read `GET /api/crash_report` and recorded the raw
frame here (owner explicitly authorized acknowledgment; the frame is not
otherwise preserved anywhere once cleared):

```
present=true acknowledged=false
exc_task='profile_executo' exc_cause_str='IllegalInstruction'
exc_pc=0xfffffffd exc_addr=0x00000000 exc_a0=0x3fca0070 exc_a1_sp=0x3fcb2fe4
found_on_boot_reset_reason='PANIC' frame_trustworthy=false
backtrace=['0xfffffffd'] backtrace_corrupted=true
```

This is the same signature already root-caused as the `profile_executor`
stack overflow fixed by `379f3fe6` (three frames heap-allocated) — confirmed
an ancestor of the currently-running ESP build (`e8cfe344`/current HEAD).
Acknowledged via `POST /api/crash_report/ack` -> `{"ok":true}`; read back
`GET /api/crash_report` afterward and confirmed `acknowledged=true`.
`/api/readiness`'s `crash_report` item reads `ok`.

## Stage 3 — E-stop verified as currently wired

Used the existing write path, `POST /api/estop/verify`
(`estop_verification.c` / `diagnostics_http.c`'s `estop_verify_post_handler`)
-> `{"ok":true}`. Read back `/api/readiness` and confirmed `estop_verified`
flipped to `status: ok`, with the firmware's own detail text already
correctly scoped: *"confirmed by operator. Known gaps: welded contactor
undetectable; ACTIVE_LOW polarity can't see a cut signal line -- use
ACTIVE_HIGH default"*.

**This verification reflects the CURRENT bench wiring only** — pole 1
(contactor coil) is not yet in series with the line contactor, matching the
owner's exact framing. The second pole must be wired before any real firing
that depends on the E-stop actually cutting mains. Do not read this as full
verification of the completed interlock.

Confirmed the readiness gate's four blocking items
(`READINESS_GATE_BLOCK_RECOVERY_MODE/SAFETY_TRIP/CRASH_REPORT/ESTOP_VERIFIED`,
`readiness_gate.h`) were all clear before proceeding to Stage 4. The
separate `safety_commissioned` readiness item (3 of 65 applicable params
still unset) is NOT one of the four gate-blocking enums and was left as-is —
it does not block firing.

## Stage 4 — heat proof run

**Ambient at start:** 36.3–36.5 C on all three main-board thermocouples
(cooler than the 43–45 C the task brief anticipated from an earlier
session). **Target:** profile #0 `cplval75` (user slot, `zone_mask=0x7`,
unmodified — segment 0 target 62 C, ramp 60 C/hr, dwell 30 min), started
as-is with no edits to any user profile and no builtin schedule touched.
62 C clears the 80 C ceiling on both processors by 18 C and clears ambient
by ~26 C.

Started via `profiles_start(profile_id=0)`. Ran ~13 minutes
(19:56:14–20:09:25 local), then stopped deliberately (well short of the 62 C
segment target) once closed-loop rise was unambiguously demonstrated:

| t | z0 | z1 | z2 |
|---|---|---|---|
| start | 36.48 C | 36.49 C | 36.34 C |
| +4.5 min | 41.39 C | 41.53 C | 41.03 C |
| +9 min | 47.83 C | 47.73 C | 46.95 C |
| +13 min | 53.38 C | 52.02 C | 51.15 C |

~17 C rise in 13 minutes (~1.3 C/min, ~78 C/hr), well under S8's 20 C/min
trip threshold and never within 20 C of the 70 C abort line or the 80 C
ceiling. Duty was not saturated (temperature tracked the ramp, not stuck at
a plateau) — no abort condition was reached.

**Which CT channel responded — dispositive.** Idle baseline before the run:
`ct_counts = [16, 17, 64]`. Under load, channel 2 (GPIO28/J17, the
documented "summed" CT) swung to **150–172 counts**, cycling with the
relay's PWM window (readable in the TSV as ct2 alternating high/low every
few samples, e.g. 166 -> 63 -> 61 -> 154 across four consecutive 50 s-apart
samples) while channels 0 and 1 stayed flat at 16–17 throughout, matching
"not fitted." This directly confirms the owner's report: the CT the board
calls "not fitted" (in the sense of S14/S15 not commissioned) is not the
same question as which channel is physically wired — **channel 2 is
unambiguously the one physically responding to load**, consistent with
CLAUDE.md's own note and the schematic.

**Safety processor, watched continuously alongside temperature (via
`safety_get_status`/`safety_get_diag`/`safety_get_link_stats` at each
check-in, and via `status`'s safety fields in every 5 s capture line):**
`safety_relay_energized` (K4) went `true` for the run and `false` again
after stop; `state armed`/`trip_reason 0`/`trip_mask 0x0000` throughout, no
trips, no warns; `cmd_status_count` climbed monotonically the whole time
(18908 at run start area -> comfortably higher at every check-in, e.g.
context frames ok 1341 -> 1653 -> 1703); Pico uptime climbed continuously
(676 s -> 966 s -> 1218 s at three check-ins, i.e. no reboot); link error
counters (`crc/framing errors`, `resync`, `length/crc mismatch`) stayed flat
at their pre-run values throughout. No Pico instability at any point — the
one hard-abort-ranks-above-everything condition never came close to firing.

## Stop and final board state

`profiles_stop()` -> `ok`. After a 3 s settle: `GET /api/status` showed
`safety_relay_energized=false` and `ct_counts` back to idle baseline
(`[16, 17, 62]`, matching pre-run `[16, 17, 64]` within ADC noise) —
confirms relays are ACTUALLY off, not merely commanded off (the class of bug
CLAUDE.md records: stopping the host previously left the ESP still firing).
`profiles_get_exec_status()` -> `state=0` (idle), `segment=0/0`. Safety
processor: `state armed`, `trip_reason 0`, `trip_mask 0x0000` — no trip
latched. **Board left safe and idle.**

## Bottom line

- Pico flash verified; S6a expected-trip cleared; S8 (20 C/min) and S1
  (80 C) both survived the reset commissioned and ARMED.
- Crash frame preserved above before acknowledgment; acknowledgment
  confirmed via read-back and `/api/readiness`.
- E-stop verification confirmed via read-back and `/api/readiness`, scoped
  explicitly to current wiring (pole 1 not yet in the contactor coil
  circuit).
- Temperature rose ~17 C over 13 minutes under closed-loop control —
  demonstrated for the first time in this task chain (the prior
  `cplval75_aborted_executor_panic_2026-09-09.md` attempt got zero heat and
  panicked; neither problem recurred here).
- **Channel 2 (GPIO28/J17) is the CT that responds under load** — direct,
  measured confirmation, not inference.
- Safety processor was stable throughout; board left safe, idle, relays
  confirmed off.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
