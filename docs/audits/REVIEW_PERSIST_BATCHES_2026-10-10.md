# Review of three persist batches on origin/dev (2026-10-10)

This is a review only. Nothing in this document has been fixed. All three batches were read at
origin/dev 1d3f7dac8.

| Batch | Commits |
| --- | --- |
| A. Persist campaign 10 (K10) | fd18342d6, c7aafdb7e |
| B. fwbatch14 (fixes for reviews 14 and 15) | ea14c8fbd, f81607868, f5c91ecde, c83c63584, e5bdb8a3c, 138975bee, f7d4b08fb, 3408a12ad, a62493c24, 1d3f7dac8 |
| C. suitefix | c41cffab6, 0248c33fe, cf3359523, 0a03e8b01, 4ec7e3bf1, 049ea2442, efab45a18, d70ee7816, 6e5e30b52, e60f5a2b5, 2f8192dd2, 1453133ce |

The review concentrated on five risks:
- data loss, or a silent fallback to defaults;
- backup import refusing a real export;
- whether persistence fails closed on an I/O error or OOM;
- whether the stack ceiling re-pins are backed by measurement;
- vacuous tests.

## Summary

| Severity | Count | Items |
| --- | --- | --- |
| MED | 1 | M1: a cfg file read error discards a valid NVS copy and later saves build on defaults |
| LOW | 3 | L1: zones config read error zeroes RAM and does not latch saves; L2: the zones page shows "Accepted." for REFUSED_NOT_WRITTEN; L3: the new zones read-error test covers only the over-size case |
| NIT | 3 | Fix-status SHAs in reviews 14 and 15 are pre-rebase; review 15 LOW-5 is mis-marked fixed; trip_seq.h comment is stale |
| INFO | several | Listed per batch below |

Nothing in these batches regresses heating safety. Every finding is about configuration that is
lost or that the operator sees misreported.

## Findings

### M1 (MED): a cfg file read error discards a valid NVS copy, and later saves build on defaults

**Where.** `pref_cfg_fs_resolve()` in `firmware/KilnFW/App/drivers/persist/pref_cfg_fs.c` (line 477
onward). This is the resolve path for every preference that has a cfg mirror.

**What happens.** When the cfg file exists but cannot be read (any `cfg_fs_read` error other than
not-found), the function zeroes `out_bytes`, sets `*out_rev = 0` and returns false. The NVS
candidate it was passed is discarded, even when `nvs_valid` is true.

The callers treat false as "nothing stored", load firmware defaults and return ESP_OK. Affected
callers:
- unit_pref
- ramp_assist
- display_power
- update_settings
- relay_names
- zone_normals
- the profiles_builtin hidden mask
- favorites
- setup_wizard
- the time_sync time zone
- aux_outputs
- the adaptive_tune ki_base
- iter_tune
- relay_cycles (which restarts at rev 0)

**Consequence.** The next save writes a blob that started from defaults, at rev 1. That save can
be an operator edit, or one of the automatic writers (adaptive_tune, iter_tune, the relay_cycles
wear counters). The real stored values are lost.

If the file stays unreadable, the NVS write lands at a low rev while the file keeps its higher
rev. A later boot that can read the file again then picks the stale file, so the edit made in
between is reverted.

**Recommendation.** On a read error that is not not-found:
- Adopt the valid NVS candidate in RAM, without writing the file.
- Mark the rev as unknown, so that saves are either refused or floored above anything the file
  could hold.

This mirrors the zones and profiles "cannot decide" handling. No test covers this caller-level
consequence; the K10 tests stop at resolve().

### L1 (LOW, borders on MED): zones config read error zeroes the whole config for the boot and does not latch saves

**Where.** 138975bee, review 15 LOW-3.

**What happens.** `load_raw_impl()` in `zones_config_cfg_fs.c` now reports
`ZONES_CFG_RESOLVE_OOM_VERSION` for any `cfg_fs_read` error except not-found. The steps after that:
1. `zones_config_cfg_fs_resolve()` zeroes `out_cfg`, keeps the rev floor at `nvs_rev`, and returns
   false.
2. `nvs_load` in `zones_config_store.c` zeroes `s_zones.cfg` and returns `ESP_ERR_NO_MEM`.
3. `zones_http.c` around line 743 logs "starting unconfigured", sets `s_zones_config_valid = false`,
   refuses zone commanding, and skips migration.

For heating, this is fail-safe. It is also disruptive: one transient I/O error discards the tuned
configuration for the whole boot, even though a valid NVS copy exists.

**The gap.** Nothing stops an operator save afterwards:
- The OOM/cannot-decide path does not call `note_load_fault`.
- `nvs_save` gates only on `kiln_cfg_swap_zone_edits_at_risk` and on the reset fence.

A save from the zones page in this state writes a near-empty config at `nvs_rev + 1`. The file
that could not be read may hold a higher rev and the real tuning, and that is overwritten or
superseded.

**Recommendation.** After a cannot-decide load, latch a load fault and refuse saves with a 409
until a reboot reads cleanly. Alternatively, adopt the NVS copy read-only. This is the same class
as M1.

### L2 (LOW): the zones page reports "Accepted." when autotune's ceiling was refused and not written

**Where.** 3408a12ad, review 15 LOW-2.

**Firmware side.** The firmware side is correct:
- `autotune_engine_guard.c` reads the ceiling back
  (`zones_config_get_max_ramp(zone,&live) && live == predicted`).
- It reports `AUTOTUNE_CEILING_REFUSED_NOT_WRITTEN` when the store refused.
- The HTTP handler emits `"REFUSED_NOT_WRITTEN"`.

**UI side.** The `msgs` map in `firmware/KilnFW/App/drivers/http/zones_page.html` (about lines
4335-4354) has no entry for that value, and falls back to `msgs[...] || 'Accepted.'`. The operator
is therefore told the result was accepted while the ceiling was not written. That is the same
silent class as the original finding, moved to the page.

No other consumer exists: the UART bridge passes `adopt_ceiling=false`, and PcTools does not read
the field.

**Recommendation.**
- Add a `REFUSED_NOT_WRITTEN` message.
- Make the fallback for an unknown value say "unknown result", not "Accepted.".

INFO: when the old ceiling already equals the predicted one, a refusal reads back as matching and
is reported as `FAILED_TO_PERSIST`. That is harmless.

### L3 (LOW): the new zones read-error test covers only the over-size case

The test added for review 15 LOW-3 (`test_zones_config_cfg_fs.c`, around line 1117) uses only a
file too large for the read buffer, which gives `ESP_ERR_INVALID_SIZE`.

Negtest result: changing `if (err != ESP_ERR_NOT_FOUND)` to `if (err == ESP_ERR_INVALID_SIZE)`
treats a transient I/O error as "absent" again, which is the defect the fix exists for. The
mutation was **MISSED**. A test with an injected generic read failure (for example ESP_FAIL) would
close this.

### NIT

**N1. Fix-status SHAs are pre-rebase.** The fix-status sections of
DEV_FIRMWARE_REVIEW_14_2026-10-09.md and DEV_FIRMWARE_REVIEW_15_2026-10-09.md cite these SHAs,
none of which is on origin/dev:
- b77c0ce94
- 19bd98965
- 283937330
- 38eaf6bcd
- f06944f45
- 3f5afcd16
- 6bda2442f
- d89acb987
- e005c1983

The commits that actually landed are the batch B list at the top of this document.

**N2. Review 15 LOW-5 is mis-marked fixed.** It is marked "FIXED in 38eaf6bcd (shared with review
14 LOW-5)", but it is a different finding: the fuzz findings document overstates its coverage for
the duplicate-index and ct_mask cases (commit 34d2c40c). No batch commit touched that document, so
it remains open.

**N3. trip_seq.h has a stale comment.** It says "link_frame.c and the host tests use this same
function", but cf3359523 removed the link_frame.c copy and link_frame.c no longer calls it. Two
older audit documents still name `link_frame_next_trip_seq`; they are historical and can stay.

## Batch A (K10): other results

- **live_profile:** fixes correct.
- **ct_verify:** fixes correct. Two INFO items:
  - The lazy start in `zones_current_sweep_task.c` never retries, and readiness then shows NEVER_RUN
    rather than "file unusable".
  - A NaN `measured_a` or `threshold_a` is stored as 0. Nothing reads those fields today, so no
    consumer can take a 0 for a measurement. A future consumer could, so a sentinel or a comment is
    advisable.
- **backup_json:** strict parsing is correct.
- **touch_cal:** fixes correct. `lvgl_port.c` lines 1203 and 1424 ignore the new error return.
  This is harmless, because `is_calibrated` reads only `.calibrated`, so a corrupt record just
  prompts recalibration.
- **test_persist_campaign10.c:** the former CHARACTERIZATION checks are now real assertions, with
  positive controls (exact fit, normal numbers, fresh board). They are not vacuous.

**Round trip against real exports.** Every one of the 40 real exports under `logs/backup_export`
imports with ok=1 once kiln_configs is stripped (14579 of 14579 checks passed). With kiln_configs
kept, the only failure was "package hash does not match". That is a host-stub artifact:
`zones_config_export_canonical` is stubbed in `test_kiln_cfg_store.c`.

The stricter parser therefore refuses no real export. The exports do include empty names and
numbers such as 9.99999975e-05. None of them contains a JSON string escape.

## Batch B (fwbatch14): other results

- **ea14c8fbd (MED-1, LOW-1, LOW-7, LOW-8): correct.**
  - The v3 crash-record migration is sound: v3 is 208 bytes, and v4 only appended fields, as
    d06bf7841 confirms (208 to 228 bytes). The test fixture relies on that append-only layout, which
    is acceptable.
  - LATE capture happens only for a dump whose ELF sha matches the running image and that has no
    record.
  - Every scope that erases kiln_nvs (kiln, all) clears the crash report before erasing, so LATE
    cannot re-mint after a reset.
  - The negtests for LOW-7 and LOW-8, which review 14 had reported as MISSED, are now CAUGHT (see
    below).
- **f5c91ecde (LOW-2/3/4/6, NIT-1/2): correct.**
  - Topology is resolved once.
  - A topology-only restore now saves; parse_only returns before the save.
  - POST /api/zones is fenced with a 409 while a restore is in flight.
  - INFO: removing the safety_link stale bound changes behaviour only at the clamp edge, where
    poll_period * UP exceeds 65534 ms.
- **c83c63584 (LOW-5): no permanent outage.** `profiles_writers_blocked()` cannot leave writers
  blocked for good, because `s_profiles_loaded` is set even when the load fails. A refusal at
  conversion commit rolls back at stage 2.
- **f7d4b08fb (review 15 LOW-1): correct.** Boot restore keeps the active id on
  `ZONES_IMPORT_REASON_RUN_CLAIMED`. The swap-rollback path is skipped, as documented.
- **e5bdb8a3c (review 15 LOW-4): correct.** The hostile fuzz shapes now carry all three gains, and
  positive controls prove that the same shape with a good value is accepted. Each refusal is
  therefore about the named field.
- **138975bee:** see L1 and L3.
- **3408a12ad:** see L2. Its test has a refusal case and a RAM-on-failure case.

## Batch C (suitefix): results

- **Stack re-pins (6e5e30b52).** These raise measured ceilings, not configured stacks. The
  headroom figures in the comments agree with the ceilings once the scripts' 300-byte unmodelled
  overhead is subtracted:

  | Task | Stack (bytes) | New ceiling (bytes) | Free (bytes) | Free (%) |
  | --- | --- | --- | --- | --- |
  | http_async_job | 10240 | 7712 | 2228 | 21.8 |
  | bx_flash_worker | 10240 | 6240 | 3700 | 36.1 |
  | system_uart_bridge | 4096 | 3152 | 644 | not given |

  The growth is small, between 16 and 80 bytes, and each is attributed to a named chain. The
  absolute measurements could not be re-derived here, because doing so needs a target-build ELF,
  which this review did not build. INFO: system_uart_bridge, at 644 bytes free, is the tightest of
  the three; it is above the 256-byte floor.
- **0248c33fe (relay OFF write result capture): correct and fail-closed.**
  - A failed OFF write leaves the mask pending, and `zone_off_pending_retry()` re-drives it every
    tick.
  - The new test asserts that the mask stays pending across two failed retries and clears once a
    write lands. Negtest CAUGHT.
  - INFO: the retry path logs ESP_LOGE on every failing tick, without the de-duplication that
    `apply_relay()` has. A wedged owner therefore floods the log.
- **1453133ce and 0248c33fe (allowlist additions).** The `check_relay_authority_paths.py` entries
  for `guard9_prelock_check` and `relay_unknown_prelock_check` are both lock-free, OFF-only cuts on
  the watchdog task. They have the same justification as the existing `watchdog_task_entry` entry.
  The check passes.
- **1453133ce (danger_mode re-entry): correct.** The result of releasing the enable request is now
  checked and logged.
  - On failure, the local flag is cleared while the Pico's request may still be set.
  - That disagreement is bounded: `danger_mode_stop()` and the timeout exit send an unconditional
    release.
  - INFO only.
- **cf3359523 and 2f8192dd2 (trip_seq.h): correct.** The wrap rule (255 goes to 1, never to 0) is
  unchanged, and no caller of the old name remains in firmware or tests. See N3.
- **0a03e8b01 (backup_import scratch).** `persist_scratch_alloc()` behaves exactly like the code it
  replaces: SPIRAM first, then an internal fallback.
  - Every early-return path frees what it allocated.
  - OOM still refuses with "nothing was changed" in parse-only mode.
  - The persist scratch check passes with the two allowlist entries removed.
- **c41cffab6 (ramp mirror).**
  - The mirror now carries `exec_elapsed_accumulate()`, and the drift check compares its body with
    production's.
  - Negtest: dropping the remainder carry in production is CAUGHT by the drift check.
  - The sub-second test exercises the mirror, which the drift check now binds.
- **4ec7e3bf1 (test_danger_mode):** a mechanical CHK-to-CHECK rename, so the vacuity check can see
  the assertions.
- **efab45a18 (baseline message):** message text only, and the test regex was updated with it.
- **049ea2442, d70ee7816, e60f5a2b5:** documentation, the submodule pointer and citation refreshes.
  Not reviewed further.

## Negative tests run

All were run with `tools/negtest.ps1` in throwaway worktrees. In every run the baseline passed, the
real tree was unchanged, and the copies were removed.

The firmware rows ran `build_host_tests.ps1` with `-Only` set to test_ota_http,
test_autotune_engine_prestart, test_zones_config_cfg_fs and test_profile_executor_prestart.

| Mutation | Test | Result |
| --- | --- | --- |
| factory_reset.c: "all" row `clear_crash_report` true to false (review 14 LOW-7) | kilnfw-host (4 exes) | CAUGHT |
| elf_archive.py: `len(p) < 6` to `len(p) < 0` (review 14 LOW-8) | pytest test_crash_elf_recovery_archive, test_elf_archive | CAUGHT |
| autotune_engine_guard.c: `if (!live_in_ram)` to `if (!live_in_ram && false)` (review 15 LOW-2) | kilnfw-host | CAUGHT |
| zones_config_cfg_fs.c: `if (err != ESP_ERR_NOT_FOUND)` to `if (0)` (review 15 LOW-3) | kilnfw-host | CAUGHT |
| zones_config_cfg_fs.c: same line to `if (err == ESP_ERR_INVALID_SIZE)` | kilnfw-host | **MISSED** (L3) |
| profile_executor_relay_io.c: retry forces `off_err = ESP_OK` (0248c33fe) | kilnfw-host | CAUGHT |
| profile_executor.c: `*rem_ms = 0` in `exec_elapsed_accumulate` (c41cffab6) | ramp_stepping_gate_mirror_drift_check.py | CAUGHT |

Checks run directly, all passing:
- `ramp_stepping_gate_mirror_drift_check.py`
- `check_relay_authority_paths.py`
- `check_persist_scratch_malloc_caps.ps1`
