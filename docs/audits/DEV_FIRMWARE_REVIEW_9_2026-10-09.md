# Dev firmware review 9 (2026-10-09)

Scope: every `firmware/KilnFW` commit on origin/dev after `523ba6dc`, up to the review base. Excluded:

- `68d34942..cececc45` and `b475e7d7`, which were reviewed elsewhere.
- `47fc83f1` and `5c046ea1`, which another reviewer has.

Reviewed in depth:

- Save-lock series: `0880162b`, `9df03347`, `252e3780`, `d55ab48b`, `fb236ec4`, `2014cde8`, `b952c302`.
- `f9b100c3` (relay_cycles init).
- `3047beb6`, `8f81eb27` and `5c638423` (zones POST ceiling restore and late heat re-check).
- `9651bfeb` and `15f4edd3` (adaptive tune F3 split).
- `2fcd20c1` (coupled-plan heap arrays, volatile autosave job pointer).
- `e51f9402` (login body on the heap, route count, cycle probe on the heap).
- `b51dfc59`, `4115bc19` and `fdec416a` (update_fetch wedge and owned abort).
- `bcc75d61` (Pico reboot detected from DIAG uptime).
- `c2b151b5` (kilnlink 17 CLEAR_TRIP bound to trip_seq).
- The factory reset mark lifecycle.

Skimmed only, with no finding:

- LCD commits: `ef99c327`, `60c02217`, `0968a699`, `74bf36ec`, `8ddb7a88`.
- Input-parsing batch: `e7c98209`, `953903c9`, `2086e1de`, `595bd701`, `697d1027`, `02ddc944`, `647e7d9d`, `d85e9e98`, `db395fa7`, `a517d292`, `0e04c0a9`.
- kiln_cfg swap: `7f10efe3`, `106dcc3d`.
- Persist results: `aa16ae24`, `4d79ff1a`.
- `6856e4dc` and `473c2ac7`.
- Test-only and doc-only commits.

Review base: origin/dev at `8fcd3237`.

High: none. Medium: none.

## Low

**L1 (9651bfeb, 15f4edd3). Autotune Accept can interleave with the run-end adaptive apply.**

Code:

- `firmware/KilnFW/App/drivers/control/profile_executor.c:786-795` calls `adaptive_tune_run_end()` after giving `s_exec.lock`, on the first tick in DONE. By then the run's heat claim is already released, so the mode gate no longer refuses zone writes.
- In `adaptive_tune.c`, `ki_clear_gen` (`:629`, bumped at `:1064`) guards only the Ki-baseline re-latch.
- `write_in_flight` (`:669`, `:732`, `:758`) blocks only an adaptive revert (`:1233-1276`). It does not block `autotune_engine_accept()`.
- The apply pass runs `zones_config_set_model()` then `zones_config_set_pid()` (`adaptive_tune_model.c:249-260`). It does this outside `adaptive_tune_lock`, then commits (`:265-330`).
- Accept re-checks the mode gate at `autotune_engine_guard.c:450`, then writes at `:466`.

Scenario: an autotune result is pending when a profile run reaches DONE, and the operator presses Accept during the run-end apply.

- If Accept's `set_pid` lands first, the SIMC gains from run-end overwrite the accepted gains, though Accept reported success.
- If it lands second, the commit still records `has_applied` with a revert snapshot of the gains from before Accept. A later adaptive revert then silently undoes the accepted gains.

The window is a few setter calls long, on one tick.

Fix: while any zone's `write_in_flight` is set, refuse Accept with busy/409. Or have the commit skip a zone whose gains changed since the plan was taken.

**L2 (5c638423, residual). Only POST /api/zones re-checks the heat claim inside the commit lock.**

Code: `zones_http_post.c:718-727` re-reads the heat claim under `zones_cfg_lock`, and that closes the race with a starter's `zones_config_changed_since()` check (`profile_executor_run.c:1412`, `autotune_engine.c:1335`). The other gated writers check the gate on entry only, then call a setter that bumps the generation later:

- `zones_http_pid.c` gates at `:110-115` and calls `zones_config_set_pid()` at `:174`.
- The UART SET_ZONE_PID and SET_ZONE_MODEL handlers.
- The adaptive revert.
- kiln_cfg apply.
- autotune Accept, after its re-check at `autotune_engine_guard.c:450`.

Scenario: a starter takes its generation snapshot, publishes its claim and passes `zones_config_changed_since()`. Only after that does the writer's setter bump the generation. The writer has then changed gains or model mid-run, against the owner's blanket mode gate (refuse zone writes while running, 409).

The comment at `autotune_engine_guard.c:436-446` already names this residual as deliberately unchanged. It is listed here so it stays tracked.

Fix: move the heat-claim re-check into `zones_config_set_pid_no_save()` and its siblings (`zones_config_accessors.c:561-616`), under the same `zones_cfg_lock` section that bumps the generation. Or record it as an accepted risk in the mode-gate plan.

## Earlier findings, status

- Review 8 L1-L4: fixed in `523ba6dc`, the base of this range.
- Review 9 L1: fixed in `4e3282fb` (tests `b813b027`). Accept refuses via the mode-gate 409 path while any adaptive `write_in_flight` is set; the adaptive apply skips a zone whose gains changed since the plan, and commit records no applied state or revert snapshot if the live gains are no longer ours.
- Review 9 L2: fixed in `4e3282fb`. `zones_config_set_pid/model[_no_save]_checked` re-check the heat claim inside the `zones_cfg_lock` section and return `ZONES_SET_BUSY_RUNNING`; zones_http_pid, UART SET_ZONE_PID/MODEL and Accept map it to 409/refusal. The executor and the adaptive run-end write run after the claim is released, so need no exception. The adaptive revert and backup import now get the refusal too (bool false). kiln_cfg apply has no direct setter call.

## Checked, no defect

- **Factory reset mark** (`factory_reset.c:480-525`). The mark is set at `:487` and cleared both when the mode gate refuses (`:489`) and when dispatch fails (`:506`). The reboot task is created for every scope (`:515`). If that creation fails, the call returns `FACTORY_RESET_ERR_REBOOT_FAILED`, and the caller reboots through `factory_reset_reboot_fallback`. The mark cannot stick.
- **Save-lock order.** Profile save (`profiles_http.c:1715-1772`) checks `convert_busy` before the lock. Under the lock it re-runs `validate_on_off_rules`, assigns RAM, then calls `nvs_save_slot_locked` (`:1223-1257`). The rev advances only on a successful file write. Delete (`:1860-1915`) reads `active_id` before the lock and runs `favorites_set` and `firing_stats_erase` outside it. Retarget runs whole under the lock. None of this breaks the nesting rule in `cfg_save_lock.h`, and executor state is read before the save lock. The flash worker lock audit (`2014cde8`) covers the same code.
- **Persist-then-RAM.** When a file write fails, the saved profile stays in RAM and is logged as applied live. The rev stays put. This convention predates the range.
- **Favorites and unit_pref** (`profiles_favorites.c:245-288`, `b952c302`). The save lock is taken. A failed commit sets `s_fav_dirty`, so the next call retries. No caller holds another save lock.
- **relay_cycles** (`f9b100c3`). `relay_cycles_init` loads into locals and publishes under `s_rc.lock`, before the flash worker starts.
- **Zones POST ceiling** (`3047beb6`, `8f81eb27`). `zones_post_track_ceiling_lower` (`zones_http_post.c:41-60`) is called from both the run-started refusal (`:65-74`) and the lost-update 409 (`:728-743`). That 409 takes its live-maxima snapshot under the lock. Both refusals undo the raise. One interaction was not established: a 409 lower running at the same time as a backup import's raise off `httpd_worker`. Starting points to check are the import's own generation and in-flight checks.
- **2fcd20c1.** The coupled-plan observation arrays (576 B) are allocated with plain malloc, freed on every path, and set a refusal reason on OOM. That is a short-lived draw, well inside the 8 KB internal floor. The autosave job pointer is `volatile`, so the worker cannot run a stale copy.
- **e51f9402.**
  - The login body is a 512 B heap buffer, wiped and freed, with the cap checked at `:423`.
  - The login lock fails closed.
  - `kiln_http_register` counts a route only once it has registered.
  - The settings_source cycle probe is now on the heap. Its unlocked read of `s_zones` predates the range.
- **update_fetch.** `b51dfc59` adds the `wedged_at_start` fix. `4115bc19` makes the abort owned and treats a missing give as a wedge. `fdec416a` was also checked.
- **bcc75d61.** `safety_note_pico_reboot_locked` clears `trip_last_seq`, `trip_event_ever_received` and `safety_relay_state_known`, and sets `reannounce_pending`. Both detection paths, a boot_id change and DIAG uptime going backwards, use it, so both sides of the dedup pair are reset.
- **c2b151b5.** CLEAR_TRIP is bound to the trip_seq from a 31-byte DIAG. A 3-byte clear built from a cached 30-byte DIAG received before ANNOUNCE is refused once. It recovers on the next DIAG. That robustness question belongs to the kilnlink audit.
- **Run-end call sites.** `pending_coupling` and the `adaptive_tune_run_end()` callers run outside `s_exec.lock`.
- **Starter pairing.** Each starter takes its generation snapshot, then publishes its claim, then calls `changed_since`. Against POST's re-check under the lock, either the POST sees the claim or the starter sees the bump.
- **Stack.** No commit in range adds a large stack local. The two arrays that used to be on the stack (coupled-plan observations and the login body) moved to the heap.
- **Tests.** Most fixes come with targeted host tests. L1 and L2 now have interleaving host tests (negtest: all CAUGHT).

## Fix review (`4e3282fb`, `b813b027`)

Read-only review of the L1/L2 fixes at origin/dev `8a8cb663`. Paths are relative to `firmware/KilnFW/App/drivers/`. Line numbers are approximate (`~`) where the function is long.

### Findings

- **FIXED in ffd50690. F1 (LOW) Accept can still race the run-end apply.** In `control/autotune_engine_guard.c` the `adaptive_tune_any_write_in_flight()` check (~:471) and Accept's own `zones_config_set_pid_checked()` (~:483) are separate steps. Nothing marks Accept as an in-flight writer that the adaptive plan honours. Also covers review 10: Accept's later unchecked set_model can no longer land after the run-end writes, because Accept now writes model and gains in the same atomic setter.
  - Scenario: Accept passes the check, then the executor task preempts it. `adaptive_tune_run_end()` plans (`control/adaptive_tune_model.c:50`) and captures the pre-Accept gains as the prior. The apply-time stale check (`:254-266`) still sees those same gains and passes. Accept's set_pid lands. Then the adaptive `set_model` (`:268`) and `set_pid` (`:270`) overwrite it. The commit (`:280-345`) compares live against what the adaptive apply wrote, finds a match, and records `has_applied` plus a revert snapshot of the pre-Accept gains. Accept's later `set_model` (`:569`) then overwrites the adaptive model. The zone ends up with adaptive gains and the Accept model, while Accept reports success.
  - The window is a few instructions wide, but it is the L1 outcome.
  - Fix: Accept sets an `external_write_in_flight` flag under `adaptive_tune_lock` before its check. The plan skips the zone while that flag is set, symmetric with `write_in_flight`.
- **FIXED in ffd50690. F2 (LOW) The revert snapshot outlives later writers.** `revert_available` is set only in `control/adaptive_tune.c:1195` (capture_revert). It is cleared only by a successful revert (`:1294`) and by the commit refusals (`adaptive_tune_model.c:294`). No external `zones_config_set_pid*` caller invalidates it, and neither does `adaptive_tune_clear_ki_baseline()` (`adaptive_tune.c:1069`).
  - Scenario: a refinement commits. Later the operator accepts an autotune result, or POSTs gains, writes over UART SET_ZONE_PID, restores an iter_tune record, or imports a backup. `revert_available` is still true. The operator then presses the adaptive "revert". `adaptive_tune.c:1277-1278` writes the pre-refinement snapshot over the newer gains with no warning. `has_applied` keeps describing gains that are no longer live.
  - This is the L1 outcome, still reachable outside the narrow in-flight window the fix closed.
  - Fix, either of:
    - Record the applied gains at commit, and make the revert refuse unless live gains still equal them.
    - Have the checked setters clear the adaptive snapshot for that zone.
- **FIXED in ffd50690. F3 (LOW) A BUSY refusal between the paired model/PID writes leaves a half-applied zone, with misleading results.** Each pair is two checked calls, and the heat claim can be published between them. The three pairs are:
  - The adaptive apply (`adaptive_tune_model.c:268/270`).
  - The adaptive revert (`adaptive_tune.c:1277-1278`).
  - Accept (set_pid ~:483, then `set_model` `autotune_engine_guard.c:569`). Accept's window is widened by the `clear_ki_baseline` NVS save in between.
  - Scenario: a profile starts between the two writes. The first write lands, the second returns BUSY, and the firing runs on new gains with the old model (or the reverse).
  - How each call site misreports this:
    - The revert returns HTTP 200 `ok:false` with a "rejected/write failed" reason, not the 409 refusal (`http/adaptive_tune_http.c:266-273`). `revert_available` stays true, but the snapshot now no longer matches the half-applied live state.
    - The adaptive commit's reason says `zones_config_set_pid() rejected`, which reads as a validation failure.
    - Accept's `set_model` failure is only logged, and Accept still returns true.
  - Fix, either of:
    - A combined `zones_config_set_model_and_pid_checked()` that checks the claim once and writes both under one `zones_cfg_lock` section.
    - Roll back the first write when the second returns BUSY.
  - Either way, map `ZONES_SET_BUSY_RUNNING` to the 409 refusal in the revert handler.
- **FIXED in ffd50690. F4 (LOW) The run-end apply is refused while an autotune runs on another zone.** The fix note says the adaptive run-end write runs after the claim is released, but it does not hold here. `relay_authority_heat_run_active()` is true while either claim is held, and `control/autotune_engine.c` ~:1253-1345 allows an autotune to start on zones disjoint from a running profile.
  - Scenario: a profile reaches DONE while an autotune is still running on another zone. The run end calls `adaptive_tune_run_end()` after `release_profile_relay_claim` (`control/profile_executor_relay_io.c:897`), but the autotune claim is still held. Every planned zone's `set_model` returns BUSY, and the commit records the reason `zones_config_set_model() rejected` (`adaptive_tune_model.c:298-300`). That firing's refinement data is discarded.
  - The same thing happens when a new profile is started quickly over DONE, before the run-end work finishes.
  - Fix: a distinct `busy` reason, so the loss is visible. Either defer the apply until both claims are clear, or correct the fix note and this doc's L2 status line.
- **FIXED in ffd50690. F5 (LOW) L2 is only partly closed: several mid-run-capable writers still bump the generation with no claim re-check.**
  - Accept's follow-on setters after `set_model`: `fit_context`, `baseline_k_dc`, `set_max_ramp`, `coupling_diag_k_dc`, `tuning_quality` (`autotune_engine_guard.c` after `:569`). These are plain unchecked setters and their results are ignored.
  - The adaptive coupling-cell write (`adaptive_tune_model.c:756`) and the bootstrap baseline write (`:257`).
  - kiln_cfg apply through `zones_config_import_blob()` (`persist/zones_config_accessors.c` ~:2506-2509). `http/kiln_cfg_http.c:387` checks only at entry.
  - Scenario: Accept's checked `set_model` succeeds, then a profile starter takes its generation snapshot, publishes its claim and passes `changed_since`. Accept's `set_max_ramp` then lands mid-run, changing the ramp cap of a running firing. This is the outcome L2 describes.
  - Fix: give these setters (and `import_blob`) the same in-lock claim re-check, or run Accept's whole write sequence under one checked section.
- **FIXED in ffd50690. F6 (INFO) Backup import BUSY is transient and safe, but the message misleads.** `http/backup_import.c:2437/2443`: when a starter publishes its claim and then withdraws it on seeing `restore_in_flight`, the import's setter can still see the claim and return false. Both sides then refuse. The snapshot restore runs, so nothing is half-written. The reported reason reads as a validation failure, not "a run was starting, retry".
- **F7 (INFO) The plan-time gain comparison is exact, and the reads are unlocked.**
  - Exact compare: the prior is a raw float copy of the live gains, compared with `==`. NaN gains are rejected by the setters, so a NaN prior cannot occur. `+0` and `-0` compare equal, which is harmless.
  - `zones_config_get_pid()` (`persist/zones_config_accessors.c` ~:548) reads the triple without `zones_cfg_lock`. A torn read is possible only during a real concurrent write, and that write then makes the compare correctly fail as stale.
  - `adaptive_tune_model.c:232` and `:235` read the prior twice. This is redundant, not wrong.

### Checked, no defect

- **Lock order.** `zones_cfg_lock` is taken before the leaf `s_heat_claim_mux` spinlock. The adaptive lock only covers the unlocked `get_pid` read. `s_exec.lock` is taken before `zones_cfg_lock`, never the reverse. The checked setters run outside `adaptive_tune_lock`. No inversion was found.
- **Claim re-check pairing.** The claim is read inside the same `zones_cfg_lock` section that bumps `s_config_generation`. A starter's `changed_since()` takes that lock after it publishes its claim, so either the setter sees the claim or the starter sees the bump.
- **Wrapper blast radius.** These callers were checked and either handle the bool/result or need no change:
  - `http/zones_http_pid.c:174` (BUSY maps to the 409 `system_mode_gate_http_send_refusal`).
  - UART SET_ZONE_PID/MODEL (`bridge/uart_bridge_ext_control.c`, BUSY maps to a "run active" error through `uart_bridge_ext_reply_ok_err`).
  - iter_tune restore (`http/iter_tune_http.c:221`, false maps to 500 and the record is untouched).
  - backup import and the adaptive revert (see F3, F6).
  - No LCD or profile-executor caller of these setters exists. `control/adaptive_tune_ki.c` only reads gains.
- **UART BUSY reply.** The reply frame carries the error string. Neither writer is called on a refusal.
