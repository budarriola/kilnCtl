# Firing profiles, the execution engine, and the relay rule syntax

Three things share this document because they share a page and a mental
model: what a firing profile *is* (`profiles_http.{c,h}`), what actually runs
it (`profile_executor.{c,h}`), and the separate relay rule DSL
(`rules_http.{c,h}`) that looks like automation but is not yet wired to
anything.

TODO.md sections 5, 6 and 6A remain the authoritative design docs and change
logs, with dated "Implemented"/"Not built" notes on every bullet; this file
is the shorter "what is the format and what does it do" companion.
[`docs/PID_CONTROL.md`](PID_CONTROL.md) covers the control math, the thermal
guards and autotune — everything below assumes it. Where this file and the
code disagree, the code wins.

**HARDWARE STATUS**: per `docs/PROJECT_STATUS.md`, the profile CRUD API and
the executor's state machine were live-verified on a real board on
2026-08-11 — but against a bench unit with **no thermocouple daughterboard
and no relay expander attached**. Every ramp, dwell, ramp-lock and guard
behavior below is logic-verified against the absent-hardware code paths and
against the host-side plant model (`App/test/`), and has **never driven a
real relay from a real reading**. Multi-zone execution specifically has never
run on more than the absent hardware. The rule engine has no evaluator at
all, so there is nothing about it to verify.

## Profile format

`profile_t` / `profile_segment_t`, defined in `profiles_http.h` because
`profile_executor.c` needs them too (via `profiles_http_get()`).

```c
typedef struct {
    float    target_c;       /* the temperature this segment ends at */
    float    ramp_c_per_hr;  /* 0 = no ramp-rate constraint (step/hold segment) */
    uint32_t dwell_min;      /* minutes to hold target_c once reached */
} profile_segment_t;

typedef struct {
    char     name[PROFILE_NAME_MAX_LEN + 1];   /* 15 chars + NUL */
    uint8_t  zone_mask;                        /* bit i = zone i participates */
    uint8_t  segment_count;
    profile_segment_t segments[PROFILE_MAX_SEGMENTS];  /* 12 */
} profile_t;
```

Limits, all compile-time:

| Constant | Value | Meaning |
|---|---|---|
| `PROFILES_MAX_COUNT` | 8 | storage slots, ids 0..7 |
| `PROFILE_NAME_MAX_LEN` | 15 | name characters, not counting the NUL |
| `PROFILE_MAX_SEGMENTS` | 12 | segments per profile |

`zone_mask` replaced an earlier single `zone_index` when TODO.md 6A.5 made a
profile able to drive several zones at once. A single-zone profile is just a
mask with one bit set, so this is a superset rather than a behavior change
for that case.

### Storage

NVS namespace `kiln_cfg`, one blob per slot under keys `prof0`..`prof7`, plus
a one-byte `prof_used` bitmap (bit N = slot N in use). All eight slots are
loaded resident at boot — each is well under 200 bytes, so lazy per-request
loading would be more code than it saves, and the bitmap still means a
listing never has to probe eight keys.

If the bitmap says a slot is used but its blob is missing or the wrong size
(a stale layout from before a struct change), **the blob wins**: the slot is
marked unused and zeroed rather than handing a client a garbage-decoded
profile. Same rule as `zones_http.c` and `rules_http.c` apply to their own
blobs.

`profiles_http.c` is the sole owner of this storage. `profile_executor.c`
reads through `profiles_http_get()` and never touches NVS — the same
one-owner discipline `zones_http.c` established for zone config.

### Built-in schedules (2026-08-20)

28 published [Digital Fire](https://digitalfire.com/schedule) firing
schedules ship read-only in flash, addressed at `PROFILE_BUILTIN_ID_BASE`
(128) + index — a separate id space from the 8 user slots above, so they can
neither fill them nor be evicted by them. Full design (why a separate id
space, why "removable" is a hide recorded in an NVS mask rather than a
delete, how the table is generated, and the feasibility formulas that badge
an unusable schedule) is `TODO.md` section 5A — this file doesn't repeat it.
Credited on `profiles_page.html`.

A handful of defects specific to the built-ins were found and fixed
2026-08-20 (`TODO.md`, full list in the commit message):
`profiles_http_get()` used to fail a built-in with no zones configured,
which surfaced as "no such profile" instead of the real "configure a zone
first"; readiness now counts visible built-ins toward "at least one fire
profile available"; a reset restores the shipped defaults explicitly
(`profiles_builtin_restore_all()`) instead of relying on a partition-erase
side effect; and the dashboard now shows a built-in's readable title where
it used to show its short code (e.g. "BQ1000").

### Creation and validation (`POST /api/profile`)

Form-encoded fields: `id` (optional), `name`, `zone_mask`, `seg_count`, then
`seg<i>_target`, `seg<i>_ramp`, `seg<i>_dwell` for `i` in `0..seg_count-1`.

Slot selection: `id` empty, absent, or `-1` takes the first free slot and
answers 400 `profile storage full` if there is none; an `id` in 0..7 targets
that exact slot, creating or overwriting it. A client that already knows its
id can address it directly.

| Field | Rule |
|---|---|
| `name` | required, non-empty, ≤15 chars |
| `zone_mask` | must be >0, ≤255, and select **only zones below the configured `thermo_count`** — otherwise 400 `zone_mask must select at least one configured zone (check Thermocouples & Zones settings)` |
| `seg_count` | 1..12 |
| `seg<i>_target` | 0..2015 °C (`PROFILE_TARGET_C_MIN/MAX`) |
| `seg<i>_ramp` | 0..1000 °C/hr |
| `seg<i>_dwell` | 0..1440 minutes (24 h) |

Those bounds are **firmware sanity bounds against a typo, not kiln-safety
limits**. 2015 °C is cone 42, the top of the standard pyrometric cone table
(owner request, 2026-09-02: gas-kiln profiles up to that scale must be
storable), not a ceiling on what any real kiln may reach.
`ZONE_MAX_TEMP_C_MAX` (`zones_http.h`, the per-zone safety ceiling a real
kiln's `max_temp_c` is validated against) is 2500 °C, and a compile-time
`_Static_assert` in `profiles_http.c` enforces `PROFILE_TARGET_C_MAX <=
ZONE_MAX_TEMP_C_MAX` so a profile target can never be unrepresentable
against some legally configurable zone ceiling.

### Save-time ceiling is advisory, not a refusal (owner correction, 2026-09-02)

**Profiles are portable between kilns.** A cone-10 or gas-kiln profile
authored on one rig is a legitimate thing to *save* on a low-temperature
bench rig -- the kiln's ceiling is a property of the installation, not of
the profile. `profiles_http_save()` and `POST /api/profile` therefore never
refuse a segment whose `target_c` exceeds a participating zone's current
`max_temp_c`; they only WARN (one more entry in the `warnings` array /
`out_warning_count`), and `GET /api/profiles` / `GET /api/profile?id=`
surface it persistently as `"exceeds_ceiling":true` (plus `"ceiling_note"`
on the detail endpoint) so the condition is visible at edit time, not only
after a failed start hours later.

**The single enforcement point is run start**: `profile_executor_run()`
re-checks every `ZONE_RAMP` segment's `target_c` against each participating
zone's *current* `max_temp_c` and refuses the start (never clamps) if any
segment is over. This is deliberately the same re-check pattern as the
ramp-rate feasibility check below, and for the same reason: `max_temp_c` can
be edited on the zones page at any time after a profile was saved, so
save-time validation alone could never be sufficient by itself even if it
were still a refusal.

Everything parses into a scratch `profile_t`; nothing is stored unless the
whole submission validates. NVS write failure is logged and the profile still
applies live, with an explicit "will not survive a reboot" log line.

### Feasibility check (TODO.md section 5)

Run at **creation time** here, and again at **start time** in
`profile_executor_run()` — the ceiling can change on the zones page between
the two.

For every segment with `ramp_c_per_hr > 0`, against every zone in
`zone_mask`, compared to that zone's `max_ramp_c_per_hr` from
`zones_config_get_max_ramp()`:

- `rate > ceiling` → **hard error**, 400
  `{"ok":false,"error":"segment N: ramp rate X C/hr exceeds zone Z's Y C/hr ceiling"}`.
  Nothing is stored.
- `rate > 0.8 * ceiling` (`PROFILE_RAMP_WARN_FRACTION`) → **warning**, not a
  refusal. The profile is stored and the response carries
  `{"ok":true,"id":N,"warnings":["segment N: ramp rate X C/hr is within 20% of zone Z's Y C/hr ceiling", ...]}`.
  TODO.md section 5's explicit "warn, don't block at that margin" rule.
- `ramp_c_per_hr == 0` → exempt. A segment with no rate is a step/hold, not a
  ramp, and has no rate to check.

A zone with **no ceiling on record** reads back `0.0` — which is also the
"never configured" default — so every nonzero rate is infeasible against it.
That is correct, not a bug: there is nothing to feasibility-check against
until the zone's max ramp rate is set on the Thermocouples & Zones page.

Multi-zone profiles must satisfy **every** participating zone's ceiling.
A profile that is infeasible for even one zone would just sit ramp-locked
against that zone forever (below), so refusing it up front is the honest
answer.

## The execution engine (`profile_executor.c`)

One FreeRTOS task at 1 Hz (`PROFILE_EXECUTOR_TICK_MS`), priority 5, plus a
second independent watchdog task. It is the only module in the control set
that talks to FreeRTOS, `kiln_io`, `relay_authority` or HTTP; the math
(`pid.c`), the guards (`thermal_guard.c`) and the duty→relay rendering
(`heater_output.c`) are pure and testable without it — see
`docs/PID_CONTROL.md`.

### Run states

`profile_exec_state_t`: `IDLE`, `RUNNING`, `PAUSED`, `DONE`, `FAULTED`.

`DONE` means the last segment's dwell completed; relays are off and a new run
can start immediately. `FAULTED` means a **global** guard tripped (or every
active zone individually faulted, leaving nothing to run); it is **latching**
and stays until `profile_executor_halt()` explicitly acknowledges it —
TODO.md 6A.3's one required operator acknowledgement.

Anything other than `RUNNING` forces every active zone's relays off on every
tick, not just on the transition. A stuck state cannot leave a relay on.

### Per-zone control state

`profile_executor_run()` snapshots the profile (so an edit to the stored
original cannot disturb an in-progress run) and builds one `zone_runtime_t`
per bit in `zone_mask`, each carrying its own `pid_cfg_t`/`pid_state_t`,
`heater_output_cfg_t`/`_state_t`, `thermal_guard_cfg_t`/`_state_t` and
control mode, read from `zones_http.c`'s config at start:

- control mode from `zones_config_get_control_mode()`, defaulting to
  BANGBANG if the zone cannot answer
- gains from `zones_config_get_pid()`
- relay timing from `zones_config_get_heater_cfg()`, substituting
  `HEATER_WINDOW_MS` 60000 / `HEATER_MIN_ON_MS` 2000 / `HEATER_MIN_OFF_MS`
  2000 for any field left at 0
- guard limits from `zones_config_get_temp_limits()` and
  `zones_config_get_sanity_rate()`, the latter substituting
  `PROFILE_EXECUTOR_DEFAULT_SANITY_RATE_C_PER_MIN` (0.5 °C/min) when unset —
  a safety monitor must not silently disable itself because a field was left
  blank

PID-mode zones in a multi-zone run get their time-proportioning window
phase-offset by `active_rank * window_ms / n_active_zones` via
`heater_output_seed_phase()`, using the zone's rank among *this run's* active
zones rather than its raw index, so a 2-zone run on zones {0,2} still gets a
clean 50/50 split.

### Ramp baseline

`target_c` starts at the first active zone's current calibrated reading if
one is available, otherwise at the first segment's own `target_c`. Ramping
from a fabricated zero would command an enormous, meaningless initial climb;
falling back to the segment target makes the first ramp a no-op instead.

### Segment stepping

Each tick measures its own `dt_s` from the FreeRTOS tick count rather than
assuming exactly 1 s, and hands that measured value to `pid_update()` and
`heater_output_*`.

While **ramping** (`dwelling == false`):

```
new_target = target_c ± ramp_c_per_hr * (dt_s / 3600)
```

clamped to the segment's `target_c` in whichever direction it is moving.
`ramp_c_per_hr == 0` jumps straight to `target_c`. When `target_c` is
reached, the segment flips to dwelling and `segment_elapsed_s` resets to 0.

While **dwelling**, `target_c` is pinned at the segment target until
`segment_elapsed_s >= dwell_min * 60`, then `segment_index` advances. Past the
last segment, the run goes `DONE` and drops all relays. The new segment
inherits the current `target_c` as its ramp start — segments chain, they do
not jump.

**A dwell is a timer, not a soak.** `dwell_min` counts down from the moment
the ramp's setpoint reaches the segment target, not from the moment the kiln
itself gets there — the two are close but rarely identical, since the kiln is
usually still catching up when the timer starts. This is a deliberate,
owner-decided design: it matches how commercial kiln controllers have always
worked, keeps every profile's total run length predictable, and avoids
silently changing what an already-stored profile does. If your work needs a
true minutes-at-temperature soak rather than a minutes-of-timer dwell, pad
the segment's `dwell_min` — the executor does not do this for you. See
`firmware/KilnFW/docs/PID_EXPANSION_PLAN.md` §7.3 for the measured size of
the gap between the two.

### Ramp-lock (TODO.md 6A.5(d))

Before stepping, the executor asks whether **every active, not-already-
faulted zone** is within `PROFILE_EXECUTOR_RAMP_LOCK_BAND_C` (25 °C) of the
current shared setpoint. If any is not — including any zone with no valid
reading — the schedule does not advance at all this tick: `segment_elapsed_s`
does not increment, `target_c` does not move, and a dwell does not count
down. The slowest zone sets the pace.

`ramp_lock_held` and `ramp_lock_lagging_mask` are published in
`/api/profile_exec` and `/api/control` so the reason is visible rather than
looking like a stalled firing.

A zone that has already tripped its own per-zone guard is excluded from the
check: that is guards 1/2/4/7's problem, not a reason to hold the whole
firing hostage forever.

### Per-zone approach-rate cap (PID_EXPANSION_PLAN.md sec 3.6d,
### PER_ZONE_TARGET_DESIGN_STUDY.md option (b))

The shared `target_c` above is one scalar every active zone reads — there is
still only one commanded *destination* per tick, and ramp-lock/segment-advance/
feasibility all key off that one value, completely unchanged by what follows.
What a zone *can* have of its own is a ceiling on how fast **its own**
commanded setpoint approaches that shared value: `zone_cfg_t::
approach_rate_cap_c_per_hr` (0 = uncapped, the default and every zone's only
behaviour before this field existed).

Each tick, after `target_c` is stepped, every active zone's own
`effective_target_c` (`zone_runtime_t`) is updated:

- **Uncapped** (`approach_rate_cap_c_per_hr` reads 0): `effective_target_c` is
  set to `target_c` outright, every tick — bit-identical to reading
  `s_exec.target_c` directly, which is exactly what every consumer of this
  value did before this field existed.
- **Capped**: `effective_target_c` moves toward `target_c` by at most
  `cap_c_per_hr * dt_s/3600` this tick, in whichever direction closes the gap.
  If the cap is numerically looser than the rate `target_c` is actually
  moving at (a segment ramping at 30 °C/hr with an 1000 °C/hr cap configured,
  say), `effective_target_c` tracks `target_c` exactly and the cap is a
  mathematical no-op — **this option can only ever tighten a segment's own
  ramp rate, never loosen it.**

`effective_target_c`, not the shared `target_c`, is what a zone's own
feedforward (`zone_taper_climb_rate()`/`zone_feedforward()`), PID error term
(`pid_update_terms()`), BANGBANG hysteresis compare, and the "cooling
limited" diagnostic are all driven from — and it is also what is fed to
`thermal_guard_input_t.setpoint_c` for that zone, **not** `s_exec.target_c`
unconditionally, so a capped zone's guard sees what it is actually being
asked to do this tick rather than the group's eventual destination (the
`project_autotune_feeds_fake_setpoint.md` bug class, deliberately avoided
here rather than reproduced). An uncapped zone's guard-visible setpoint is
therefore unchanged.

Deliberately **untouched** by this option, by design: `target_c` itself (the
shared destination), segment-advance's own code (still `target_c ==
seg->target_c`), ramp-lock's own comparison code (still each zone's
`actual_c` against the one shared `target_c`), `profile_segment_feasibility()`
(never reads this field), and the wire protocol (`safety_link_frames.c` still
sends `pstat.target_c`, the shared destination, per zone — SaftyFW's S2 guard
already reduces per-zone setpoints with `max()` and needs no change either
way, but this option does not even exercise that path since the wire value
never becomes per-zone).

Leaving that code untouched does **not** mean a configured cap is inert
against it — the opposite is the more useful thing to know:

- **Ramp-lock can still be tripped, group-wide, by a cap.** Ramp-lock
  compares `actual_c` (never `effective_target_c`) against the shared
  `target_c` (`profile_executor.c` ~line 482). A cap slow enough that a
  zone's `actual_c` falls more than `EXEC_RAMP_LOCK_BAND_C` (25 °C) behind
  the shared `target_c` sets `lock_ok = false` for the whole tick, which
  freezes `target_c`/`segment_elapsed_s` for *every* zone, not only the
  capped one — a per-zone slowdown becomes a global one. This is
  self-limiting and deadlock-free: the frozen shared target lets the capped
  zone's own setpoint (and, on the same rate limit, its actual reading)
  catch up, then the lock releases and the firing continues throttled to
  roughly the cap rather than hanging. The accurate claim is "cannot disturb
  ramp-lock for caps that keep the zone inside the 25 °C band" — not
  unconditional immunity.
- **Segment advance does not wait for the capped zone.** Because
  segment-advance keys on the *shared* `target_c == seg->target_c`, a capped
  zone can still be short of its own `effective_target_c` when the group
  advances (or enters a dwell). With a short `dwell_min`, the segment can
  move on before the capped zone ever arrives at temperature — bounded by
  ramp-lock's 25 °C band as above, but real within it.
- **Guard 1's sensitivity to a dead element is reduced on a capped zone.**
  Feeding `thermal_guard_input_t.setpoint_c` from `effective_target_c`
  (rather than the shared `target_c`) means a capped zone tracks its own
  setpoint closely, so it spends far more time in guard 2's branch (120 s
  window, falling-while-heating test) than in guard 1's (which arms only
  once the commanded setpoint has pulled `progress_band_c`, 3 °C by default,
  ahead of `actual_c`). `progress_band_c` is per-zone operator config as of
  ZONES_CFG_VERSION 22 (`zones_config_get_progress_band_c()`/`z%u_progressband`
  on the zones POST wire) -- 0 still resolves to the 3 °C firmware default,
  and every existing zone migrates onto that default with no operator
  action required. The bound is `[ZONE_PROGRESS_BAND_C_MIN, ZONE_PROGRESS_BAND_C_MAX]`
  = `[0.5, 20.0]` °C; an operator narrowing this to arm guard 1 sooner
  should not set it near the 0.5 °C floor without first checking the
  zone's own steady-state tracking offset — on this bench's zone 0, actual
  temperature has settled 1.3 °C below the commanded setpoint (50.7 vs
  52.0 °C) during ordinary dwell, which alone exceeds a 0.5 °C band and
  will trip guard 1 on a perfectly healthy zone. At a 20 °C/hr cap that is roughly 9 extra minutes of
  latency before guard 1 can arm after an element dies; a smaller cap adds
  more. This is intrinsic to any slow ramp (an equally slow profile segment
  has the identical property today) and is an accepted trade-off, not a
  regression — but it is a real reduction in guard-1 sensitivity and was
  previously undocumented.

A capped zone's own segment "reached" (its `effective_target_c` catching up
to `seg->target_c`) can happen strictly *later* than the group's own
segment-advance/dwell-entry — the capped zone keeps closing the gap on later
ticks even after the shared schedule has already entered a dwell. This is
the intended shape of the option, not a bug: the destination and the
schedule's own timing are shared; only the capped zone's own rate of arrival
is not.

`zones_config_get_approach_rate_cap_c_per_hr()`/`..._set_...()`
(`zones_config_json.h`) are the accessor pair; `GET`/`POST /api/zones` expose
it as `approach_rate_cap_c_per_hr` / `z<i>_approachratecap`, same per-zone
convention `ease_off_window_mult` uses (ZONES_CFG_VERSION 16→17). Bounds:
[`ZONE_APPROACH_RATE_CAP_C_PER_HR_MIN` (1.0 °C/hr), `..._MAX`
(`ZONE_MAX_RAMP_C_PER_HR_MAX`, 1000.0 °C/hr)] or exactly 0 — refused, never
clamped.

### Load cap (TODO.md 6A.5, load staggering)

`zones_config_get_max_simultaneous_relays()` (0 = unlimited, the default) is
a board-wide cap — a breaker or supply limit applies to the whole board, not
one zone. Applied after every zone has decided what it wants but before any
relay is written: while more zones want ON than the cap allows, the zone with
the **lowest** `deferred_on_ms` (least owed) is suppressed, and the denied
on-time is added to its debt.

A suppressed PID zone gets that time back as a duty boost on its next
time-proportioning window, debited only when a fresh window actually opens
(`heater_output_duty()` ignores a duty change mid-window, so crediting
unconditionally and debiting only at a real boundary keeps the books exact).
A suppressed **bang-bang** zone accumulates the debt too but never gets it
back — there is no window to pay it into. That is a documented limitation of
that mode, not an oversight.

### Guard escalation

Per-tick, each zone's `thermal_guard_tick()` is fed the **raw**, uncalibrated
reading — a calibration offset must not be able to hide an out-of-range
sensor — along with the setpoint, the post-gate commanded duty and the
measured `dt_s`.

Guards 3/5/6 escalate **globally**: a `safety_link` fault source is asserted
(`SAFETY_FAULT_SRC_THERMO` for guard 6, `SAFETY_FAULT_SRC_THERMAL_SANITY` for
3 and 5), every active zone is dropped and marked faulted, and the run enters
`FAULTED`. Guards 1/2/4/7 block only their own zone via
`relay_authority_set_zone_blocked()`; the run continues for the rest, and
only turns into a whole-run `FAULTED` if every active zone ends up faulted.
The full guard table is in `docs/PID_CONTROL.md`.

`profile_executor_halt()` is the only thing that clears any of it: it drops
relays, releases exactly the global source *this run* asserted (never one
some other caller set), clears each zone's per-zone block, resets every
thermal guard latch and returns to `IDLE`.

### Guard 9 — the control-tick watchdog

A **second, independent FreeRTOS task** (`watchdog_task_entry()`), on the
principle that a control loop cannot be its own watchdog. It wakes every 2 s
and compares now against `last_tick_tick`, which the control task stamps on
every iteration *regardless of run state*. If more than 10 s have passed it:

- calls `kiln_io_all_relays_off()` directly, bypassing the executor entirely
- asserts `SAFETY_FAULT_SRC_APP`
- moves a RUNNING/PAUSED run to `FAULTED` with the staleness in the reason

If the watchdog task itself fails to spawn at boot, that is logged
(`guard 9 unavailable this boot`) and the executor still starts —
same non-fatal bring-up convention as everything else in this firmware.

### Start / stop / pause / resume

| Call | Endpoint | Behavior |
|---|---|---|
| `profile_executor_run()` | `POST /api/profile_exec/start` | refuses if RUNNING/PAUSED, if a guard is latched (FAULTED — Stop first), if any targeted zone has an autotune run active, if the profile is missing/empty/targets no zones, or if the start-time feasibility re-check fails |
| `profile_executor_halt()` | `POST /api/profile_exec/stop` | drops relays, clears this run's faults and guard latches, → IDLE. No-op from IDLE, never refuses |
| `profile_executor_pause()` | `POST /api/profile_exec/pause` | **drops every active zone's relays** and freezes the shared schedule. False unless RUNNING |
| `profile_executor_resume()` | `POST /api/profile_exec/resume` | reseeds each PID zone bumplessly, resets `prev_control_tick` so the pause is not charged as one huge `dt_s`, → RUNNING. False unless PAUSED |

Pause dropping relays is the important detail: it is a **safe** pause, not a
hold-current-output pause. Because the ramp/dwell state is simply not ticked
while paused, there is nothing to un-shift on resume — the schedule picks up
exactly where it stopped. Bumpless transfer means each PID zone resumes as if
it had been driving u=0 throughout (which it had — the relays were off),
rather than the integral jumping on the first post-resume tick.

Autotune and profile execution are mutually exclusive, checked from both
directions: `profile_executor_run()` refuses a zone with an autotune active,
and `autotune_engine.c` asks `profile_executor_zone_is_active()` before
starting. The check is per zone, not global, so an autotune on zone 2 does
not block a profile on zone 0.

### History ring buffer

`HISTORY_MAX_SAMPLES` 2880 entries at `HISTORY_SAMPLE_PERIOD_S` 30 s = 24 h,
RAM-only, oldest overwritten once full — "this firing's trend", not
indefinite history, matching TODO.md section 0's settled design. Reset at the
start of every run.

Each entry is `{elapsed_s, actual_c, desired_c, duty, guard}`. `actual_c` is
NaN when the reading was invalid. `guard` is `thermal_guard_trip_t`, so the
dashboard graph can mark trips without a second request.

Scoped to **one representative zone** — the lowest-indexed active zone in the
run — even though multi-zone execution exists: per-zone history would
multiply a ~58 KB buffer by up to `MAX31856_CHANNEL_COUNT`, and TODO.md 6A.9
only ever asked for the buffer to gain a duty field, not to fan out per zone.

`profile_executor_get_history()` is paged (start index + max count) so
`GET /api/history.csv` can stream it in small batches instead of allocating a
second full-size buffer — see `docs/PID_CONTROL.md`'s closing section for the
out-of-memory failure that lesson came from.

## The relay rule DSL (`rules_http.c`) — REMOVED 2026-08-27

**`rules_http.c`/`rules_eval.c`/`rules_task.c` (the relay rule DSL editor,
`/settings/relays`, `GET`/`POST /api/rules`) were deleted 2026-08-27** along
with `rules_page.html`, per `docs/WEB_UI.md`'s removal notes. The rest of
this section is kept as historical record of the DSL's grammar and
validation rules — none of it is live. Manual relay control now goes through
`diagnostics_http.c`'s Danger Zone (`POST /api/diagnostics/danger/relay`),
documented in `docs/WEB_UI.md`.

### Nothing evaluated these rules while they existed

**There was no rule-evaluator task anywhere in this firmware.** TODO.md
section 0 designed the rule engine and section 6 is where it would run;
neither built it. A saved rule set was stored, validated and echoed back, and
that is all it did — no relay was ever switched by it. `/settings/relays`
said so on the page itself, `rules_http.h` said so in its header comment, and
`rules_http_start()` logged `rules API up (config storage only -- no rule
evaluator exists yet)` at boot. Manual relay control on the dashboard was
entirely unaffected by anything saved here.

A relay's `DRIVEN` flag has the same status: it mirrors TODO.md's `RULE`
owner tag, but the ownership machinery that would enforce it against
MANUAL/PROFILE ownership was never generally built (see `docs/PID_CONTROL.md`
on the missing `AUTOTUNE` tag for the same gap). It is recorded intent.

### Model

A relay has up to 3 rules; a rule has up to 3 conditions.
**Rules are OR'd, conditions within a rule are AND'd.** Condition types are a
zone's temperature against a threshold, elapsed time since profile start
against a threshold, or another relay's *commanded* state.

### Grammar

A hand-typed, line-based text format — the field-count explosion (4 relays ×
3 rules × 3 conditions × ~4 fields) made a flat HTML form impractical.
`GET /api/rules` regenerated this text from the stored config;
`POST /api/rules` took it back as the raw request body. (Both removed
2026-08-27, see this section's header.)

```
RELAY <n 1-4> DRIVEN <0|1>
R <rule 0-2> TEMP <zone 0-2> <GE|LE> <threshold_c>
R <rule 0-2> TIME <GE|LE> <seconds>
R <rule 0-2> RELAY <other 1-4> <0|1>
```

An `R` line applies to whichever `RELAY ... DRIVEN` line most recently
preceded it. Blank lines are ignored; leading whitespace and trailing
whitespace/CR are trimmed (a `<textarea>` submission commonly carries
`\r\n`). Lines are matched by `sscanf` against the four patterns in order,
and anything matching none of them is an error.

Example — vent relay 2 opens once zone 0 passes 500 °C:

```
RELAY 2 DRIVEN 1
R 0 TEMP 0 GE 500.0
```

The regenerated text always emits a `RELAY n DRIVEN x` line for **every**
relay, including ones with no rules, so the full editable state is visible up
front rather than leaving an unconfigured relay implicit.

### Validation

Any bad line rejects the **entire** submission with
**400 `line <n>: <reason>`** (1-based), before the live config or NVS is
touched — a partially-applied relay config is a worse failure mode than a
rejected one.

| Reason | Trigger |
|---|---|
| `relay number out of range (1-4)` | `RELAY n` outside 1..`KILN_IO_RELAY_COUNT` |
| `DRIVEN must be 0 or 1` | anything else in the `DRIVEN` field |
| `relay declared twice in this submission` | a second `RELAY n` block for a relay already given a `DRIVEN` flag or any condition in this submission — which block "wins" would be surprising either way |
| `condition line with no preceding RELAY line` | an `R` line before any `RELAY` line |
| `rule index out of range (0-2)` | `R n` outside 0..2 |
| `zone index out of range` | `TEMP` zone outside 0..`MAX31856_CHANNEL_COUNT-1` |
| `TEMP comparator must be GE or LE` / `TIME comparator must be GE or LE` | anything else in the comparator field |
| `TEMP threshold out of range` | outside -50..1400 °C, or NaN |
| `TIME seconds exceeds 24h sanity bound` | > 86400 — a typo bound, matching the history buffer's own 24 h design point, not a claimed safety limit |
| `referenced relay out of range (1-4)` | `R n RELAY m` with m outside 1..4 |
| `a relay's rule cannot reference itself` | `m == ` the current relay |
| `referenced relay state must be 0 or 1` | anything else in the state field |
| `too many conditions in this rule (max 3)` | a fourth condition on one rule index |
| `unrecognized line format` | matches none of the four patterns |

Note what is **not** validated: a `TEMP` line may name a zone index that has
no zone configured on the Thermocouples & Zones page (only the hardware
channel count is checked), and there is no cycle detection across
`R n RELAY m` references beyond the self-reference check. Both would matter
to an evaluator; neither does to storage.

### Storage

NVS namespace `kiln_cfg`, key `rules_cfg`, one fixed-size blob for all four
relays. A missing key is first boot, not an error; a wrong-size blob (a stale
layout) starts unconfigured rather than decoding garbage — the same rule
`zones_http.c` and `profiles_http.c` apply.

## What's still open

See TODO.md sections 5, 6 and 6A for the itemized, dated checklist. The
largest pieces touching this document:

- **The rule evaluator itself.** Everything above stores and validates; the
  tick that would evaluate it does not exist.
- **Relay ownership tags** (MANUAL / PROFILE / RULE / AUTOTUNE). Designed in
  TODO.md section 0, never generally built, which is why `DRIVEN` is inert
  and why nothing stops a dashboard manual override from fighting a running
  profile for the same relay.
- **Per-zone history.** One representative zone today; see the ring-buffer
  section above for the memory reasoning.
- **A real multi-zone firing.** Concurrent execution, ramp-lock, phase offset
  and the load cap all exist in code and none has ever run against more than
  one absent thermocouple.

---

## Warm-start: joining a profile already at temperature (owner request 2026-08-30)

**The request.** When a profile is started and the kiln is already hotter
than the profile's opening segments, do not run those segments — begin at the
first segment that is at or above the current kiln temperature.

**Why it matters.** Firing back-to-back loads, or restarting after a brief
halt, currently means the executor commands a setpoint far below the actual
kiln temperature. Guard 2 (WRONG_DIRECTION) and guard 4 (DRIFT) then watch a
kiln that cannot cool fast enough to follow, and the run wastes hours on
segments whose work is already done.

### Design questions that must be answered before writing code

These are not polish. Getting any of them wrong is worse than not shipping
the feature.

**1. RELAY_IO segments carry commands, not temperatures — skipping them
silently drops those commands.** A profile can contain
`PROFILE_SEG_KIND_RELAY_IO` segments ("open the damper", "switch the vent")
whose `target_c` is meaningless. If a warm start skips segment 0-3 because
their targets are below the current temperature, and segment 2 was a RELAY_IO
that opened a damper, the kiln now fires with the damper shut. The elements
on this board are live (see `profile_executor.c`'s `io_seg_finish()`), so
this is a physical-consequence bug, not a scheduling one.

Options: (a) replay every skipped RELAY_IO segment's command immediately at
start, in order, before the first ramp tick; (b) refuse to skip *past* a
RELAY_IO segment and warm-start only up to it; (c) require operator
confirmation listing what will be replayed. **Owner decision 2026-08-30: (a), replay them.** The commands are idempotent
state-setting ("relay N to state X"), and their point is what state the kiln
hardware is in during the segments that follow. Replay every skipped RELAY_IO
segment's command, in profile order, before the first ramp tick, and log each
one — a silent replay is as bad as a silent skip.

Replay details to get right: a skipped **blocking** segment's `dwell_min`
wait is NOT replayed (its purpose was to delay the schedule, and that
schedule position is already past); a skipped **non-blocking** segment's hold
is likewise not restarted. Only the on/off command itself is reapplied. A
skipped segment with `io_leave_on_at_end` set must still be tracked by
`io_seg_finish()`'s end-of-run sweep, so the run's end forces it off exactly
as it would have — replaying a command without registering the segment would
leave a relay energized with nothing owning it.

**2. Entering a segment mid-ramp, not at its start.** If segment 2 ramps
200 → 600 °C and the kiln is at 300 °C, jumping to "the start of segment 2"
commands 200 °C — a setpoint *below* the current temperature, which is the
exact problem this feature exists to avoid. The correct behavior is to enter
segment 2 at the point where its ramp reaches 300 °C, and to carry the
segment's remaining time accordingly. This means the chosen segment needs an
entry *offset*, not just an index.

**3. Dwell segments already satisfied.** If the matched segment is a dwell at
a temperature the kiln has already reached, the dwell should still run — a
soak is about time at temperature, not about arriving there. Do not treat "we
are already at the target" as "the soak is done". Say so explicitly in the
code, because it is the tempting wrong optimization.

**4. Which temperature is "current"?** Multi-zone kilns have several
readings. Use the same reading the executor already uses for its own
control/guard decisions rather than introducing a second notion — and if the
zones disagree substantially, prefer the **coolest** zone, so the warm start
never skips work a colder zone still needs.

**5. Falling edges.** A profile that comes *down* (a controlled cool, an
anneal) has segments whose targets fall below the current temperature
legitimately, in the middle of the profile. "First segment at or above
current temp" must mean the first segment of the *opening ascent*, not any
later segment that happens to match after a cooling leg — otherwise starting
a cool-down profile in a hot kiln could jump into the wrong part of it.
Scan only the leading run of segments, and stop at the first descent.

**6. The operator must see it.** A run that silently starts at segment 4 is
indistinguishable from a bug. The UI must state what happened and why
("starting at segment 4 — kiln already at 312 °C"), and it should be
possible to decline and run the profile from the beginning.

### Status

**Implemented** (2026-08-30), in `profile_executor.c`/`.h`. Questions 2-6 are
resolved as follows.

**Entry algorithm (Q2/Q5).** A pure, host-testable decision function,
`profile_executor_plan_warm_start(profile_t *, float current_c)`, runs in two
passes before the first ramp tick:

1. **Ascent boundary (Q5).** Walk the segments in order, considering only
   `PROFILE_SEG_KIND_ZONE_RAMP` ones (a `RELAY_IO` segment has no `target_c`
   of its own and is skipped for this purpose). The first segment whose
   `target_c` is *lower* than the previous ZONE_RAMP segment's `target_c`
   ends the leading ascent; everything from there on is out of reach of
   warm-start. This is a separate pass, deliberately not folded into the
   entry scan below, because the entry scan's own starting point is
   `current_c` (segment 0 always ramps from wherever the kiln actually is) --
   comparing segment 0's target against `current_c` to detect "descent" would
   wrongly truncate the ascent to nothing on an ordinary ascending profile
   whenever the kiln happens to be hotter than segment 0's own target (a
   `current_c` above the profile, not a real cool-down leg).
2. **Entry point (Q2).** Walk the leading ascent with a running `prev_level`
   (the ramp-start level for the segment under examination -- `current_c`
   itself for the first ZONE_RAMP segment, the previous ZONE_RAMP segment's
   own `target_c` for every one after that). The first segment whose
   `target_c >= current_c` is where the run enters. If `current_c` is still
   at/below that segment's own `prev_level` (segment 0 always is), the run
   enters at the segment's own start -- not a warm start. Otherwise the entry
   `target_c` is seeded at `current_c` itself (never below it -- the bug this
   feature removes) and the entry offset (`segment_elapsed_s`, Q2's "entry
   offset") is computed from how much of that segment's ramp distance
   `current_c` already covers, `(current_c - prev_level) / ramp_c_per_hr *
   3600`. The ordinary per-tick ramp step then carries on from there with no
   further special-casing -- seeding `target_c` at `current_c` is what
   "carries the remaining time," nothing else needs to.

**Dwell segments (Q3).** Never shortened. A segment landed on because
`current_c` has passed it entirely enters as a dwell with
`segment_elapsed_s == 0` -- the full configured soak is still ahead of it.
An entry segment matched mid-ramp (target not yet fully reached) enters with
`dwelling == false`; if its `target_c` happens to already equal its own
entry `target_c`, the ordinary ramp/dwell machinery flips it to dwelling
with `segment_elapsed_s` reset to 0 on the very first control tick, same as
segment 0 has always done for a profile that starts already at temperature.
No code path ever pre-credits soak time.

**Coolest-zone rule (Q4).** "Current temperature" is the coolest active
zone's own combined-and-calibrated reading, sampled from the exact same
start-of-run read `run_start_c`/`baseline_target_c` already use (not a
second, separately-timed read). Every active zone's reading is considered,
not just the first active one, so a colder zone elsewhere on the same kiln
can never have its own still-needed segments skipped just because a hotter
zone reads further along. No valid reading anywhere (absent thermocouple
bus, every active zone's sensor unhealthy) falls back to the exact
pre-feature default: segment 0, not warm-started.

**RELAY_IO replay and registration (Q1, decided before implementation).**
Every `RELAY_IO` segment skipped by warm-start (index < the entry segment)
has its on/off command reapplied, in profile order, via the same
`io_seg_start()` a normally-reached segment uses -- this gets the hardware
write, the `relay_authority` claim, and the `claimed_relay_mask`/`io_segs[]`
registration all "for free," identical to a segment reached the ordinary
way. The one deliberate difference: the replayed segment's runtime record is
then forced to `blocking = true` regardless of what the profile actually
said, so `io_segs_tick()` (which skips any `blocking` segment) never touches
its `remaining_s` countdown -- the segment's own hold/`dwell_min` timer is
NOT replayed, only the command is, per the owner's decision above. It is
retired the same way a real blocking segment's command is: by the
end-of-run sweep (`io_segs_force_all_off()`), honoring `leave_on_at_end`
only on the clean DONE path, same as every other segment -- so a replayed
relay is never left energized with nothing owning it.

**Hotter than the entire (leading-ascent of the) profile.** Decided: land on
the LAST segment of the leading ascent, entered as a dwell (its own full
soak still runs). Rejected: refusing to start (the kiln is at a perfectly
fireable temperature -- refusing would make the "wastes hours" problem this
feature exists to fix worse, not better, for a kiln that never fully
cooled between firings) and silently skipping straight past the top segment
(would skip exactly the segment most likely to be the firing's actual point,
the final maturing soak). This applies even when the "leading ascent" is
the profile's only segment (see `profile_executor_plan_warm_start()`'s doc
comment in `profile_executor.c` for the full reasoning) -- the commanded
`target_c` in this one fallback case can end up below `current_c` (there is
no higher segment to enter instead), which is the one place in this feature
where the "never below current" property does not hold, by design.

**Visibility (Q6).** `profile_exec_status_t` gained `warm_started`,
`warm_start_reason` (e.g. "starting at segment 4 -- kiln already at 312.0
C"), `warm_start_replayed_segments[]` and `warm_start_replayed_count`,
populated once in `profile_executor_run()` and served through
`profile_executor_get_status()`. Every warm-start decision and every
replayed command is also logged (`ESP_LOGI`). No UI consumes any of this
yet (out of scope per the request), but the data is queryable.

**Tests:** `firmware/KilnFW/App/test/test_profile_executor_prestart.c`,
`test_warm_start_*` (6 tests, added 2026-08-30) -- cold-kiln regression,
mid-ramp entry offset with the never-below-current-temperature property,
RELAY_IO replay + end-of-run sweep registration, an already-reached dwell
not shortened, a descending profile not jumping into its cooling leg, and
the hotter-than-everything landing. All six were confirmed to actually fail
under a targeted deliberate break of the specific behavior each one covers,
then restored.

---

## Scheduled start + candling (owner request 2026-08-30)

**The request.** When starting a profile from the web GUI, offer a dialog to
(a) start the firing at a specified date and time, and (b) optionally candle
first, for an operator-set duration and temperature.

**Candling timing is anchored to the firing start, not to "now".** The
selected time is the target for *the firing*, and candling ends just before
it. The owner's worked example: it is 5pm, the operator asks to fire at 8pm
and candle for 2 hours — candling starts at 6pm and runs until 8pm, when the
profile proper begins. Candling may begin immediately if there is not enough
time before the firing.

(Candling is the low-temperature hold that drives residual moisture out of
greenware before the real ramp. Firing wet work spalls or explodes it, so
this is about a real physical need, not scheduling convenience.)

### Blocking constraint: this board has no wall clock

`run_state.c:320` says it outright — *"no wall clock on this board"*. There
is no RTC and no SNTP client. The firmware cannot be told "8pm" and know when
that is, and after a reboot it would not know what time it is either.

The workable design is therefore **relative, computed browser-side**: the web
GUI knows the operator's local date/time, so it converts the chosen absolute
start into a *delay in seconds from now* and sends that. The firmware counts
down against its existing monotonic uptime clock and never deals in wall-clock
time at all. The GUI is then also responsible for displaying the absolute
time back to the operator ("firing starts 8:00pm, candling starts 6:00pm"),
since only it can render one.

Consequences that must be handled, not assumed away:

- **Reboot loses the schedule** unless the remaining delay is persisted and
  resumed. Decide deliberately: persist-and-resume, or cancel-on-reboot with
  a clear indication that the schedule was dropped. Silently forgetting a
  scheduled firing is the one outcome to avoid.
- **A long delay drifts** against wall time — the uptime clock is not
  disciplined to anything. Over a 12-hour wait this is likely minutes, which
  is fine for candling but should be stated rather than discovered.
- **The browser's clock could be wrong.** The delay is only as good as the
  device that computed it. Echo the resolved times back for confirmation.

### Safety — a scheduled start is an unattended start

This feature makes the kiln energize elements with nobody necessarily
present. That is a genuine step up in risk from every other control on this
page, and it deserves treatment beyond a normal confirm dialog:

- The existing pre-start readiness/interlock checks must run **at fire time**,
  not only at schedule time. A kiln that passed its checks at 5pm may have a
  door opened at 7pm.
- Decide what happens if a check fails at fire time: refuse and log loudly,
  rather than firing anyway or silently retrying.
- The schedule must be visible and cancellable from every surface that can
  see the kiln (web and LCD), not only the page that created it.
- Consider whether an unattended scheduled start should be gated behind an
  explicit acknowledgement.

### Candling behaviour to specify

- **Temperature and duration are operator-set** in the dialog. Candling
  temperature is conventionally around 90-100 °C (below boiling, to drive
  moisture without steam damage) — offer a sane default, do not hardcode.
- **Overlap case:** if the requested candling duration does not fit before the
  requested firing time (5pm now, fire at 6pm, candle 3 hours), candling
  begins immediately and the shortfall must resolve one of two ways: shorten
  the candle and keep the firing time, or keep the full candle and push the
  firing later. **Undecided — needs an owner answer**; the request says only
  "may happen immediately if need".
- Candling must be visible as a distinct run phase, not a fake profile
  segment that confuses the previous-run banner or the plan curve.
- Interaction with **warm-start** (previous section): if the kiln is already
  above the candling temperature, candling has no work to do. Decide whether
  it is skipped or still held.

### Status

Not implemented. The no-wall-clock constraint shapes the whole design and is
settled (browser computes a relative delay). The overlap case and the
reboot-persistence question are open.
