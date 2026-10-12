# Review: 8cf1bc60a "suitefx" (origin/dev), 2026-10-11

Commit: `8cf1bc60a` "Fix dev check failures: log phantom cfg file delete failure in
profile save rollback; land test fixture carries scratch-repo marker for
-ChecksScript seam". Reviewed in worktree `C:\wt\rvsfx_s8nojc` at `8cf1bc60a`.

Two changes:

1. `firmware/KilnFW/App/drivers/http/profiles_http.c` (`profiles_http_save_ex()`,
   fwlow16 LOW-1 / misc8 rollback path): the result of
   `profiles_cfg_fs_delete(target_id)` is now captured and logged with `ESP_LOGE`
   unless it is `ESP_OK` or `ESP_ERR_NOT_FOUND`. The save's return value and
   `*out_persisted` are unchanged.
2. `tools/check_main_baseline.ps1`, case 6 (land.ps1 `-AllowKnownFailures`): writes
   the `kilnctl_scratch_repo` marker into the scratch clone `$lc`'s `.git`, so the
   later "land fetches before the gate" sub-case can pass `-ChecksScript`.

## Verification

| Run | Result |
|---|---|
| `tools/check_cfgfs_never_gates_nvs.ps1` | PASS (8 call sites scanned, every result consumed) |
| `tools/check_main_baseline.ps1` | PASS, 81 assertions, including "land with a known failure lands (exit 0)" and "post-rebase run re-runs the known-failing check under -FailOnlyOnNew" |
| `build_host_tests.ps1 -Only profiles` | all 5 executables built and passed (test_profiles_http 621/621, test_profiles_builtin 276/276, ...) |
| `negtest.ps1 -Preset check -PresetArg tools\check_cfgfs_never_gates_nvs.ps1`, mutation `esp_err_t derr = profiles_cfg_fs_delete(target_id);` -> `(void)profiles_cfg_fs_delete(target_id);` | CAUGHT (exit 1, `profiles_http.c:2037: result of profiles_cfg_fs_delete() is discarded`); baseline passed; real tree unchanged |

## Findings

### LOW-1: a failed rollback delete leaves a file that the next boot adopts

Is log-only right? Mostly yes, but the log text understates what happens. A stale
cfg file does not just "may reappear". The rollback deletes only a file it has
just confirmed carries rev `s_profile_rev[target_id] + 1`. That rev is higher than
any persisted rev for the slot. On the next boot `profiles_cfg_fs.c`'s reconcile
(`!nvs_valid` branch, `file_rev > nvs_rev || nvs_rev == 0`) adopts the file as
"looks like a failed NVS write". NVS is now a legacy, read-only source, and the
file is authoritative. So the file does win over NVS, and the slot comes back
after a reboot with the content the caller was told failed (live SAVE_AS treats
`persisted=false` as a failure).

Impact is limited:
- Reaching this needs two failures in a row: a cfg write that fails after its
  rename (read-back verify), then a failed unlink of that same file. Both
  point at a sick `cfg` partition, where an immediate in-line retry would likely
  fail too.
- The resurrected content passed full validation (`validate_on_off_rules()`
  under the save lock, name-collision check). It is a user profile, not a
  safety-relevant state.
- It heals itself in the common case. The RAM slot is cleared, so the next
  fresh-slot save usually allocates the same `target_id` and atomically
  overwrites the file at the same rev.

Leaving the return value alone is correct: the save already reports
`persisted=false`, and failing harder would change no outcome. If this ever needs
closing, the narrow option is to mark the slot so the boot path drops a file at
exactly that rev (for example, persist a rev bump for the slot as a delete does,
so `file_rev > nvs_rev` no longer holds). That costs an extra flash write on an
already failing partition. Not recommended now. Optional: reword the log to "slot
WILL reappear at next boot (file adopted as newer)".

### INFO-1: the `ESP_ERR_NOT_FOUND` test is dead code

`profiles_cfg_fs_delete()` (`profiles_cfg_fs.c:173`) already maps
`ESP_ERR_NOT_FOUND` to `ESP_OK`, so `derr != ESP_ERR_NOT_FOUND` can never be what
skips the log. It is harmless and defensive against a future change in the
wrapper. The wrapper also logs its own `ESP_LOGW` on failure, so a failure logs
twice. The new `ESP_LOGE` adds the slot-rollback context, which is fine.

### INFO-2: no host test, but one is easy with the existing fakes

All the parts already exist in `firmware/KilnFW/App/test/test_profiles_http.c`:
- `write_then_fail_fn` (file lands, then the write reports `ESP_FAIL`). It
  drives the misc8 LOW-1 test `test_save_ex_rollback_unlinks_written_file()`.
- `pcfg_failing_delete_fn` (delete returns `ESP_FAIL`) through
  `profiles_cfg_fs_set_delete_fn()` / `profiles_cfg_fs_reset_delete_fn_for_test()`.
- `stubs/esp_log.h` captures `ESP_LOGE` text: `esp_log_test_capture_reset()` /
  `esp_log_test_capture_contains("phantom cfg file delete failed")`.

A test would install both fakes, call `profiles_http_save_ex(PROFILES_MAX_COUNT,
...)`, and assert the following. Save returns `ok && !persisted`. The RAM slot is
cleared. The log capture contains the new message. The file is still valid,
which records the LOW-1 residual. A negtest that removes the `ESP_LOGE` would
catch it. Today the change is covered only by the static check, which proves the
result is read, not that the log fires.

### INFO-3: check_main_baseline change does not weaken any assertion

`land.ps1:195` refuses `-ChecksScript`/`-PinCheckScript` unless the marker
`kilnctl_scratch_repo` sits in the target repo's git common dir. Without the
marker, the sub-case's land call ended with exit 1 at that guard. That broke
"land with a known failure lands" and never exercised the post-rebase re-run the
case exists to prove. The marker goes only into the temp clone `$tmp\lclone`, the
same way `check_land.ps1:34` and `check_dev_promote.ps1:47` set up their
fixtures. No assertion text, condition or `ExpectedAssertions` count changed. The
other case-6 `Run-Land` calls pass no seam flag, so the marker does not affect
them. The guard's own refusal in an unmarked repo is still tested by
`check_land.ps1:328` ("refused in an unmarked repo"). The marker is written to
`.git` directly, which is the common dir for a plain clone. That is correct here.

## Verdict

Both changes are correct. No HIGH or MED findings. LOW-1 is a documented residual
in an already best-effort path. The main follow-up worth doing is the cheap
host test in INFO-2.

## Fix status

- LOW-1: fixed. The rollback log now says the stale cfg file WILL reappear at next boot.
- INFO-2: done. `test_save_ex_rollback_delete_failure_logs_reappear` in `test_profiles_http.c`.
