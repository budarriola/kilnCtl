# Safety processor commissioning — completion check, 2026-09-09

## Context

Prior attempt (`docs/audits/commissioning_gap_and_no_heat_2026-09-09.md`) could
not proceed: the safety link's status channel was down, so no write could be
read back. That was fixed by `5b8fc53d` (a `thermo_task` stack overflow) —
confirmed at the start of this pass: link up, `cmd_status_count` climbing
across the session (10877 -> still climbing), `trip_mask 0x0000`, relays off.

Separately, `b5cb83a4` fixed `config_params_all_required_set()`
(`firmware/SaftyFW/src/config_params.c`) so `ct_channel_map` is required only
when `ct_installed != 0 AND ct_topology != SUMMED` — this bench board is
`ct_topology=summed`, so that field is correctly no longer demanded.

**No writes were made in this pass.** Stage 2's re-derivation of the gate
found the RP2040's commissioning record was already complete — see below.

## Stage 1 — baseline (session start)

- `link_status`: connected, COM14, `registered_tasks` includes 200 (safety),
  `cmd_status_count` climbing — link genuinely up, proceeded.
- `get_fw_version` (ESP): commit `316967b7`, built 2026-09-09 21:39:34Z, clean
  tree, **15 commits behind HEAD** (`5b8fc53d`) at session start.
- `safety_fw_version` (Pico, via `get_board_state`): commit `5b8fc53d`, clean,
  protocol 12, `config_version=134`, `config_crc=42374`.
- `safety_get_status`: link up, safety TC valid 39.27 C (CJ 39.14 C), currents
  "not fitted, not fitted, 0.00 A", ct zone `-`, 320 ms old, `tx_dropped 0`,
  `ct_counts 17, 17, 59`.
- `safety_get_diag`: boot reason `watchdog`, state `grace` (startup grace
  window still running), `trip_reason 0`, `warn_mask 0x0000`,
  `trip_mask 0x0000`, `context frames ok 2 / bad 0`.
- `safety_get_commissioning`: **`commissioned=True` already**, config CRC
  42374 matches live/not stale. S1 `abs_max_temp_c=80C` ARMED, S8
  `max_rate_c_per_min=20C/min` ARMED (set by the concurrently-running agent
  noted in the task brief — not touched here). S14/S15 DORMANT, all six
  channels "`i_normal_a` not measured" (expected — no live current yet).
  `estop_active_level=0` (ACTIVE_HIGH, matches bench wiring — left alone).
  `tc_placement_mode=0` (CHAMBER_AGREED), `mains_voltage_v=240`, `tc_type=3`
  (Type K).
- `safety_get_ct_cal`: all three channels `uncalibrated` (this is the
  separate, WARN-only `ct_cal[]` display-correction record, not part of the
  commissioning gate — see that tool's own docstring).
- `thermo_read` / `get_board_state.thermo`: CH0 39.46 C, CH1 39.44 C,
  CH2 39.26 C, all status 0 / flags 0 — nominal, no faults.
- Relays: `io.relays = 0` (all off).
- `profiles_get_exec_status`: `state=0` (idle), no zones running.
- `autotune_get_status`: `state=idle`, no run in progress.

Confirmed: nothing firing, executor and autotune idle, link genuinely up.
Proceeded to stage 2.

## Stage 2 — re-derived checklist (from code, not from the older audit doc)

Read `commissioning_gate_is_commissioned()` (`firmware/SaftyFW/src/commissioning_gate.c`):

```c
bool commissioning_gate_is_commissioned(const config_store_record_t *rec) {
    if (rec->calibration_missing) return false;
    return config_params_all_required_set(rec);
}
```

and `config_params_all_required_set()` (`firmware/SaftyFW/src/config_params.c`,
current HEAD, post-`b5cb83a4`):

```c
required = TC_SOURCE | BORROWED_ZONE_INDEX | TC_PLACEMENT_MODE |
           ABS_MAX_TEMP_C | MAX_RATE_C_PER_MIN | MAINS_VOLTAGE_V |
           TC_TYPE | CT_INSTALLED;
if (ct_installed != 0 && ct_topology != SUMMED) required |= CT_CHANNEL_MAP;
```

`i_normal_a[0..2]` is **not** in this mask at all (S14 treats an unmeasured
channel as inactive, never faulted — it is armed later, from live current,
correctly out of scope for this pass).

Cross-checked against the live record via `GET /api/safety/commissioning`
(raw `params[]`, ESP's cached mirror of the Pico's config page):

| id | field | required? | live value | set? |
|---|---|---|---|---|
| 0x0101 | tc_source | yes | 0 | **yes** |
| 0x0102 | borrowed_zone_index | yes | 0 | **yes** |
| 0x0103 | tc_placement_mode | yes | 0 (CHAMBER_AGREED) | **yes** |
| 0x0104 | abs_max_temp_c | yes | 80 | **yes** |
| 0x0105 | tc_type | yes | 3 (Type K) | **yes** |
| 0x0109 | ct_installed | yes | 1 | **yes** |
| 0x0204 | max_rate_c_per_min | yes | 20 | **yes** (written by the concurrent agent noted in the brief) |
| 0x030E | mains_voltage_v | yes | 240 | **yes** |
| 0x031F | ct_topology | n/a (marker, safe default) | 1 (summed) | n/a |
| 0x0106-0x0108 | ct_channel_map[0..2] | **no** (ct_topology=summed) | unset | unset — correctly not required |
| 0x031A-0x031C | i_normal_a[0..2] | **no** (out of scope per brief) | unset | unset — genuinely requires live current, a later step |

Every REQUIRED bit was already set before this session touched anything.
**No write was needed** — the commissioning gate was already satisfied,
almost certainly by the concurrent agent's `max_rate_c_per_min=20` write
(the last previously-missing required field) landing between the prior
blocked attempt and this session's start.

## Stage 3 — writes

**None.** All gate-required parameters were already set (see table above).
`max_rate_c_per_min` was written by the other concurrently-running agent per
the task brief — not duplicated here. `abs_max_temp_c` (80C) was left
unchanged and confirmed still >= the ESP's own `max_temp_c` (80C, from
`control_get_zones`) — the Pico ceiling is not tighter than the ESP's, per
constraint. `estop_active_level` was left unset/default (0, ACTIVE_HIGH),
matching bench wiring and owner preference, per constraint.

## Stage 4 — verify

- `safety_get_commissioning` (re-read at end of session): `commissioned=True`,
  `config CRC 42374` (unchanged — no commit happened, since no write was
  needed), `config_version=134` (unchanged).
- Guards:
  - **S1** (`abs_max_temp_c=80C`) — **ARMED**.
  - **S8** (`max_rate_c_per_min=20C/min`) — **ARMED**.
  - **S14** (per-channel over-current, WARN only) — **DORMANT** on all three
    channels: `i_normal_a` not measured. Correctly out of scope (requires
    live current, a later step per the brief).
  - **S15** (per-zone under-current, WARN only, `ct_topology=summed`) —
    **DORMANT** on all three zones, same reason as S14.
  - No trip latched (`trip_mask 0x0000`, `warn_mask 0x0000`) throughout.

### ESP-side mirror check (readiness item 10a)

`GET /api/readiness` on the ESP (192.168.1.156, running commit `316967b7`,
built 2026-09-09 21:39:34Z) reports:

```
"safety_commissioned": status "not_done",
  detail "6 of 68 applicable safety parameters still have no value"
```

This **disagrees** with the Pico's own `commissioned=True` verdict — reported
as a finding, not worked around (flashing is out of scope for this task
regardless).

Root cause, confirmed by reading source: `firmware/KilnFW/App/drivers/http/
readiness_http.h`'s `readiness_param_required_for_commissioning()` was fixed
by commit `2900db99` ("Fix ESP readiness item 10a's ct_channel_map mirror
drift on summed-CT boards") to gate `ct_channel_map[0..2]` on
`ct_topology != SUMMED`, mirroring the Pico's `b5cb83a4` fix exactly. That
commit is present in this repo's HEAD (`5b8fc53d`) history but **is 8
commits ahead of the ESP's currently-flashed build** (`316967b7`):

```
316967b7 (flashed)
...
2900db99  Fix ESP readiness item 10a's ct_channel_map mirror drift  <-- fix, not yet flashed
...
5b8fc53d (repo HEAD)
```

The flashed build still runs the OLD, unconditional
`readiness_param_required_for_commissioning()` that counts `ct_channel_map[0..2]`
as applicable/unset regardless of topology. That is exactly 3 of the "6
unset": `ct_channel_map[0..2]` (wrongly still counted, pre-fix) +
`i_normal_a[0..2]` (correctly counted — deliberately unmeasured, per the
header comment this field is never topology-gated). Once `2900db99` is
flashed, the same board should report 3 unset (`i_normal_a[0..2]` only), not
6, and the two sides' summaries will read consistently (item 10a still
"not_done" until current is measured, but for the right, documented reason).

**No flash was performed** — out of scope for this task, and the brief
explicitly forbids it.

### Other findings noticed, not acted on (out of scope for this task)

- `get_heap_status` surfaced an **UNACKNOWLEDGED CRASH REPORT**:
  `exc_task='profile_executo'`, `exc_cause_str='IllegalInstruction'`,
  `reset_reason='PANIC'`. The board's current live `reset_reason` is
  `software (esp_restart)` at `uptime_s=8311` (a later, clean reboot) — the
  panic is a **prior** boot's unacknowledged crash, not a mid-session event
  during this work. `GET /api/readiness`'s `crash_report` item independently
  flags this as `not_done`. This is unrelated to CT/safety commissioning and
  was not investigated further here; flagging per the standing practice of
  not treating an unacknowledged crash as "board is healthy."
- `estop_verified` readiness item is `not_done` ("never confirmed... Run the
  README.md bench procedure"). Also out of scope for this pass (config-only,
  no heat).

## Stage 5 — what is now possible

`safety_core_request_enable()` refuses any enable while `calibration_missing`
is set (`commissioning_gate_energize_allowed()` calls
`commissioning_gate_is_commissioned()`, which is false whenever
`calibration_missing` is true). Since `commissioning_gate_is_commissioned()`
already returns true on this board (`config_params_all_required_set()` is
satisfied and `calibration_missing` is therefore clear), **the commissioning
gate itself would now permit `safety_core_request_enable()` to proceed** —
the CT/calibration block that caused the original no-heat symptom is
resolved.

This says nothing about whether a real enable/heat request is otherwise
safe or advisable right now (E-stop interlock still `not_done`/unverified per
readiness, and an unacknowledged crash report is outstanding) — those are
separate gates/considerations the owner is confirming separately. No enable
request, relay closure, or heat-path verification was attempted, per the
task's explicit instructions.

## Board left in

- Relays: off (`io.relays = 0`).
- Not enabled, nothing firing, no autotune running (`profiles_get_exec_status`
  state=idle, `autotune_get_status` state=idle).
- No trip latched (`trip_mask 0x0000`, `warn_mask 0x0000`) at end of session.
- No writes were made to the safety processor's config in this session (all
  required fields were already set); `config_crc`/`config_version` are
  unchanged (42374 / 134) from session start to end.
