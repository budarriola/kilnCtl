# Review: safetyfx8 (SAFETYFX7 LOW-1 / LOW-5 / I-1 / I-2 fixes), 2026-10-12

Adversarial safety review of these dev commits:

| SHA | Subject |
|---|---|
| `f844f041b` | Autotune host test fixture for SAFETYFX7 MED-1 (stub reports a fresh ARMED DIAG). It precedes the fix and is included for context. |
| `7398cbeb3` | The fix. Stale DIAG refuses a new aux / relay-segment / segment-retry ON (LOW-1). Adds a behavioural `watchdog_task_entry` test (LOW-5). Fixes stale "fresh TRIPPED" comments (I-1). `trip_is_test` bound changes from `<` to `<=` (I-2). |
| `e3a155635` | Audit doc: REVIEW_SAFETYFX7 LOW-1, LOW-5, I-1 and I-2 marked fixed. |

The review was done in worktree `C:\wt\rvsafetyfx8_iutq0w` at `origin/dev` `1142b1899`, which contains `e3a155635`. Line numbers below are at that tree. Paths under `firmware/KilnFW/App/drivers/` are given without that prefix.

**Verdict: 0 HIGH, 0 MED, 3 LOW, 4 INFO.**

The fix is sound and fails closed.

- Every ON path that does not sit behind K4 now needs a fresh, since-reboot DIAG before it can newly switch on during a run.
- An unreadable link status counts as stale.
- The stale check is used only on ON paths, so a DIAG gap never drops an output that is already on and never aborts a run.
- The new behavioural watchdog test catches the SAFETYFX7 M05 mutant.
- No ungated relay ON outside K4 is reachable during a firing.

The three LOWs are:

- an untested branch (unreadable status), which survives mutation;
- an aux bookkeeping divergence while a new ON is refused;
- a pre-existing gap where a Pico trip that happens during a DIAG gap is invisible to an aux output that is already on.

## Host suite

The full `firmware\KilnFW\App\test\build_host_tests.ps1` run (no `-Only`) at `1142b1899` passed: `Built: 99/99 executables`, "all 99 host test executables built and passed", exit 0. SAFETYFX7 MED-1 (`test_autotune_engine_prestart` red) is gone at this tree.

## What was checked

### Q1. Every relay ON writer, and which ones need a fresh DIAG

**Gated by `pico_diag_stale_for_on()`** (`control/profile_executor_relay_io.c:683`):

- The aux rule tick, through `aux_apply_relay_pico` (`:713`). It gates only a NEW ON (`!commanded_on`).
- Relay-segment start, through `io_seg_start_since_pico` (`:1248`), including the warm-replay start. The relay is driven OFF, `state_on` is cleared, and `wants_on` is kept.
- Segment retry, through `io_segs_retry_on` (`:1402`).

**Not gated on DIAG freshness, but behind K4** (Pico-owned contactor coil interlock):

- zone heat `apply_relay` (`control/profile_executor_relay_io.c:135-185`), ON_OFF zones included;
- autotune `autotune_apply_relay` (`control/autotune_engine_guard.c:100-135`);
- the zone current sweep (`zones_current_sweep_engine.c:1118`, manual `kiln_io_owner` gate).

A stale DIAG cannot hide a Pico trip from these outputs, because a tripped Pico opens K4 in hardware.

**Manual ON writers:** `dashboard_set_relay`, the LCD, UART `SET_RELAY`/`SET_RELAY_MASK` and `/api/aux_outputs/manual`.

- They go through `kiln_io_owner`'s `relay_on_blocked`, which checks authority, OTA, an unacknowledged crash and the system-mode gate.
- They have no DIAG check.
- The system-mode gate refuses them while a firing, autotune or restore is active, so they are reachable only when idle. This is pre-existing (SAFETYFX7 LOW-1 "Manual writes" remains open; see INFO I-3).

**Non-relay IO segments** (`kiln_io_owner_command_set_io`) are not gated at all. This is pre-existing and not a relay.

**Conclusion:** no ungated relay ON outside K4 is reachable during a firing.

### Q2. Fail-closed

- **Unreadable status:** `safety_link_get_status() != ESP_OK` returns `true` (stale) at `:689-690`. This is correct, but no test covers it (LOW-1).
- **No link bound:** `!s_exec.safety` returns `false` (no gate) at `:685-686`. It cannot open a gate on a target build:
  - `main_control_bringup.c:363-365` passes NULL only when the safety link failed to start.
  - `relay_authority_on_blocked(NULL)` returns true with `SAFETY_FAULT_SRC_APP` (`owners/relay_authority.c:16-29`).
  - Every ON path calls `relay_authority_on_blocked()` before the DIAG check: aux at `:707`, segment start at `:1243`, retry at `:1402`.
  - So with no link bound, every ON is already refused. The "host/sim" comment is misleading (INFO I-1).
- **Watchdog:** if `safety_link_get_status()` fails, `safety_processor_tripped` stays false (`control/profile_executor.c:2333`, `:2360`). That is the fault path, not an ON path. A failed read cannot permit a new ON, because the ON gates read the status themselves and fail closed.

### Q3. Chatter or retry loops under a persistently stale DIAG

There is none.

- **Segment ON refused:** a refused segment ON writes OFF to a relay that is already off. `state_on=false` and `wants_on` stays set.
- **Retry:** `io_segs_retry_on` skips the segment every tick while the DIAG is stale. When the DIAG turns fresh, it switches the segment ON once. `state_on` then becomes true, the retry stops, and nothing drops the segment on a later stale DIAG. Physical ON events are therefore no more than requested ON events.
- **Aux refused:** the aux path writes OFF to a relay that is already off, every tick, which is idempotent.
- **Log volume:** the `ESP_LOGW` at `:714` and `:1250` repeats every tick for as long as the DIAG stays stale. Logs go to the UART bridge (`esp_log_set_vprintf`), not to flash, so this is noise, not wear (INFO I-2).

### Q4. Holding an aux that is already on, against the owner's aux fault-drop decisions

Holding is consistent with those decisions.

- A stale DIAG is not a fault.
- A cached TRIPPED, at any age, still drops every aux:
  - through the fault-drop path while idle or paused (`:853`);
  - through `pico_tripped` in the aux tick gate (`:1024`) and in `aux_apply_relay_pico` (`:707`);
  - through the watchdog FAULTED transition while running.

The behavioural test `test_safetyfx7_watchdog_any_age_tripped` pins the watchdog case with a 60000 ms old TRIPPED.

**Stale DIAG plus a real Pico trip:** this is covered only once a DIAG reporting TRIPPED arrives. A Pico that trips during the DIAG gap leaves an aux that is already on still ON (LOW-3).

### Other checks

- **Stack:** `pico_diag_stale_for_on()` adds one `safety_link_status_t` frame per call on the executor task.
  - It is called up to once per aux and once per segment each tick. The frames are sequential, not nested.
  - SAFETYFX7 LOW-4 still applies: the executor stack budget is unmeasured until the next target ELF.

## Findings

### LOW-1: the "unreadable status = stale" branch is untested (mutant M01 survives)

- **Where:** `control/profile_executor_relay_io.c:689-690`.
- **What:** mutant M01 changes `return true;` to `return false;` and passes all 99 executables. A regression would let an aux or segment newly switch ON while the link status cannot be read.
- **Likelihood:** low. `safety_link_get_status()` fails only on a NULL argument or a lock timeout.
- **Fix:** let the test stub return an error code with a link bound, then assert:
  - an aux ON is refused;
  - a segment ON is driven OFF with `wants_on` kept;
  - a retry is skipped.

### LOW-2: a stale-DIAG aux refusal is invisible to the aux trigger state and the status JSON

- **Where:** `control/profile_executor_relay_io.c:1016-1081` (`profile_executor_aux_tick_pico`).
- **What:** the aux tick feeds only `authority_blocked` (on_blocked or pico_tripped) into `profile_executor_aux_on_off_input()`. It does not feed in the stale-DIAG refusal, which happens later, inside `aux_apply_relay_pico`. While the DIAG is stale and the rule wants ON:
  - `actuated_on` is true and `held_s` accrues, while the relay is physically off (`commanded_on` false).
  - `rule_reason` reads `NONE`, not `AUTHORITY`. `/api/profile_exec` and the dashboard therefore show "actuated, no denial" for an output that is off.
  - `relays_on_count` counts the refused aux. This is conservative for the load cap and harmless.
  - When the DIAG turns fresh, the relay switches ON with `held_s` already past `min_on_s`. A trigger that drops soon after can switch it OFF before it was physically on for `min_on_s`. The cause is a missed `min_on` hold, not chatter.
- **Safety:** none. The output fails toward OFF. This is an accounting and display issue.
- **Fix:** compute `pico_diag_stale_for_on() && !s_exec.aux[i].commanded_on` once per aux in the tick and OR it into the trigger's blocked input for that aux, so that `actuated_on`, `held_s` and `rule_reason` follow the real state. Give `rule_reason` a distinct value such as `AUTHORITY` or a new `PICO_STALE`.

### LOW-3 (pre-existing): a Pico trip during a DIAG gap does not drop an aux that is already on

- **Where:**
  - `safety/safety_link_frames.c:803` masks `SAFETY_FLAG_FAULT` out of the STATUS flags.
  - `safety_apply_trip_event()` (`safety/safety_link_frames.c`) records the trip but does not update `diag_state`.
  - `relay_authority_pico_tripped()` (`owners/relay_authority.h:350`) reads only `diag_state`.
- **What:** if DIAG frames are lost while STATUS frames and TRIP_EVENT frames still arrive, a Pico trip reaches none of the drop paths. An aux that is already on stays on until a DIAG reporting TRIPPED arrives. The new ON gate cannot help, because it refuses only new ONs, and the owner chose to hold an already-on aux on a DIAG gap.
- **K4:** heater zones are not affected, because K4 opens in hardware.
- **Likelihood:** low. It needs selective DIAG loss and a trip at the same time.
- **Fix:** have `relay_authority_pico_tripped()` also treat a TRIP_EVENT newer than the last DIAG as tripped. Or set `diag_state = TRIPPED` (age 0) from `safety_apply_trip_event()` for a new event. Either way, the watchdog and the aux drop paths would act on it at once.

### INFO

- **I-1. Misleading comment.** `control/profile_executor_relay_io.c:682` says "No safety link bound (host/sim) means nothing to gate on." On a target build, a NULL link also happens when the safety link fails to start (`main_control_bringup.c:363-365`). That case is safe only because `relay_authority_on_blocked(NULL)` blocks every ON first. The comment should say so, so that a later refactor does not move the DIAG check ahead of the authority check.
- **I-2. Log flood.** The stale-DIAG `ESP_LOGW` at `:714` and `:1250` fires every executor tick for as long as the DIAG stays stale. The neighbouring write-failure log uses a log-once mask (`s_write_fail_logged_mask`). The same pattern would do here.
- **I-3. Manual writes still have no DIAG check.** Carried from SAFETYFX7 LOW-1. `kiln_io_owner`'s `relay_on_blocked` still has no Pico TRIPPED or DIAG-freshness check. It is reachable only when idle, because of the system-mode gate.
- **I-4. `trip_is_test` ignores `diag_ever_received`.** `http/dashboard_status_http.c:757-759` checks `diag_since_reboot` and the age, but not `diag_ever_received`, as `relay_authority_pico_diag_fresh()` does. This is harmless in practice, since a frame since reboot implies one was received. It is display only.

## Mutation table

Each mutant was run through `tools\negtest.ps1` with `-Preset kilnfw-host` (the full 99-executable suite, no `-Only`) and `-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`, with 4 workers. Every mutant was compiled from scratch in a throwaway worktree, and the unmodified baseline passed first.

For every CAUGHT row, the log shows `Built: 99/99 executables` (so the mutant compiled), at least one `  FAIL file:line:` assertion, and `RUN FAILURES (1)`. No CAUGHT row is a compile failure.

Line numbers in the last column are in `test_profile_executor_prestart.c` unless another file is named.

| # | File | Mutation | Verdict | Failing assertion (first) |
|---|---|---|---|---|
| M01 | `profile_executor_relay_io.c:690` | unreadable status counts as fresh (`return false`) | **MISSED** | none (LOW-1) |
| M02 | `relay_authority.h:359` | `pico_diag_fresh` `<=` becomes `<` | CAUGHT | `:13786` diag age == FRESH_MS allowed |
| M03 | `profile_executor_relay_io.c:713` | aux stale gate removed (`false &&`) | CAUGHT | `:13892` stale DIAG: aux ON writes OFF instead |
| M04 | `profile_executor_relay_io.c:1248` | segment stale gate removed (`false &&`) | CAUGHT | `:13914` stale DIAG: segment ON driven OFF, wants_on kept |
| M05 | `profile_executor_relay_io.c:1402` | retry stale gate removed | CAUGHT | `:13919` stale DIAG: retry does not switch ON |
| M06 | `profile_executor_relay_io.c:713` | aux drops an already-on output on a stale DIAG (`!commanded_on` removed) | CAUGHT | `:13902` aux already on is held, not dropped |
| M07 | `relay_authority.h:359` | `pico_diag_fresh` drops `diag_since_reboot &&` | CAUGHT | `:13799` DIAG not since Pico reboot refused |
| M08 | `profile_executor_relay_io.c:1253` | segment stale branch keeps `state_on` | CAUGHT | `:13914` segment ON driven OFF, wants_on kept |
| M09 | `dashboard_status_http.c:759` | `trip_is_test` `<=` becomes `<` | CAUGHT | `test_dashboard_status_http.c:352` age == bound gives true |
| M10 | `profile_executor.c:2360` | watchdog `tripped = safety_diag_valid && any_age` (the SAFETYFX7 M05 mutant) | CAUGHT | `:13960` old cached TRIPPED faults the run (behavioural) |
| M11 | `profile_executor.c:2360` | watchdog tripped forced false | CAUGHT | `:13960` (behavioural) |
| M12 | `profile_executor_relay_io.c:692` | stale gate disabled for every caller | CAUGHT | `:13892` stale DIAG: aux ON writes OFF instead |

About the results:

- **M10:** this is the mutant SAFETYFX7 LOW-5 could not catch. It is now caught by the behavioural `watchdog_task_entry` assertion ("MUST GO RED if the any-age wiring is gated on freshness"), not by a source-text pin.
- **M01:** expected to survive (LOW-1).
- **Clean run:** the baseline passed, and negtest reported `real_tree_unchanged: true` and `copies_removed: true` with `base_sha` `1142b1899`. The overall negtest exit was 1 only because M01 was MISSED.

## Fix status

Open. Nothing in this review has been fixed yet.
