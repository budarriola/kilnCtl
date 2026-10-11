# Review: firing-path fix batches firefx4, ldfx2, rebootfx (2026-10-10)

Reviewer: Opus, worktree at origin/dev 68e3b4966. No board access.

Scope (all already on origin/dev):

- firefx4: 020c23526, 32b33598c. These cover `profile_executor_relay_io.c`, including
  `exec_io_segment_relay_mask()`, the zone_off_pending retries and the `force_all_relays_off()`
  terminal ownership filter (MED-3).
- ldfx2: cb0874b68, 01c74a3cc, d48f77266, 2590ea462, c9777156f. These cover:
  - the resume gate and the commit-time start recheck;
  - the wifi status JSON cap;
  - typed fake_kv, with the favorites and used-bitmap follow-ups;
  - the literal NUL fix;
  - the saved_nets retry delay.
- rebootfx: 1fcf3148e, a3c6475c1. These cover:
  - MED-4, `pico_boot_id_ever_seen`;
  - MED-5, the reboot verdict that survives release and pause;
  - A1-A4;
  - `autotune_engine_abort_bounded()`.

Not re-reported here because fixers are already assigned:

- rvsaftyfx6 F3/F8, including the T3 lost trip not carried by the MED-5 claim-less verdict (saftyfx7).
- rvexecfx MED-A (execfx2).

These owner decisions served as the reference:

| Pico reboot verdict | Required behavior |
|---|---|
| Benign | Auto-resumes |
| Fatal | Pauses with `pico_fatal_reboot` |
| Undecided | Withholds heat |

## HIGH

None.

## MED

### MED-1: a real Pico reboot that spans a link-down is detected only by the 8-bit boot_id (rebootfx MED-4 interaction)

Files: `safety_link_frames.c:396`, `safety_link.c:300-320` (`safety_reset_stale_peer_info_if_link_down()`) and
`safety_link_frames.c` ~1108 (DIAG uptime regression).

**What MED-4 changed.** It compares the FW_VERSION boot_id against `pico_boot_id_ever_seen` instead of
`pico_boot_id_known`, so a ~1.5 s link blip with the same boot_id is no longer counted as a reboot.
Before this change, any relink after a link-down was treated as a reboot.

**Why the uptime backstop does not help.** Reboot detection had a second, independent signal: the DIAG
uptime regression. `safety_reset_stale_peer_info_if_link_down()` clears `pico_uptime_baseline_known` on
the same link-down that clears `pico_boot_id_known`. The first DIAG after relink therefore only
re-establishes a baseline and cannot flag a regression.

**What is left.** A real Pico reboot nearly always produces at least `SAFETY_LINK_UP_PERIODS` (3) silent
polls. Once the link drops, the 8-bit boot_id is the only reboot signal. The Pico draws it randomly at
boot (`link_task.c` ~3439, XOR-folded), so about one reboot in 256 keeps the same id. Such a reboot goes
completely unseen:

- `pico_reboot_seq` does not bump.
- `heat_enable_note_pico_boot()` never classifies it.
- No fatal hold is set, and T3 is never consulted.

F1 then re-requests K4 as soon as the new boot reports ARMED. A watchdog or brownout reboot during a
firing would therefore auto-resume heat in that case, which contradicts the owner decision that a fatal
reboot pauses. Before MED-4, this reboot was caught by the conservative relink rule.

**Fix options:**

- (a) Keep the uptime baseline across a link-down, so the first post-relink DIAG with a lower uptime
  flags the reboot. The 49.7-day wrap false positive is fail-safe because it pauses.
- (b) Treat a same-id relink whose first DIAG uptime is below the last pre-drop uptime plus the elapsed
  down time as a reboot.

**Test gap.** `test_link_blip_same_boot_id_is_not_a_reboot` pins only the benign side. No test covers a
real reboot whose boot_id collides across a link-down.

## LOW

### LOW-1: a stale pending OFF defeats `leave_on_at_end` (firefx4 MED-1 interaction)

**Sequence:**

1. `io_seg_finish()` now records a failed OFF in `zone_off_pending_mask` (MED-1).
2. A later RELAY_IO segment in the same run drives the same relay ON with `leave_on_at_end`.
   `io_seg_start()` claims PROFILE and writes, but does not clear that relay's pending bit.
3. On a clean DONE the relay is deliberately left ON and released to NONE.
4. The next non-RUNNING tick runs `zone_off_pending_retry()`. The F4 filter passes owner NONE, so the
   retry writes the relay OFF, and the leave-on intent is silently lost.

The failure direction is safe (relay off). Still, a pending bit should not outlive a later successful
write that supersedes it.

**Fix:** clear the relay's pending bit on a successful `io_seg_start()` write, and in the
`leave_on_at_end` branch of `io_seg_finish()`.

**Test gap:** no test covers this.

### LOW-2: a wrong-size used-bitmap blob is now a permanent save/delete outage with no repair path (01c74a3cc)

**What changed.** `used_bitmap_load()` now returns `HAL_IO` for a blob that is neither 16 bytes nor
1 byte. This is correct as fail-closed (no silent slot loss).

**The outage.** `nvs_load_all_from()` fails, which takes the degraded files-only path.
`nvs_save_slot()` and `nvs_delete_slot()` (`profiles_http.c` ~1356, ~1502) then fail on every attempt.
Nothing rewrites or quarantines the key, so recovery needs a factory reset.

**Downgrade case.** The same rule refuses a longer bitmap written by newer firmware with more slots.
That is inconsistent with the rev array's own policy in the same file ("a longer-than-ours array ... is
rewritten at its FULL original length").

**Suggested fix:**

- Accept `len > 16` by reading the first 16 bytes and preserving the tail on rewrite, as the rev array does.
- For any other size, either rebuild the bitmap from the slot keys present, or surface the condition
  in readiness/diagnostics instead of only failing saves.

### LOW-3: favorites still read a wrong-size blob as "nothing favorited" (01c74a3cc)

**What happens.** `favorites_load_user_mask()` now tries the legacy u32 before declaring the key absent
(correct). However, a blob of the wrong size takes the same u32 read. On target, and in the typed fake,
that read answers NOT_FOUND because of the type mismatch, so the function returns `HAL_OK`. The user
favorites silently reset to empty: the silent-loss class that the same commit fixed for the used bitmap.

The impact is cosmetic. There is no safety impact.

**Stale comment.** The header comment above the function still says a typed mismatch "fails with
HAL_INVALID_ARG". The new inline comment (and `used_bitmap_load()`'s) says it ends in NOT_FOUND. One
of them is wrong: update the header comment and the matching comment in `used_bitmap_load()`.

**Test gap.** Negtest confirms the legacy-u32 fallback is pinned. No test covers a wrong-size favorites blob.

## INFO

- **INFO-1: an executor-only pending bit can turn off an aux relay while paused.** In PAUSED,
  `zone_off_pending_retry()` writes with no owner filter. A pending bit on a relay that an aux rule
  holds ON would turn the aux OFF while `aux[i].commanded_on` stays true. `profiles_validate.c:92-108`
  refuses a RELAY_IO segment on an aux-enabled relay at save time, so this needs an aux enabled after
  the profile was saved. Not reproduced, recorded for completeness.
- **INFO-2: the MED-3 branch of `force_all_relays_off()` leaves stale state.** On a successful direct
  write it neither clears the matching `zone_off_pending_mask` bits nor calls
  `sim_backend_note_zone_relay()`. The first leaves a redundant OFF retry (harmless). The second can
  leave the sim plant believing a zone heats after the terminal write.
- **INFO-3: a fatal latch costs one resume.** The latch is consumed by the first acquire after it.
  `profile_executor_resume()` sets RUNNING before `heat_enable_acquire_since()`, so the first resume
  after a fatal reboot returns true with heat withheld. The watchdog then re-pauses with
  `pico_fatal_reboot`, and only a second resume heats. This is consistent with "operator resumes", but
  the first resume reports success for a run that cannot heat. Consider refusing the resume while
  `reboot_fatal_latched` or `reboot_verdict_pending` is set, with a decoded message.
- **INFO-4: `autotune_engine_abort_bounded()` logs `s_at` fields after giving the lock.** It logs
  `s_at.zone_index` and the abort reason after releasing the lock. This is a benign log-only race.
- **INFO-5: the commit-time recheck message names the wrong cause.** The ldfx2 recheck in
  `profile_executor_run()` passes `err` NULL and writes a fixed "safety link went down" message. When
  the gate closed because of a fault source other than the link, that message names the wrong cause.
  Pass `err_msg` through as the door check at line 413 does.
- **INFO-6: lock ordering checked.** Both new `relay_authority_start_blocked()` call sites hold
  `s_exec.lock` and take the relay_authority and safety-link locks inside it. This is the same order as
  the existing door check at `profile_executor_run.c:413`, so no new ordering is introduced.
- **INFO-7: the wifi status formatter is sound.** It now refuses (500) instead of sending truncated JSON
  as a 200. d48f77266 fixes the `' '` vs `'\0'` comparison that briefly made `ap_password_set` always
  true. The JSON cap host test pins the worst case.

## Tests run

`build_host_tests.ps1 -Only "profile_executor|heat_enable|safety_link|safety_cfg|wifi_prov|profiles|autotune_engine|relay_authority"`
built 25/25 executables and all passed. Two capture-dependent harnesses SKIPped by design because the
captures are gitignored.

## Negative tests

`tools/negtest.ps1 -Preset kilnfw-host -Parallel 4` with
`-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`.

| Fix | Mutation | Result |
|---|---|---|
| F4 (firefx4) | drop `owned |= exec_io_segment_relay_mask();` in `zone_off_pending_retry_running()` | CAUGHT |
| MED-1 (firefx4) | drop the `zone_off_pending_mask |= bit` on a failed OFF in `io_seg_finish()` | CAUGHT |
| LOW-4 (firefx4) | drop the pending bit on a failed write in `force_relay_mask_off()` | CAUGHT |
| LOW-2 (firefx4) | failed aux OFF in the disabled branch no longer `continue`s | CAUGHT |
| MED-3 (firefx4) | disable the terminal owner filter in `force_all_relays_off()` | CAUGHT |
| MED-4 (rebootfx) | `pico_boot_id_ever_seen` back to `pico_boot_id_known` | CAUGHT |
| MED-5 (rebootfx) | `reboot_verdict_pending = false` on a new reboot | CAUGHT |
| MED-5 (rebootfx) | fatal latch consumed without setting `reboot_hold` | CAUGHT |
| A3 (rebootfx) | send_enable ignores `reboot_classify_pending` | CAUGHT |
| MED-1 (ldfx2) | resume gate `relay_authority_start_blocked` disabled | CAUGHT |
| LOW-4 (ldfx2) | commit-time start recheck disabled | CAUGHT |
| 01c74a3cc | used-bitmap wrong-size blob no longer `HAL_IO` | CAUGHT |
| 01c74a3cc | favorites NOT_FOUND short-circuit restored before the legacy u32 read | CAUGHT |
| INFO-3 (ldfx2) | saved_nets retry delay removed | CAUGHT |

All 14 mutations were CAUGHT. The baseline passed and the real tree was unchanged.

The command was `build_host_tests.ps1 -OutDir {OUT} -Only "profile_executor|heat_enable|safety_link|safety_cfg|wifi_prov|profiles|autotune_engine|relay_authority"`, passed through `-Command`. The
unfiltered `kilnfw-host` preset could not be used: its baseline fails on origin/dev 68e3b4966 with two link errors
that predate this review. `test_dashboard_settings_http` and `test_setup_progress_http` hit LNK2019, unresolved external
`cfg_fs_degraded_is`, so neither executable builds. That dev break is outside this review's scope.
