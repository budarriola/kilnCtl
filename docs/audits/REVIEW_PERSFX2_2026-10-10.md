# Review: persfx2 batch (2026-10-10)

Scope: the persfx2 commits on origin/dev.

- `8a8b35e7a`: persist changes. Refuses a zones POST while the zones config is undecided, returns 409 when stores are degraded, uses safe defaults for the control stores, adds relay_cycles backoff and adds the backup stale marker.
- `26da1f472`: zones_http test showing a failed save keeps the load fault.
- `f28189d99`: links the degraded-store stub into kiln_io_owner_sx_dispatch.
- `5fabb8b02` and `a1438d03b`: audit doc fix marks and SHAs.

This was a review only: no code was changed and no board was touched. Line numbers refer to origin/dev `166b937b8`.

Focus areas:

1. Outage risk and recovery routes.
2. Safe defaults for the five control stores.
3. Whether the 409 reaches HTTP, LCD, MCP and the web pages.
4. relay_cycles backoff.
5. Lifecycle of the backup stale marker (the reset-one-side bug class).
6. Whether the tests discriminate (spot negtests).

Summary: 0 HIGH, 3 MED, 5 LOW, several INFO.

- The zones undecided refusal, the relay_cycles backoff and the five-store safe defaults are correct for the cases they target.
- The main gaps:
  - A standing check fails on dev because of the new backup key.
  - The 409 mapping is global rather than per store.
  - The MED-3 "no stale NVS adoption" fix covers only the *read-error* branch. The absent, corrupt and unmounted branches still adopt the frozen NVS copy and write it back to the file.

---

## MED-1: `check_cfg_convert_field_mirror_drift` fails on origin/dev; convert_config strips the new marker (FIXED in ad3640f9f)

- `firmware/KilnFW/App/drivers/http/backup_export.c:253` emits the top-level key `stale_or_unknown_stores`.
- `firmware/KilnFW/App/test/cfg_convert_field_mirror_drift_check.py` (run by `check_cfg_convert_field_mirror_drift.ps1`) fails on dev tip with: `firmware emits/reads zone key(s) cfg_convert.py has never heard of: ['stale_or_unknown_stores']`.
- Separately, `tools/PcTools/src/kilnctrl/cfg_convert.py:145` `ADDITIVE_TOP_LEVEL_KEYS` does not list the key, so `convert_config` silently drops it.

**Failure scenario:**
- The next coordinator full run of dev reports a NEW failure and blocks promotion to main.
- A backup taken while a store was degraded, then passed through `convert_config`, loses the only in-file record that some of its values were stale defaults.

**Fix:**
- Add `stale_or_unknown_stores` to the check's non-zone structural key list.
- Add it to `cfg_convert.py`. Either pass it through in `ADDITIVE_TOP_LEVEL_KEYS`, or name it in the conversion report.
- Negative-test the check after the change.

## MED-2: the 409 `store_unreadable_at_boot` is decided by the global degraded count, not by the store that failed (FIXED in 703173bb2)

`firmware/KilnFW/App/drivers/common/cfg_fs_refusal_http.h:56-70`: `cfg_fs_http_persist_failed()` returns 409 "store_unreadable_at_boot ... reboot" whenever `cfg_fs_degraded_count() > 0`, whatever the error was and whichever store it came from.

**Failure scenarios:**
- Store A, for example `ramp_assist`, could not be read at boot.
- Later, a save to unrelated store B fails because of:
  - a genuine flash write error;
  - running out of memory (OOM);
  - a factory reset in flight;
  - the kiln_cfg rollback journal (`kiln_cfg_swap_zone_edits_at_risk()`);
  - or a profile `s_profile_rev_unknown` refusal (`profiles_http.c:1397`).
- The user is told "a stored setting could not be read at boot ... reboot" about B. That misattributes the cause and hides a real flash fault behind a reboot instruction that may not help.

The opposite case also exists, where a genuine unreadable-store refusal still comes back as 500:

- `cfg_fs_http_persist_failed_adopted()` (used by `dashboard_settings_http.c:82`) never checks the degraded registry.
- Profile rev-unknown is not registered as degraded, so with no other degraded store it returns 500 "could not be saved to flash".

**Fix:**
- Decide per call. Pass the `rel_path` (or the store identity) and the `esp_err_t`.
- Return 409 only when that store is rev-unknown (`pref_cfg_fs_rev_unknown(rel_path)`, `zones_config_is_undecided()`) and the error is `ESP_ERR_INVALID_STATE`.
- Map the rollback journal case to its own code.
- Register profile rev-unknown in the degraded table.
- Apply the same rule in the `_adopted` variant.

## MED-3: MED-3 (no stale NVS adoption for retired stores) covers only the read-error branch (FIXED in 703173bb2)

`firmware/KilnFW/App/drivers/persist/pref_cfg_fs.c:461-503`: `adopt_nvs_if_unreadable=false` takes effect only when `cfg_fs_read` returns an error (line 475). The `!file_valid` branch (486-503) still copies the frozen NVS bytes into RAM and writes them back as the file through `pref_cfg_fs_save()`. That branch is taken when:

- the file is absent;
- the file has the wrong length or is `ESP_ERR_INVALID_SIZE`;
- the validator rejects it;
- `cfg` is not mounted (`load_raw_impl` returns ESP_OK and valid=false).

The five stores are:

- `aux_outputs_cfg.c:196`
- `ramp_assist_cfg.c:125`
- `adaptive_tune.c:1471`
- `iter_tune_store.c:211`
- `ct_verify_store.c:282`

None of them erases its legacy NVS key after the file becomes authoritative; aux erases only its journal key. Their NVS copies are therefore frozen at the 2026-10-06 dual-write close.

**Failure scenario:** the user disables an aux on/off rule; the file is now authoritative with the rule disabled and NVS still holds it enabled. Then one of these happens:

- `cfgfs_format` with `force_healthy=1` (itself a recovery route);
- a torn or corrupt `aux_outputs.json`;
- a validator change in newer firmware;
- a boot where `cfg` failed to mount.

On the next boot the enabled rule (or an old `ki_baseline`, iter_tune state or CT "pass" verdict) is resurrected from NVS and written back as the file. This is the exact hazard MED-3 set out to close, reached through a different branch. The defaults are safe; the problem is that the defaults are not chosen.

**Fix:** pick one.

- In retired mode, treat an invalid or absent file as "safe defaults" and never migrate NVS into the file, once a marker shows the store has ever been file-authoritative (for example, a `rev > 0` file seen once, or a one-shot `migrated` NVS flag).
- Or erase each legacy NVS key after a verified file adoption, so no frozen copy remains to resurrect.

The second is simpler and matches the cfg dual-write close owner decision.

## LOW-1: the zones undecided check runs after the Pico ceiling raise (FIXED in 703173bb2)

`firmware/KilnFW/App/drivers/http/zones_http_post.c:765-780`: the `zones_config_is_undecided()` refusal runs inside `zones_cfg_lock`, after the slow safety-ceiling raise. On refusal, `zones_post_track_ceiling_lower()` undoes the raise on a best-effort basis.

**Failure scenario:** the controller is ARMED or the link is degraded, and the user retries a POST that is refused with undecided. Each retry raises the Pico ceiling and the lowering may fail, which leaves `abs_max_temp_c` looser than the stored zone maxima.

The owner rule ("Pico ceiling same or looser") allows this outcome, so it is LOW. It is still needless link traffic and a needless window.

**Fix:** add an unlocked pre-check of `zones_config_is_undecided()` before the ceiling raise. Keep the locked re-check.

## LOW-2: the refusal text names no recovery route beyond reboot (FIXED in 703173bb2)

`zones_http_post.c:778` and `cfg_fs_refusal_http.h:65` say only "reboot the controller to retry". If the read error persists across reboots (bad sector, persistent OOM at that boot stage), the user has no stated escape:

- zones saves, firing and zones import stay blocked;
- stop and abort are unaffected.

Two recovery routes exist but are not mentioned:

- `POST /api/cfgfs/format_confirm` with `force_healthy=1` (`cfg_fs_format_http.c`).
- Factory reset `scope=kiln`, which deletes `zones.json` through `kiln_scope_cfg_files_delete`.

**Fix:** append "if this persists after a reboot, see GET /api/cfgfs; factory reset (scope kiln) or a cfg format clears it". Have `GET /api/cfgfs` name the route.

## LOW-3: web pages that show only `.error` now display a raw machine code

**FIXED (webfx6, b51ab9455).** Pages render `reason || error`.

The previous `error` text was human-readable ("could not be saved to flash"). The new 409 puts the machine code `store_unreadable_at_boot` in `error` and the human text in `reason`. `zones_page.html:3032-3064` reads `reason`. These pages render only `.error` and would show the bare code:

- `diagnostics_page.html:797`
- `kiln_configs_page.html:269/313/555`
- `live_profile_page.html:419/448/482`
- `profiles_page.html:2028/2222/2301`

**Fix:** render `reason || error` in those handlers (a shared helper in `app.js`), or keep `error` human-readable and add a separate `code` field.

## LOW-4: MCP and PcTools do not recognise the new codes or marker (FIXED in 703173bb2)

- `tools/PcTools/src/kilnctrl/mcp_server_control.py:812-825` classifies only `safety_ceiling_raise_failed` and the system-mode-gate 409. `zones_config_undecided` and `store_unreadable_at_boot` fall through to the generic `error: POST /api/zones failed: {exc}`. The text is shown, but it carries no guidance and is not distinguishable from a transport fault.
- No PcTools file reads `stale_or_unknown_stores`:
  - `backup_export` does not warn that the saved backup carries stale stores;
  - `backup_import` (firmware and tool) ignores the marker.

**Fix:**
- Add a branch for both codes that names the cause and the recovery route.
- Have `backup_export` print the marker list when non-empty.
- Have `backup_import` warn, or refuse unless confirmed, when importing a backup whose marker lists stores.

## LOW-5: narrow zones setters still change RAM while undecided (pre-existing) (FIXED in 703173bb2)

`firmware/KilnFW/App/drivers/persist/zones_config_accessors.c:49-54` (`zones_config_set_max_ramp` and siblings) apply the change through `*_no_save` and then call `nvs_save()`. While undecided, `nvs_save()` refuses with `ESP_ERR_INVALID_STATE`, so the setter returns false but RAM keeps the new value. A later firing in the same boot runs on a value the caller was told was not saved.

This predates persfx2. persfx2 closed only the HTTP POST path.

**Fix:** check `zones_config_is_undecided()` before the `_no_save` step in each saving setter, or roll back RAM on save failure.

---

## INFO

- **relay_cycles backoff** (`relay_cycles.c:1151-1196`) is sound:
  - On failure it stamps `last_persist_us` and re-sets `dirty` under a short `s_rc.lock`, so counts are never lost.
  - `relay_cycles_flush()` (1221) ignores the interval and still persists at once, with its persist_lock wait bounded at 3 s.
  - No new lock is held across the flash dispatch.
  - One cost: a failed flush at halt also pushes the next periodic persist out by 600 s. That is negligible.
- **Safe defaults** for the five retired stores are conservative:
  - aux: all disabled;
  - ramp_assist: disabled;
  - `ki_baseline`: invalid;
  - iter_tune: `zone_count` 0;
  - ct_verify: no verdict.
- **ct_verify bypasses the registry.** `ct_verify_store.c:259-271` returns early on an existing-but-unusable or unreadable file, before resolve. That store is never marked unknown or degraded, so it is missing from `GET /api/cfgfs` and the backup marker, and a later verdict save overwrites the inspected file. Its safe state (no verdict) is preserved.
- **Name lists are truncated and unescaped.**
  - `cfg_fs_status.c:189-196` lists at most 6 degraded names and `backup_export.c:247-261` at most 8.
  - Neither escapes JSON. Names are internal path constants today, so nothing is injectable, but a future dynamic name would need escaping.
  - With 40 table slots, truncation should say "+N more".
- **Backup marker lifecycle:** the marker is computed live from the registry and cleared by a clean resolve or zones load, which in practice means a reboot. Both sides are reset by the same event, so this is not an instance of the reset-one-side class.
- **Test helper resets one side only.** `pref_cfg_fs_clear_rev_unknown_for_test()` clears the `zones.json` degraded entry but leaves `s_zones_cfg_undecided` set. This is test-only, but it is exactly the reset-one-side shape, and a later test could see "not degraded" alongside "undecided".
- **Table overflow mismatch.** The pref unknown table and the degraded table could disagree on overflow, but with the current store count that is unreachable.
- **Stub returns zero.** `test_stub_cfg_fs_degraded.c` returns count 0 everywhere it is linked, so no test exercises the 409 branch of `cfg_fs_http_persist_failed()` itself. MED-2's per-store fix should come with a test that has one store degraded and another failing.

## Spot negtests

`tools/negtest.ps1` ran with `-Mutations` and three parallel copies. The command was `build_host_tests.ps1 -Only "test_zones_http\.c|test_relay_cycles\.c|test_aux_outputs_store\.c"`, with the KilnFW `-ExpectPattern`. The baseline passed, and the verdict was ALL_CAUGHT.

| Mutation | Result | First failing assertion |
|---|---|---|
| `zones_http_post.c`: `if (zones_config_is_undecided())` changed to `if (0 && ...)` | CAUGHT | `test_zones_http.c:2146` "refusal names the cause", plus 2148-2150 (config still invalid, live config untouched, generation not bumped) |
| `relay_cycles.c`: backoff stamp on failure removed | CAUGHT | `test_relay_cycles.c:1206` "the next tick does NOT retry (backoff)" |
| `pref_cfg_fs.c:475`: `nvs_valid && adopt_nvs_if_unreadable` changed to `nvs_valid` | CAUGHT | `test_aux_outputs_store.c:457` "the frozen NVS enabled rule is NOT adopted" |

The new tests catch the regressions they target. None of them covers the MED-3 branch (an absent or invalid file still adopts NVS) or the global-count 409 mapping (MED-2), because the stub returns a degraded count of 0.
