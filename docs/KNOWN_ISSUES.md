# Known issues

The single list of open problems (bugs and accepted trade-offs, not planned features). One line per item; the source doc holds the detail. Remove an entry when it is fixed.

## KilnFW
- Autotune thermal-guard zero-duty feed | severity: safety review pending | autotune feeds zero duty to the thermal guard window | [REVIEW_FIXERS](audits/REVIEW_FIXERS_2026-10-15.md), [AUDIT_CONTROL_EXECUTOR](audits/AUDIT_CONTROL_EXECUTOR.md) | open, see "Safety review 2026-10-16" below
- Aux manual fence | severity: unknown | aux manual output fence not yet settled | [SPARE_RELAY_ONOFF_PLAN](SPARE_RELAY_ONOFF_PLAN.md) | open, safety review pending
- Wi-Fi TX buffer RAM cut | MED | `ESP_WIFI_STATIC_TX_BUFFER_NUM` 16 -> 8 to free internal RAM; web/TLS TX throughput risk, bench soak needed | [AUDIT_INTERNAL_RAM_XIP](audits/AUDIT_INTERNAL_RAM_XIP.md) | open
- Stack budget unmeasured (TRIPWP3 INFO-4) | INFO | needs a target ELF measurement on the bench-flashed build | [REVIEW_TRIPWP3](audits/REVIEW_TRIPWP3_2026-10-11.md) | open
- Test-trip `trip_is_test` stays true after clear (TRIPFX5 LOW-1/LOW-2/INFO-2) | LOW | stale cached DIAG/TRIP_EVENT after clear; redundant term untested; stale docstring | [REVIEW_TRIPFX5](audits/REVIEW_TRIPFX5_2026-10-12.md) | open
- Test-trip readback ignores `diag_boot_id` (TRIPFX4 INFO-3) | INFO | covered in practice by `diag_since_reboot` | [REVIEW_TRIPFX4](audits/REVIEW_TRIPFX4_2026-10-12.md) | open

## SaftyFW
- A5 SET_PARAM bounds | owner 2026-10-16: reject out-of-range values | SET_PARAM accepts out-of-range values | [KILNLINK_ROBUSTNESS_AUDIT](audits/KILNLINK_ROBUSTNESS_AUDIT_2026-10-09.md) | fix pending, see SR-01..SR-07 below
- ctfx LOW-4: uncalibrated CT presence fallback is sensitive | LOW | deliberate (S3/S9 false negatives are the dangerous direction); mitigated by readiness naming the channel | [REVIEW_FIXERS](audits/REVIEW_FIXERS_2026-10-15.md) | accepted, will not fix

## Recovery / OTA / update
- Bench pre.3 cannot self-update from GitHub | HIGH (bench) | fetch refuses with `low_heap`, largest internal block 10752 B, steady state across reset | [BENCH_UPDATE_2026-10-15](bench/BENCH_UPDATE_2026-10-15.md) | open
- pre.3 has no `/api/update/status`, so `update_install_start` fails (404) after a PC-side stage | MED (bench) | install of staged pre.4 not completed | [BENCH_UPDATE_2026-10-15](bench/BENCH_UPDATE_2026-10-15.md) | open
- `flash_firmware` otadata gap | MED | JTAG flash does not touch `otadata`; a board pointing at factory/recovery boots recovery, verify fails loud | [CLAUDE.md](../CLAUDE.md) flash section, [MCP_SERVERS](MCP_SERVERS.md) | known, deliberate

## Web / auth
- Web page ignores `saved:true` on a failed live-profile decide (PROFILESFX3 INFO-2) | INFO | [REVIEW_PROFILESFX3](audits/REVIEW_PROFILESFX3_2026-10-12.md) | open

## PcTools / MCP tooling
- No MCP wrapper for `POST /api/cfgfs/discard_rejected` | LOW | route exists (ADMIN, mode-gated), no tool | [REVIEW_PERSFX](audits/REVIEW_PERSFX_2026-10-10.md) | open
- `.cmd`/`.bat` git shim resolution not addressed (TOOLFX12 INFO-5) | INFO | [REVIEW_TOOLFX12](audits/REVIEW_TOOLFX12_2026-10-12.md) | open
- `check_lcd_admin_gates` R12 false-fail (LCDFX9 LOW-2/LOW-3) and gate semantics resting on host tests (LCDFX7 INFO-3) | LOW | [REVIEW_LCDFX9](audits/REVIEW_LCDFX9_2026-10-12.md), [REVIEW_LCDFX7](audits/REVIEW_LCDFX7_2026-10-11.md) | open

## Hardware
- None recorded here yet; hardware-gated items live in [ROADMAP](../ROADMAP.md) and `firmware/*/TODO.md`.

## Docs
- This list is seeded from the 2026-10 REVIEW_* finding tables and the named items above; the per-area `TODO.md` files and ROADMAP bug rows have not been swept in full. Add entries as found.

## Safety review 2026-10-16

Review only, base origin/dev. Rule applied for SET_PARAM (owner A5, 2026-10-15): the Pico must reject, not clamp, any out-of-range value and report the refusal. Bounded fields in `config_params_set()` do reject (`return false`, no clamp); the fields below have no reject bound beyond type (and finiteness for f32).

- **SR-01** MED, `firmware/SaftyFW/src/config_params.c:600` (0x0402 `link_timeout_s`), `:599` (0x0401 `context_max_age_s`). No upper bound. Scenario: a bad or hostile SET_PARAM of 65535 makes S6b (link dead with current present, `safety_guards.c:753`) wait ~18 h, effectively blinding it. Fix: reject above a documented max (e.g. 60 s) at SET_PARAM and in the commit-time range check. Status open.
- **SR-02** MED, `config_params.c:496,498,500,502,506,507,508,545,583` (U16 time windows: `overshoot_time_s`, `rate_window_s`, `frozen_window_s`, `tc_disagreement_time_s`, `cj_time_s`, `borrowed_stale_s`, `borrowed_stale_trip_s`, `stuck_on_time_s`, `overcurrent_time_s`). No max. Scenario: 65535 s turns S2/S3/S11/S12/S13/S14 debounce windows into ~18 h, blinding the guard while it still reads "commissioned". Fix: per-field max (as already done for `blind_grace_s`, `trip_verify_s`). Status open.
- **SR-03** MED, `config_params.c:467` (0x0104 `abs_max_temp_c`). Only `> 0`. Scenario: 1e30 disables S1 while satisfying the "never tighter than zone maxima" rule. Fix: reject above a physical ceiling (e.g. thermocouple-type range max, ~1370 C for K). The lower side must stay "not below zone max" per owner rule. Status open.
- **SR-04** LOW, `config_params.c:582` (0x031D `overcurrent_pct`). No range. Scenario: 65535 % makes S14 overcurrent unreachable. Fix: reject outside e.g. [100, 300]. Status open.
- **SR-05** LOW, `config_params.c:494,504,505,553` (`firing_margin_c` (inert), `cj_warn_c`, `tc_expected_offset_c`, `mains_voltage_v`) finite-only; `:540,571,579-581` (`i_present_a`, `max_expected_power_w`, `i_normal_a[]`) non-negative only. Scenario: absurd values (mains 1e9 V, i_normal 1e6 A) skew power/current guards toward never tripping. Fix: physical-range rejects (e.g. mains 90-264 V). Status open.
- **SR-06** LOW, `config_params.c:474-476` (0x0106-0x0108 `ct_channel_map[]`), `:541-543` (`zero_counts[]`), `:544` (`correlation_window_s`), `:554` (`power_window_s`). No range; `ct_channel_map` has no `CHECK_U8_MAX` while sibling `zone_ct_channel` has `<= 2`. Scenario: map value 200 indexes outside the 3 CT channels (check every reader bounds it) or silently maps a zone to no channel. Fix: `CHECK_U8_MAX(2)` and documented maxima. Status open.
- **SR-07** INFO, refusal reporting not verified end to end in this pass: confirm a `config_params_set()` false becomes a NAK the ESP surfaces (HTTP 4xx naming the param), never a silent drop. Status open.
- **SR-08** INFO, known item "autotune feeds thermal guard zero duty" (`firmware/KilnFW/App/drivers/control/autotune_engine.c:213-229,408-419`): on current dev the guard's `commanded_duty` (`sensor_ok ? want_duty : 0`) is the same value passed to `heater_output_duty_relay_step()`, so the guard sees exactly what actuates; SETTLING really is duty 0. Sensor-invalid ticks feed 0 but the guard's own S-invalid debounce (`thermal_guard.c:179`) trips independently. Guards are not blinded. Earlier history: `docs/audits/AUDIT_CONTROL_EXECUTOR.md`. Status: no defect found; owner may close.
- **SR-09** INFO, mode gate: `system_mode_gate` is called from every relay/zone/aux/config writer found (`zones_http_post.c`, `aux_outputs_http.c`, `kiln_io_owner.c`, `relay_authority.c`, uart bridge ext control, backup import, factory reset, update). No ungated writer found in this pass. Prior aux fence review: `docs/audits/AUX_OUTPUTS_SAFETY_REVIEW_2026-10-09.md`. Status: no defect found.
- **SR-10** MED, `firmware/KilnFW/App/drivers/control/thermal_guard.c:448` (guard 4, runaway/welded relay). The off window only arms while `commanded_duty <= 0.0f`, and any positive duty resets it (`:470-473`). A positive duty that heater_output renders as OFF (on-time under the 10 s `HEATER_MIN_ON_MS_FLOOR`, `heater_output.c` on_ms=0) or a feedforward hold term (`profile_executor_pid_tick.c:135`, `u_ff` is setpoint-based) keeps guard 4 disarmed, and guards 1/2 only run at duty >= 0.5 and guard 7 needs a frozen reading. Scenario: zone relay welded closed, temperature climbs above setpoint, PID+feedforward sits at a small positive duty: guard 4 never arms; only drift (25 C for 600 s) and the Pico (S1 abs_max, S13 stuck-on if a CT is fitted) remain. Fix: arm guard 4 on "relay commanded off by heater_output" (physical command) rather than duty == 0, or add an overshoot check (measurement > setpoint + margin and rising while duty < progress_duty_min). Status open.
- **SR-11** MED, `firmware/KilnFW/App/drivers/control/heat_enable.c:792-816` with `firmware/SaftyFW/src/tasks/link_task.c:1120-1123`. The benign/fatal verdict for the owner's "benign Pico reboot auto-resumes heat" rule depends on boot_reason, but the Pico never reports BROWNOUT (no source, always 0) and reports every non-watchdog reset as POWERON. Also there is no per-firing limit on benign resumes. Scenario: a sagging 5 V/3.3 V rail under heater load browns out the Pico repeatedly; each reboot is classed benign and heat re-requested once ARMED, and each reboot restarts the Pico's windowed guards (S2/S3/S11-S14 debounce windows are RAM state), so a reboot period shorter than those windows hides them. Fix: count Pico reboots per heat claim and hold (fatal) on the second within N minutes; treat a Pico reboot within T s of K4 closing as fatal; document that POWERON includes brownout. The owner rule itself is unchanged for a single reboot. Status open.
- **SR-12** LOW, `firmware/KilnFW/App/drivers/persist/zones_config_accessors.h:233` `ZONE_MAX_TEMP_C_MAX 2500.0f`. Above any supported thermocouple range (K ~1370 C), so a zone max_temp_c (ESP guard 5) and, via the ceiling sync, the Pico abs_max can be set where neither ever trips. Same class as SR-03 on the Pico side. Fix: cap at the thermocouple type's range max. Status open.
- **SR-13** INFO, executor endings (item 2). Checked: normal DONE (`profile_executor.c:1095,1216`, force_all_relays_off + io_segs off), mode-state violation (`:228-236`), global and per-zone thermal-guard trips (`profile_executor_relay_io.c:1590-1700`), operator stop/halt (`profile_executor_status.c:35-113`, also releases claims and heat_enable), fault_halt (`:225-260`), watchdog FAULT for stale tick / Pico trip / safety link silent 30 s (`profile_executor.c:2415-2475`). Every non-RUNNING control tick re-runs force_all_relays_off and the pending OFF retry (`:761-779`), and a stale control task is covered by guard 9's direct `kiln_io_all_relays_off`. ESP reboot never resumes (`run_state.h:2-13`). Relays owned by another owner (manual/aux) are deliberately skipped. No path found that ends a firing with a profile-owned relay left on. Status: no defect found.
- **SR-14** INFO, heater_output (item 1). Non-finite or <= 0 duty becomes 0, `window_ms == 0` renders off, min-on floor can extend an ON by at most 10 s, `force_off` clears window state. Sensor-invalid debounce (`thermal_guard.c:179`) is bounded by `ZONE_GUARD_DEBOUNCE_TICKS_MAX 100` so the uint8 cast in `effective_ticks()` cannot wrap. A zone with max_temp_c 0 (guard 5 off) is refused at run start (`profile_executor_run.c:685`). Status: no defect found.
- Not covered by this pass (reassigned by the coordinator): SaftyFW guards S1-S16, Pico config store A/B sectors, SR-07 end-to-end refusal reporting.


## Review 2026-10-16: persistence

Scope: `drivers/persist` (kiln_cfg_store incl. the v1/v2 cfg-file migration from `3b38c6f03`/`439a20576`, zones_config load/migration, cfg dual-write resolve), backup import, quarantine/discard paths. Items already marked fixed in `docs/audits` or listed above were skipped.

- **PS-01** (MED) `firmware/KilnFW/App/drivers/persist/kiln_cfg_store.c:620-655,706`. Problem: `migrate_older_cfg_file()` returns false for three different reasons: a `persist_scratch_alloc` failure, a `cfg_fs_read` error, or bytes that really are an unknown layout. The caller treats all three the same way. It resets to defaults and quarantines with "older layout this firmware cannot migrate". Scenario: the boot reaches the migration while PSRAM scratch is short, or a cfg read fails once. A good v1/v2 kiln config file is then quarantined as unmigratable, and all saved kiln configs show as empty. The operator follows the message and runs quarantine_clear/discard. A config that the next boot would have migrated is lost. Fix: return a tri-state (migrated / transient failure / unknown layout). On a transient failure, keep the file, quarantine with a "retry next boot" reason, and do not write `.bad`. Status: open.
- **PS-02** (LOW) `firmware/KilnFW/App/drivers/persist/kiln_cfg_store.c:653`, `kiln_cfg_store_cfg_fs.c:143,171`. Problem: the `.bad` preserve result is cast to `(void)`, but the quarantine reason still says "copy kept as .bad". Scenario: the cfg partition is full, or the write fails. The operator trusts that a copy exists and discards, and no copy exists. Fix: check the return value and say "copy could NOT be kept" in the reason. Status: open.


## Safety review 2026-10-16: SaftyFW guards

Scope: S1..S16 trip/clear paths in `firmware/SaftyFW/src/safety_guards.c`, `tasks/safety_core.c`, `tasks/link_frame.c`, the config store, and SR-07. Items fixed in `docs/audits/SAFTYFW_GUARD_REVIEW_2026-10-09.md` (F1..F7) and later fix reviews, and SR-01..09 above, are not repeated.

- **SR-30** LOW, `firmware/SaftyFW/src/tasks/safety_core.c:1230-1234`, `safety_guards.c:764-771`. Problem: on a CT-less board (`ct_installed=0`) `any_current_present` is forced false, so the S6b soft tier (`link_timeout_s` with current present) can never fire. Only the `link_dead_hard_s` backstop is left (default 120 s, max 600 s). `GUARD_TEST_MATRIX.md` lists this as a precondition, but nothing compensates for it. Scenario: the ESP hangs mid-firing while K4 is ARMED (a latch), and the elements stay powered for up to `link_dead_hard_s` with no controller. Fix: when `cts_disabled`, use `relay_owner_is_energized()` as the heat evidence for the S6b soft tier (and for S11 `heat_commanded`, as the 2026-10-09 review already suggests). This only adds trips. Status open.
- **SR-31** LOW, `firmware/SaftyFW/src/config_params.c:599,602,603` (0x0401 `context_max_age_s`, 0x0404 `mainfault_debounce_ms`, 0x0405 `telemetry_period_ms`). Problem: these are accepted, persisted and read back by GET_PARAM, but nothing in `src/tasks/` uses them. Context age is the hard-coded `CONTEXT_MAX_AGE_MS 5000` (`safety_core.c:183`). Scenario: a commissioner tightens `context_max_age_s` or `mainfault_debounce_ms`, sees the new value read back, and believes it applies, but it has no effect. This also means SR-01's `context_max_age_s` blinding scenario cannot happen today. Fix: either wire them in (with ranges, per SR-01), or refuse them in SET_PARAM as reserved and remove them from the commissioning list. Status open.
- **SR-32** LOW, `firmware/SaftyFW/src/tasks/link_task.c:2766-2773`, `firmware/KilnFW/App/drivers/safety/safety_cfg_write.c:258-306,527`. This answers SR-07. A SET_PARAM refusal sends no reply frame; the Pico only writes a WARN log line. The refusal is not silent end to end: the ESP read-back finds the mismatch, `apply_pairs_ex` returns false (HTTP error), and the MCP tool `safety_set_commissioning_fields` returns `failed: ...`. But (a) the reason text blames a staging timeout or an ESP reboot, not "value out of range", and (b) the other params in the same POST still go through COMMIT_CONFIG and are persisted, while the message says "treating the write as FAILED". Scenario: the operator posts {abs_max ok, overcurrent_pct out of range}, sees FAILED, and assumes nothing changed, but abs_max was persisted. Fix: add a SET_PARAM_REJECTED reply carrying param_id and reason (fold it into the next protocol bump), or have the ESP pre-check ranges against a mirrored table before staging. Name which pairs did land. Status open.
- **SR-33** LOW, `firmware/SaftyFW/src/safety_guards.c:132` (`s5_bad_read_now`), `:830-845` (S1). Problem: MAX31856 TCRANGE/OPEN make the read "bad", so a temperature above the converter's range takes the S5 path. It trips only after `blind_grace_s` (default 60 s, max 300 s), not after S1's 3-reading (~300 ms) streak. Scenario: `abs_max_temp_c` is set at or near the TC range limit (which SR-03 allows), and a runaway that crosses the range in one jump is caught by S5 up to 300 s late. Fix: if bad reads start while the last valid reading is within a margin of the ceiling (or TCHIGH is set), treat it as S1, or cap `abs_max_temp_c` below the TC type's range (combine with the SR-03 fix). Status open.
- **SR-34** INFO, `firmware/SaftyFW/src/safety_guards.c:848` (S11 uses an exact float `!=`). A stuck path that reads with 1-LSB noise, or a TC that has come out of the load and reads ambient with noise, never counts as frozen. This is within the guard's stated "identical reading" scope, and S2/S10 cover parts of it. Fix (optional): compare against a deadband (e.g. 0.25 C) instead. Status open.
- **SR-35** INFO, checked and clean, no defect:
  - S6a clear: `link_frame_decide_clear_trip` (`link_frame.c:279-281`) requires the wire mask to equal `1<<(reason-1)`, so S6a needs exactly 0x0020. If mainFault is still asserted, the clear fails because `try_clear` re-ticks.
  - S9: unclearable at both layers.
  - ARMED stays latched across link loss.
  - S16: unclearable until reboot, because `s_ram_integrity_recurrence_pending` is reset only in `config_store_boot_load` (`config_store_flash.c:811`). This is intentional fail-safe behaviour; the only exit is a power cycle.
  - Config store: the A/B sectors and torn-slot read-back verify (`24090c9a`, `98d237b0`) hold. One sector always keeps a valid record across the erase-then-program step.
  - `borrowed_zone_index` is bounded to 0..2 before it is used as an array index (`safety_core.c:1117`).
  - Status: no defect.

## Review 2026-10-16: OTA/update

Scope: KilnFW `drivers/update` (check/fetch/stage, heap admission, stage verify), `POST /api/update/install`
(`ota_recovery_boot_run`), recovery `apply_staged`, boot_guard/otadata, Pico auto-update. Review only, at
origin/dev. Items already marked fixed in `docs/audits/` (UPDATE_PATH_ROBUSTNESS_AUDIT_2026-10-13,
AUDIT_UPDATE_FAULTS, AUDIT_PICO_UPDATE, RECOVERY_IMAGE_REVIEW etc.) are not repeated. No HIGH found: the
apply copies only a header-VERIFIED stage whose sha256 is re-hashed before the first erase, re-hashes `app`
after the copy, runs `esp_image_verify`, and only then sets the boot partition; otadata keeps pointing at
recovery for the whole copy, so a power cut mid-copy reboots into recovery with the stage intact
(bounded retry via `update_install_after_fail`). `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` covers a new app
that dies before `esp_ota_mark_app_valid_cancel_rollback()`. Install, recovery_boot, stage writes and the
fetch all refuse during a firing/autotune.

- **UP-01** LOW, `firmware/KilnFW/App/drivers/net/pico_auto_update_boot.c:115-129`
  (`attempt_update_acquire`). Problem: the interlock (no firing, no heat) is checked BEFORE
  `ota_http_update_try_begin()`, and nothing re-checks after the claim. `ota_recovery_boot_run`
  (`ota_http_recovery.c`) closes the same race with an authoritative relay/heat read after the claim.
  Scenario: an operator starts a firing in the first seconds after boot; the profile start passes
  `ota_http_heat_blocked_by_update()` (no claim yet) while the auto-update task passes its interlock
  read (run not committed yet), then claims and pushes a SaftyFW image; the Pico reboots mid-firing
  (fails safe: heat cut, run aborts). Fix: after the claim succeeds, re-run
  `ota_http_check_interlocks()` and `relay_authority_heat_run_active()`; on refusal call
  `ota_http_update_end()` and stand down. Status: open.
- **UP-02** LOW, `firmware/KilnFW/App/drivers/update/update_install_http.c` (request write in
  `install_prepare`) and `ota_http_recovery.c` `ota_recovery_boot_run` step 5. Problem: the one-shot install
  request is removed only by `undo()` on an in-handler failure; the application never drops a lingering
  request at boot. Scenario: power cut during `esp_ota_set_boot_partition(factory)`'s otadata erase leaves
  one valid otadata sector, so the app boots again with the request still stored and the stage still
  VERIFIED; days later an unrelated entry into recovery (boot_guard threshold after a crash loop) consumes
  it and installs the staged image unattended, masking the crash cause and surprising the operator.
  Fix: at app boot (after `boot_guard_init`, normal mode only) delete any stored install request and write
  a result record `reason=stale_request`, or store the request's boot_count and have recovery refuse a
  request not issued on the immediately preceding boot. Status: open.
- **UP-03** INFO, `firmware/KilnFW/App/drivers/update/update_fetch_heap.c:4-12`,
  `update_fetch_heap.h:154-189`. pre.3's `low_heap` refusal at "10752 B largest" was the FREE check, not the
  block check: idle free 29815 B (INTERNAL_HEAP_MARGIN_2026-10-09) is below the 31728 B admission
  (ABORT_BELOW 9216 + MODEL_DRAW 22512); 10752 B largest is above `FETCH_LARGEST_BLOCK_MIN` 6144 B. dev
  (`6880bfefb`, XIP from PSRAM) measures about 45 KB idle free and a 16384 B largest block
  (AUDIT_INTERNAL_RAM_XIP), so dev should admit with about 13 KB to spare. Not yet shown by a GitHub fetch
  on a dev build: confirm with `update_check` + `update_stage_release` on the bench and record
  `heap.start_free`. A pre.3 board can never self-update to dev over GitHub (its own admission refuses);
  it needs JTAG or a recovery push. Status: open (bench confirmation).
