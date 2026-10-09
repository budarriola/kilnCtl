# cplval45 discriminating-plateau capture — ABORTED, Pico instability (2026-09-10)

## Purpose and pre-registered predictions

`cplval75` (commit `1efbdc0c`, `logs/coupling/cplval75_20260910_settled_hold_points.tsv`)
found the forward residual `G·u − ΔT` negative on all three zones and growing
with temperature (`docs/audits/cplval75_coupling_verdict_2026-09-10.md`,
commit `d2e570ad`). Two hypotheses fit the 62/70/75 °C data equally well:

- **per-row SCALE error** (a single scalar per row, 1.12 / 1.19 / 1.31, fits
  all nine measurements to ~2 %)
- **constant OFFSET** error

The 62–75 °C span (1.4x rise above ambient) cannot discriminate them. A
45 °C plateau (ΔT ≈ 16 above the ~34 °C ambient measured today, a ~2.9x span)
was chosen to discriminate. Pre-registered predictions, recorded before any
measurement was taken:

- if SCALE: z2 residual ≈ **−2.9 °C**
- if OFFSET: z2 residual ≈ **−8.4 °C**

**No measurement was collected.** The run was aborted during the ramp-in on
safety grounds (below), so neither prediction has been tested. This document
exists to record the pre-registration and the reason for the abort, per
standing practice of recording predictions before they can be
retrospectively fitted.

## What happened

Pre-flight (all nominal): link up, `commissioned=True`, S1 `abs_max_temp_c=80C`
ARMED, S8 `max_rate_c_per_min=20C/min` ARMED, `trip_mask=0x0000`, no
unacknowledged crash banner from `get_heap_status` (ESP `reset_reason`
showed a prior `panic/exception` boot, `uptime_s=1675`, but the crash-report
check did not flag it as unacknowledged), all three thermocouples plausible
at ~34.1–34.3 °C (a rested, near-ambient baseline — no waiting needed),
relays off, executor and autotune idle, safety-processor `boot_id=58`,
`uptime≈818 s`.

A user profile was used, not a builtin: user slot #7 (`pv08311918`,
target 40/45/60 °C across 3 segments) was temporarily overwritten with a
single-segment profile `cplval45` (target 45.0 °C, ramp 120 °C/hr, dwell
70 min, `zone_mask=0x7`) sized to exceed 8τ (τ≈271 s live ⇒ 8τ≈36 min) at the
45 °C plateau. The original segments were saved before overwriting.

`profiles_start(profile_id=7)` was issued. ~30 s into the run (segment
target 35.2 °C, all zones still ramping, duties 0.32/0.42/0.43, no relay
above 45 °C, no zone above ambient+2 °C) the safety-processor diagnostic
(`safety_get_diag`) showed **`uptime 12006 ms`** — down from the pre-flight
~818 s. The RP2040 had rebooted. A follow-up MCP call briefly failed to
connect (`Unable to connect`); the ESP's own `/api/status` (`get_heap_status`)
remained reachable throughout at a consistent, unbroken uptime, so this was
isolated to the safety link/processor, not a broader outage.

The run was stopped immediately (`profiles_stop`, then `io_all_relays_off`).
A follow-up check showed the Pico had rebooted **a second time**
(`uptime 6006 ms`, `boot_id` had advanced from 58 to 60 — two boot events)
within roughly two minutes of the first observation. `trip_mask` read
`0x0000` throughout both events (`state=grace`, `trip_reason 0
[SAFETY_TRIP_NONE]`) — no safety trip latched, but two unexplained
reboots of the safety processor coincident with the start of a heating run
is exactly the "Pico instability" class that this task's brief ranks above
every other abort criterion, regardless of trip state.

## Post-abort verification

After the second reboot, the Pico was watched (not reset again) until it
held a single `boot_id` (60) with `uptime` climbing cleanly for 48+ s,
`context frames ok` climbing, zero `bad`, `trip_mask 0x0000` throughout.
Final state confirmed safe and idle:

- `profiles_get_exec_status`: `state=0` (idle), no active run
- `io_read`: `R1=0 R2=0 R3=0 R4=0` (all four relays off)
- `safety_get_diag`: `trip_mask 0x0000`, `trip_reason 0`, `boot_id=60` stable
- `get_heap_status`: ESP reachable, no unacknowledged-crash banner
- user profile slot #7 restored exactly to its original segments
  (40 °C/300 °C/hr/45 min, 45 °C/120 °C/hr/8 min, 60 °C/120 °C/hr/8 min,
  `zone_mask=0x7`), confirmed by `profiles_get` readback

**Board left safe and idle: relays off, executor idle, no trip latched,
safety processor stable.**

## Cause — not yet determined

Two RP2040 reboots (`boot_id` 58→60) occurred within about two minutes of
`profiles_start`, both `boot reason: watchdog`. Nothing in this session
touched the Pico directly (no flash, no OTA, no `debug_reset`) — the only
action taken was starting an ESP-side profile firing. Candidate causes not
yet investigated: SX1509 I/O expander re-init timing coinciding with the
first relay-duty transitions (see the "external I2C peripherals" note
elsewhere in this repo's firmware gotchas, though that note is about
post-OTA reboots, not this), a marginal safety-link handshake retry storm,
or a genuine RP2040-side watchdog defect unrelated to this session's
actions. This needs a follow-up investigation before the next attempt at
this measurement — starting a heating run against a safety processor with
an unexplained double-reboot in its very recent history is not something
to simply retry.

## Status

**Open.** The discriminating 45 °C plateau measurement described above has
not been made. No `.tsv` of settled-hold data accompanies this document
because no plateau was reached — do not read the absence of a data file as
an oversight. Next session: diagnose the double Pico reboot before
re-attempting; the pre-registered SCALE (−2.9 °C) vs OFFSET (−8.4 °C) z2
residual predictions above still stand and should be tested once the
safety processor's stability is confirmed independent of any run.
