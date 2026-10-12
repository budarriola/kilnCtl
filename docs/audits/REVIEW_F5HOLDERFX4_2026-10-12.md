# Review: F5HOLDERFX4 (3d40ca574) -- fixes for REVIEW_F5HOLDERFX3

Date: 2026-10-12. Reviewer: Claude (Opus), read-only review plus negtests. No code changed.

Target: origin/dev 3d40ca574, which addresses REVIEW_F5HOLDERFX3 LOW-1 (start gate via
`relay_authority_pico_tripped_fresh()`), LOW-2 (under-lock re-sample before `io_segs_retry_on`),
LOW-3 (`check_app_hold_registration.ps1` RHS whitelist), LOW-4 (test scoped to
`executor_task_entry()`, 700 B lookback) and the INFO items (comment fix,
`test_f5holderfx3_aux_tick_pico_gate`).

Owner rule under review: never energize copper while the Pico is TRIPPED.

## Verdict

The four LOW fixes do what they say and add no new lock-order hazard. One MED remains. The
helper's "fresh DIAG" test does not measure DIAG freshness. A DIAG that was never received, or a
cached DIAG from before a link outage, reads as "not tripped". The comment says the 2 s watchdog
covers that case, but the watchdog uses the same rule and is just as blind. So for a few seconds
after link-up a start or warm replay can energize relays that are not behind K4 (aux, spare,
RELAY_IO) while the Pico is latched TRIPPED. The readiness gate at the door had the same blind
spot before this commit, so this is not a regression. But the commit's stated policy rests on a
coverage claim that is false.

## Findings

### MED-1: "never received or stale DIAG reads as not tripped" is not covered by the watchdog

`relay_authority.h:347-355`:

```c
static inline bool relay_authority_pico_tripped_fresh(const safety_link_status_t *st)
{
    return st->diag_ever_received && !safety_link_is_stale(st->age_ms, SAFETY_LINK_STALE_MS) &&
           st->diag_state == SAFETY_LINK_DIAG_STATE_TRIPPED;
}
```

There are three problems.

1. `st->age_ms` is the age of the last STATUS frame (Frame A, `cached_tick`, 500 ms period), not
   of the DIAG. The real DIAG age has been `diag_frame_age_ms` since TRIPFX3 LOW-B
   (`safety_link.c:702`, from `diag_tick` at `safety_link_frames.c:1176`). On a live link
   `age_ms` is always fresh, so the "stale" half of the rule never fires. A cached
   `diag_state` of any age passes as fresh.
2. The comment says a stale or missing DIAG on a live link "is covered by the 2 s watchdog
   Pico-TRIPPED fault". The watchdog (`profile_executor.c:2336-2356`) computes
   `safety_diag_valid = diag_ever_received && !safety_link_is_stale(age_ms, ...)` and
   `safety_processor_tripped = safety_diag_valid && diag_state == TRIPPED`. That is the same
   rule on the same fields. It cannot see a trip any sooner than the helper can. Its comment
   (lines 2328-2330) still says `age_ms` doubles as the DIAG age, which TRIPFX3 LOW-B made
   untrue.
3. The readiness gate (`readiness_safety_trip_status(link_up, diag_trip_mask)`) is blind in the
   same way: `diag_trip_mask` is 0 before the first DIAG and stays at its old value until a new
   DIAG arrives.

Two reachable scenarios:

- **A, ESP reboot.** An ESP reset normally latches S6a (mainFault) on the Pico. STATUS arrives
  within about 0.5 s, and `link_up` follows within 3 polls. The first DIAG (sent every
  `LINK_DIAG_TX_PERIOD_MS` = 2000) can arrive up to 2 s later. Until it does,
  `diag_ever_received` is false, so the helper, the watchdog and readiness all read "not
  tripped". A start in that window (MCP or HTTP right after a flash is realistic) passes the
  door and the at-lock gate. Warm replay (`profile_executor_run.c:1451-1478`) then drives
  skipped RELAY_IO segments ON, and the aux tick can switch aux outputs ON.
- **B, link outage.** A link outage trips the Pico (S6b). When the link recovers, STATUS is
  fresh again. The cached DIAG predates the outage and reads OK/ARMED. Its own
  `diag_frame_age_ms` is large, but nothing reads that field. Same outcome as A until the next
  DIAG arrives.

Heater relays are behind K4, which the Pico holds open while tripped, so the heating elements stay
dark. Aux, spare-relay and RELAY_IO outputs are not behind K4, so the owner rule is broken for
them.

**Window:** up to one DIAG period (2 s) for the start to slip through, plus up to one watchdog
pass (`WATCHDOG_CHECK_PERIOD_MS` = 2000) before the fault drops the outputs. That is about 4 s
nominal per event. It is longer for each DIAG frame lost to CRC errors, and unbounded if DIAG
frames never decode (for example after a protocol mismatch) while STATUS frames do.

**Better fail-closed rule.** Make it asymmetric:

- "tripped" = `diag_ever_received && diag_state == TRIPPED`, regardless of age. Pico reboot
  handling (`safety_note_pico_reboot_locked`) already keeps the cached state on purpose so that a
  stale TRIPPED fails closed. Do not let age turn a TRIPPED into "not tripped".
- "may start / may replay / may switch on" additionally requires `diag_ever_received &&
  diag_since_reboot && diag_frame_age_ms <= SAFETY_LINK_DIAG_FRESH_MS` (4500, already defined
  and unused here). Ideally it also requires a DIAG received since the last link-up edge, which
  closes scenario B exactly. Otherwise refuse with "waiting for the safety processor's status
  report -- retry in a few seconds".
- The watchdog's `safety_diag_valid` should use `diag_frame_age_ms` too, and its comment needs
  fixing.

Minor: the helper comment says stale means `age >= SAFETY_LINK_STALE_MS`, but
`safety_link_is_stale()` (`safety_link.h:489-492`) uses `>`. Negtest M1 below shows no test pins
the boundary either way.

The tests pin the fail-open policy. Negtest E3 (the helper reads `diag_frame_age_ms` against
`SAFETY_LINK_DIAG_FRESH_MS`) is CAUGHT by `test_profile_executor_prestart.c:13732` ("stale TRIPPED
DIAG on a live link -> allowed (documented policy; the 2 s watchdog covers it)") and `:12181`
("(b) stale TRIPPED is not tripped: aux stays ON"). These assertions encode the premise this
finding shows to be false. A fix for MED-1 must change them, not just the helper.

### LOW-1: right after a clear-trip, starts and resume are refused with misleading text

`safety_clear_trip` itself is not gated, which is correct. But the cached `diag_state` stays
TRIPPED until the next DIAG, up to 2 s after the clear. In that window:

- Start and autotune refuse with "the safety processor is tripped (clear the trip first)". That
  is misleading because the user just cleared it. A retry a few seconds later works. The
  readiness gate had the same behavior before this commit.
- Resume goes through `profile_executor_status.c:365` (`start_blocked(NULL, 0, "resume")`),
  which passes no buffer, so there is no reason text. The HTTP resume handler
  (`dashboard_exec_http.c` ~849) maps every refusal to 400 "nothing paused to resume", which
  is wrong when something is paused. The LCD resume (`ui_page_home_actions.c:360`) ignores the
  result entirely.

Nothing that must be allowed is blocked for good. Clear-trip, then a start or autotune after the
next DIAG, all work. Suggested fix: when a clear was sent less than one DIAG period ago, say
"trip clear pending confirmation -- retry in a few seconds". Pass an error buffer from resume and
surface it in HTTP and on the LCD.

Refusal-text surfacing otherwise checks out. HTTP start (`start_send_run_failure`) returns the
text as 400 JSON (128-char buffer, escaped). The LCD "Cannot Start" modal shows `err_msg`.
Autotune HTTP returns 400 JSON. The UART bridge (`uart_bridge_ext_control.c:571`) passes
`err_msg` through. The new text is about 80 characters, inside every buffer.

### LOW-2: LOW-2 residuals -- three consumers still use the pre-lock sample, and the lock wait is unbounded

`pre_lock_pico_tripped` is sampled before `xSemaphoreTake(s_exec.lock, portMAX_DELAY)`
(`profile_executor.c:738-746`). Only `io_segs_retry_on` got the under-lock re-sample
(2079-2088). These three still use the pre-lock value:

- `profile_executor_aux_fault_drop` (line 780);
- `io_seg_start_pico` (line 1047, a segment's first tick);
- `profile_executor_aux_tick_pico` (line 1985).

The wait is unbounded (`portMAX_DELAY`). A trip that lands while the tick waits for the lock is
missed by all three for one tick. In practice the lock is held for short sections, so the gap is
one tick plus the wait. Guard 9 cuts relays if the tick stalls outright. The next tick's sample
catches the trip, and each `relay_authority` relay-on path still checks fault sources. So this
stays LOW. Cheap fix: take one sample right after the lock take and OR it in for all four
consumers, which also removes the special case in the retry block.

### INFO-1: LOW-1's original scenario was already mostly blocked at the door

`profile_executor_run()` (~283) and autotune already refused a start on a latched trip through
the readiness gate, as long as a DIAG had arrived. The new gate adds an at-lock re-check (a trip
between the door and the commit) and covers resume. Both are real improvements, but small ones.

### INFO-2: the at-lock gate is tested for a link drop, not for a trip

`test_f5holderfx3_start_gate_pico_tripped`'s warm-start case is refused by the early
`start_blocked` call (`profile_executor_run.c:415`), so it never reaches line 1441. The line-1441
call itself is covered: negtest E4 (removing it) is CAUGHT by the LD-01 "link drops before commit"
tests (`test_profile_executor_prestart.c:3408-3410`). Because both calls share
`relay_authority_start_blocked()`, the trip branch at line 1441 is covered indirectly. A direct
test would trip the stub between the two calls and assert that nothing was replayed. Low value;
optional.

### INFO-3: the stub never sets `diag_frame_age_ms`

`test_profile_executor_prestart.c`'s stub uses `g_stub_diag_age_ms` to set `out->age_ms` only.
A test of the MED-1 fix will need the stub to set `diag_frame_age_ms` and `diag_since_reboot`
separately from `age_ms`.

### INFO-4: the 700 B lookback can be exact

LOW-4's test checks only that `PROFILE_EXEC_RUNNING` appears somewhere in the 700 bytes before
`io_segs_retry_on(`. That passes today: by inspection no other `PROFILE_EXEC_RUNNING` sits within
700 B before the call. But it would also pass if the call moved out of the `if` and a different
RUNNING test sat nearby. Exact alternative: find the literal
`if (s_exec.state == PROFILE_EXEC_RUNNING && !run_faulted_this_tick) {`, brace-match its block
inside the scoped `executor_task_entry()` body, and assert that the single `io_segs_retry_on(`
lies inside that block. Negtest E2 below shows the lookback does catch dropping the RUNNING term,
so it is adequate. It is just not exact.

### INFO-5: locking and ordering

No new hazard. The tick calls the helper in two places:

- before the lock, where only `state_lock` is taken;
- under `s_exec.lock`, where the order is `s_exec.lock -> link->state_lock`.

`run()` already calls `start_blocked` under `s_exec.lock`, so that order existed before.
`state_lock` is a leaf lock (`safety_lock`). `safety_build_and_send_context` calls
`profile_executor_get_status` without `state_lock` held, so no reverse order exists.

`retry_st` (a `safety_link_status_t`) adds stack to `executor_task_entry`. GCC likely reuses
`pre_st`'s slot for it, but no target build was run for this review. The next full dev run should
include `check_executor_task_stack_budget.ps1`.

### LOW-3 fix (check_app_hold_registration.ps1)

The RHS whitelist accepts only `=` and `|=`, and it rejects `&name`, `++`/`--` and compound
operators. That matches the finding. No issues.

## Answers to the review questions

1. **Is a stale or never-received DIAG on a live link safe to treat as not tripped?** No. See
   MED-1. The window is about 4 s nominal per ESP reboot or link recovery, and unbounded if DIAG
   frames stop decoding. The watchdog does not cover it. Fail closed on missing or old DIAG for
   starts, and treat a cached TRIPPED as tripped at any age. Rated MED.
2. **Do the LOW-2 residuals matter, and is the lock wait unbounded?** Three consumers still use
   the pre-lock sample, and the wait is unbounded (`portMAX_DELAY`). The gap is one tick, and
   guard 9 plus the next sample bound it. LOW. A single post-lock re-sample fixes all of them.
3. **Does the gate block starts that must be allowed, and is the text surfaced correctly?** Only
   briefly after a clear-trip (cached TRIPPED until the next DIAG), with misleading text. Resume's
   refusal is mislabeled over HTTP and silent on the LCD. LOW-1. Start text reaches HTTP, the LCD,
   autotune and the UART bridge intact.
4. **Is the 700 B lookback acceptable, or can it be exact?** Acceptable. E2 is caught. It can be
   made exact by brace-matching the retry block (INFO-4). The same textual test misses M2
   (dropping the pre-lock OR), which has low impact.
5. **Are there locking or ordering hazards from calling the helper in the tick?** None new
   (INFO-5). Stack growth is unmeasured.

## Negtests

All mutations ran over the FULL kilnfw-host suite (no `-Only`), in a throwaway worktree, with
`-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`. For each CAUGHT,
the log was checked to confirm the mutant compiled and an assertion failed.

| Mutation | What it does | Result | Evidence |
|---|---|---|---|
| M1_fresh_boundary_lt | helper treats `age == STALE_MS` as stale (`<` instead of `!>`) | MISSED | boundary not pinned (MED-1 minor note) |
| M2_retry_under_lock_only | retry uses only the under-lock sample, drops `pre_lock ||` | MISSED | LOW-4's test is textual (one call, `retry_pico_tripped` text present); the OR is not checked. Low impact: the under-lock sample is newer, so dropping the OR loses only a TRIPPED that a newer DIAG already cleared |
| M3_start_gate_ignores_link_up | `start_blocked` drops `!st.link_up` | CAUGHT | prestart.c:3382, 2334-2336, 3717-3723, 3782 |
| M4_trip_refusal_only_on_warm | trip refusal removed from `start_blocked`; warm replay skipped while tripped instead | CAUGHT | prestart.c:13728-13729, 13750, 13752 |
| E1_drop_diag_ever_received | helper drops `diag_ever_received &&` | CAUGHT | prestart.c:12183 |
| E2_retry_drop_running_term | retry `if` drops `state == RUNNING` | CAUGHT | prestart.c:3127 (the 700 B lookback) |
| E3_helper_uses_diag_frame_age | helper uses `diag_frame_age_ms`/`DIAG_FRESH_MS` (the MED-1 direction) | CAUGHT | prestart.c:13732, 12181 (they pin the current fail-open policy). Three other test targets (board_temps, kiln_io_owner, kiln_io_sx_fake) failed to build under this mutant; the CAUGHT rests on the prestart assertions, which compiled and ran |
| E4_drop_at_lock_start_gate | line-1441 `start_blocked` call replaced by `if (false)` | CAUGHT | prestart.c:3408-3410, 11513, 3717-3723 |
| E5_drop_resume_gate | resume `start_blocked` replaced by `if (false)` | CAUGHT | prestart.c:2334-2339 |
| E6_aux_tick_ignores_trip | aux tick passed `false` instead of the tick's trip sample | CAUGHT | prestart.c:3131 |
| E7_seg_start_ignores_trip | segment start passed `false` instead of the tick's trip sample | CAUGHT | prestart.c:3129 |

The baseline passed (334.7 s). For every CAUGHT except E3, the log was checked: the mutant
compiled and the listed assertions failed. negtest's final verdict was ERROR, not a test result.
I wrote this document into the worktree while negtest ran, which its real-tree guard flags. The
mutated files were confirmed unchanged and the throwaway copies were removed.

Mutation JSON:

```json
[
 {"name":"M1_fresh_boundary_lt","file":"firmware/KilnFW/App/drivers/owners/relay_authority.h",
  "find":"!safety_link_is_stale(st->age_ms, SAFETY_LINK_STALE_MS)",
  "replace":"(st->age_ms != SAFETY_LINK_AGE_NEVER && (uint32_t)st->age_ms < SAFETY_LINK_STALE_MS)"},
 {"name":"M2_retry_under_lock_only","file":"firmware/KilnFW/App/drivers/control/profile_executor.c",
  "find":"retry_pico_tripped = retry_pico_tripped || relay_authority_pico_tripped_fresh(&retry_st);",
  "replace":"retry_pico_tripped = relay_authority_pico_tripped_fresh(&retry_st);"},
 {"name":"M3_start_gate_ignores_link_up","file":"firmware/KilnFW/App/drivers/owners/relay_authority.h",
  "find":"if (safety_link_get_status(safety, &st) != ESP_OK || !st.link_up) {",
  "replace":"if (safety_link_get_status(safety, &st) != ESP_OK) {"},
 {"name":"M4_trip_refusal_only_on_warm","edits":[
   {"file":"firmware/KilnFW/App/drivers/owners/relay_authority.h",
    "find":"if (relay_authority_pico_tripped_fresh(&st)) {",
    "replace":"if (false && relay_authority_pico_tripped_fresh(&st)) {"},
   {"file":"firmware/KilnFW/App/drivers/control/profile_executor_run.c",
    "find":"    if (warm_replay_end > 0) {",
    "replace":"    safety_link_status_t m4st;\n    if (warm_replay_end > 0 && !(safety_link_get_status(s_exec.safety, &m4st) == ESP_OK && relay_authority_pico_tripped_fresh(&m4st))) {"}]},
 {"name":"E1_drop_diag_ever_received","file":"firmware/KilnFW/App/drivers/owners/relay_authority.h",
  "find":"return st->diag_ever_received && !safety_link_is_stale",
  "replace":"return !safety_link_is_stale"},
 {"name":"E2_retry_drop_running_term","file":"firmware/KilnFW/App/drivers/control/profile_executor.c",
  "find":"if (s_exec.state == PROFILE_EXEC_RUNNING && !run_faulted_this_tick) {",
  "replace":"if (!run_faulted_this_tick) {"},
 {"name":"E3_helper_uses_diag_frame_age","file":"firmware/KilnFW/App/drivers/owners/relay_authority.h",
  "find":"!safety_link_is_stale(st->age_ms, SAFETY_LINK_STALE_MS)",
  "replace":"!safety_link_is_stale(st->diag_frame_age_ms, SAFETY_LINK_DIAG_FRESH_MS)"},
 {"name":"E4_drop_at_lock_start_gate","file":"firmware/KilnFW/App/drivers/control/profile_executor_run.c",
  "find":"if (relay_authority_start_blocked(s_exec.safety, err_msg, err_cap, \"a firing cannot start\")) {",
  "replace":"if (false) {"},
 {"name":"E5_drop_resume_gate","file":"firmware/KilnFW/App/drivers/control/profile_executor_status.c",
  "find":"if (relay_authority_start_blocked(s_exec.safety, NULL, 0, \"resume\")) {",
  "replace":"if (false) {"},
 {"name":"E6_aux_tick_ignores_trip","file":"firmware/KilnFW/App/drivers/control/profile_executor.c",
  "find":"relays_on_count, on_off_cap, pre_lock_pico_tripped);",
  "replace":"relays_on_count, on_off_cap, false);"},
 {"name":"E7_seg_start_ignores_trip","file":"firmware/KilnFW/App/drivers/control/profile_executor.c",
  "find":"io_seg_start_pico(s_exec.segment_index, seg, pre_lock_pico_tripped);",
  "replace":"io_seg_start_pico(s_exec.segment_index, seg, false);"}
]
```
