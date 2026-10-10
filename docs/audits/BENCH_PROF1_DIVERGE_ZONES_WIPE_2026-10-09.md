# Bench findings on origin/dev 8fcd3237: prof1 DIVERGED, zones wipe, z0 tuning_valid (2026-10-09)

Read-only code investigation. Source: docs/BENCH_TEST_LOG.md section "dev 8fcd3237 flash and bench queue". No board access. Paths are under firmware/KilnFW/App/drivers/.

## (a) "prof1 file/NVS DIVERGED (file rev 41, NVS rev 40)": EXPECTED / BENIGN (log noise)

Emitted at persist/profiles_cfg_fs.c:287 (file strictly higher rev, contents differ, both sides decode). The tie-break adopts the FILE, which is correct.

Why it happens on a healthy board: since the NVS dual-write close, http/profiles_http.c `nvs_save_slot_locked` (about line 1253) writes the cfg FILE only, at `s_profile_rev[id]+1`. It never touches the legacy "prof1" NVS blob or the `prof_rev` array. A slot that still has a pre-close legacy blob in NVS (rev 40, old content) and was saved once since (file rev 41) therefore always differs and always has file_rev > nvs_rev. The comment at profiles_cfg_fs.c:288-289 ("NVS resync happens on the next explicit save") is stale: saves no longer resync NVS, so the line repeats on every boot until the slot is deleted (nvs_erase_slot_locked erases the legacy key first).

The other reading, an interrupted file-first save leaving file = NVS+1, gives the same log line and the same correct outcome (file wins, nothing lost). The log line alone cannot tell them apart.

The recent rev-floor and junk-repair commits do not cause it. They act on `prof_rev` floors (profiles_http.c about 854-1000) and only raise `s_profile_rev`; they never compare content.

Verdict: expected. Proposed fix (low priority): in profiles_cfg_fs_resolve_ex's adopt-FILE branch, retire the slot's legacy NVS blob (erase and verify under the profiles save lock) and log once per slot at INFO. Do not change the adopt-file decision.

## (b) z0 tuning_valid=no after the restore: EXPECTED, not a backup defect

Backup export DOES carry the tuning record. http/backup_export.c:647-660 emits `tuning_valid` and the `tuning_*` family, but only when `tq.valid` is true. Import (http/backup_import.c:1970-2010) restores it only when `tuning_valid` is present and 1. A zone whose record was already invalid at export time has nothing to restore.

Zone 0 was already `tuning_valid=no` before this session: BENCH_TEST_LOG.md line 269 (B3: a same-value PID write flipped it to no; since fixed by `gains_changed` in persist/zones_config_accessors.c:600-625) and line 3036 ("z0 flag as read, not investigated"). Nothing re-tuned it. On import, `zones_config_set_pid_no_save` invalidates the record when gains change beyond tolerance (accessors.c:624-626), and the record comes back only if the backup carried one (`tuning_matches_pre_commit`, backup_import.c:2405-2418).

Should the backup carry it? It already does when valid. Carrying an invalid record would be wrong (a stale record pinned to changed gains is worse than none, per the accessors.c comment). `tuning_seq` is deliberately not carried (backup_export.c:642-645).

Verdict: expected. Code fix: none. Operator action: re-run autotune on zone 0. Bench log wording should say z0 was already `no` before the restore.

## (c) Zones config found wiped before the flash: cause UNKNOWN, defect class real

"Wiped" means zones count 0 / no timing profiles (BENCH_TEST_LOG.md lines 3151-3153). Every path that leaves RAM zones empty without a factory reset:

1. File undecodable AND NVS absent. persist/zones_config_cfg_fs.c:130-139 (decode rejected, "ignoring file, NVS candidate decides", output zeroed), then resolve_with_file_buf (about 290-312) `!file_valid` uses the NVS candidate, which is zeroed because NVS is legacy and absent after the dual-write close; returns `nvs_valid=false`. s_zones.cfg is all-zero for the boot (persist/zones_config_store.c:567-731). The file itself is not overwritten by the load, but any later save of the empty RAM config (POST /api/zones, backup import, kiln-config apply) would persist it over a possibly recoverable file. Ways a file is undecodable: written by a newer ZONES_CFG_VERSION and read by an older build (persist/zones_config_migrate.c:1050 ZONES_DECODE_NEWER lands in the same ignore branch), CRC or validate reject, or fewer than 5 bytes (cfg_fs.c:107).
2. cfg file missing: removed by the .tmp sweep, or cfg partition reformatted (persist/cfg_fs_mount.c deferred auto-format, or cfgfs_format_confirm). NVS empty gives the same outcome as path 1.
3. Decode or alloc OOM: store.c:565-600 and cfg_fs.c:87,128 zero the RAM config and fail the load. The rev floor is kept (415bd942) and the file is not overwritten, but the boot runs empty.
4. Failed backup import restoring an already-empty RAM snapshot (backup_import.c:2429-2497). A propagator, not a source.
5. Factory reset scope zones/all, or kiln-config apply/rollback writing a package with an empty zones section. Cannot be excluded from the log.
6. The reset writer fence (c6e70459): `cfg_save_lock_reset_refused()` only refuses saves while a reset is in flight. It can drop a save, never empty the config. Not a cause.

Most plausible given recent commits: path 1 or 2, i.e. an older firmware reading a file stamped by a newer schema, or loss of the cfg file with the NVS legacy copy already gone. The recent zones commits (ea345348 save mutex, 667578a9 snapshot under lock, f19b7c62 CRC write-back guard, 415bd942 OOM paths) reduce write paths and keep rev floors; none writes an empty config. The CRC write-back (store.c `file_side_needs_writeback`) is gated on `used_file`, so it cannot write a zeroed config. The profile rev-floor and junk-repair commits do not touch zones data. No code path was found that creates the wipe by itself; the first boot log after the wipe (not captured) would name it. Look for "zones config file ... REJECTED", "too short", "out of memory", or a cfg format line.

Verdict: cause unknown. The defect class is real: a boot that loses its only copy runs on all-zero config silently, and backup_import and load_config_preset cannot then recover it (backup_import.c:990,1338,1433 count-0 500 partial write; "no timing_profiles").

Proposed fixes:
- FIXED ab210508: On a rejected zones file, keep a copy (zones.json.bad) instead of leaving it to be overwritten, and latch a visible load fault for the cfg-file path too (store.c:366 latches only for the NVS partition).
- Let backup_import and load_config_preset seed from a count-0 config (treat it as defaults and create the timing profiles first).
- FIXED ab210508 (fix 4: ESP_LOGE naming the path; log_store holds binary firing/autotune records only, so the line reaches the UART log bridge, not a persisted store): Capture the boot log of any zones load with `trustworthy=false` in diagnostics so the next wipe is attributable.
