# Backup / import / factory reset / config_convert coverage of every cfgfs row (2026-10-09)

Read-only audit. Paths are under `firmware/KilnFW/App/drivers/` unless noted.
Rows = the 15 items `/api/cfgfs` lists (`http/diagnostics_http.c:1557-1689`).
Owner position (memory): only the Wi-Fi password should be irreducible from a backup.

Columns: EXP = written by `http/backup_export.c`; IMP = restored by `http/backup_import.c`;
RESET = cleared by factory reset (`http/factory_reset.c` `kScopes[]` at :127 with
`kiln_scope_cfg_files_list` `persist/kiln_scope_cfg_files.c:21-35` for scope kiln,
`profiles_scope_cfg_files_delete` `persist/profiles_scope_cfg_files.c:64` for scope profiles,
cfg format `factory_reset.c:~280` for scope all); CONV = PC `tools/PcTools/src/kilnctrl/config_convert.py`.

| cfgfs row (diag line) | cfg file (define) | EXP | IMP | RESET kiln / profiles / all / wifi | CONV |
|---|---|---|---|---|---|
| zones (:1557) | zones.json (`persist/zones_config_cfg_fs.h:63`) | yes, `zones[]` (`backup_export.c:491-607`) | yes (`backup_import.c:1727` etc.) | kiln yes (kscf list) / no / all via format / no | yes: zones_blob (`config_convert.py:~979`), backup via `cfg_convert` (:1486) |
| kiln_cfg_store (:1564) | kiln_configs.json (`persist/kiln_cfg_store_cfg_fs.h:78`) | yes `kiln_configs` (`backup_export.c:756`) | yes (`backup_import.c:354`) | kiln yes / no / all / no | yes: kiln_package (`config_convert.py:222`) |
| unit_pref (:1570) | unit_pref.dat (`persist/unit_pref.h:43`) | NO | NO | kiln yes / no / all / no | NO |
| profiles_hidden (:1576) | profiles/hidden.json (`persist/profiles_builtin.c:49`) | NO | NO | profiles yes (`factory_reset.c:229-235`) / all yes / kiln no | NO |
| zone_normals (:1582) | zone_normals.dat (`persist/zones_http_internal.h:273`) | yes `normal_current_a` (`backup_export.c:592`) | yes (`backup_import.c:1839`) | kiln yes / all | no (not in convert) |
| ramp_assist (:1588) | ramp_assist.dat (`control/ramp_assist_cfg.h:58`) | NO | NO | kiln yes / all | NO |
| display_power (:1594) | display_power.dat (`persist/display_power_cfg.h:65`) | NO | NO | kiln yes / all | NO |
| update_repo (:1600) | update_repo.dat (`update/update_settings.h:55`) | yes (`backup_export.c:798`) | yes (`backup_import.c:2808`) | kiln yes / all | NO |
| tz (:1606) | tz.dat (`net/time_sync.h:43`) | NO | NO | kiln yes / all | NO |
| profiles (:1650) | profiles/*.bin slots | yes `profiles[]` (`backup_export.c:285`) | yes | profiles yes / all | yes profile_blob (:246-500) |
| relay_cycles (:1662) | relay_cycles.dat (`persist/relay_cycles.h:34`) | NO (counters; `relay_type` per zone is exported, `backup_export.c:491`) | NO (only relay type via zone, `backup_import.c:2498` comment) | kiln yes / all | NO |
| adaptive_tune (:1669) | ki_base.dat (`control/adaptive_tune.h:56`) | partial: `autotune_baseline_k_dc`, `adaptive_tune_enabled` in zones (`backup_export.c:503-550`); the Ki baseline file itself NO | partial | kiln yes / all | NO |
| firing_stats (:1676) | firing history files (`persist/firing_stats_cfg_fs.c:48`) | NO | NO | profiles yes / all (history is operator data, arguably fine to lose) | NO |
| relay_names (:1683) | relay_names.dat (`persist/zones_http_internal.h:163`) | NO | NO | kiln yes / all | NO |
| aux_outputs (:1689) | aux_out.dat (`persist/aux_outputs_cfg.h:46`) | yes (`backup_export.c:210`) | yes (`backup_import.c:2868`) | kiln yes / all | NO |
| (not a cfgfs row) iter_tune | iter_tune.bin (`persist/iter_tune_store.h:44`) | NO | NO | kiln yes / all | NO |

Wi-Fi scope clears only `wifi_nvs` plus `esp_wifi_restore()` (`factory_reset.c:~245-265`); it touches no cfg file, which is correct.
Reset coverage is complete: every cfg file above appears in `kKilnScopeFiles` or the profiles scope. No reset gap found.

## Recorded greps (absence evidence), run at origin/dev 90b75e27
- `grep -n "unit_pref|profiles_hidden|profiles_builtin|ramp_assist|display_power|tz\b|timezone|relay_cycles|firing_stats|relay_names" http/backup_export.c` returned only comments and include lines for zone_normals/adaptive_tune/relay_type; no unit_pref, hidden, ramp_assist, display_power, tz, relay_names, firing_stats emission.
- Same pattern on `http/backup_import.c`: only `relay_cycles_set_type` comment (:2498); no restore call for any of those.
- `grep -n "unit_pref|relay_names|ramp_assist|display_power|\btz\b|firing_stats|aux_outputs|update_repo|hidden|relay_cycles" tools/PcTools/src/kilnctrl/config_convert.py` returned nothing; `detect_kind` (:207-232) knows only backup, profile_blob, safety_config_blob, zones_blob, kiln_package.
- `grep -n "iter_tune" http/backup_*.c` returned nothing.

## Ranked gaps
1. Backup omits six operator preferences: unit_pref, profiles_hidden, ramp_assist, display_power, tz, relay_names. All are reset by the kiln/profiles/all scopes, so a reset followed by restore silently returns defaults, contradicting the "only Wi-Fi password irreducible" rule. Fix: add top-level keys (`unit`, `hidden_builtin_profiles`, `ramp_assist`, `display_power`, `tz`, `relay_names`) via small NOINLINE helpers like `backup_export_aux_outputs`; absent key = preserve on import. Setters exist: `unit_pref_set`, `profiles_builtin_set_hidden`, `ramp_assist_cfg_set_enabled`, `display_power_cfg_set`, `time_sync_set_tz`. Files: `http/backup_export.c`, `http/backup_import.c`, `http/backup_http_internal.h`, `http/backup_page.html` (dry-run listing).
2. relay_names has no setter outside zones plumbing grep hit in http (setter lives in persist, see `persist/zones_http_internal.h:163`); needs a validated import path with the same length/charset rules as the live POST. Highest user value after item 1 because labels are typed by hand. Files: `http/backup_import.c`, `persist/zones_http_internal.h`.
3. iter_tune.bin and the adaptive-tune Ki baseline file are erased by the kiln scope but not exported (tuning results the operator paid heat time for). Fix: export under zones or a new `tuning` key; or document as intentionally derived. Files: `http/backup_export.c`, `http/backup_import.c`, `persist/iter_tune_store.c`.
4. relay_cycles counters (wear tracking) are erased by kiln reset and not exported. Decide: either export (`counts` per relay) or declare non-restorable in docs. Files: `http/backup_export.c`, `docs/` note.
5. config_convert.py cannot convert or even recognize the new preference keys, and `aux_outputs`/`update_repo` already in backups are not covered by `backup_cfg_convert`; once gap 1 lands, version the backup and extend `tools/PcTools/src/kilnctrl/cfg_convert.py` plus `tools/check_config_convert_mirror.py`. Lower priority (additive optional keys convert as pass-through). Files: `tools/PcTools/src/kilnctrl/config_convert.py`, `cfg_convert.py`.
Minor: firing_stats history is not exported; reasonable to leave out (history, not configuration) but record the decision.
