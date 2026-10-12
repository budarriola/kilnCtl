# Review: web7 fix batch on origin/dev (2026-10-10)

Reviewer: Opus, worktree `rvwebgenfx` at origin/dev `99461ef0c`. No board access. No code changed.

Reviewed commits:

- `59d33f341`, `221cbb522`, `0520638d6`, `99461ef0c`. They fix `REVIEW_WEB7_2026-10-10.md` MED-1 and LOW-2..LOW-9. LOW-8 was deliberately not fixed (documented only).

## Verdict

The batch does what its "Fix status" section claims for MED-1, MED/LOW-2, LOW-3, LOW-4, LOW-7 and LOW-9. There are no HIGH or MED findings.

- Three LOW findings:
  - The MED-1 restore now succeeds, but it re-posts the whole pre-case snapshot, so it silently reverts any edit made during the case.
  - LOW-5 is only partly fixed: the LCD captures the rev at slot pick, not at edit load, and it checks outside the save lock.
  - LOW-6 infers the 409 from the flag instead of threading the real error through, and the delete clears favorite and history before the refusal.
- Test gaps:
  - The LCD guard test is a source scan only (negtest MISSED).
  - The verify loop of `legacy_nvs_erase_keys` has no probe-error test (negtest MISSED).
  - The zones seed's forced-odd bit is not asserted (negtest MISSED; harmless).
- The fixer did not negtest the MED-1 pytest or the page JS test. Both are now negtested here: all 5 mutations CAUGHT. Of 11 host mutations, 8 were CAUGHT and 3 MISSED (table below).

## Answers to the review questions

**Can a random seed collide with a stale client token or with the odd/even convention? Can it wrap?**

- Zones: `zones_http_start` (`zones_http.c:720`) sets `s_config_generation = (hal_sysinfo_random_u32() & 0x3fffffff) | 1`.
  - The seed is odd, never 0, and at most about 1.07e9.
  - POST parses `expected_generation` with a ceiling of INT32_MAX (`http_form_parse_long(..., 0, 2147483647L)`). Page tokens therefore stay valid for about 1.07e9 bumps after the worst seed, and a uint32 wrap needs about 3e9 bumps. Neither is reachable: a bump is one config write.
  - A stale pre-reboot token matches only if the new seed lands within the number of bumps made since. The probability is roughly bumps / 2^30, which is negligible.
  - The zones generation has no odd/even meaning, so forcing it odd only keeps it nonzero, as the comment says.
- Profiles: `profiles_http_start` (`profiles_http.c:2320`) stores one seed, `hal_sysinfo_random_u32() & 0x3ffffffe`, in every slot before `profiles_boot_load()`.
  - The seed is even, so no slot starts looking "write in flight", and `profiles_http_slot_runnable_rev` accepts it.
  - The seed can be 0 (probability 2^-29). That is harmless: `profiles_http_slot_rev()` returns 0 only for builtins and out-of-range ids, and the web check compares a user slot's rev with the page token only, never with 0 as a sentinel.
  - The ceiling and wrap arithmetic is the same as for zones: each save adds 2, so about 5e8 saves of headroom.
  - All slots share one seed, so two slots can hold the same rev. That is harmless: every comparison is per slot.
- `live_profile.c:857` already seeded its generation (`& 0x3fffffff`). Its header comment at `live_profile.h:330` still says "reset to 0". This is a stale-comment nit.
- RNG quality: `hal_sysinfo_random_u32()` is `esp_random()` on target. `wifi_prov_start` runs in `main_boot_early.c` before `zones_http_start` (`main_network_http.c:367`) and `profiles_http_start` (`:423`), so RF is up and the RNG is a true RNG at seed time.

**Is the seed applied everywhere a generation is initialised?**

Yes.

- `zones_http_start` and `profiles_http_start` are each called once, from main.
- No other path assigns either counter. Every other write is a `++` (zones) or a begin/end pair (profiles).
- Factory reset and backup import reboot, so they pass through the seeded start. A config reload (`zones_config_*` setters, profile_executor reload) bumps the counter and never resets it.

**Is the LCD revision check race-free?**

No. See LOW-B. Its window is narrow (two tasks, two operators), but it is not closed, and the long edit window that LOW-5 described is not covered at all.

**Can the restore retry loop spin forever or overwrite a concurrent edit?**

- It cannot spin: `_HP07_RESTORE_ATTEMPTS = 2` bounds the loop.
- It does overwrite concurrent edits. See LOW-A.

**Is the probe-error handling fail-safe in every caller?**

Yes.

- `legacy_nvs_erase_keys` (`persist/legacy_nvs_erase.h`):
  - First loop: `HAL_NOT_FOUND` means absent. Any other non-OK status logs a warning, sets `ESP_FAIL` and keeps going.
  - Verify loop: anything other than `HAL_NOT_FOUND` counts as still present.
- All five callers discard the result with `(void)`: `adaptive_tune.c:1477`, `ramp_assist_cfg.c:131`, `aux_outputs_cfg.c:203`, `ct_verify_store.c:291`, `iter_tune_store.c:215`.
- In every one, the cfg file is already authoritative and the erase is retried on the next boot. A probe error now leaves the legacy key in place, which is the safe direction.
- The target `hal_kv_key_exists` maps `ESP_ERR_NVS_NOT_FOUND` to `HAL_NOT_FOUND`, so an absent key is not misread as an error.

**Are the tests vacuous?**

Mostly not. The negtests below catch removal of each code fix. These parts remain weak:

- The LCD guard is a source scan, so it misses a disabled check (H8).
- The verify-loop probe error is untested (H5b).
- The zones seed's `| 1` is untested (H3b).
- The forced-even profile seed has no dedicated test, but it is covered incidentally. The fake RNG fallback is `0xA5A5A5A5` (odd), so an odd-seed mutation (H7) fails 13 existing seqlock assertions. That coverage would vanish if someone made the fallback even.

## Findings

### LOW-A: HP-03/HP-07 restore reverts edits made during the case

Where: `tools/PcTools/src/kilnctrl/bench_test/cases_heat.py`, `_post_zones_restore` (line 1140; callers at lines 998 and 1230).

On each attempt the function GETs `/api/zones`, copies the whole pre-case snapshot, swaps in the fresh `generation`, and POSTs `build_post_body(merged, {})`. The 409 now goes away, but the fresh token comes from the GET made one moment earlier. It only guards the GET-to-POST window, not the case's whole duration.

Scenario:

1. HP-07 runs a firing with lowered `max_temp_c`.
2. During the firing, adaptive tune writes new gains (or an operator edits a relay name, or an autotune is accepted).
3. The restore re-posts every pre-case field. That silently reverts the tuned gains or the edit, and reports success.

This matches the pre-webfx5 behaviour, so it is not a regression. It does defeat the lost-update guard that webfx5 added.

A second, smaller problem: if the fresh GET raises or lacks `generation`, the code falls back to the stale `restore_body`. That gives a certain 409, and the reported error then names `zones_config_stale` instead of the GET failure.

Fix:

- Build the body from the fresh GET, and override only the fields the case changed (`zone_type` for HP-03, `max_temp_c` for HP-07) from the snapshot.
- On a GET failure, record the GET's own error in `last_error` and skip the POST for that attempt.

### LOW-B: LOW-5 only partly fixed; LCD rev captured at slot pick, checked outside the save lock

Where: `firmware/KilnFW/App/drivers/ui/ui_page_profile_builder_review.c`:

- `slot_clicked_cb` captures `s_pick_rev` at line 179.
- `do_save_apply` checks it at lines 97-113, then calls `profiles_http_save()` at line ~121.

Web7 LOW-5 described this hole: "a web save made between the LCD load and the LCD confirmation is overwritten silently". The LCD edit flow is:

1. `ui_page_profile_detail.c:476` calls `ui_page_profile_builder_start_edit(id)`, which loads the profile.
2. The user works through the builder pages, which is the long window.
3. The slot grid calls `slot_clicked_cb`, which captures the rev.
4. The overwrite confirm and the admin PIN gate run.
5. `do_save_apply` runs.

The fix covers only steps 3-5.

Scenario: an LCD operator opens profile 4 for edit. Meanwhile the web page saves profile 4. The LCD operator finishes, picks slot 4 and confirms. The rev captured at pick already includes the web save, so the check passes and the web save is overwritten.

Second gap: the check runs unlocked on the LVGL task, and `profiles_http_save` takes the save lock later. A web save on the httpd task between those two points is overwritten. The web path does it correctly: it compares `expected_rev` under `profiles_save_lock()` (`profiles_edit_http.c:673-701`).

Fix:

- Capture `profiles_http_slot_rev(source_id)` in `ui_page_profile_builder_start_edit`.
- Pass an expected rev into `profiles_http_save_ex` (a new optional parameter) and compare it under the save lock, as the web path does.
- When the user picks a different slot than the source, use the pick-time rev for that slot.

### LOW-C: rev-unknown delete 409 is inferred from the flag; favorite and history are cleared before the refusal

Where:

- `firmware/KilnFW/App/drivers/http/profiles_edit_http.c:868-873`
- `profiles_delete_slot` in `profiles_http.c` (~2155-2225)

The handler now sends `cfg_fs_http_persist_failed_state(req, s_profile_rev_unknown[id], s_profile_rev_unknown[id] ? ESP_ERR_INVALID_STATE : ESP_FAIL, -1)`. The error is derived from the flag, not threaded from the failing call. `profiles_delete_slot` does three things in order:

1. Clears the favorite.
2. Runs `firing_stats_erase`, which can return `PROFILES_DELETE_PERSIST_FAILED`.
3. Only then runs `nvs_erase_slot_locked`, which refuses a rev-unknown slot.

Scenarios:

- On a rev-unknown slot, a real flash failure in `firing_stats_erase` is labelled 409 `store_unreadable_at_boot`, not 500. The operator is sent after the boot-load state instead of the flash.
- Pre-existing, but now more visible because the refusal is the expected answer on a rev-unknown slot: a delete that will certainly be refused first wipes the slot's favorite mark and its firing history. The profile stays and its metadata is gone.

Fix:

- Check `s_profile_rev_unknown[id]` at the top of `profiles_delete_slot`, before any side effect, and return a distinct result (for example `PROFILES_DELETE_REV_UNKNOWN`) that maps to the 409.
- Thread the real `esp_err_t` out with `PROFILES_DELETE_PERSIST_FAILED`.

### Test gaps (LOW)

- **H7, forced-even profile seed (covered incidentally, no dedicated test).** Changing the mask to `0x3fffffff` would make about half of all boots start every slot odd, which reads as "write in flight". `profiles_http_slot_runnable_rev` then rejects every profile start (`profile_executor_run.c:464`, "was re-saved -- not started"), and the LCD's new odd-rev check refuses every LCD save to an untouched slot. The LOW-4 test is a source scan for the seed line and does not check parity. The negtest was still CAUGHT, but only because the fake RNG's unscripted fallback (`0xA5A5A5A5`, `fake_sysinfo.c:7`) is odd and existing seqlock tests then fail. Fix: script an odd seed explicitly and assert `profiles_http_slot_rev(i)` is even for every user slot after `profiles_http_start`.
- **H8, LCD guard.** `test_profiles_http.c` scans the source for the check substring. Wrapping the condition in `0 && (...)` still matches it. A fake-LVGL host harness does not exist, so the honest fix is LOW-B's move of the check into `profiles_http_save_ex`, where a host test can call it directly.
- **H3b, zones seed `| 1`.** Dropping it allows a 0 seed (probability 2^-30), which the comment says a zero-initialised cache must never match. The LOW-4 seed test uses tolerance-based seeds and does not assert odd. Fix: script seed `0` and assert the generation is nonzero. Harmless in practice.
- **H5b, verify-loop probe error.** `test_legacy_erase_probe_error_is_not_absent` injects one probe error, and the first loop consumes it. Fix: script the error for the second `hal_kv_key_exists` call of a key, and assert the function returns `ESP_FAIL`.
- **Source-scan page test.** `test_page_lost_update_tokens.js` checks that `reloadUnlessDirty(` appears in the Enable and Revert segments, not that it runs on the success path only. It catches removal (J1, J2) but not a move into the error branch. This is acceptable for a page with no DOM harness. It is noted, not filed.

### Nit

- `live_profile.h:330` still says the generation resets to 0. It is seeded randomly (`live_profile.c:857`).

## Tests run (worktree only)

- `build_host_tests.ps1 -Only "zones_http|profiles_http|aux_outputs_store"`: all 4 executables built and passed.
- `node firmware/KilnFW/App/test/test_page_lost_update_tokens.js`: 7/7 PASS.
- `pytest tests/test_bench_test_cases_heat.py`: 118 passed.

## Negative tests (`tools\negtest.ps1`, throwaway copies, real tree verified unchanged)

How each group was run:

- Host: `-Command` running `build_host_tests.ps1 -OutDir {OUT} -Only 'zones_http|profiles_http|aux_outputs_store'`, with `-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`.
  - Deviation: the `kilnfw-host` preset ignores `-PresetArg` and builds every executable, so `-Command` with `-Only` was used instead. The verdict pattern is the required one.
- Pytest: the `pytest` preset with `-PresetArg "tests/test_bench_test_cases_heat.py -k RestoreFreshGeneration"`.
- JS: the `check` preset with `firmware/KilnFW/App/test/check_js_host_tests.ps1`.

| Mutation | Target | Result |
|---|---|---|
| H1: remove the relay-name `s_config_generation++` | MED/LOW-2 | CAUGHT (`test_zones_http.c:11026`) |
| H2: remove the relay-type `s_config_generation++` | MED/LOW-2 | CAUGHT (`test_zones_http.c:11030`) |
| H3: remove the zones seed line | LOW-4 | CAUGHT (`test_zones_http.c:3415`) |
| H3b: zones seed `& 0x3ffffffe` without `\| 1` (allows 0) | LOW-4 | MISSED (test gap, harmless) |
| H4: delete error `ESP_FAIL` instead of the flag-derived error | LOW-6 | CAUGHT (`test_profiles_http.c:5778-5779`) |
| H5a: first-loop probe-error branch `if (0)` | LOW-9 | CAUGHT (`test_aux_outputs_store.c:135,138`) |
| H5b: verify loop `ex == HAL_OK` instead of `ex != HAL_NOT_FOUND` | LOW-9 | MISSED (test gap) |
| H6: LOW-7 early undecided check `if (0)` | LOW-7 | CAUGHT (`test_zones_http.c:2195`) |
| H7: profile seed mask `0x3fffffff` (odd allowed) | LOW-4 | CAUGHT incidentally (13 seqlock assertions, odd fake fallback) |
| H7b: drop the `atomic_store` of the profile seed | LOW-4 | CAUGHT (`test_profiles_http.c:2746`) |
| H8: LCD rev check wrapped in `0 && (...)` | LOW-5 | MISSED (source scan) |

The host run ended with negtest verdict ERROR, "REAL TREE CHANGED": this review doc was written into the caller worktree while it ran. `git status` afterwards listed only this untracked doc, every copy was removed, and all 11 mutation verdicts are valid.

| Mutation | Target | Result |
|---|---|---|
| P1: skip the fresh-GET block (`if False:`) | MED-1 `_post_zones_restore` | CAUGHT (`test_restore_after_case_post_bumped_generation_succeeds`) |
| P2: drop `merged["generation"] = fresh["generation"]` | MED-1 | CAUGHT (same test) |
| J1: remove the Enable `reloadUnlessDirty` | LOW-3 page | CAUGHT ("adaptive-tune Enable refreshes via reloadUnlessDirty") |
| J2: remove the Revert `reloadUnlessDirty` | LOW-3 page | CAUGHT ("adaptive-tune Revert refreshes via reloadUnlessDirty") |
| J3: remove the zones `expected_generation` push | webfx5 page | CAUGHT ("zones save body pushes expected_generation") |

## Fix status

- LOW-A: not fixed in this batch (bench test restore, separate pooled item).
- LOW-B: fixed. `ui_page_profile_builder_zones.c` captures the source slot rev at `start_edit`; the review page uses it when overwriting the edited slot; `profiles_http_save_expect_rev()` compares the rev inside the profiles save lock. Host test `test_save_expect_rev_under_lock`; the source-scan test now checks the new call.
- LOW-C: fixed. `profiles_delete_slot` returns `PROFILES_DELETE_REV_UNKNOWN` before the favorite clear and the stats prune; the handler maps it to the 409, a real persist failure to 500. Test: favorite survives a refused rev-unknown delete.
- Nit `live_profile.h` generation comment: fixed.
- Test gaps H3b, H5b, H7 (parity): not done here. H8 is superseded by the LOW-B test.
