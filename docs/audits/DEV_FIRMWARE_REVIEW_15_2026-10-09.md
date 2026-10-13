# Dev firmware review 15 (2026-10-09)

Review only, no fixes. Scope, all on origin/dev:

| Commit | Subject |
|---|---|
| ffd50690 + f4acabf0 | Review 9 F1-F6 fixes, plus status lines |
| 013ff420 | HAL allowlist entry and citation refreshes |
| fa4a62ef + db1f4df5 | Review 11 fixes (LOW-1 claimed partly fixed), plus SHA update |
| 473f597e | danger_mode stack back to 3072 B |
| 2a1d69a8 | Target-build fixes |
| 5dcd6dbd + 34d2c40c | HTTP fuzz part 2 tests, findings doc F4-F9 |

Each commit gets four answers: does it fix what it claims, does it regress
anything, are its tests non-vacuous, and do other call sites share the defect.
Line numbers are origin/dev at review time and may drift by a few lines.

Summary: no HIGH or MED findings. 5 LOW, the rest INFO.

---

## ffd50690 (review 9 F1-F6) and f4acabf0

**Fixes what it claims.** Yes. F1 is the atomic model+PID setter
`zones_config_set_model_and_pid_checked`. F2 is the revert_expect_* snapshot,
which matches the values written. F3/F4/F5 are claim re-checks under
zones_cfg_lock (`zones_config_run_claimed_locked`) that return
`ZONES_SET_STALE_PRIOR` or a claim refusal. F6 rewords the PID message. Lock
order is consistent with cfg_save_lock.h, and a relay-method Accept stays
gains-only. f4acabf0 changes only status lines and is correct.

**Regressions.**

- **LOW: F5 claim refusals reach callers that read any import failure as invalid data.**
  - `kiln_cfg_store.c:942` (boot import). A claim refusal is handled like a
    validation failure: active_id is cleared and persisted ("failed validation
    at boot"). Scenario: the HTTP and LCD servers are already up before
    `kiln_cfg_store_init` runs at `main_network_http.c:801`. An operator starts
    a firing in that window, the import is refused on the claim, and the
    active kiln config is cleared for good, although it was valid.
  - `kiln_cfg_swap.c:532` (rollback when esp_was_committed). A claim refusal
    during rollback is reported as "ROLLBACK FAILED / diverged" and raises an
    alarm. Before F5 this rollback always landed. A transient claim now turns
    a recoverable rollback into a reported divergence.
- **LOW: an Accept max_ramp claim refusal is reported as "failed to persist".**
  `autotune_engine_guard.c:721` maps a claim refusal to
  `AUTOTUNE_CEILING_FAILED_TO_PERSIST`. The log and `autotune_engine.h:402` say
  the value is "live in RAM, reverts on reboot". That is false here, because
  nothing was written to RAM either. The follow-on setters are still
  non-atomic (about lines 598, 627, 784, 820). Scenario: a profile claim lands
  between the atomic model+PID write, which sets tuning_valid=0, and the
  tuning_quality write. The zone is then left with no quality record, and
  fit_context or baseline can also be missing.
- **INFO: a step-method Accept now fails entirely when the model is rejected.**
  Before this change the gains landed with a warning. The impact is small,
  because the model is already bounded at identify
  (`autotune_engine_step_identify.c:217`, `:508`).

**Tests.** `test_zones_fix_review9_atomic_pair_and_claim_gated_writers` in
test_zones_http.c is non-vacuous for the atomic pair and the cell write.

- **INFO: test gaps.** There is no mid-run case for
  `set_max_ramp_no_save` or `set_tuning_quality_no_save`. The only idle positive
  control is for the cell write; baseline, coupling_diag, fit_context and
  import_blob have none, so a writer that always refuses would pass. The stub
  in `test_autotune_engine_prestart.c` ignores the set_model result.

**Shared defect elsewhere.**

- **INFO:** `backup_import.c:2558, 2680, 2753, 2768, 2793` still give
  validation-style messages when an F5 setter refuses on a claim. F6 reworded
  only the PID message at `:2500`. The operator is told the backup is invalid
  when a firing merely started mid-import.
- **INFO:** the status getter at `adaptive_tune.c:1183` reports
  `revert_available=true` while the snapshot is stale. The F2 check is lazy and
  runs only when revert is pressed, so the UI offers a revert that will then
  refuse.
- **INFO: stale comments and dead code.**
  - The `!plan->pid_ok` branch comment in the adaptive_tune_model.c commit
    path (about :290-360) no longer describes the code.
  - The comment above `zones_config_run_claimed_locked`
    (`zones_config_accessors.c:590-596`) does not hold for F4, an autotune
    running on another zone.
  - `model_persisted=true` at `autotune_engine_guard.c:577` makes a later
    branch dead.
  - The reason `deferred_autotune_active` is also used for a profile claim,
    and the observations are discarded, not deferred.
  - adaptive_tune_model.c (about :228-250) reads the prior gains twice.

## 013ff420 (HAL allowlist and citation refreshes)

**Fixes what it claims.** Yes. The cited blob 379b1a56 matches origin/dev's
wifi_provision_http.c. The HAL allowlist entry is narrow: one file and one
header. The esp_netif use at `wifi_provision_http.c:887-889` is real, commit
2d1f637d exists, and no wifi_prov accessor for the AP IP exists to use
instead. The ELF-hash relabels are correct.

**Regressions, tests, shared defect.** None found. No tests are needed for a
citation change.

## fa4a62ef (review 11) and db1f4df5

db1f4df5 only changes the SHA 730ceeff to fa4a62ef, and is correct.

**MED-1 (rejected or newer zones.json overwritten from NVS): fixed for the cases named.**
`resolve_with_file_buf` in zones_config_cfg_fs.c (about :373) neither adopts
nor overwrites a newer file when NVS is valid, and returns false. A corrupt
file with no .bad copy makes NVS the adopted copy, and the file is not
overwritten. The legacy-partition migrate in zones_config_store.c (about :438)
is gated. nvs_load zeroes s_zones.cfg and sets out_valid=false. The tests
`test_rejected_file_with_older_copy_present` and
`test_legacy_partition_does_not_overwrite_newer_file` are non-vacuous.

- **LOW: the same defect class remains for read errors.** `load_raw_impl`
  treats any `cfg_fs_read` error as "absent" and records no reject. That
  includes `ESP_ERR_INVALID_SIZE` for a file larger than `ZCFG_FILE_BUF_MAX`
  (4 + 1024 B, `zones_config_cfg_fs.c:48`) as well as transient I/O errors. The
  `!file_valid && nvs_valid` branch then overwrites zones.json from NVS.
  Scenario: a newer firmware grows zones.json past 1028 B, or stops writing
  NVS once the owner closes the cfg dual-write. After a rollback to this
  firmware, NVS holds an older valid blob, and the newer, larger file is
  overwritten with it. pref_cfg_fs got an over-size newer-file probe in review
  2. zones has not.

**MED-2 (load-fault latch set for a trustworthy adopt): fixed.** The latch is
set only when `!trustworthy`, and a warning is logged when a trustworthy copy
is adopted. The banner wording is per reason. main_page.html changed from
`kcEscapeHtml(reason)` to the raw value. That is safe, because `detail` is
escaped as a whole, and before this the reason was escaped twice.

**MED-3 (blank or omitted guard fields reset to the default): fixed.** Parse
keeps the stored value on blank or missing, using live `s_zones.cfg.zones[i]`
passed as current_z. The page refuses a blank xzone. The test now checks that
an omitted field keeps the stored 9.0, and the JS test checks that xzone is not
in the optional-drop regex.

- **INFO:** the comment at `zones_http_post_parse.c` (about :598-605) still says
  an omitted guard field leaves the firmware default in force.
- **INFO: behavior change.** Preset or whole-page posts that omit the guard
  keys used to reset them to the default. Now they keep the stored value. This
  is the intended direction, because a guard is never silently disabled, but
  any caller that relied on omission to reset needs to post the value.

**LOW-1 (preserve_rejected_file outside the save lock): the "partly fixed" claim is accurate.**
load_raw no longer preserves, and only the boot resolve writes .bad.
`preserve_rejected_file` still has a check-then-write race between
`pref_cfg_fs_reset_refuses_write` and `s_write_fn`, outside the save lock. In
practice it runs at boot only.

**LOW-2 (bad_copy_failed not reported): fixed.** bad_copy_failed is in the
record and in the banner.

**LOW-3 (latch never cleared): fixed.** `zones_http_post.c:780` clears the
latch under zones_cfg_lock.

- **INFO:** the test calls only `zones_config_load_fault_clear()` directly, so
  nothing proves the POST path calls it. Deleting the call at
  `zones_http_post.c:780` would not fail a test.
- **INFO:** `zones_config_load_fault_clear` does its memset without a lock, so a
  reader can see a torn record. This is harmless, because the record is
  diagnostic only.
- **INFO:** the LCD adds a "+CFG SWAP FAULT" suffix. This is fine.

## 473f597e (danger_mode stack back to 3072 B)

**Supported. INFO only.**

- **INFO:** the static 800 B figure came from an origin/main 129586d4 ELF, not
  from dev. Between main and dev, safety_link_frames.c grew by 168 lines,
  including new calls inside `safety_apply_fw_version`, which is on the walked
  path (`safety_note_pico_reboot_locked`,
  `safety_diag_reannounce_consider_locked`). The live 780 B figure is an idle
  high-water mark from firmware that is not dev tip, and the request_enable
  path probably never ran. The margin is still ample: a pessimistic ~1900 B is
  62% of 3072 B, and the checker ceiling is 2256 B. A re-measure on dev tip
  would make the evidence match the code.
- **INFO:** comment indentation at `danger_mode.c` (about :60) is off. This is
  cosmetic.

## 2a1d69a8 (target-build fixes)

**Fixes what it claims.** Yes. The new message is about 154 characters, within
`err_msg[160]` at `backup_import.c:4279`. The cfg_fs.h include in main.c is
correct.

- **INFO:** for a v7 or later backup that carries thermo_count
  (`backup_import_resolve_topology`, about :978), the message "set the zone count
  under Thermocouples & Zones first" is misleading when an index is at or above
  the backup's own count. Only a hand-edited backup can produce that. The header
  comment at about :3637 ("The backup carries no thermo_count") is stale for
  v7+.

## 5dcd6dbd (HTTP fuzz part 2 tests)

**Non-vacuous.** The aux fuzz in test_aux_outputs_http.c is non-vacuous:
zones_union is 0, the minimal "relay=N&enabled=1" is accepted elsewhere, and
the duplicate on= case has a 200 positive. Its 4xx check accepts 409, but the
gate is not blocked in that test. The kiln_cfg fuzz is non-vacuous: it uses the
same mechanism as run_apply, and `test_apply_idle_and_clear_reaches_swap_worker`
is the positive control.

- **LOW: vacuous cases in test_backup_import.c `test_fuzz_hostile_backup_shapes`.**
  The cases "pid_kp is a string", "pid_kp is null", "pid_kp 1e999",
  "ct_mask 256/-1/string" and "duplicate zone index in one import" all carry
  pid_kp only. `backup_import.c:1401-1402` requires pid_kp, pid_ki and pid_kd,
  so each case is refused on the missing pid_ki/pid_kd ("missing, negative, or
  malformed"). None of them reaches the type, finite, ct_mask or duplicate check
  its label names. The duplicate case fails at entry 0, before the duplicate
  loop at `:1385`. Scenario: delete the isfinite check or the ct_mask range
  check, and every one of these cases still passes. Only the 5000-entry case
  carries all three gains and really reaches the duplicate check.
  - The test asserts only `!ok`, never the error text, which is how this went
    unnoticed.
  - There is no positive control that the full `valid` body is accepted. The
    prefix loop stops at n < vl.
  - The "embedded raw NUL" case is only another prefix, because a C literal
    truncates at \0.
  - The deep-nesting loop "close as many as opened" is a no-op.
  - The skipped F4 cut points and the commented-out F5/F6 cases are documented.
- **INFO: test_profiles_http.c `test_fuzz_profile_post_hostile`.** It runs
  first in run_test_profiles_http (`:5144`), before profiles_http_test_set_loaded.
  It has no in-context positive control that a good body stores into slot 0.
  The 400 assertions for short and empty bodies do prove the handler gets past
  the cfg mount check at `profiles_edit_http.c:485`. The appended `seg` suffix
  creates duplicate keys (zone_mask=0&zone_mask=1, seg_count=0&...seg_count=1),
  so the test depends on first-wins parsing. The seg_count=3 case skips
  pcfg_reset_all.

## 34d2c40c (fuzz findings doc F4-F9)

F4-F9 are documented, and the not-covered list (retarget, delete, run_queue,
live-edit, auth forms) is explicit.

- **LOW: the coverage claim is overstated.** "Covered: ... duplicate zone index,
  ct_mask range/type" is not true, for the reason given in the 5dcd6dbd finding
  above. Those cases never reach the checks they name.

---

## Findings list

| # | Sev | Commit | Location | Finding |
|---|---|---|---|---|
| 1 | LOW | ffd50690 | kiln_cfg_store.c:942, kiln_cfg_swap.c:532 | F5 claim refusal clears active_id at boot / turns rollback into "diverged" |
| 2 | LOW | ffd50690 | autotune_engine_guard.c:721 | claim refusal reported as failed-to-persist "live in RAM"; follow-on setters non-atomic |
| 3 | LOW | fa4a62ef | zones_config_cfg_fs.c:48, load_raw_impl | read error / over-size file treated as absent, NVS overwrites newer zones.json |
| 4 | LOW | 5dcd6dbd | test_backup_import.c fuzz; backup_import.c:1401 | type/finite/ct_mask/duplicate cases refused on missing pid_ki/kd, vacuous |
| 5 | LOW | 34d2c40c | fuzz findings doc | coverage claim for duplicate index, ct_mask overstated |
| 6 | INFO | ffd50690 | backup_import.c:2558, 2680, 2753, 2768, 2793 | claim refusal worded as validation failure |
| 7 | INFO | ffd50690 | adaptive_tune.c:1183 | revert_available true while snapshot stale |
| 8 | INFO | ffd50690 | test_zones_http.c, test_autotune_engine_prestart.c | missing mid-run and idle positive controls |
| 9 | INFO | ffd50690 | adaptive_tune_model.c, zones_config_accessors.c:590, autotune_engine_guard.c:577 | stale comments, dead branch, double read |
| 10 | INFO | ffd50690 | autotune_engine_step_identify.c:217 | step Accept now fails whole on model reject |
| 11 | INFO | fa4a62ef | zones_http_post_parse.c:598 | stale omitted-guard comment; omission now keeps stored value |
| 12 | INFO | fa4a62ef | test_zones_http.c, zones_http_post.c:780 | POST latch-clear call not tested; unlocked memset |
| 13 | INFO | fa4a62ef | preserve_rejected_file | LOW-1 check-then-write race remains (boot only), claim accurate |
| 14 | INFO | 473f597e | danger_mode.c:60 | stack figures from main / non-tip ELF; margin still ample |
| 15 | INFO | 2a1d69a8 | backup_import.c:978, :3637 | misleading zone-count hint for v7+ backups; stale comment |
| 16 | INFO | 5dcd6dbd | test_profiles_http.c:5144 | no in-context positive control; duplicate keys rely on first-wins |

## Fix status (fwbatch14)

- LOW-1: FIXED for the boot restore path in f7d4b08fb (a restore refused only because a run holds the claim keeps the active id). The `kiln_cfg_swap.c` rollback path is FIXED in the pooled firmware LOW batch commit ("Pooled firmware LOW batch: strict kiln_cfg id, ..."): a re-import refused only by the run claim (`ZONES_IMPORT_REASON_RUN_CLAIMED`) reports "ROLLBACK REFUSED" instead of "ROLLBACK FAILED" (still returns false; journal kept for the boot retry).
- LOW-2: FIXED in 3408a12ad (new outcome `REFUSED_NOT_WRITTEN`, distinct from `FAILED_TO_PERSIST`).
- LOW-3: FIXED in 138975bee.
- LOW-4: FIXED in e5bdb8a3c.
- LOW-5: FIXED by e5bdb8a3c (the LOW-4 fix: the duplicate-index and ct_mask cases now carry all three gains and have positive controls, so the coverage claim in HTTP_PARSER_TEST_FINDINGS_2026-10-09.md is true). Earlier text citing review 14 LOW-5 was a different finding.
- INFO 9 (accessors comment), 11 (omitted-guard comment), 14 (indentation): FIXED in a62493c24. The `backup_import.c` ":3637" comment from INFO 15 is already corrected on dev.
- INFO 6, 7, 8, 10, 12, 13, 15 (message hint), 16, and the rest of INFO 9 (dead branch, double read): SKIPPED, behavioural or multi-file test work outside this batch.
