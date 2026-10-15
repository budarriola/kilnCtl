# Audit: backup export/import (KilnFW)

HIGH 0, MED 1, LOW 3, INFO 4

Scope: `GET /api/backup/export` (`backup_export.c`) and `POST /api/backup/import`
(`backup_import.c`, `backup_json.c`, `backup_http.c`, `backup_restore_state.h`) on
origin/dev d5bbf13fc. The audit was code-only. The board was not touched. Coverage
gaps already listed in `BACKUP_CFGFS_COVERAGE_AUDIT_2026-10-09.md` are not repeated
here, and neither are the heap findings in `INTERNAL_HEAP_MARGIN_2026-10-09.md`
section 3.

Verified sound, no finding:

- **Parser bounds.** `backup_json.c` validates the whole document with a state machine
  (depth 32), uses a strict number grammar, and `field_str` fails on overflow.
  `BACKUP_BODY_MAX` is 131072 and the body is held in PSRAM.
- **Field ranges.** Every zone field is checked against the same constants its live
  setter uses, in pass 1, before any write: PID, model, tc_type, name, the masks, cal,
  max_ramp, sanity, control_mode, the temperature limits, heater timing, the 8 guards,
  cross_zone, fuzzy and coupling. As an example, max_temp/min_temp match
  `zones_config_set_temp_limits_no_save` (`zones_config_accessors.c:1726-1730`) and
  `zones_http_post_parse.c:547-552`.
- **Mode gate.** The mode gate and the OTA interlock run in the handler and again in
  the job. The factory-reset mark is re-checked in the job.
  `s_backup_restore_in_flight` fences the run starters, `POST /api/zones`
  (`zones_http_post.c:235`) and factory reset (`factory_reset.c:477`).
- **Rollback.** The zone batch commits from a whole-`zones_cfg_t` snapshot
  (`backup_import.c:2479`), and every setter failure restores that snapshot before
  returning.
- **Pico ceiling.** A raise goes through `safety_ceiling_sync_guard_raise` before any
  zone field commits (`:2357`). `backup_import_track_ceiling_lower()` runs on every exit
  (`:4115`). It only tightens the Pico down to the live ESP max, never below it, so the
  rule "Pico ceiling never tighter" holds on success, on refusal and on rollback.
- **Export.** The stream buffer and all scratch are allocated in PSRAM up front, and an
  OOM returns a clean 500 before the first byte is sent.

## Findings

### MED-1: profile ramp-feasibility pre-check reads the live zone ceiling, not the backup's

- **Evidence.** In pass 1a, `backup_import.c:1186-1205` checks each ramped segment
  against `zones_config_get_max_ramp(zi, &ceiling)`, which reads the live config.
  - That getter returns false for `zi >= thermo_count` (`zones_config_accessors.c:21`),
    and the caller then leaves `ceiling` at 0.
  - The same file parses the backup's own `zones[].max_ramp_c_per_hr` at `:1563-1570`
    and commits it at `:2578`.
  - Pass 2 commits zones (`:3010`) before `profiles_http_save()` (`:3040`). That save
    repeats the check against the now-imported ceiling (`profiles_http.c:1949-1960`).
  - `backup_import_profiles_precheck()` models the post-import zone_type, relay_mask and
    aux state, but not max_ramp.
  - The profile `zone_mask` check at `:1107` already uses the resolved backup topology
    (`:1044`). The ramp check therefore disagrees with the check right next to it.
- **Scenario A (400, restore impossible).** Factory-reset a board, then upload a v7
  one-file backup (topology plus zones plus profiles):
  1. `thermo_count` is 0, so every `get_max_ramp` call returns false.
  2. Every ceiling reads as 0.
  3. Any profile with a ramped segment is refused with "exceeds zone 0's 0.0 C/hr
     ceiling".

  This defeats the one-file restore. The same refusal happens on a configured board when
  the backup raises a zone's `max_ramp` above the live value and holds a profile that
  needs the higher value.

  The workaround leaves the board misconfigured. The operator must first restore a copy
  with the profiles removed, then restore again.
- **Scenario B (500, partial write).** The backup lowers a zone's `max_ramp` below one of
  its own profiles' rates, and the live ceiling is still high:
  1. Pass 1 passes.
  2. kiln_configs, zones, update_repo, aux, relay_cycles and prefs commit.
  3. `profiles_http_save` then refuses at `:3040`, which returns a 500 with
     `X-Kiln-Partial-Write`.

  An export taken from a consistent board cannot produce this, because the live save path
  enforces the rule there too. A hand-edited or converted file, or an older backup whose
  profiles were later retuned, can. The point of pass 1 is to refuse this as a clean 400.
- **Fix.** Build a per-zone post-import ceiling before the 1a loop:
  - Use the candidate's `max_ramp_c_per_hr` when `has_ramp` is set.
  - Otherwise use the slot value when `topo.apply` is set, the way `:2345` does for
    `max_temp`.
  - Otherwise use the live value.

  Then check against that array. The check needs zone candidates, so either move the
  ramp check after the zone parse (`:1400+`), or fold it into
  `backup_import_profiles_precheck()`, which already builds a post-import view.
- **Host test.** None. `test_backup_import.c:892` stubs every zone's ceiling at 1000, so
  no test has a backup ceiling that differs from the live one. Needed tests:
  - v7 onto an unconfigured board with a ramped profile: expect 200.
  - A backup that lowers `max_ramp` below its own profile: expect 400 with no write.

### LOW-1: version check truncates a non-integer and casts an unbounded double

- **Evidence.** `backup_import.c:1030` checks `(int)dver < MIN || (int)dver > VERSION`.
- **Scenario.**
  - `"version":7.9` is accepted as 7.
  - `"version":1e300` converts a double that is out of `int` range, which is undefined
    behavior in C. On Xtensa GCC it saturates, so it is refused in practice.
- **Fix.** Compare the double directly and require an integer:
  `dver != floor(dver) || dver < MIN || dver > VERSION`.
- **Host test.** None for a fractional or huge version.

### LOW-2: dry-run plan cap of 32 lines silently drops the later lines

- **Evidence.**
  - `kiln_cfg_plan_add` (`backup_import.c:397-409`) drops lines past
    `KILN_CFG_PLAN_MAX_LINES` 32. Its comment budgets 30 lines for kiln_configs alone.
  - The plan has since gained other lines:
    - the topology line (`:4159`)
    - update_repo (`:3134/3136`)
    - up to one line per relay for relay_cycles "live count kept" (`:3194`)
    - prefs (`:3408`)
    - aux_outputs (`:3604/3654`)
- **Scenario.** A board with 10 kiln configs is restored in MIRROR mode against a file
  carrying 10 different ones. That produces 30 kiln lines, plus topology and update_repo,
  which fills the cap. The relay_cycles, prefs and aux lines never appear in the confirm
  dialog, but those changes are still committed. The operator confirms changes the
  dialog did not show.
- **Fix.** Raise the cap to cover every producer. Better, add a final "... N more lines
  not shown" line, so a dropped line is never silent.
- **Host test.** None at the cap.

### LOW-3: profile, aux and update-settings writers are not fenced against an import in flight

- **Evidence.**
  - Only `zones_http_post.c:235`, `factory_reset.c:477`, `zone_aux_convert_http.c` and
    the run starters check `backup_import_restore_in_flight()`.
  - `POST /api/profile` and the other `profiles_http_save()` callers have no such check:
    `profiles_edit_http.c`, `profiles_export_http.c` and `profiles_live_http.c`.
  - The aux and update-settings POST routes have no check either.
  - The import's own pass-1 slot and name simulation (`:1214-1320`) assumes the profile
    store does not change between pass 1 and pass 2. Pass 2's comment at `:3043` admits
    the gap ("a race with a concurrent change").
- **Scenario.** A second browser tab saves a profile while an import's Pico round trips
  are running, which takes seconds. Either outcome follows:
  - The import's simulated slot is taken, or the name now collides, so the commit
    refuses mid-loop and the import returns a 500 partial write.
  - The import overwrites the other tab's save.
- **Fix.** Add the same `backup_import_restore_in_flight()` 409 refusal to
  `profile_post_handler`, the profile delete, edit, import and live decide routes, and the
  aux and update-settings POST routes. That is the review 14 LOW-3 pattern already used
  for `/api/zones`.
- **Host test.** None.

## INFO

- **INFO-1: export is not fenced against an import in flight.**
  `backup_export_get_handler` (`backup_export.c:319`) reads stores one field at a time
  with no `backup_import_restore_in_flight()` check. An export taken during an import can
  mix pre-import and post-import state. It is harmless unless that file is later
  restored as a "known good" backup. A 409 while an import is in flight would close it.
- **INFO-2: duplicate keys inside nested objects resolve silently.** Duplicate-key
  refusal is top-level only (`backup_json.c`). Inside a zone or profile entry,
  `obj_find` returns the first occurrence. That matches the first-wins convention every
  other parser here uses (HOST_TEST_CAMPAIGN_FINDINGS), so it is not a defect. It is
  noted because a converted file could carry both spellings.
- **INFO-3: older backups after a GitHub update.**
  - Versions 1-7 are accepted. A missing key preserves the live value (omit-preserves).
  - A backup from a newer format is refused cleanly with a 400 naming the supported
    range.
  - v1-v6 carry no topology, so on a factory-reset board every zone entry is refused at
    `:1404` until thermo_count and relay_count are set by hand. The message names the
    settings page, so this is expected.
  - MED-1 also hits these versions: an older backup's profiles are checked against
    whatever ceilings the board has at restore time.
- **INFO-4: the `ok_json` response cannot overflow.** `ok_json[96]` (`:4378`) holds the
  worst-case `relay_cycles_kept` list for 8 relays (about 50 characters).
  `s_rc_kept_mask` is reset at the start of every commit (`:3160`), so a value from an
  earlier import cannot be reported.

## Test gaps

1. A backup ceiling that differs from the live one, both raised and lowered, and a v7
   restore onto an unconfigured board with ramped profiles (MED-1).
2. A fractional or out-of-range `version` (LOW-1).
3. A dry-run plan with more than 32 lines: the cap behavior and a truncation marker
   (LOW-2).
4. A profile save, aux POST or update-settings POST while `s_backup_restore_in_flight`
   is set, expecting 409 (LOW-3).
