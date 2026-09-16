# Adversarial review: kiln-profiles feature (2026-09-15)

Scope: the feature as a whole, not one diff. Anchor commits `ef3500b1`
(upload/download JSON envelope, HTTP wiring, auto-save), `44fab4c1` (route the
kiln-config swap's Pico push through `APPLY_CONFIG_VOLATILE` 0x2D), `ddde0b4c`
(SaftyFW ARMED-loosening carve-out; backstop distinguishes a failed read from
unconfigured).

Everything below was checked against code, against the ELF, or by executing a
test. Where something could not be verified, it says so.

## Verdict against the owner's requirements

| Requirement | Status |
| --- | --- |
| Up to 10 complete configs, no re-entry by hand | Complete. `KILN_CFG_MAX_COUNT 10`; slot 11 is refused with an explicit "kiln config store is full". |
| Name, add, remove, select from a dropdown | Complete (`/api/kiln_configs` plus `/apply` `/save` `/clone` `/rename` `/delete`, wired in `main_page.html`). |
| Download and upload | **Partial.** The endpoints and the JSON envelope exist and are strict; there is no Download or Upload control anywhere in `main_page.html`, so the owner cannot reach the feature from the UI. |
| Each config packages both processors, swapping as a unit | Complete. `kiln_cfg_store_get_full_package()` refuses a half-package; `kiln_cfg_swap_apply()` is a 13-step transaction with a persisted stage record and boot recovery. |
| "If a config doesn't land and match on both sides then alarm and disable heaters" | **Partial.** Complete at swap time; the standing detector is ceiling-only. See Defect 2. |
| Upload verifies validity and compatibility; matching = format version + a hash over the set | Mostly complete; missing the import-time compatibility rules of plan section 5.3. See Defect 5. |
| Changing the current config auto-saves to the current slot and recalculates the hash | **Partial, and wrong during a swap.** See Defect 1. |
| Configs live in ESP flash, run out of RAM on both processors | Complete. NVS (`kilncfgs`/`kilncfgrv`) plus the `cfg` LittleFS dual-write; the Pico half installs volatile via 0x2D. |
| "Make the config sharing and changing robust" | Largely yes inside the transaction. Every weakness found is outside it: auto-save policy, the standing divergence check, and the absent UI. |

## Hazards checked

1. **Flash-worker re-entrancy.** The lint is green on a justification phrase
   that is factually false. `nvs_save()` *is* reachable on-worker:
   `coupling_persist_job` (`autotune_engine_step_identify.c:525`) ->
   `zones_config_set_coupling_cell()` -> `nvs_save()`, and
   `control_handle_message` (`uart_bridge_ext_control.c:186`) ->
   `CONTROL_CMD_SET_ZONE_PID`/`SET_ZONE_MODEL` -> `nvs_save()`. It is not a
   deadlock only because `bx_run_on_internal_stack()`'s task-identity backstop
   silently runs a re-entrant job inline -- which the comment never cites, and
   which nests the ~3.5 KB `zones_autosave_job` frame inside whatever on-worker
   frame is already there (worst measured nesting ~4256 B of the 8192 B
   `bx_flash_worker` stack). That nesting is invisible to
   `check_all_task_stack_budgets.py`, which grades the worker INDETERMINATE at a
   48 B ceiling because its body is an indirect `callx`. Not an overflow today;
   a false justification plus unmeasured nesting. Reported, not fixed -- see
   "Fix attempted and rejected" below, which is itself the strongest evidence
   that the situation is real.
2. **httpd shared stack.** Verified against the ELF, not the commit message.
   `check_httpd_task_stack_budget.ps1` OK. Deepest path is the pre-existing
   `cfgfs_status_get_handler` at 4304 B; honest free 2088 B (25.5%, LOW).
   `export_get_handler` 2464 B and `import_post_handler` 1568 B are not in the
   top five, so "no shared-stack growth" is loose wording (export does carry
   ~1.6 KB of locals) but the measurement is unaffected.
3. **PSRAM-stacked task + NVS.** Clear. `bx_flash_worker` uses plain
   `xTaskCreatePinnedToCore` (internal SRAM), explicitly commented as not the
   `*WithCaps` variant; `kiln_cfg_swap.c`'s `save_pending()` additionally has an
   `hal_kv_write_safe_here()` PSRAM backstop.
4. **NVS 15-char key cap.** Clear. `NVS_KEY_LEN_CHECK` on every new key;
   `check_nvs_key_length.ps1` PASS.
5. **0x2D silent failure.** Closed *at swap time*: `push_and_verify_pico()`
   forces `safety_cfg_store_refetch(link, 0)` (0 = unconditional, never a cached
   echo) and compares every param field by field, so a pre-15 Pico that discards
   0x2D is detected. Not closed afterwards -- Defect 2.
6. **`abs_max_temp_c` parity / never-unarmed.** The swap raises the ceiling first
   (`safety_cfg_http_set_and_confirm_f32_volatile`) and never lowers it inside
   the transaction, and `kiln_cfg_swap_boot_recover()` handles
   NONE/STAGED/PICO_OPEN/PICO_DONE/ESP_DONE plus corrupt-CRC and unreadable
   records without ever clearing evidence. No path was found that leaves the
   Pico unarmed across a reboot or a link drop mid-swap. The residual is that a
   Pico reboot *after* a successful swap silently reverts to its flashed record
   (step 13's flash fallback is expected to fail while ARMED) -- Defect 2.
7. **Consumer-without-producer / bypassed-owner.** Not systematically swept.
   Stated as unverified.
8. **Schema versioning / `zones_cfg` rollback.** Clean. The load path
   *quarantines* rather than overwrites on wrong-size, older-without-migration,
   and newer-than-firmware blobs; the v1->v2->v3 migration chain is
   heap-allocated. Does not worsen the known rollback hazard.

## Defects found

**1. The auto-save writes the incoming config into the outgoing kiln's slot.**
`zones_config_import_blob()` ends in `nvs_save()`, which dispatches the kiln
auto-save. In `kiln_cfg_swap_apply()` step 8 the ESP half is imported while
`active_id` is still the OLD kiln -- it only moves to `target_id` at step 12 --
so the auto-save writes the incoming config over the outgoing kiln's saved slot.
The same applies to `rollback()`'s import. Plan section 2.4 rule 5 (suppress the
auto-save during the apply transaction) and rule 6 (suppress it while
CONFIG_DIVERGENCE is latched) are not implemented; rule 4 (5 s / 60 s debounce)
and rule 3 (read back before recomputing `pkg_hash`) are also absent. Reported,
not fixed: the fix is a policy decision about where the suppression flag lives
and who owns it.

**2. The standing divergence check compares one field.**
`safety_ceiling_sync.c` calls `config_divergence_check(esp_fields, pico_fields,
1, ...)` -- `abs_max_temp_c` only. After a successful swap the Pico holds the
config in RAM only; a Pico reboot reverts all ~60 params to its flashed record,
and if the two profiles happen to share a ceiling **nothing detects it**.
`44fab4c1`'s commit message claims this case "is still caught by the existing
ceiling/arming divergence check" -- true only when the ceilings differ. This is
the gap against the owner's "doesn't land and match on both sides then alarm and
disable heaters" as literally stated. The correct fix is to compare package
identity (`pkg_schema` + `pkg_hash`) on the standing path, not one float.

**3. The auto-save hook had zero test coverage.** Proven, not inferred: with the
dispatch replaced by `esp_err_t autosave_dispatch_err = ESP_OK;
(void)zones_autosave_job;`, all 45 host-test executables still passed.
`g_stub_autosave_called` existed in `test_zones_http.c` and was never asserted
anywhere. **Fixed** -- see Fixes.

**4. Import reports success when the store write fails.** `kiln_cfg_store`'s
import path returns success even when `nvs_save_store()` fails, leaving the slot
in RAM only; it disappears at the next boot with no warning. Reported.

**5. Import omits plan section 5.3's compatibility rules.** No forcing of
`calibrated=false` / unsetting `i_normal_a[]` for a foreign `source_board`, no
`abs_max_temp_c >= max zone max_temp_c` rule, no `ack_hardware_differs`.
Reported.

**Closed 2026-09-16.** All three named gaps are now implemented in
`kiln_cfg_store.c`'s import path, plus one owner-approved extension beyond
what this defect originally asked for:

- Foreign-`source_board` reset (`ct_cal[].calibrated` forced `SET`+`false`,
  `i_normal_a[]` forced `UNSET`) at param ids 0x0316/0x0317/0x0318 and
  0x031A/0x031B/0x031C. `foreign` is `!has_source_board ||
  (source_board_id != kiln_board_identity_get())` -- an OLDER package with no
  `source_board_id` field at all is treated as foreign too, fail-safe by
  construction, not merely by convention. Covered by
  `test_import_cross_board_forces_calibration_reset()` (cross-board triggers
  the reset), `test_import_matching_board_preserves_calibration()` (same
  board does not), and
  `test_import_absent_source_board_id_forces_calibration_reset()` (an
  absent field is treated as foreign, never as "trust it").
- `abs_max_temp_c >= max zone max_temp_c` at import time was already landed
  in an earlier pass (`test_import_accepts_abs_max_temp_c_at_or_above_zone_max`).
  **New this pass:** the same ceiling is now re-checked a SECOND time, at
  **apply** time, against the controller's LIVE `safety_cfg_store` cache --
  not only the package's own captured-at-save-time value already checked at
  import. This closes a real gap: nothing previously re-verified an applied
  package's zones against what the Pico is actually configured for right
  now, only against what the package itself claimed when it was saved. It
  is inert today (`kiln_cfg_store_apply()` pushes no Pico half yet, so there
  is nothing live to fall out of step with), and skips (does not refuse)
  when the live cache has no `abs_max_temp_c` SET yet -- "not yet
  commissioned" is not "unsafe", same convention the ack-gate below uses.
  Added now, while still inert, specifically so it is not a missing gate
  discovered under pressure the day `apply()` starts pushing a Pico half.
  See `docs/KILN_PROFILES_PLAN.md` section 5.3's new row. Covered by
  `test_apply_refuses_live_ceiling_tighter_than_zone_max()` (refuses even
  WITH `ack_hardware_differs=true` -- no ack bypasses a ceiling check) and
  `test_apply_skips_live_ceiling_check_when_live_unset()` (non-regression:
  an uncommissioned board is not refused).
- `ack_hardware_differs` gate (`apply_hardware_differs()`, param ids
  0x0109/0x031F/0x0211 -- `ct_installed`/`ct_topology`/`safety_tc_installed`
  only) refuses `kiln_cfg_store_apply()` on a hardware mismatch unless the
  caller explicitly acks it. **Stated loudly, per the original task
  brief:** this gate's scope is those three ids ONLY. `relay_count` and
  `thermo_count` are NOT covered by it and never will be by this gate --
  they are separately hard-refused at import time, unconditionally, with no
  ack path at all, because a relay/thermocouple COUNT mismatch is a wiring
  fact no acknowledgement can make safe to import. Covered by
  `test_apply_refuses_hardware_mismatch_without_ack()` and
  `test_apply_allows_hardware_mismatch_with_ack()`.

Section 5.3's `ct_cal[].gain`/`.offset`/`k_ct_v_per_a[]` row was already
closed in an earlier pass (`calibrated` forced false cross-board, above).
The `a_fs`/`zero_mv` range-check half of that same row is recorded in
`docs/KILN_PROFILES_PLAN.md` as **unimplementable as written**: those two
fields are plain ESP-side `safety_cfg_store` fields, never Pico
`CONFIG_PARAM_TABLE` entries, so they are not present in `kiln_pkg_safety_t`
and never travel in the package at all -- there is nothing to range-check
on import.

Bears on the Pico-arming invariant as follows. The board-id reset and the
apply-time live-ceiling recheck both exist to keep the Pico's
`abs_max_temp_c` from ever silently drifting looser than what the kiln
actually needs (a foreign calibration, or a since-changed live ceiling,
laundered through an import/apply that nobody re-checked) -- neither can
make the Pico's ceiling tighter than the ESP's either, since both are pure
refusals, never writers of a new ceiling value. The `ack_hardware_differs`
gate never touches `abs_max_temp_c` at all (0x0109/0x031F/0x0211 only) and
does not bypass the live-ceiling recheck (proven by
`test_apply_refuses_live_ceiling_tighter_than_zone_max` passing `ack=true`
and still refusing) -- so no combination of these changes creates a path
where the Pico ends up unarmed or carrying a ceiling that disagrees with
the ESP's. `kiln_cfg_store_apply()` still pushes nothing to the Pico, so
none of this is reachable in production yet; all of it is validated against
a heap-allocated candidate `zones_cfg_t` (never stack), consistent with this
codebase's httpd-reachable-path stack discipline.

**6. No Download/Upload control in the web UI.** Reported (see the table).

## What held up

- Import refusals are genuinely non-partial -- **executed, not read**. Breaking
  the hash check (`kiln_cfg_store.c` ~1678) produced a real
  `FAIL test_kiln_cfg_store.c:1817`; zeroing a canonical serialization byte
  (`kiln_package.c:153`) produced real `FAIL test_kiln_package.c:251` and `:299`.
  Both restored by hand and re-proven with a forced full rebuild.
- `kiln_package_compute_hash()` refuses rather than truncates when a package
  exceeds its 4096 B scratch. The hash is over the canonical binary
  serialization, never the JSON text, so reformatting cannot change it, and the
  value the ESP computes is the value both sides' views are compared against.
- `kiln_package_import_json()` is strict: wrong `kind`, `pkg_schema` newer than
  `KILN_PKG_SCHEMA_VERSION` or 0, missing name, bad `esp_blob_len`,
  non-hex or length-mismatched `esp_blob_hex`, missing `pico` section, unknown
  param types, and `count >= KILN_PKG_SAFETY_PARAM_CAP` bounded *before* the
  write to `entries[count]`.
- LCD: `ui_page_home_refresh.c` grew `status_buf` 64 -> 96 and appends
  `"  Kiln: %s"` with `snprintf(status_buf + used, sizeof(status_buf) - used,
  ...)`. Bounded; existing content cannot be overflowed or truncated -- the kiln
  name is what gets ellipsized (`LV_LABEL_LONG_DOT`). 480x320 landscape, no
  scrolling, no new colours.

## Fixes made in this pass

`firmware/KilnFW/App/test/test_zones_http.c` -- added
`test_nvs_save_dispatches_the_kiln_config_autosave()`, covering that the
dispatch happens at all and that a *refused* auto-save does not fail the
zones-config save that triggered it. **Negative-tested**: with the dispatch
removed the new test fails at `test_zones_http.c:2033` and `:2041`; production
code was then restored by hand (empty `git diff`) and all 45 executables were
force-rebuilt from a deleted build directory before the final measurement.

Everything else above is reported, not fixed.

## Fix attempted and rejected (hazard 1)

The obvious fix for hazard 1 -- replace the false comment and add the standard
`if (uart_bridge_ext_is_on_flash_worker()) { zones_autosave_job(NULL); }` guard
used by `adaptive_tune.c`, `cfg_fs_mount.c` and `factory_reset.c` -- was
implemented, built, and **reverted**, because making the inline call explicit
makes it visible to the static stack-depth checks and immediately fails two of
them:

```
check_httpd_task_stack_budget: FAIL -- revert_post_handler reaches 5504 B,
  exceeding the 4832 B ceiling.
check_executor_task_stack_budget: FAIL (same cause)
```

This is exactly the failure the original comment describes having hit on its
first attempt. The important consequence: that stack cost is *already being
paid* today, through `bx_run_on_internal_stack()`'s backstop, on the same call
paths -- the checks simply cannot see it because the call goes through an
indirect `callx`. The situation is therefore not "safe because unreachable"
(the comment's claim) but "unmeasured". A real fix has to shrink the
`kiln_cfg_store_save_current()` frame (heap-allocate its
`scratch[ZONES_CONFIG_BLOB_MAX_SIZE]`) before the guard can be added honestly.
That is a larger change than this review should make unannounced.

## Measurements

- Host tests: 45/45 executables built from a cleaned build directory;
  `kilnctl_host_tests_zones.exe` 1926/1926 checks passed.
- Target build: `build_kilnfw` OK in 68.5 s, after the revert.
- Against that ELF: `check_httpd_task_stack_budget` OK (2088 B honest free,
  LOW); `check_executor_task_stack_budget` OK (940 B, LOW);
  `flash_worker_lint` clean; `check_nvs_key_length` PASS.
- `tools/run_all_checks.ps1`: the only failures observed with the tree in its
  final state are unrelated to this feature -- `check_doc_hash_citations.ps1`
  (one stale citation in another agent's in-progress doc) and two
  `test_pid_fuzzy_confidence.c` failures belonging to another session's live
  uncommitted work.

## Not verified

- Hazard 7 (consumer-without-producer, bypassed-owner shapes) was not
  systematically swept.
- The SaftyFW half of `ddde0b4c` (`config_store_flash.c`, `link_task.c`,
  `safety_core.c`, `config_store_seqlock_write()`) was not read.
- `test_kiln_cfg_swap.c` and `test_safety_cfg_http.c` were not negative-tested.
- Slot-11 and full-flash-zone behaviour was read, not exercised on hardware.
- No board was flashed and no run was started, per the brief.
