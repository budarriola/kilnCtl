# Review: misc batch 8 (fwfx17, r3kfw, testfx5), 2026-10-10

Reviewer: Claude Opus (review only, no board access). Base: origin/dev 68e3b4966.

Commits reviewed:

- `e6201482f` (fwfx17): `profiles_http_save_ex()` rolls back a fresh slot under the
  save lock on a failed persist; `profiles_http_drop_unpersisted()` removed; factory
  reset `try_begin` test; LINK_PROTOCOL.md CT wording; relay_authority.h comments.
- `537fd6402` (r3kfw): new host tests `test_cfg_fs_format_http.c`,
  `test_setup_progress_http.c`, `test_dashboard_settings_http.c`; `char uri[256]` in
  the `esp_http_server.h` host stub.
- `0598cd290`, `f006d22d9` (testfx5): R2ACE/SL3 test fixes,
  `DASHBOARD_JSON_STATUS_BUF_SIZE` 5760 -> 5888, `profile_exec_wdt` stack ceiling
  2752 -> 2816, `run_all_checks.ps1` excludes `logs/`, cp1252 mojibake classes in
  `lint_pages.js`, SaftyFW identity record stub.

No HIGH or MED findings. Four LOW findings, two observations.

## Findings

### LOW-1: post-rename read-back failure can resurrect a rolled-back SAVE_AS slot (pre-existing)

Status: FIXED (misc8fx): the save_ex rollback now also calls `profiles_cfg_fs_delete()`; test `test_save_ex_rollback_unlinks_written_file`.

`profiles_http.c` `profiles_http_save_ex()` now clears the fresh slot inside the
lock/generation window when `nvs_save_slot_locked()` fails and the caller asked for
`out_persisted` with a fresh slot (`requested_id >= PROFILES_MAX_COUNT`). That is
correct for every failure where nothing reached flash, and it also covers the
`ESP_ERR_INVALID_STATE` refusals (rev unknown, reset refused). Overwrites
(`profiles_live_http.c` OVERWRITE onto `origin_id`) and the `NULL out_persisted`
callers keep the old "applied live" convention, as intended.

One residual case: `nvs_save_slot_locked()` is file-only
(`profiles_cfg_fs_save()` -> `cfg_fs_write_atomic()`), and `cfg_fs_write_atomic()`
reports `ESP_FAIL` when the read-back verify after the rename fails. At that point
the new file is already in place. RAM is rolled back and the rev is not advanced, so
the next boot can load the slot the client was told was not saved. The removed
`profiles_http_drop_unpersisted()` had exactly the same behaviour, so this is not a
regression. Fix if wanted: on a post-rename verify failure, unlink the file (or
report "state unknown" to the client rather than "not saved").

### LOW-2: `s_format_pending_reason` 96 -> 160 not measured against DRAM or main-task stack

`cfg_fs_mount.c` grows `s_format_pending_reason` by 64 B of internal `.bss`, and
`char reason[sizeof(s_format_pending_reason)]` (around line 426) adds 64 B to a stack
frame on the `app_main` boot path. `check_main_task_stack_budget.py` names
`cfg_fs_mount_device` as the deepest `app_main` path. Both the DRAM `.bss` budget
(ceiling 101000, last measured 99016) and the main-task stack budget need a built
ELF, so they were SKIP-FAST in the landing runs. Probably fine (the margins are
larger than 64 B), but confirm both in the coordinator's next full run.
`DASHBOARD_JSON_STATUS_BUF_SIZE` is a PSRAM buffer, so that bump does not touch the
budget. The `profile_exec_wdt` ceiling change only adds margin (stack is 6144 B).

### LOW-3: `test_link_task_fuzz` case 4 lost two assertions

Status: FIXED (misc8fx): both checks restored at the end of case 4.

Case 4b (a heat-safe commit triggers an immediate reapply) was inserted above the
existing `CHECK(g_reload_cal == 1, "still reloads cal")` and
`CHECK(link_staging_count(&s_staging) == 0, "staging reset after accepted write")`.
Those two checks now run only after 4b. Case 4 (the heat-unsafe commit) no longer
asserts that the calibration reloads or that staging resets. Fix: repeat both
checks at the end of case 4.

### LOW-4: test gaps in the new r3kfw tests

Status: FIXED (misc8fx): duplicate-key test, STEP_COUNT-derived step, and strict leading-digit parsing (`+`/space refused) in setup_progress_http.c and dashboard_settings_http.c, each tested.

- `test_cfg_fs_format_http.c` does not cover a duplicate key
  (`?force_healthy=0&force_healthy=1`). The gate takes the first occurrence; nothing
  pins that.
- strtol leading-space note (requested): `setup_progress_http.c:181` and
  `dashboard_settings_http.c:147` parse with `strtol()`, which skips leading
  whitespace and accepts a leading `+`. So `step=%201` is accepted as 1, and
  `level=+2` (or `level=%202`) as 2. The range checks still apply, so nothing out of
  range gets in. This is harmless. No test pins the behaviour either way, though, so
  if strict digits-only parsing is wanted it is a one-line check before `strtol()`.
- `test_setup_progress_http.c` hardcodes `step=12` as the first invalid step
  (STEP_COUNT 12). It should derive this from the header constant so a new step does
  not silently turn the case into a valid write.

## Observations (no action)

- `esp_http_server.h` stub `char uri[256]`: only `cfg_fs_format_http.c` and
  `wifi_provision_http.c` read `req->uri`, and the real IDF size is
  `HTTPD_MAX_URI_LEN + 1` (512 by default). The change hides nothing. It only means
  host tests cannot exercise URIs between 256 and 512 bytes.
- `run_all_checks.ps1` `logs/` exclusion: no tracked `check_*.ps1` lives under
  `logs/`. If `$repoRoot` ever ended in a backslash, the regex would stop matching
  and the exclusion would become a no-op. That fails open (more checks run, not
  fewer), so it is safe.
- SaftyFW identity record stub: its comment notes the record has no CRC, so a
  flipped commit or config byte reads as a different identity rather than as
  corruption. This is an existing design property, not something introduced here.
- The `profiles_store.h` comment left behind by the removed
  `profiles_http_drop_unpersisted()` now floats with no declaration under it
  (cosmetic).
- `lint_pages.js` mojibake classes (`Â` + cp1252-mapped bytes / `€`, and
  `Ã€`) have fixture tests and look right.

## Negative tests (tools/negtest.ps1, throwaway worktrees)

Base 68e3b4966. Both baselines passed and the real tree was unchanged.

KilnFW (`-Only '^(profiles_http|cfg_fs_format_http) '`, ALL_CAUGHT):

- `save_ex_no_rollback` (rollback branch disabled) -- CAUGHT,
  test_profiles_http.c:2251/2255 (phantom RAM-only profile; same-name retry).
- `save_ex_rollback_overwrites_too` (requested_id condition dropped) -- CAUGHT,
  test_profiles_http.c:2262 (overwrite stays applied).
- `force_healthy_prefix_1` (exact-length check dropped in cfg_fs_format_gate.c) --
  CAUGHT, test_cfg_fs_format_http.c:179/180.
- `format_mode_gate_removed` (system_mode_gate check disabled) -- CAUGHT,
  test_cfg_fs_format_http.c:144/145/150.

SaftyFW (preset saftyfw-host, ALL_CAUGHT):

- `commit_heat_safe_no_immediate_reapply` (case 4b path) -- CAUGHT,
  link_task_fuzz_tests.exe.
- `volatile_arms_retry_instead` (volatile write arms retry instead of reapplying) --
  CAUGHT, link_task_fuzz_tests.exe.

Not negative-tested: the `dashboard_settings_http` and `setup_progress_http`
mutations (`peer_len > 0` fall-through, `step >` vs `>=`). Those two exes do not
link on dev today (`cfg_fs_degraded_is` unresolved, the known break devbreak is
fixing), so the baseline cannot pass. Re-run them once that fix lands.
