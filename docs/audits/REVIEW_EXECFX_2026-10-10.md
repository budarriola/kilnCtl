# Review: execfx (FIRING_PATH_AUDIT MED-2, LOW-3) -- 2026-10-10

Scope: `ec5581cee` (MED-2: a late start refusal from DONE restores the DONE
snapshot) and `cc0eeed74` (LOW-3: relay-off epoch drops a stale queued
AUTHORIZED ON), as found on origin/dev at `29c52d6be`. Adversarial review. No
code fixes and no board access. Line numbers refer to `29c52d6be`.

Summary: 0 HIGH, 1 MED, 4 LOW, INFO notes. The lock discipline of the MED-2
restore is sound. The main gap is a warm-start relay replay that happens before
the late refusals, and the restore then hides it. Test coverage is thin: 2 of
the 11 restore sites, and the LOW-3 post-time stamp is never exercised (see the
negtest table).

## MED

### MED-A: a warm-start relay/IO replay before a late refusal leaves a relay ON and owned by PROFILE, and the restore deletes the record of it

**Status: FIXED 2026-10-10: the warm-start replay now runs after the last refusal check (after LD-01); a refused start never energizes or claims the relay.**

- Replay: `profile_executor_run.c:1219` calls `io_seg_start()` for each skipped
  RELAY_IO segment. In `profile_executor_relay_io.c:1106-1110`, `io_seg_start()`
  does three things:
  - sets `s_exec.claimed_relay_mask |= bit`;
  - calls `relay_authority_claim_mask(bit, RELAY_OWNER_PROFILE)`;
  - posts an AUTHORIZED write, which is ON when `state_on`. Non-relay IO
    targets get `kiln_io_owner_command_set_io(.., state_on)` instead.
- Seven late refusals come after it and call `run_refuse_unlock(done_snap)`:
  - zone claim, `:1338`
  - sweep, `:1371`
  - OTA, `:1388`
  - backup restore, `:1404`
  - danger mode, `:1421`
  - factory reset, `:1438`
  - zones generation changed, `:1457`
- None of these paths turns the replayed outputs off or releases the
  relay_authority claim.
- `run_refuse_unlock()` (`:212`) then memcpys the DONE snapshot over `s_exec`.
  That erases `io_segs[]` and the new `claimed_relay_mask` bits. The hardware
  write and the external `relay_authority` ownership (PROFILE) are not part of
  `s_exec`, so they survive.
- Failure scenario:
  1. A firing ends DONE while the kiln is still hot.
  2. The operator restarts a profile with an "IO on" segment that is past
     its schedule position, so the warm start replays it.
  3. The start is then refused, for example because an OTA claim or
     backup restore is in progress, or because the zones generation
     changed mid-start.
  4. Result: the relay (vent fan, damper, light, or spare relay) stays
     energised, and relay_authority still reports PROFILE as owner, so manual
     control of that relay is blocked.
- No path recovers this:
  - `profile_executor_halt()` (`profile_executor_status.c:43+`) would
    normally run `io_segs_force_all_off(false)` and
    `relay_authority_release_mask(s_exec.claimed_relay_mask)` from DONE.
    After the restore, both work from the old snapshot and no longer
    include the bit.
  - `sweep_unowned_relays()` only runs while RUNNING.
- What changed with this fix: before MED-2, the refused start left the mutated
  `s_exec` in place, so a later halt from DONE cleaned the relay up. The restore
  turned "cleanup possible" into "orphaned".
- Pre-existing part: starting from IDLE has the same orphan, because halt
  returns early at IDLE.
- Fix, either of:
  - (a) Move the warm-start replay after the last refusal (after `:1457`),
    which is the cleanest option.
  - (b) Before restoring, undo the replay: call `io_seg_finish(i, false)`
    for each `warm_start_replayed_segments[]` entry and release the bits
    added since the snapshot (`claimed_relay_mask & ~done_snap->claimed_relay_mask`).
- Test: a warm-start refusal at one of the seven sites must leave no ON write
  and no PROFILE claim behind.
- Note: the in-flight firefx4 work (IO-segment owned relays) touches this area
  and should take this case into account.

## LOW

### LOW-B: lazily allocated history buffer leaks on a refused start

**Status: FIXED 2026-10-10: a refused start keeps the history buffer and the live `last_tick_tick`.**

- `history_buf_ensure_alloc()` (`profile_executor_run.c:50-56`) allocates
  `s_exec.history` from PSRAM once and never frees it.
- If the DONE snapshot has `history == NULL` and this start allocates the
  buffer before a late refusal, the memcpy restore writes NULL back and the
  buffer is lost. The next start allocates a new one.
- This repeats on every such refusal. It is PSRAM only and bounded by the
  number of refusals, but it is still a leak.
- Fix: keep the live `s_exec.history` pointer across the restore, the same way
  `last_tick_tick` is kept, or allocate only after the commit point.

### LOW-C: the snapshot falls back to internal RAM without checking the 8 KB floor

**Status: FIXED 2026-10-10: the DONE snapshot is PSRAM only; if PSRAM fails the start is refused.**

- At `profile_executor_run.c:692-693`, if PSRAM fails, the code takes
  `sizeof(s_exec)` (0xcf8 = 3320 B per the KilnCtrl.map of the bench build)
  from `MALLOC_CAP_8BIT` internal heap with no check against the owner's
  8192 B internal floor.
- This draw happens while the HTTP or LCD start handler is running, and while
  a TLS or KDF burst may be in flight.
- In practice PSRAM almost never fails, but this code path is the one that
  would take the floor below its limit unseen.
- Fix: allocate from PSRAM only and refuse ("out of memory", already handled)
  if that fails, or check
  `heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - sizeof(s_exec) >= 8192`
  before falling back.

### LOW-D: the stale check runs outside the kiln_io lock (TOCTOU)

**Status: FIXED 2026-10-10: `kiln_io_set_relay_mask_if_epoch()` compares the epoch under the kiln_io lock; the owner handler uses it.**

- `kiln_io_owner.c:472` compares `cmd.off_epoch` with the current epoch, then
  `:480` calls `kiln_io_set_relay_mask()`, which takes the kiln_io lock.
- Race: the guard-9 or relay-unknown watchdog calls `kiln_io_all_relays_off()`
  directly from its own task. It bumps the epoch (`kiln_io.c:580`) and does its
  locked OFF write in the window between `:472` and the owner task acquiring
  the lock.
- In that case the stale ON is written immediately after the fail-safe OFF.
  This is exactly the case LOW-3 set out to close.
- The window is a few instructions wide plus any lock wait, so it is narrow
  but real on a dual-core part.
- The next control tick re-commands from fresh state anyway, which bounds the
  impact.
- Fix: do the epoch comparison while holding the kiln_io lock, for example by
  passing the expected epoch into a `kiln_io_set_relay_mask_if_epoch()` that
  checks under the lock. Keep the bump before the lock as it is now, so that
  an unlocked-fallback all-off still invalidates.

### LOW-E: the epoch is stamped at post time, after the caller's gate decision

**Status: FIXED 2026-10-10: callers sample the epoch before their gate decision and call `kiln_io_owner_command_set_relay_mask_authorized_since()`.**

- `kiln_io_owner.c:771` samples the epoch inside
  `kiln_io_owner_command_set_relay_mask_authorized()`.
- Callers decide to turn ON earlier: `apply_relay()` checks
  `relay_authority_zone_blocked()`, aux checks its rules, `io_seg_start()`
  works from the warm-start state.
- If an all-off runs between that decision and the post, the ON is stamped
  with the new epoch and goes through, even though the decision predates the
  fail-safe.
- Fix: use the `heat_enable_acquire_since()` pattern. The caller samples
  `kiln_io_relay_off_epoch()` before its gate check and passes it in. The
  owner refuses if the epoch has changed since.

## INFO

- **Epoch increment and visibility.** `s_relay_off_epoch++` on a `volatile
  uint32_t` is not atomic across the cores and tasks that can call
  `kiln_io_all_relays_off()`. Two concurrent bumps can lose one increment, but
  the value still changes, and "changed" is the only property the stale check
  needs.
  - Wrap would need 2^32 all-offs between a post and its dispatch, so it is
    not a concern.
  - An aligned 32-bit volatile access is single-copy atomic on Xtensa LX7.
  - Taking the lock after the bump orders the store before the OFF write for
    any reader that later takes the lock. LOW-D is the reader that does not.
- **Callers that receive ESP_ERR_INVALID_STATE.** The owner already returns
  this code for "not ready", so the meaning is ambiguous. No caller retries or
  escalates on it:
  - `apply_relay()` logs it, but still sets `relay_commanded_on = want_on`
    on error (pre-existing). It sets `zone_off_pending` only for OFF.
  - The aux path marks `write_ok = false` and logs once.
  - `io_seg_start()` logs "state is unknown".
  - `autotune_engine_guard.c:120` logs only.
  - None of them takes the code as permission to retry within the same tick,
    and the next tick re-commands with a fresh epoch, as designed.
- **Non-AUTHORIZED paths.** `CMD_SET_RELAY_MASK` (manual) and `CMD_SET_IO` do
  not carry an epoch. Safety blocks are enforced by the owner's execute-time
  gate instead, so these are out of LOW-3's scope. IO outputs (non-relay
  segment targets) are not cut by all-off at all, which is pre-existing.
- **Aux and spare relays.** These go through the AUTHORIZED path, so they are
  covered by the epoch. The aux start handoff (`profile_executor_run.c:1516-1530`)
  writes OFF after the last refusal, which is fine.
- **Reinit and resync** paths do not bump the epoch. That is acceptable,
  because they do not drive relays toward OFF as a fail-safe.
- **Unlocked fallback.** An unlocked all-off while the lock holder is part-way
  through an ON write is the pre-existing K7 class and is unchanged here.
- **MED-2 lock analysis: OK.**
  - The snapshot (`:691-700`) and every restore are under `s_exec.lock`.
  - `s_exec.state` stays DONE until the commit at `:1562`, so neither the
    control task's lock-free peek (`profile_executor.c:731`) nor a status
    reader can see a half-started state.
  - `last_tick_tick`, which is written lock-free by the control task, is kept.
  - Restored fields such as `fs_persisted` and the last-run card come from a
    snapshot taken under the same lock hold, so no legitimate concurrent write
    can be overwritten.
  - Stats ack and the firing_stats cache live outside `s_exec`, and nothing
    before a refusal touches them.
  - Refusals before `:691` do not modify `s_exec`.
  - The snapshot is freed on success at `:1570`.

## Negative tests

Command: `tools\negtest.ps1 -Command "build_host_tests.ps1 -Only
test_profile_executor_prestart|test_kiln_io_owner -OutDir {OUT}"
-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`.
The baseline passed (166.8 s), the real tree was unchanged, and all copies
were removed.

| Mutation | Result |
|---|---|
| skip restore, no-heating-zone refusal (`:861`) | CAUGHT |
| skip restore, cap aux (`:919`) | MISSED |
| skip restore, cap on/off (`:934`) | MISSED |
| skip restore, no on/off rule (`:983`) | MISSED |
| skip restore, zone claim (`:1338`) | MISSED |
| skip restore, sweep (`:1371`) | MISSED |
| skip restore, OTA (`:1388`) | MISSED |
| skip restore, backup restore (`:1404`) | MISSED |
| skip restore, danger mode (`:1421`) | MISSED |
| skip restore, factory reset (`:1438`) | CAUGHT |
| skip restore, zones generation (`:1457`) | MISSED |
| drop `last_tick_tick` preservation (`:217`) | MISSED |
| remove the memcpy restore entirely | CAUGHT |
| owner post stamps `off_epoch = 0` (`kiln_io_owner.c:771`) | MISSED |
| stale OFF-only command also reports INVALID_STATE (`:483`) | MISSED |
| bump only after a successful locked all-off (not on fallback/error) | MISSED |
| no epoch bump at all (`kiln_io.c:580`) | CAUGHT |

Test gaps this table shows:

- `test_run_refused_from_done_leaves_done_state_untouched` exercises only 2 of
  the 11 restore sites. Fix: parametrise it over every refusal hook the
  prestart harness can trigger (OTA, restore, danger, zones generation, cap,
  sweep, zone claim).
- `test_authorized_on_queued_before_all_off_is_dropped` hand-sets
  `c.off_epoch` and calls `dispatch()` directly, so the post-time stamp is not
  tested. Fix: post through
  `kiln_io_owner_command_set_relay_mask_authorized()`.
- No test covers the unlocked-fallback bump, a stale OFF-only command
  returning OK, or keeping `last_tick_tick` live across a restore.

**Status: ALL FIXED 2026-10-10.** Tests added: a table-driven restore test over all 11
refusal sites (the backup-restore hook opens only after the heat claim so it
exercises the late site) plus a separate test for the cap on/off, cap aux and
no on/off rule refusals; a `last_tick_tick` and history-buffer preservation test;
a post-through-the-real-function epoch stamp test; a stale OFF-only command test
(returns OK, never INVALID_STATE); an epoch-bump-on-failed-all-off test; and a
caller-samples-epoch-before-gate test. Re-run of the same mutations plus the new
LOW-D and LOW-E ones through `tools/negtest.ps1`: 20 of 20 CAUGHT (including every
mutation the original review recorded as MISSED), real tree unchanged.
