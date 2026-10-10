# Firing control path audit, 2026-10-10

## Scope and method

This is a fresh end-to-end review of the KilnFW firing control path as of origin/dev `d93cf774a`.

- **Read:**
  - the profile executor: `profile_executor.c`, `_run.c`, `_status.c` and `_relay_io.c`
  - `heat_enable.c`
  - relay authority and ownership: `relay_authority.*` and `kiln_io_owner.c`
  - aux outputs during a run
  - pause and resume, including the owner decision on Pico reboots: a benign reboot auto-resumes heat; a fatal one pauses with `pico_fatal_reboot`; an undecided one withholds heat until a DIAG arrives
  - `system_mode_gate.c`
  - the readiness start gate: `readiness_gate.c`, `estop_verified` and `relay_authority_start_blocked`
- **Recent commits weighed:** 3c57e1d54 (pending zone OFF retry while RUNNING), 51df2ccc1 (sl3 safety link batch) and 804f67e85 (LD-01/02).
- **Method:** read-only. No firmware was edited, built or run, and the board was not touched.

All paths below are relative to `firmware/KilnFW/App/drivers/`. Line numbers are at `d93cf774a`. Between that commit and this doc's base, origin/dev changed no file under `control/`, `safety/` or `owners/` except one-line edits to `adaptive_tune.c` and `ramp_assist_cfg.c`.

**Overall:** the review found no path that energizes heat during a trip, a fatal-reboot hold or an idle state. Every finding below either fails safe for heat or is a lost OFF on a non-heat relay.

## Findings

### MED-1. A failed OFF for an IO-segment relay is dropped for good

- **Where:** `control/profile_executor_relay_io.c:1156-1175`, in `io_seg_finish()`.
- **What happens:**
  - When `kiln_io_owner_command_set_relay_mask_authorized(bit, 0)` fails, the function only logs. The log claims "sweep_unowned_relays() will keep retrying".
  - It then clears `bit` from `s_exec.claimed_relay_mask` (line 1174) and calls `relay_authority_release_mask(bit)`. Its caller marks the segment inactive.
- **Why nothing ever retries it:**
  - `sweep_unowned_relays()` computes `stray = shadow & claimed_relay_mask & ~owned` (`relay_io.c:1573`). With the claimed bit gone, the sweep can never see this relay. The sweep also runs only while RUNNING (`profile_executor.c:2000`).
  - `io_segs_force_all_off()` skips inactive segments.
  - `force_all_relays_off()` addresses only zone masks.
- **Scenario:**
  1. A profile has a relay segment on spare relay 3 (a vent fan, a valve).
  2. The segment ends, or the operator aborts, while the SX1509 or I2C owner queue is stalled, so the OFF write fails.
  3. The claim and ownership are dropped, and no code path ever commands relay 3 off again.
  4. It stays energized after the run until a manual write or a reboot.
- **Fix:** on failure, keep the bit claimed, or put it in a pending-OFF mask that is also retried outside RUNNING (as `zone_off_pending_mask` is). Drop the claim only on success or on the leave-on path. The "firefx4: IO-segment owned relays" work in flight is in this exact area.

### MED-2. A start refused from DONE corrupts the finished run and persists a bogus firing-stats record

- **Where:** `control/profile_executor_run.c`. The function refuses RUNNING and PAUSED (426) and FAULTED (431), but not DONE.
- **What it overwrites first:** under the lock, it overwrites the previous run's state before several later refusals:
  - `s_exec.profile = p` (674)
  - `claimed_relay_mask = 0` (703)
  - `memset(s_exec.zones)` (739)
  - `fs_persisted = false` (748)
- **Refusals that come after those writes:**
  - no heating zone (853)
  - aux and on/off caps (903, 915)
  - on/off zone with no rule (964)
  - the late zone-claim, heat-claim, OTA, restore, danger, factory-reset and zones-changed refusals (about 1309-1436). By then the per-zone loop has already set `zones[].active = true` and line 1209 has set `total_elapsed_s = 0`.
- **Scenario:**
  1. A firing finishes and sits in DONE. Its stats are persisted and `fs_persisted` is true.
  2. The operator taps Start on a profile that is refused, for example because a backup restore or OTA started meanwhile, or because the profile has an on/off zone with no rule.
  3. The state stays DONE, but `fs_persisted` is now false and `s_exec.profile` names the refused profile.
  4. The next non-RUNNING tick (`profile_executor.c:796-812`) runs `firing_stats_maybe_finalize()` again. A record is persisted for a firing that never ran: the refused profile's id, a zero or stale duration, and zeroed zone accumulators.
  5. `adaptive_tune_run_end(&fs_rec, at_clean_run = true)` is called with that record, so a fabricated clean run can become adaptive-tune training data.
  6. Status shows the refused profile as the one that finished.
- **Side effect:** a late refusal also leaves `zones[].active` set for zones the refused run never claimed, which feeds MED-3.
- **Fix:** either refuse DONE the same way as FAULTED (the caller halts or dismisses first), or stage the new run in a local copy and write `s_exec` only after the last refusal. The second keeps the "start again from DONE" UX.

### MED-3. In DONE/FAULTED the executor still writes OFF to relays it no longer owns, defeating autotune and manual control

- **Where:** `control/profile_executor.c:754-755` (non-RUNNING tick) and `control/profile_executor_relay_io.c:601-611` (`force_all_relays_off`) and 505-510 (`force_zone_relay_off`, which calls `apply_relay(zi, false)`).
- **What happens:**
  - At DONE and FAULTED, `release_profile_relay_claim()` drops relay ownership, the zone claim and the heat claim. `exec_enter_terminal_state()` deliberately leaves `zones[].active` true.
  - Every 1 s tick afterwards, `force_all_relays_off()` writes OFF through the AUTHORIZED path to every still-active zone's relays.
  - `apply_relay()` also re-adds those masks to `claimed_relay_mask` (relay_io.c:79).
  - This continues until the operator halts or dismisses the run.
- **Contrast:** `zone_off_pending_retry()` (relay_io.c:520-530) explicitly skips relays held by another owner once the run has ended ("Review F4"). The bulk force-off has no such filter.
- **Nothing stops another owner taking these relays:**
  - Autotune refuses a zone only when `profile_executor_zone_is_active()` is true (`autotune_engine.c:1188`), and that is gated to RUNNING/PAUSED (`profile_executor_status.c:640`).
  - The manual-relay mode gate keys on `relay_authority_heat_run_active()` (`owners/kiln_io_owner.c:225-230`), which is false once the heat claim is released.
- **Scenario:**
  1. A firing finishes (DONE) and the operator does not press Stop or dismiss it.
  2. The operator starts an autotune on zone 1. It is allowed.
  3. Autotune closes zone 1's relay, and within 1 s the executor tick writes it OFF. This repeats every tick.
  4. Autotune sees a plant that barely heats. It either times out or fits garbage gains, and the relay chatters at the tick rate.
  5. A manual relay ON from the web page or LCD is reverted the same way.
- **Severity:** fails safe for heat, so not higher than MED. Combined with MED-2 it can also hit zones the refused run never owned.
- **Fix:** in DONE/FAULTED, skip a zone's relays when they are owned by someone other than NONE/PROFILE (the F4 filter), or stop forcing once the terminal-state OFF has landed (it is already tracked through `zone_off_pending_mask`).

### MED-4. A safety-link blip of about 1.5 s is classified as a Pico reboot, and can pause a firing with `pico_fatal_reboot`

- **How a blip becomes a "reboot":**
  - `safety/safety_link.c:310-319` clears `pico_boot_id_known` whenever `!safety_link_up_locked()`, which is about 3 missed polls.
  - When the link recovers, FW_VERSION is re-read and `safety/safety_link_frames.c:391` evaluates `boot_id_changed = (!link->pico_boot_id_known) || ...`, which is true even with the same boot id.
  - `safety_note_pico_reboot_locked()` (412) then bumps `pico_reboot_seq` and clears `diag_since_reboot`.
  - With a claim held, `control/heat_enable.c:710-718` sets `reboot_classify_pending`.
- **How it can turn fatal:** the next DIAG is classified on `diag_boot_reason` (726-731). The Pico caches that for its whole boot (`SaftyFW/src/boot_reason.c`), so it describes the real boot from hours earlier.
- **Scenario:**
  1. The Pico's current boot was a watchdog or brownout boot. This is typical right after a real fatal reboot that the operator then resumed past.
  2. Mid-firing, the ESP misses 3 status polls (httpd or flash load, a UART glitch).
  3. When the link comes back, the run is paused with `pico_fatal_reboot` (`profile_executor.c:2511-2516`), and any autotune is aborted.
  4. Every later blip repeats this.
  5. When the cached reason is benign, status instead shows `pico_reboot_undecided` ("heat withheld") for one DIAG period, while the Pico in fact still has K4 closed.
- **Severity:** fails safe, but spurious. No test covers a same-boot-id relink. Only `test_heat_enable.c` mentions `pico_reboot_seq`.
- **Fix:** do not count "unknown because the link dropped" as a change. Keep the last known boot id across the outage and compare against it, relying on the uptime-regression signal for a repeat-id reboot.

### MED-5. Pause/resume during an undecided reboot drops the pending verdict, so a late fatal DIAG never holds heat

- **The stated rule:** `control/heat_enable.c:71-73` and 746-747 say there is no timeout fallback and a late fatal DIAG must still hold.
- **How the verdict is lost:**
  - The last-claimant release clears `reboot_classify_pending` and `reboot_hold` (`he_release_common()`, 528-536).
  - `seen_reboot_seq` already holds the new seq (711), so nothing re-arms classification.
  - `profile_executor_resume()` (`control/profile_executor_status.c:350`) then reaches `heat_enable_acquire_since()` (418), and `send_enable()` checks neither `reboot_classify_pending` nor `diag_since_reboot`.
- **Scenario:**
  1. The Pico reboots mid-firing. The ESP has not yet received a DIAG from the new boot, and status reads `pico_reboot_undecided`.
  2. The operator pauses, and the pending classification is cleared.
  3. The operator resumes, and REQUEST_ENABLE(true) is sent.
  4. The new boot's first DIAG then reports WATCHDOG or BROWNOUT. `heat_enable_note_pico_boot()` sees an unchanged seq and does nothing.
  5. The firing keeps heating after a fatal Pico reboot, with no `pico_fatal_reboot` pause, contrary to the owner decision.
- **Related race:** a reboot that happens while already PAUSED is classified only if the resume lands before the next watchdog tick. When `held_mask == 0` the seq is consumed with no verdict (720-722).
- **Relation to the LD review:** this is a separate gap from the LD review's MED-1 ("resume has no start gate", `docs/audits/` review in 17945863f), which is about link and fault state. A resume gate built only on `relay_authority_start_blocked()` would not close it.
- **Fix:** keep the verdict per seq independent of claims, and apply it when the DIAG arrives. Or refuse/flag resume until `diag_since_reboot` is true for the current seq.

### LOW-1. 3c57e1d54's running retry can force OFF a relay the new run's IO segment is driving ON

- **Where:** `control/profile_executor_run.c:704-705` now deliberately keeps `zone_off_pending_mask` across a run start.
- **What the retry excludes:** `zone_off_pending_retry_running()` (`profile_executor_relay_io.c:553-593`) skips active zones' relays, autotune zones and `aux_claim_mask`.
- **The gap:** an IO-segment relay is owned `RELAY_OWNER_PROFILE`, so the owner filter keeps it in the mask.
- **Scenario:**
  1. Run A's OFF for relay 5 fails, so its bit stays pending.
  2. Run B starts with a non-blocking segment that turns relay 5 ON.
  3. Within 1 s the retry writes relay 5 OFF and clears the bit, while the segment still thinks relay 5 is ON.
- **Severity:** OFF direction only. **Fix:** also exclude relays of active IO segments.

### LOW-2. An aux disabled mid-run is forgotten if its OFF write fails

- **Where:** `control/profile_executor_relay_io.c:945-951`. The return value of `aux_apply_relay(i, false)` is ignored, and `profile_executor_aux_reset_runtime(i)` then clears `commanded_on`/`actuated_on` unconditionally.
- **Why nothing retries it:** aux relays are not in `claimed_relay_mask`, so the sweep does not see them, and `profile_executor_aux_fault_drop()` runs only when not RUNNING.
- **How it gets disabled mid-run:** `aux_outputs_cfg_set()`'s mode gate is check-then-act (`http/aux_outputs_http.c:30` and the backup-import callers) and holds no claim across the save, so a disable can land on a run that started in that window.
- **Effect:** the aux stays energized for the rest of the run. It is opened at the terminal state by `force_aux_relays_off()`.

### LOW-3. A queued AUTHORIZED ON can execute after the watchdog's direct all-off

- **Where:** `owners/kiln_io_owner.c`, `post_and_wait()`. A command is queued with `xQueueSend(..., 0)` and the caller gives up after 200 ms, but the command still runs later. `CMD_SET_RELAY_MASK_AUTHORIZED` has no gate inside the owner task.
- **Scenario:**
  1. The I2C owner queue is slow, so an `apply_relay()` ON times out but stays queued.
  2. A Pico trip or relay-unknown condition makes the watchdog call `kiln_io_all_relays_off()` directly (`profile_executor.c:2151`, 2195, 2397), bypassing the queue.
  3. The stale ON then lands and re-closes a zone relay. The next non-RUNNING tick reverts it about 1 s later.
- **Backstop:** the Pico's own trip opens K4, so the hazard is relay wear and a confusing log, not heat. **Fix:** stamp AUTHORIZED commands with a generation that the direct all-off bumps.

### LOW-4. A superseded-mask force-off that fails near run end is never retried

- **Where:** `control/profile_executor_relay_io.c:1473-1489` (`force_relay_mask_off`). It keeps the claimed bit so the sweep can retry, but the sweep runs only while RUNNING.
- **Gap:** if the run ends first, `halt()` and `force_all_relays_off()` name only the current masks, so the old relay is not retried until the next run.

### INFO-1. Locks held across producer calls (no new deadlock)

- `s_exec.lock` is held across several `kiln_io_owner` writes per tick, each bounded at 200 ms. This is documented.
- `zone_off_pending_retry_running()` (new in 3c57e1d54) takes `s_at.lock` through `autotune_engine_is_active_on_zone()` under `s_exec.lock`. That copies the established order in `sweep_unowned_relays()` (`relay_io.c:1559-1583`).
- `relay_authority`'s `s_relay_owner[]` has no lock. Its writers hold `s_exec.lock` or the autotune lock, and the owner task reads single bytes. Benign today.

### INFO-2. Double count of one Pico reboot

If the new boot's first DIAG arrives before its FW_VERSION, one reboot is counted twice: once by uptime regression (`safety_link_frames.c:1101-1105`) and once by boot id (412). The second count gives one extra undecided DIAG period with the same verdict. Harmless.

## Checked and correct

**Gates**
- **`system_mode_gate.c`:** fails closed on a NULL snapshot. START_PROFILE refuses for recovery mode, danger mode and restore in flight. Danger mode itself refuses while a run is RUNNING or PAUSED (`safety/danger_mode.c:55`). Manual relay ON is refused while a heat run is active, and danger mode does not bypass the mode gate (`owners/kiln_io_owner.c:262-277`).
- **Readiness start gate:** `readiness_gate_collect`, then `system_mode_gate_check`, then `readiness_gate_evaluate` run before the lock. `estop_verified` comes from `estop_verification_is_verified()`. A link down maps to CANNOT_YET and is left to `relay_authority_start_blocked()` (`profile_executor_run.c:397`). That requires no fault source and a positively up link (LD-01, 804f67e85), and it is checked before any state is mutated.

**Run start and stop**
- **Late refusals:** the second-look refusals in `profile_executor_run()` (zone claim, heat claim, OTA, restore, danger, factory reset, zones changed) release both the zone claim and the heat claim before returning.
- **Commit order:** aux take, then `relay_authority_claim_mask(PROFILE)`, then RUNNING, then unlock, then `heat_enable_acquire_since(epoch)`. A release in between bumps the epoch, so a stale acquire is refused (`heat_enable.c:437-468`).
- **`halt()`** (`profile_executor_status.c:34`): forces zone, aux and IO relays OFF, releases the claimed mask, the heat and zone claims and heat enable, finalizes stats, then enters IDLE.
- **Watchdog FAULT:** direct all-off, then the terminal state, then release. Guard 9 cuts relays before taking the lock.

**Relay writes**
- **`apply_relay()`:** an unreadable mask falls back to an OFF write over this run's claimed relays, excluding other zones' and aux relays, and marks a failure pending. ON is gated by `relay_authority_zone_blocked()` every tick.
- **`force_aux_relays_off()`:** keeps `aux_claim_mask` and sets `aux_off_pending` on failure. `aux_outputs_cfg_set()` commits RAM only after a successful save.
- **3c57e1d54:** the running retry excludes bits owned by others, clears only the bits it wrote, and fails closed on an unreadable mask. LOW-1 is the one gap.

**Heat enable**
- **Releases are not lost:** `release_pending` clears only on a successful send. `release_inflight` serializes the flush, and an enable that lands under a hold queues a release (`heat_enable.c:307-315`).
- **Re-requests:** reconcile does not re-request while held or undecided (864-865). On a real reboot, nothing re-requests heat until the verdict arrives, except the MED-5 path.
- **Benign reboot:** re-requests once the Pico is ARMED, which matches the owner decision.

**Reset-one-side class**
- On a reboot, the link side clears `trip_last_seq`, `diag_trip_seq` and the reannounce budget together.
- The K4 episode counters reset on a seq change.
- `claimed_relay_mask` is zeroed at run start, while `aux_claim_mask`, `aux_off_pending` and `zone_off_pending_mask` are deliberately kept.
- The only unpaired reset found is the verdict-vs-seq pair in MED-5.

## Recent commits

| Commit | Change | Findings in this doc |
|--------|--------|----------------------|
| 3c57e1d54 | Running retry | Correct apart from LOW-1 and the lock order noted in INFO-1 |
| 51df2ccc1 | sl3 safety link batch | Its reboot and reset handling is the base for MED-4 and MED-5, and its paired resets are correct |
| 804f67e85 | LD-01/02 | Start gate is correct. Resume is ungated (LD review MED-1, being fixed separately), plus MED-5 here |
