# S13/S14/S15 current-guard arming — ground truth, plan, and status (2026-09-10)

**Status: preparation complete, measurement NOT YET RUN.** The board is mid-firing
(`cplval75`, a 3-segment, ~3.3-hour profile run, confirmed live via
`profiles_get_exec_status()` at the time of this audit: `state=1` (RUNNING),
`segment=0/3`, `elapsed=33s`). Per standing instruction not to interrupt it, no
relay-energizing or config-write step below was executed. This document records
the ground truth and the exact procedure to run once the board is idle.

## 1. Ground truth: what S13/S14/S15 actually are

Read from `firmware/SaftyFW/src/safety_guards.c`, `docs/GUARD_TEST_MATRIX.md`,
`docs/CURRENT_SENSE.md`, and confirmed live via `safety_get_commissioning()`.

| Guard | What it checks | Arms on | Live state (2026-09-10) |
|---|---|---|---|
| **S13** `SAFETY_TRIP_BORROWED_STALE` | The safety processor's *own* thermocouple reading is being substituted by a borrowed KilnFW zone reading (`tc_source = BORROWED_ZONE`/`BOTH`), and that borrowed sample stream stalls (`sample_counter` stops incrementing). Graduated WARN at `borrowed_stale_s` (10s default), TRIP at `borrowed_stale_trip_s` (60s default). | `tc_source` set away from `OWN_J7` + `borrowed_zone_index` chosen. **Not a current guard at all** — no relation to `i_normal_a`/CT. | `tc_source=0` (`OWN_J7`) — the safety processor is using its own dedicated MAX31856 on `SaftyThermocoupleBoard`, which is physically fitted and reading correctly (30.2°C, valid). DORMANT **by design**, not by a missing measurement. |
| **S14** `s14_warn[ch]` over-current | Per-CT-channel: is measured current above `overcurrent_pct` (150% default) of that channel's commissioned normal, for `overcurrent_time_s` (30s)? WARN only, non-latching. | Per channel: `i_normal_valid[ch]` + `i_normal_a[ch]` committed, `ct_installed=yes`. | All 3 channels report DORMANT ("i_normal_a not measured"). |
| **S15** `s15_warn[z]` under-current (summed-CT deficit) | `ct_topology=summed`-only: commanded-sum-minus-measured deficit vs. `0.7×` that zone's own `i_normal_a`, for 30s. WARN only, per zone. | Per zone: that zone's `i_normal_a` committed. | All 3 zones report DORMANT ("i_normal_a not measured"). |

**The owner's "s13/s14" and this repo's own prior audits ("S14/S15 are the
dormant current guards, S13 is dormant-by-commissioning") are describing the
same reality**: S13 is unrelated to current sensing and is dormant because this
board deliberately uses its own dedicated safety thermocouple, not a borrowed
zone reading — there is nothing to "fix" there, and switching `tc_source` away
from `OWN_J7` would remove a working independent sensor for no safety benefit.
**This pass targets S14 and S15**, the two guards genuinely blocked on the
unmeasured `i_normal_a[]`. S13 was left alone; arming it would mean deliberately
degrading sensor independence, which the owner did not ask for and which is not
a "current guard" in the sense of this request.

## 2. Hardware constraint: single CT, summed topology

Confirmed by prior direct measurement (see `CURRENT_SENSE.md` §6a and
`docs/GUARD_TEST_MATRIX.md`'s 2026-09-06 note): this board is
`ct_topology=summed` with exactly **one** physical CT, on channel 2
(GPIO28/J17). Channels 0 and 1 have unpopulated front ends and are permanently
`amps_valid[0]=amps_valid[1]=false` — this is the same "not fitted, never a
plausible 0.00 A" discipline as `ct_installed=no`, just scoped to two of three
channels.

Consequence for S14/S15 on **this hardware specifically**:
- S14's per-channel check on channels 0 and 1 can **never** be armed — there is
  no CT behind them to measure. This is not a bug or a gap to close; it is a
  structural fact of the populated hardware.
- S14's channel-2 ("`CT_SUMMED_CHANNEL`") check *can* be armed: it compares the
  one shared CT against the **sum** of `i_normal_a[]` for every zone currently
  commanded on.
- S15 (summed-topology-only by design) can be armed per zone, all three zones,
  using the same shared-CT deficit logic. `GUARD_TEST_MATRIX.md` already notes
  the honesty limit here: a single open heater can clear more than one zone's
  S15 threshold at once (one shared scalar, three per-zone thresholds), so
  several `s15_warn` bits together mean "the fault is in one of these," never
  "all of these are independently faulty." That is inherent to a one-CT board,
  not something this pass can or should paper over.

**Net: on this hardware, "arm S14/S15" concretely means committing three
`i_normal_a[z]` values (one per zone) and letting S14's summed-channel check
and S15's three per-zone checks go live. S14's channel-0/1 rows stay
permanently dormant, correctly, forever, on this board.**

## 3. What `i_normal_a` means and how it is obtained

`i_normal_a[z]` is the *shared CT's* average current reading while zone `z`
alone is commanded on and every other zone's relay is forced off — i.e. the
zone's own current draw isolated by construction, not a per-conductor
measurement (there is only one conductor being watched). `safety_guards.c`
derives it as `sum_with_zone_on_a - sum_idle_a` (`zone_sweep_summed_normal_a()`
in `firmware/KilnFW/App/drivers/control/zones_current_sweep_engine.c`).

**The sanctioned route already exists and should be used as-is, not
reimplemented**: `zones_current_sweep_engine.c` / `zones_current_sweep_task.c`,
reachable at `POST /api/zones/current_sweep/start` on the ESP
(`firmware/KilnFW/App/drivers/http/zones_http.c`; **no kilnctrl MCP tool wraps
this endpoint today** — it must be called directly over HTTP, e.g.
`curl -X POST http://192.168.1.156/api/zones/current_sweep/start`, then polled
via `GET /api/zones/current_sweep/status`). It already:
- refuses to start unless the board is idle (no profile/autotune running, link
  up, no trip latched, all relays confirmed off, CT topology known) —
  `zone_sweep_check_refusal()` enumerates every refusal reason including
  `ZONE_SWEEP_REFUSE_PROFILE_RUNNING`, which will itself block a start attempt
  during the current `cplval75` run as a second line of defense;
- energizes exactly one zone's relay(s) at a time (every other relay forced to
  0xFF-masked off) for 5s (1s settle + 4s sample, oversampled over safety-link
  polls) and records that zone's average current;
- derives `k_ct_v_per_a` (the current-sense gain) automatically from the same
  sweep, self-calibrating rather than requiring a hand-entered probe spec;
- self-aborts per-zone on ceiling breach, link loss, latched trip, or lost
  thermocouple reading, and forces relays off through one documented choke
  point (`zone_sweep_force_relays_off()`) on every exit path;
- refuses to persist a measurement it cannot trust (negative delta → reported
  as "not measured," never silently zeroed).

This is unambiguously the sanctioned route per the code's own review history
(`M12`/`M12b`, opus-reviewed 2026-08-27/28) — no new measurement path is
needed.

## 4. Absolute calibration: required, but self-derived and bounds-checked — not fabricated

Checked live: `k_ct_v_per_a[2] = 0` (uncalibrated) as of this audit
(`GET /api/safety/commissioning`, param id 778). `zero_counts[2] = 63` is
already set. Per `CURRENT_SENSE.md` §5's documented behavior, `k_ct_v_per_a ==
0` makes the Pico's amps conversion (`cs_counts_to_amps()`) hard-return `0.0`
regardless of real current — confirmed live: `safety_get_status()` shows
`ct_counts 17, 17, 67` (channel 2 well above its ~17-count idle floor, i.e.
real signal) but `currents ... 0.00 A` for all three channels.

**This means the raw `i_normal_a` sweep cannot produce a nonzero measurement
until `k_ct_v_per_a[2]` is calibrated first.** The task brief is right to flag
this as a hard stop *if* it required a hand-supplied number — it does not:
`zone_sweep_derive_k_ct()` (§3 above, same sweep call) derives
`k_ct_v_per_a[2]` from `I_expected = max_expected_power_w / mains_voltage_v`
(nameplate figures already answered during commissioning — `mains_voltage_v =
240` confirmed live; `max_expected_power_w` needs confirming at run time, see
open item below) against the sweep's own measured whole-kiln total, and
**refuses outright** (no value written) if:
- the expected nameplate current cannot be computed (unset voltage/power),
- the measured total is below noise floor,
- the correction ratio implied falls outside 0.2×–5× (a `CURRENT_SENSE.md` §0
  documented ~2× accuracy scope — anything wider means a wiring/units error,
  not a scale error), or
- the resulting `k_ct_v_per_a` falls outside a physically plausible
  0.0005–0.5 V/A band for any real CT part.

This is exactly the "relative, bounds-checked, refuse rather than fabricate"
design the task asked for, already implemented and already the one path that
both derives the calibration and records the per-zone `i_normal_a` values S14/
S15 need, in the same sweep run. **No manual number needs to be invented, and
no owner input is required beyond what commissioning already has** — unless
`max_expected_power_w` turns out to be unset, in which case the sweep itself
will refuse cleanly with `ZONE_KCT_DERIVE_NO_NAMEPLATE` and name it; that one
open question is resolved automatically by the refusal path, not by guessing.

## 5. Open items before running (to check the moment the board is idle)

1. Confirm `max_expected_power_w` is set (KilnFW zones/system config) — the
   sweep will refuse cleanly and say so if not; if it does, report to the
   owner rather than typing a nameplate figure in on their behalf.
2. Confirm `profiles_get_exec_status()` shows idle, `io.relays` all off, and
   `/api/status` / `safety_get_status()` agree (relays off both sides), no
   trip latched, `cmd_status_count` climbing, `capability_preflight` passing —
   the standard pre-flight this repo requires before any relay-energizing
   step.
3. `POST /api/zones/current_sweep/start`, poll
   `GET /api/zones/current_sweep/status` to completion (~15s total for 3
   zones), watching `safety_get_status()` continuously throughout for any
   Pico instability (hard-abort criterion, ranks above all else) and any zone
   approaching 70°C (sweep's own ceiling logic caps this at the tightest
   configured zone ceiling, 80°C here, but the standing 70°C human margin
   still applies).
4. After completion, read back (not trust) every written value: `GET
   /api/zones` (`normal_current_measured`/`normal_current_a` per zone) and
   `safety_get_commissioning()` (`i_normal_valid[z]`/`i_normal_a[z]`,
   `k_ct_v_per_a[2]`) — this repo's standing rule (`boot_guard` write-lies
   class) is never to trust a write's return code alone.
5. Note the Pico-ARMED config-write grace-window caveat from the task brief:
   `safety_set_commissioning_fields`/the sweep's own persistence path may need
   to land in a post-reset grace window if the Pico refuses while ARMED — not
   yet hit in practice since nothing has been written yet; will be handled and
   reported explicitly if encountered.
6. Confirm via `safety_get_commissioning()` that S14's summed-channel row and
   all three S15 rows read ARMED, and that S14's channel-0/1 rows still read
   DORMANT with the *correct* reason (no CT fitted) — DORMANT there is the
   success condition, not a leftover failure.
7. Log the sweep continuously to `logs/coupling/` in the existing format
   (raw `ct_counts`) alongside the `/api/zones/current_sweep/status` polls.

## 6. Why this was not run today

`profiles_get_exec_status()` confirmed the board is live-firing `cplval75`
(profile #0, 3 segments, target 29.7°C at the time of the check, all three
zones actively controlling). The sweep's own refusal logic
(`ZONE_SWEEP_REFUSE_PROFILE_RUNNING`) would refuse a start attempt regardless,
but the standing instruction not to touch the board mid-capture is followed
directly: no config write, relay command, or sweep call was issued. This audit
and the plan above are the full preparation; the measurement itself is queued
for the next window when `profiles_get_exec_status()` reports idle and
`io.relays`/`/api/status` confirm all relays off.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
