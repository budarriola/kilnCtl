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
| `seg<i>_target` | 0..1400 °C (`PROFILE_TARGET_C_MIN/MAX`) |
| `seg<i>_ramp` | 0..1000 °C/hr |
| `seg<i>_dwell` | 0..1440 minutes (24 h) |

Those bounds are **firmware sanity bounds against a typo, not kiln-safety
limits**. There is no separate safety authority for firing profiles; 1400 °C
is comfortably above any home-kiln cone this board's use case implies, and a
real ceramics kiln's ceiling would come from the kiln manufacturer's data,
not from this file. The 1400 °C ceiling is deliberately shared with
`zone_cfg_t.max_temp_c`'s range in `zones_http.c` — a guard-5 limit tighter
than what a profile could request would be a contradiction between the two
checks.

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

## The relay rule DSL (`rules_http.c`)

### Nothing evaluates these rules

**There is no rule-evaluator task anywhere in this firmware.** TODO.md
section 0 designed the rule engine and section 6 is where it would run;
neither built it. A saved rule set is stored, validated and echoed back, and
that is all it does — no relay is ever switched by it. `/settings/relays`
says so on the page itself, `rules_http.h` says so in its header comment, and
`rules_http_start()` logs `rules API up (config storage only -- no rule
evaluator exists yet)` at boot. Manual relay control on the dashboard is
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
`GET /api/rules` regenerates this text from the stored config;
`POST /api/rules` takes it back as the raw request body.

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
