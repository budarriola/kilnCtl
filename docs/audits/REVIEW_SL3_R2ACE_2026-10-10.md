# Review: sl3 safety-link fixes, r2ace host tests, toolfx4 (2026-10-10)

Opus review, no code fixes. Base reviewed: `origin/dev` at `6b3534e55`.

- **A. sl3:** `51df2ccc1`, `35d6fda6a`, `6b3534e55`, read together with ld01 (`804f67e85`) and firefx3 (`3c57e1d54`), both of which are ancestors.
- **B. r2ace test commits:** `51ab1ea36`, `68548bef8`, `a3950b140`, `2e515d27e`, `0131bb43a`, `1d5c5d683`.
- **C. toolfx4:** `ae7124b01`.

No HIGH findings. Two MED/LOW-MED findings are in A. Everything else is LOW.

## A. sl3 safety-link fixes

### Owner-decision check: an undecided Pico reboot withholds heat

Verdict: **heat is withheld.**

Path:

1. `heat_enable.c` `note_pico_boot()` sets `reboot_classify_pending` on a reboot_seq change.
2. `heat_enable_reconcile()`'s retry gate (`heat_enable.c:865`, `!s_he.reboot_hold && !s_he.reboot_classify_pending`) never re-requests heat while the flag is set.
3. The profile watchdog feeds `note_pico_state()` INIT for any DIAG from the old boot.
4. A benign classification clears the flag, and F1 re-requests heat.
5. A fatal classification sets `reboot_hold`, clears granted/pending and queues a release. The executor then pauses with `pico_fatal_reboot` and aborts autotune.
6. `profile_executor_status.c:597` surfaces `pause_reason = "pico_reboot_undecided"` while RUNNING, without changing state.

Caveats (each LOW):

- `pause_reason` has one consumer, the `/api/profile_exec` JSON (`dashboard_exec_http.c`). No web page, LCD page or PcTools tool displays it, so an operator watching a cold RUNNING firing sees nothing explain it.
- An autotune run is neither paused nor surfaced while the reboot is undecided. It runs cold until DIAG arrives.
- `granted` is not cleared by `note_pico_boot()`, so `heat_enable_is_granted()` can read true while undecided. Its only caller is the LCD auth-reset gesture (`ui_page_home_actions.c:604`), so there is no heat consequence.

### A1 (LOW-MED): post-commit DIAG freshness can be satisfied by a pre-commit DIAG

`safety_cfg_write.c:330`:

```c
bool fresh = nonblocking_refetch || (int32_t)(diag_count - diag_applied_before_commit) > 0;
```

**Problem.** The baseline is sampled *before* `send_commit_config()`. That send (`safety_link_commands.c` ~1476) drains the inbox before transmitting and also applies DIAGs that arrive during its ~345 ms reply window. A DIAG the Pico built and queued before it processed the COMMIT can therefore advance `diag_count` and count as "post-commit". The blocking persist verdict would then be decided on pre-commit state, which defeats the intent of "a blocking persist verdict needs a post-commit DIAG".

**Why it is not worse.** The window is one DIAG period (~2 s) racing a ~345 ms reply. The verdict concerns persist state, not heat.

**Fix options:**
- Sample `diag_applied` after `send_commit_config()` returns; or
- require the count to advance by 2 or more; or
- key on a DIAG field that echoes the commit.

The `test_safety_cfg_http.c` stub does not model the drain inside `send_commit`, so the test cannot see this. Its expected `diag_calls` changed from 1 to 2 in this batch.

### A2 (MED/LOW): `autotune_engine_abort()` from the watchdog waits on `s_at.lock` with `portMAX_DELAY`, every tick the hold persists

`profile_executor.c:2516/2519` calls `autotune_engine_abort("pico_fatal_reboot")` or `autotune_engine_abort("heat_grant_unconfirmed")`. It does so on every guard-9 watchdog tick while `reboot_hold` or `k4_unconfirmed` stands.

The profile pause beside it is bounded (`pause_with_reason_bounded`, 1000 ms). `autotune_engine_abort()` (`autotune_engine_guard.c:274`) is not: it does `xSemaphoreTake(s_at.lock, portMAX_DELAY)`.

The autotune start path holds `s_at.lock` across a long section (`autotune_engine.c` ~1197-1587). A watchdog tick landing there blocks guard 9's own task until the start finishes.

Mitigations seen:
- the acquire calls run outside `s_at.lock` (~1594/1705);
- `abort_locked()` reaches `force_relays_off()`, which releases the AUTOTUNE claim (`autotune_engine_guard.c:172`);
- lock order (`s_exec.lock` before `s_at.lock`) is respected, because the call is made with no exec lock held.

So this is a liveness stall, not a deadlock. **Fix:** a bounded variant of `autotune_engine_abort()` for the watchdog, or a `state_is_running()` precheck before the take.

### A3 (LOW): `send_enable()`'s in-flight check covers `reboot_hold` but not `reboot_classify_pending`

`heat_enable.c:307`: firefx3 added "enable landed under a reboot hold, so record nothing and queue a release". If `note_pico_boot()` sets `reboot_classify_pending` (not the hold) while an enable is on the wire, the `else if (err == ESP_OK)` branch records `granted = true`. The Pico then holds an enable granted to a board that has not yet classified the reboot.

A fatal classification still clears it afterwards, so the exposure is one DIAG period. The window is narrow. `note_pico_boot()` runs on the watchdog task, and `reconcile()`'s retry from that task is already gated. The race needs a `heat_enable_acquire_since()` send from another task in flight at that moment: the profile start path (`profile_executor_run.c:1543`) or the autotune task (`autotune_engine.c:1594/1705`). `heat_enable_acquire_since()` itself checks only the release epoch, not either reboot flag, before it sends. **Fix:** extend the line 307 condition to `s_he.reboot_hold || s_he.reboot_classify_pending`.

### A4 (LOW): slow re-announce comment says "one frame"; it is a 4-frame burst

`safety_link_frames.c:334-336` says "at most one frame per SLOW_GAP_MS". The slow branch sets `reannounce_pending`, and `safety_link_poll.c:573` services that with `safety_link_send_announce_version_burst()`: 4 frames, 250 ms apart, about 750 ms of poll-task block.

**Starvation check.** About 750 ms of block every ~10-12 s, only while the Pico sends 30-byte DIAGs (protocol unbound). The same poll pass defers `heat_enable_service_pending_release()` and `ct_leak_alarm_service()` by up to the burst length. The link-up threshold (3 x 500 ms) keeps ~1250 ms of margin against a 750 ms stall, and the Pico's S6b 10 s timeout is unaffected. No trip risk. The cost is a delayed heat release by up to ~750 ms in a state (Pico at protocol 0) that should itself be rare. Fix the comment. Optionally send a single frame in the slow branch.

**Reset-one-side check.** `diag_reannounce_count` and `diag_reannounce_last_ms` are reset together on both a Pico boot_id change and link-down. The fast/slow branch split keys on the count alone. Not an instance of the class.

### A5 (LOW, cosmetic): `kiln_cfg_swap.c` `%.40s`

`kiln_cfg_swap.c:541-545` is reached only when `strcmp(sub, ZONES_IMPORT_REASON_RUN_CLAIMED) == 0`. That string is 57 chars ("a profile or autotune run is active -- retry when it ends"). `%.40s` cuts it to "a profile or autotune run is active -- r".

The surrounding prefix already states the same thing, so **no operator information is lost**, but the visible text ends mid-word. Drop the `%s` (the branch already knows the reason) or reword. `test_kiln_cfg_swap.c:481` re-`#define`s the reason string instead of including it (mirror-drift risk).

### Other items checked, no finding

- **E-stop clear after an ACKed commit** (`safety_cfg_write.c:589`): fail-closed. `estop_verification_clear()` returns ESP_OK only on a confirmed read-back, and a failure fails the POST. "ACKed" means "not rejected within the reply window", which is the right side to err on.
- **Autotune abort on fatal hold and on unconfirmed grant:** present and ordered before reconcile. The only issue is the A2 wait.
- **Lock order:** `profile_executor_status.c:597` calls `heat_enable_reboot_undecided()` (he_lock, a leaf) under `s_exec.lock`. This is allowed.
- **ld01 interaction:** `relay_authority_start_blocked` gates start only; it does not touch the reboot/hold path. No conflict.
- **firefx3 interaction:** the in-flight hold branch and the `relay_unknown_release_locked` reorder are intact. A3 is the one gap left beside the branch firefx3 added.
- **K5/K9/K11 tests:** K9 (`test_heat_enable.c` ~1239/1290) is a source-text `strstr` test. It pins the presence of the pause/abort calls, not their behaviour. K11 (`test_profile_executor_prestart.c`) exercises the real status path. `test_safety_link_compile.c` `test_diag_reannounce_is_bounded` expects 6-8 slow announces over 84 s.

## B. r2ace host-test commits

### B1 (LOW): `$totalExpected = 95` is right; its history comment is wrong

There are 95 `Invoke-HostTestExe` calls, no duplicate names, and a mismatch is enforced (`build_host_tests.ps1:3561`). The comment at lines 3526-3527 credits both "93 -> 94" and "94 -> 95" to `test_kiln_io_owner_sx_dispatch.c`. Those steps were `test_dashboard_http_relay.c` and `test_cfg_fs_mount_state.c`. `test_kiln_io_owner_sx_dispatch.c` is a source of an existing executable.

### B2 (LOW): `test_cfg_fs_mount_state.c` pins a truncated operator message as expected

The gate reason "LittleFS superblock signature found but its commit failed CRC/version validation (corrupt filesystem)" is 101 chars. `s_format_pending_reason[96]` (`cfg_fs_mount.c:78`, copied at ~433) cuts it to 95 chars, ending "(corrupt fil". The text is shown to operators via `GET /api/cfgfs/format_pending`. The test asserts the truncated form, which encodes the defect as expected behaviour. Fix: grow the buffer (or shorten the text), then assert the full string.

### B3 (LOW): `test_saftyfw_image_identity_record.c` overclaims

The header says "a damaged record reads as absent, never as a wrong id". The record has no CRC. Only magic/version/len/trailer (offsets 0, 4, 8, 11, 56) are guarded, so a flipped commit or config byte reads back as a different, wrong id. Also, "dirty normalised to 1" is not exercised, because the stub already supplies 1. Fix: narrow the header claim, and feed a dirty value of 2 or more.

### B4 (LOW): `test_link_task_fuzz.c` COMMIT_CONFIG/APPLY_CONFIG_VOLATILE scenario 4 is looser than the code

The volatile scenario asserts "exactly one of immediate reapply or armed retry". `link_task.c` (~2903-2905) always reapplies immediately for the volatile path. A regression to deferring would pass. See the negtest M6 below. Fix: assert the immediate reapply for the volatile path.

### Fine

- `test_kiln_io_owner_sx_dispatch.c`: solid. The AUTHORIZED mask bypassing the safety gate is documented by design (`kiln_io_owner.c:465`).
- `test_dashboard_http_relay.c`: reporting the trip mask verbatim on link-down matches what its callers expect.

## C. toolfx4 `ae7124b01` (quick check)

**OK.** negtest's bare-name check resolution mirrors `run_all_checks.ps1`'s glob and exclusions. It resolves against the caller's tree rather than the throwaway copy, which is fine because the name only selects the file.

In the shared main tree, a name that is also archived under `logs/wt_archive_*/untracked_files/` resolves as "ambiguous". That fails loud, so it is safe. `run_all_checks.ps1` has the same behaviour.

The lint mojibake fixture is fine.

## Negative tests (`tools\negtest.ps1`)

All runs happened in throwaway copies at `6b3534e55`, and each unmutated baseline passed.

- KilnFW runs used `build_host_tests.ps1 -Only "safety_cfg_http|test_heat_enable|profile_executor_prestart|test_safety_link_compile"` with `-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`.
- SaftyFW runs used the `saftyfw-host` preset.
- Both runs reported "real tree changed". The only change was this review file, written in the worktree while they ran (`?? docs/audits/REVIEW_SL3_R2ACE_2026-10-10.md`). No source was changed.

| # | Mutation | Result | Caught by |
|---|----------|--------|-----------|
| M1 | `safety_cfg_write.c:330` freshness becomes `fresh = true` | CAUGHT | `test_safety_cfg_http.c:1591` "pre-commit clean DIAG alone does not confirm a persistent commit" |
| M2 | `heat_enable.c:865` drops `!reboot_classify_pending` from the retry gate | CAUGHT | `test_heat_enable.c:1199` "while the cause is undecided the pending request is not re-sent" |
| M3 | `profile_executor.c:2516` `autotune_engine_abort("pico_fatal_reboot")` becomes `(void)0` | CAUGHT | `test_heat_enable.c:1294` (K9, source-text `strstr` only) |
| M4 | `profile_executor_status.c:597` `heat_enable_reboot_undecided()` becomes `false` | CAUGHT | `test_profile_executor_prestart.c:3624` (K11) |
| M5 | `SAFETY_DIAG_REANNOUNCE_SLOW_GAP_MS` 10000u becomes 1000u | CAUGHT | `test_safety_link_compile.c:3024/3038` |
| M6 | SaftyFW `link_task.c` volatile apply defers the tc_type reapply (sets `s_tc_type_reapply_pending`) instead of reapplying now | **MISSED** (expected) | `test_link_task_fuzz` ran, with 32295 checks and 0 failures. This confirms B4. |

M1 being caught shows the test pins "a pre-commit DIAG alone is not enough". It does not cover A1, because the stub's `send_commit` does not drain or apply DIAGs inside the send. M3 is caught only by a source-text check. No behavioural test proves that the watchdog aborts a running autotune.
