# Review: f5holderfx3 (F5HOLDERFX2 LOW-1..4 fixes), 2026-10-12

Scope: code commit `bd0b1e9c2` plus doc commit(s) up to `6c4acd56c` on origin/dev.
Adversarial review of safety-relevant relay-drive code: retry placement in the
profile executor tick, the new Pico-TRIPPED gate on ON writes, the source-order
test, and the LOW-2 rule added to `tools/check_app_hold_registration.ps1`.
Reviewed in worktree `C:\wt\rvf5h3_0tptb6` at `6c4acd56c`. No board access.

## Verdict

**No HIGH or MED findings. This does not block promotion.** The fix does what it
claims for the per-tick path: the retry now runs after every guard, the aux
tick and the mode check, still under `s_exec.lock`, and every ON write in the
tick (segment start, aux tick, retry) now refuses on a fresh Pico TRIPPED.
The residual gaps below are all bounded by the 2 s watchdog Pico-TRIPPED fault,
the same bound the prior review accepted for its LOW-3.

## Findings (severity-ranked)

### LOW-1: warm-replay start can energise a relay against a latched Pico TRIPPED; the in-code justification is wrong

`io_seg_start_since()` (`profile_executor_relay_io.c:1182`) passes
`pico_tripped=false`, justified by the comment "gated by
relay_authority_start_blocked() just before". That gate does not look at the
Pico at all: `relay_authority_start_blocked()` (`owners/relay_authority.h:346`)
checks `relay_authority_on_blocked()` (ESP-side fault-source bits only; there is
no `SAFETY_FAULT_SRC_*` bit for a Pico trip) and `link_up`. It never reads
`diag_state`.

Scenario: a Pico trip faults a run (watchdog, FAULTED, everything off, APP
source asserted); the operator presses Stop, which releases APP; the Pico is
still latched TRIPPED, link up, kiln warm. A restart is accepted
(`profile_executor_run()` has no Pico-TRIPPED refusal either), warm replay runs
in `profile_executor_run.c:1466-1473` and drives each skipped RELAY_IO segment's
ON, including a spare relay that is not behind K4. It stays on until the next
watchdog pass faults the run, up to about 2 s (`WATCHDOG_CHECK_PERIOD_MS`).
Same class and bound as F5HOLDERFX2 LOW-3, just on the start path the fix left
out. `io_seg_start_since()` is the only remaining ON-capable caller of a
`false`-passing wrapper (`aux_apply_relay(i,false)` at relay_io.c:841/1014 is
OFF-only; `io_seg_start()` and `profile_executor_aux_tick()` have no non-test
callers).

Answer to Q2: passing `false` is not justified as written, but the risk is
bounded and not new. Warm replay can get the Pico state cheaply:
`relay_authority_start_blocked()` already calls `safety_link_get_status()` under
`s_exec.lock` on this exact path, and the same struct carries
`diag_ever_received`, the DIAG age and `diag_state`. Recommended fix: refuse in
`relay_authority_start_blocked()` when the DIAG is fresh and TRIPPED (same
staleness rule as `pre_lock_pico_tripped`). That closes warm replay, removes the
"start accepted, faulted 2 s later" behaviour for every start (profile and
autotune), and makes the comment true. Fix the comment either way.

### LOW-2: `pre_lock_pico_tripped` is still a pre-lock sample (residual TOCTOU)

The value is sampled before `xSemaphoreTake(s_exec.lock, portMAX_DELAY)`
(`profile_executor.c:740-748`), so it can be older than an unbounded lock wait
plus the whole tick body. A TRIPPED report that lands in that window still lets
the segment start, the aux tick and the retry write ON this tick. The prior
review recorded this as INFO; it matters slightly more now that the retry is the
last write in the tick and is explicitly gated on this value. Bound: next
watchdog pass, about 2 s; the Pico itself has already opened its own contactor
path. Cheap improvement: re-sample the DIAG under the lock immediately before
`io_segs_retry_on()` (the same `safety_link_get_status()` call `run()` already
makes under this lock) and OR it in.

Answer to Q1, otherwise: no. Pause, stop and halt all take `s_exec.lock`, so
they cannot interleave with the tick. Guard trips (`escalate_guard_trip()`
returns true only after `exec_enter_terminal_state(FAULTED)`) and mode
violations (`exec_handle_mode_state_violation()`) leave the state not RUNNING,
and the global-trip branch also asserts a fault source that
`io_segs_retry_on()` re-checks via `relay_authority_on_blocked()`. A run that
reached DONE is not RUNNING. A concurrent force-off is caught by the off-epoch
in `kiln_io_owner_command_set_relay_mask_authorized_since()`. The
`!run_faulted_this_tick` term is redundant with the RUNNING check (mutation H-a
below is MISSED for that reason, which is fine).

### LOW-3: the LOW-2 rule in `check_app_hold_registration.ps1` has false accepts

The rule resolves a variable mask by finding `NAME =`/`NAME |=` assignments in
the function body and accepting the site if every RHS contains a non-APP
`SAFETY_FAULT_SRC_*` constant, no APP, no call, and only operator characters.
Confirmed false accepts (mutations C5-C8 below, all MISSED):

- any bare identifier, field or array element on the RHS beside one non-APP
  constant (`... | s_at.global_fault_source`), including a macro that expands
  to APP;
- compound operators the regex does not match: `+=`, `^=` (also `<<=`, `&=`);
- writes through a pointer or out-parameter (`fill_source(&source)`);
- assignments to a file-scope variable made in another function.

False rejects (call on the RHS, `~`, `source &= ~APP`, a mask that is only a
function parameter) all fail closed, which is acceptable. Multi-line
assignments are handled (`[^;]+` spans newlines). A function whose closing
brace is not at column 0 only widens the scanned body, which is stricter.
Fix suggestion: whitelist the RHS token stream instead of a character class
(only `SAFETY_FAULT_SRC_<non-APP>`, `?`, `:`, `|`, `(`, `)`, and comparisons
against named enum constants), reject any operator-assignment to `NAME` other
than `=`/`|=`, and reject any `&NAME` in the body.

Today's single resolved site (`autotune_engine_guard.c:184-188`) is correct, so
this is a check-robustness gap, not a live defect.

### LOW-4: source-order test is fragile in both directions and does not pin the run-faulted term

`test_f5holderfx2_retry_placement_and_pico_gate`
(`test_profile_executor_prestart.c:2968-3066`) finds the retry with `strstr`
(first occurrence) and each guard call with `pe_last_occurrence()` over the
whole file:

- false red: any later legitimate use of `exec_mode_state_check(`,
  `thermal_guard_tick(` etc. anywhere after the tick (e.g. in the watchdog
  task) fails the test with no behaviour change;
- false green: a second, differently spelled retry before the guards
  (`io_segs_retry_on(false);`) passes (mutation H-g); text inside `#if 0` or a
  dead branch passes;
- it does not check `!run_faulted_this_tick` (harmless today, see LOW-2).

The behavioural parts (H6 pending-ON survival, `io_seg_start_pico` and
`aux_apply_relay_pico` with tripped true/false) are real tests. A behavioural
tick test would be better but the tick loop is not host-drivable (the same H5
limitation the prior review recorded). Acceptable as is; scoping the search to
the tick function body would remove the false-red mode.

### INFO

- The comment on the unowned-relay sweep (`profile_executor.c:~1991`) still
  says it is the "Last thing in the tick that touches copper"; the retry now
  runs after it. Update the comment.
- A normal segment-start ON still precedes the thermal guards in the same tick,
  so it can pulse ON then OFF within one tick. Pre-existing.
- Non-relay `IO_` segment outputs (`kiln_io_owner_command_set_io` in
  `io_seg_start_since_pico`) have no authority or Pico gate at all.
  Pre-existing; worth a deliberate decision about whether any IO_ target can
  energise a load.
- `aux_tick` Pico gate (`profile_executor_aux_tick_pico`, relay_io.c:1003) has
  no direct test; see H-f.

## Mutation testing

All runs used `tools/negtest.ps1` from `C:\wt\rvf5h3_0tptb6` at `6c4acd56c`;
baseline passed in each run; real tree unchanged and copies removed.

### Check script (`-Preset check -PresetArg tools\check_app_hold_registration.ps1`)

Target: `firmware/KilnFW/App/drivers/control/autotune_engine_guard.c`.

```json
[
 {
  "name": "C5 bare field OR APP-capable",
  "file": "firmware/KilnFW/App/drivers/control/autotune_engine_guard.c",
  "find": ": SAFETY_FAULT_SRC_THERMAL_SANITY;",
  "replace": ": SAFETY_FAULT_SRC_THERMAL_SANITY | s_at.global_fault_source;"
 },
 {
  "name": "C6 += APP",
  "file": "firmware/KilnFW/App/drivers/control/autotune_engine_guard.c",
  "find": "safety_link_set_fault_source(s_at.safety, source, true);",
  "replace": "source += SAFETY_FAULT_SRC_APP; safety_link_set_fault_source(s_at.safety, source, true);"
 },
 {
  "name": "C7 ^= APP",
  "file": "firmware/KilnFW/App/drivers/control/autotune_engine_guard.c",
  "find": "safety_link_set_fault_source(s_at.safety, source, true);",
  "replace": "source ^= SAFETY_FAULT_SRC_APP; safety_link_set_fault_source(s_at.safety, source, true);"
 },
 {
  "name": "C8 out-param write",
  "file": "firmware/KilnFW/App/drivers/control/autotune_engine_guard.c",
  "find": "safety_link_set_fault_source(s_at.safety, source, true);",
  "replace": "fill_source(&source); safety_link_set_fault_source(s_at.safety, source, true);"
 },
 {
  "name": "C1 |= APP",
  "file": "firmware/KilnFW/App/drivers/control/autotune_engine_guard.c",
  "find": "safety_link_set_fault_source(s_at.safety, source, true);",
  "replace": "source |= SAFETY_FAULT_SRC_APP; safety_link_set_fault_source(s_at.safety, source, true);"
 },
 {
  "name": "C2 later = APP",
  "file": "firmware/KilnFW/App/drivers/control/autotune_engine_guard.c",
  "find": "safety_link_set_fault_source(s_at.safety, source, true);",
  "replace": "source = SAFETY_FAULT_SRC_APP; safety_link_set_fault_source(s_at.safety, source, true);"
 },
 {
  "name": "C4 call term",
  "file": "firmware/KilnFW/App/drivers/control/autotune_engine_guard.c",
  "find": ": SAFETY_FAULT_SRC_THERMAL_SANITY;",
  "replace": ": SAFETY_FAULT_SRC_THERMAL_SANITY | pick_src();"
 }
]
```

| Mutation | Result |
|---|---|
| C5 `... THERMAL_SANITY \| s_at.global_fault_source` | MISSED (LOW-3) |
| C6 `source += SAFETY_FAULT_SRC_APP;` | MISSED (LOW-3) |
| C7 `source ^= SAFETY_FAULT_SRC_APP;` | MISSED (LOW-3) |
| C8 `fill_source(&source);` | MISSED (LOW-3) |
| C1 `source \|= SAFETY_FAULT_SRC_APP;` | CAUGHT |
| C2 `source = SAFETY_FAULT_SRC_APP;` | CAUGHT |
| C4 `... \| pick_src()` | CAUGHT |

### Host tests

Command (narrowed to the affected test file, every build into a fresh `{OUT}`):
`negtest.ps1 -Command 'powershell -NoProfile -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\build_host_tests.ps1 -OutDir "{OUT}" -Only prestart' -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES" -Parallel 3`

```json
[
 {
  "name": "H-a drop !run_faulted_this_tick",
  "file": "firmware/KilnFW/App/drivers/control/profile_executor.c",
  "find": "if (s_exec.state == PROFILE_EXEC_RUNNING && !run_faulted_this_tick) {",
  "replace": "if (s_exec.state == PROFILE_EXEC_RUNNING) {"
 },
 {
  "name": "H-b retry passes false",
  "file": "firmware/KilnFW/App/drivers/control/profile_executor.c",
  "find": "io_segs_retry_on(pre_lock_pico_tripped);",
  "replace": "io_segs_retry_on(false);"
 },
 {
  "name": "H-c seg start passes false",
  "file": "firmware/KilnFW/App/drivers/control/profile_executor.c",
  "find": "io_seg_start_pico(s_exec.segment_index, seg, pre_lock_pico_tripped);",
  "replace": "io_seg_start_pico(s_exec.segment_index, seg, false);"
 },
 {
  "name": "H-d aux_apply_relay_pico ignores pico",
  "file": "firmware/KilnFW/App/drivers/control/profile_executor_relay_io.c",
  "find": "if (want_on && (pico_tripped || relay_authority_on_blocked(s_exec.safety, &sources))) {",
  "replace": "if (want_on && (relay_authority_on_blocked(s_exec.safety, &sources))) {"
 },
 {
  "name": "H-e io_seg_start_since_pico ignores pico",
  "file": "firmware/KilnFW/App/drivers/control/profile_executor_relay_io.c",
  "find": "if (drive_on && (pico_tripped || relay_authority_on_blocked(s_exec.safety, &blocked_sources))) {",
  "replace": "if (drive_on && (relay_authority_on_blocked(s_exec.safety, &blocked_sources))) {"
 },
 {
  "name": "H-f aux tick ignores pico",
  "file": "firmware/KilnFW/App/drivers/control/profile_executor_relay_io.c",
  "find": "bool authority_blocked = relay_authority_on_blocked(s_exec.safety, &sources) || pico_tripped;",
  "replace": "bool authority_blocked = relay_authority_on_blocked(s_exec.safety, &sources);"
 },
 {
  "name": "H-g extra ungated retry before guards",
  "file": "firmware/KilnFW/App/drivers/control/profile_executor.c",
  "find": "if (thermal_guard_tick(&z->guard_state, &guard_cfg_this_tick, &gin)) {",
  "replace": "io_segs_retry_on(false); if (thermal_guard_tick(&z->guard_state, &guard_cfg_this_tick, &gin)) {"
 },
 {
  "name": "H-h retry moved before aux tick",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/control/profile_executor.c",
    "find": "io_segs_retry_on(pre_lock_pico_tripped);",
    "replace": "(void)0;"
   },
   {
    "file": "firmware/KilnFW/App/drivers/control/profile_executor.c",
    "find": "profile_executor_aux_tick_pico(dt_s, stretched_this_tick, relays_on_count, on_off_cap, pre_lock_pico_tripped);",
    "replace": "io_segs_retry_on(pre_lock_pico_tripped); profile_executor_aux_tick_pico(dt_s, stretched_this_tick, relays_on_count, on_off_cap, pre_lock_pico_tripped);"
   }
  ]
 },
 {
  "name": "H-i retry_on ignores pico",
  "file": "firmware/KilnFW/App/drivers/control/profile_executor_relay_io.c",
  "find": "if (!s_exec.io || pico_tripped) {",
  "replace": "if (!s_exec.io) {"
 }
]
```

| Mutation | Result | Note |
|---|---|---|
| H-a drop `&& !run_faulted_this_tick` | MISSED | expected: redundant with the RUNNING check (LOW-2) |
| H-b retry passes `false` | CAUGHT | test_profile_executor_prestart.c:3043 (exact-string match only) |
| H-c segment start passes `false` | CAUGHT | :3060 (string check) |
| H-d `aux_apply_relay_pico` ignores Pico | CAUGHT | :3027 (behavioural) |
| H-e `io_seg_start_since_pico` ignores Pico | CAUGHT | :3015 (behavioural) |
| H-f aux tick ignores Pico | MISSED | no test drives `profile_executor_aux_tick_pico` with tripped=true (INFO) |
| H-g extra `io_segs_retry_on(false);` before the thermal guards | MISSED | harmful; source-order test only finds the exact spelling (LOW-4) |
| H-h retry moved before the aux tick | CAUGHT | :3052, :3053 |
| H-i `io_segs_retry_on` ignores Pico | CAUGHT | :2952 (behavioural) |

Baseline passed (115 s). H-f and H-g are real coverage gaps (LOW-4 / INFO), neither is
a defect in the shipped code.

## Fix status

| Finding | Status | Fix |
|---|---|---|
| LOW-1 warm replay / start vs latched Pico TRIPPED | FIXED | `relay_authority_start_blocked()` refuses when the DIAG is fresh and TRIPPED (`relay_authority_pico_tripped_fresh()`); closes warm replay, every profile start and every autotune start. Policy: link down or never-up -> refused (link_up already required); DIAG never received or stale on a live link -> NOT treated as tripped (the 2 s watchdog Pico-TRIPPED fault covers it). `io_seg_start_since()` comment corrected. Tests: gate allowed/refused/stale, warm start refused with no ON write. |
| LOW-2 pre-lock Pico sample | FIXED (retry only) | Under-lock re-sample of the Pico state immediately before `io_segs_retry_on()`, OR-ed with the pre-lock value. Segment start and aux ON keep the pre-lock sample (not re-sampled; residual bounded by the 2 s watchdog). |
| LOW-3 check script false accepts | FIXED | Variable-mask rule now requires plain `=`/`|=`, rejects `+=`/`^=`/`&=`/`++`/`--`, rejects `&name`, and whitelists the RHS (literal non-APP `SAFETY_FAULT_SRC_*`, `? : \| ( )`, `x == NAMED_CONST` conditions). Real site `autotune_engine_guard.c` still accepted. |
| LOW-4 source-order test | FIXED | Scoped to `executor_task_entry()` body, `#if 0` blocks removed, asserts exactly one `io_segs_retry_on(` in the tick, after the guards. |
| INFO stale "Last thing in the tick" comment | FIXED | Reworded. |
| INFO aux tick Pico gate untested (H-f) | FIXED | `test_f5holderfx3_aux_tick_pico_gate`. |
