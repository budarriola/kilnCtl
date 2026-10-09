# Aux outputs safety review (2026-10-09)

Scope: spare-relay aux outputs, the monitor-only zone and the config-convert aux
path (docs/SPARE_RELAY_ONOFF_PLAN.md WP-0..9). Read-only review of origin/dev at
the time of writing. No code was changed, no board was touched, no test was run.
All paths are under `firmware/KilnFW/App/drivers/` unless stated otherwise.

Owner decisions taken as given:
- A relay-less freed HEATER zone is monitor-only. It skips PID, the ramp lock
  and the heat guards.
- PAUSE holds the last commanded aux state by design (owner Q4).
- Aux relays are never reported to the Pico (2026-10-04).

## Summary

| ID | Rating | Question | Finding |
|----|--------|----------|---------|
| F1 | MED | a, b | While PAUSED, an aux left ON stays energised through every relay-authority fault source that does not escalate to a run FAULT. |
| F2 | LOW | a | A manual aux ON made while idle survives a later Pico trip. Plan section 5 says a Pico trip drops aux. |
| F3 | LOW | d | `aux_outputs_cfg_set()` updates RAM before the commit and never rolls it back, so a failed save leaves RAM enabled while flash says disabled. |
| F4 | LOW | a | The aux store globals are read by the executor task and written by HTTP with no lock. |
| F5 | LOW | c | A monitor-only zone is dropped as a cross-zone (guard 8) peer. On a 2-zone kiln with one zone converted, guard 8 has no peer left. |
| F6 | LOW | d | After a rollback to pre-aux firmware, stored aux rules (targets 8..11) are silently inert. The freed zone runs as an ordinary HEATER with an empty relay mask. |
| F7 | LOW | b | The run-start handoff comment overstates when the failed-OFF retry runs. |

There are no HIGH findings. Questions (c) and (e) came out clean apart from F5.

## (a) Can an aux output be energised while a thermal guard is tripped or the safety link is down?

### While RUNNING: no, apart from the per-zone case below, which is by design

- Every tick, an aux ON is gated on global relay authority:
  - `control/profile_executor_relay_io.c:695` computes `relay_authority_on_blocked()` once per tick.
  - `profile_executor_aux_on_off_input()` turns that into `failsafe_override` (`profile_executor_relay_io.c:655-679`).
  - `aux_apply_relay()` checks it again before any ON write (`profile_executor_relay_io.c:481-514`).
- For aux, `on_off_trigger_decide()` resolves `failsafe_override` to OFF, because an aux output has no failsafe-ON option. `zone_aux_convert_core.c` refuses a zone with `failsafe_on` ("which an aux output does not support").
- Relay authority is blocked while `fault_sources != 0` (`owners/relay_authority.c:15-28`). Those sources include SAFETY_LINK, THERMO and THERMAL_SANITY.
- Guards 5 and 6 on the aux's own `tc_zone` force the aux OFF. If that zone is not active, is faulted or has no valid reading, `temp_ok` is false and the aux falls to its failsafe state, which is OFF (`profile_executor_relay_io.c:655-679`).
- A global guard trip, a Pico TRIPPED report, 30 s of link silence or a sustained PC link loss all end the run through `exec_enter_terminal_state()`. That calls `force_aux_relays_off()` (`profile_executor_relay_io.c:1030-1046`). The watchdog also calls `kiln_io_all_relays_off()` on all four relay bits (`control/profile_executor.c:2164`, `:2215`, `:2236`).
- By design, a per-zone trip under `continue_on_zone_trip` does not stop an aux whose `tc_zone` is a different zone.
- Manual and HTTP writes to an aux relay are refused while running: `http/aux_outputs_http_core.c` uses `SYS_ACTION_RAW_RELAY_DEBUG_WRITE` and `SYS_ACTION_WRITE_ZONES_CONFIG`, both of which the mode gate blocks.
- Danger mode cannot bypass the aux gate. `owners/kiln_io_owner.c:238-306` lets danger mode skip authority, but `aux_apply_relay()` checks authority itself before it queues the write.

### F1 (MED): PAUSED holds aux through faults that do not escalate

Evidence:
- `control/profile_executor.c:729-745`: when the state is not RUNNING, the executor calls `force_all_relays_off()`. That function iterates active zones only (`profile_executor_relay_io.c:460-469`). The aux tick is not reached, and `force_aux_relays_off()` is skipped while PAUSED.
- `control/profile_executor_status.c:283-316`: pause hands `claimed_relay_mask`, aux bits included, to MANUAL. An existing ON is not re-evaluated.
- The watchdog does treat PAUSED as a live run (`control/profile_executor.c:2200-2201`, `state_running_or_paused`). So these cases do drop every relay:
  - a fresh Pico TRIPPED diag
  - 30 s of link silence (`SAFETY_LINK_FIRING_ABORT_SILENCE_MS`, `:2119`)
  - a sustained PC link loss
- These cases do not drop the aux. An aux left ON stays energised:
  - for 1.5 s up to 30 s of safety-link silence, while the SAFETY_LINK fault source only blocks new ONs;
  - indefinitely while paused under a MANUAL, APP, THERMAL_SANITY or THERMO fault source;
  - after a thermocouple fault on the aux's own `tc_zone` while paused. Guards 5 and 6 do not run while paused, so nothing evaluates it.

Plan section 5 says aux goes OFF on every condition that turns a zone relay OFF. While paused that is not true. Zone relays are off for the whole pause, but the aux is held even under an active fault source. Owner Q4 covers holding the commanded state, but it does not say the hold should outlast a safety fault.

Mitigation: pause releases the heat claim (`profile_executor_status.c` `heat_enable_release`). If the aux load is wired downstream of K4 it loses power anyway. I could not determine the wiring: aux loads such as vents are expected to sit outside the K4 path, and nothing in the repository says which.

Proposed fix: in the not-RUNNING branch at `control/profile_executor.c:729-745`, when the state is PAUSED and `relay_authority_on_blocked()` is true, call `aux_apply_relay(i, false)` for every claimed aux that is commanded or actuated ON. The aux stays held only while authority is clear. Add a host test that pauses with an aux ON and then asserts each fault source.

### F2 (LOW): a manual aux ON made while idle survives a Pico trip

- The watchdog's idle Pico trip is `LOG_IDLE_TRIP` only (`control/profile_executor.c:2240`). No relay is dropped.
- `owners/kiln_io_owner.c:238-306` blocks only new ONs.
- The PC-link watchdog drops MANUAL-owned relays only when the PC link goes down (`bridge/uart_bridge.c:395-470`).

This is existing behaviour for every manual relay, not something aux introduced. It still contradicts plan section 5's statement that a Pico trip drops aux on the ESP side. Because aux relays are hidden from the Pico (`safety/safety_pico_relay_mask.h`), the Pico cannot account for that load. S3 still sees its current on the CT, so a miswired heater load fails safe.

Fix: either drop enabled-aux relays when a fault source is asserted while idle, or correct plan section 5.

### F4 (LOW): unlocked aux store

**Fixed in 49ced2d0.**

`s_entries`, `s_enabled_mask` and `s_conflict_mask` (`persist/aux_outputs_cfg.c:43-45`) are written by `aux_outputs_cfg_set()` (`:246-285`) on the httpd task. They are read by `aux_tick`, the run-start checks and `safety_pico_relay_mask()` on other tasks. Single-byte mask reads are atomic on the S3, but `aux_outputs_cfg_get()` copies a multi-field entry and can return a torn entry. The impact is low because the mode gate refuses aux writes while running.

Fix: add a short spinlock or mutex around set and get.

## (b) Is aux forced off on every run-exit path?

Yes, apart from F1 (PAUSED) and a narrow retry gap.

- Every terminal transition goes through `exec_enter_terminal_state()`, which calls `force_aux_relays_off()` (`profile_executor_relay_io.c:1030-1046`). The callers are:
  - the two DONE paths (`control/profile_executor.c:1051`, `:1171`)
  - the escalate-guard branches, including "every active zone individually faulted" (`profile_executor_relay_io.c:1149-1160`)
  - the watchdog FAULT (`:2160-2240`)
  - the mode-violation FAULT (`:228`, `:244-246`)
  - `profile_executor_fault_halt` (`profile_executor_status.c` around `:243`), which is also the config-divergence halt (`safety/safety_ceiling_sync.c:300-330`, which first calls `kiln_io_owner_command_all_relays_off()`)
  - `profile_executor_halt` (`profile_executor_status.c:33-112`), which calls both force-off functions before going IDLE
- A failed OFF write keeps the bit in `aux_claim_mask` and sets `aux_off_pending` (`profile_executor_relay_io.c:540-569`). The not-RUNNING loop retries it (`control/profile_executor.c:739-743`), except while PAUSED.
- An aux disabled or conflicted mid-run is written OFF and released (`profile_executor_relay_io.c:702-710`).
- Power loss: `owners/kiln_io.c` initialises all relays off (`:165`, `:241`). `aux_claim_mask` is RAM-only. A resumed run re-hands the aux at start (`control/profile_executor_run.c:1399-1421`): it claims `enabled | aux_claim_mask` and writes them OFF.
- Mode change: a `system_mode_gate` violation while running is a FAULT and is covered above.

### F7 (LOW): misleading handoff comment

The comment at `control/profile_executor_run.c:1399-1421` says a failed handoff OFF is retried by the `aux_off_pending` branch. That branch only runs when the state is not RUNNING. In practice the first `profile_executor_aux_tick()` rewrites every claimed aux on the next tick, so the behaviour is safe.

Fix: correct the comment.

## (c) Can a monitor-only zone drive a heater relay or mask a real fault?

It cannot drive a relay:
- `zone_is_monitor_only()` (`persist/zones_config_accessors.c:1127-1139`) is true only for a HEATER zone whose relay mask reads back as 0. It fails closed to false on any read failure, so a read failure never exempts a zone from guards.
- Even when it is false, the zone's relay mask is 0, so `apply_relay()` would write nothing.
- The executor skips PID and output for a monitor-only zone (`control/profile_executor.c:1375-1383`) and never calls `apply_relay()` for it (`:1552`).
- Run start refuses a profile with no heating zone and names the monitor-only cause (`control/profile_executor_run.c:801-830`).
- Autotune refuses a monitor-only zone (`control/autotune_engine.c:1140`).
- Zone relay-mask writes are mode-gated while running, so a zone cannot change in or out of monitor-only during a run.

It cannot mask a fault:
- The "every heater faulted" check skips monitor-only zones (`profile_executor_relay_io.c:1149`). An unfaulted monitor zone therefore cannot keep a run alive when every real heater has faulted.
- Guards 5, 6 and 7 still run on the monitor zone: `zone_guard_exempt` sets only `on_off_zone` (`control/profile_executor.c:861`, `:1743-1744`). A sensor fault there escalates normally.
- The ramp lock skips the monitor zone (`control/profile_executor_ramp_assist.c:262`, `:297`, `:332`), so it cannot stall the run. Coupling is masked (`persist/zones_config_accessors.c:687-692`, `control/adaptive_tune_model.c:556`).

### F5 (LOW): monitor-only zone is not a cross-zone peer

`peer_is_on_off = zone_guard_exempt` (`control/profile_executor.c:1743`) drops the monitor zone from the far side of guard 8 for every other zone (`control/thermal_guard.c:563-580`). This matches plan section 10.

The cost is that a thermocouple which was a heated zone's neighbour no longer checks the remaining heaters. On a 2-zone kiln with one zone converted, guard 8 has no peers and is inert. Guards 1-4 on the remaining heater still run, so this reduces coverage without hiding a fault.

Fix: optionally let a monitor zone act as a one-way guard 8 peer with a separate, wider delta. Otherwise, document the coverage loss in plan section 10.

## (d) Does convert round-trip aux config without silently dropping outputs?

Within this firmware, yes:
- `AUX_OUTPUTS_CFG_VERSION` is 1 and has no older version (`persist/aux_outputs_cfg.c:28`). A newer-version blob is quarantined rather than dropped (`:80-98`, `:114-195`). A corrupt blob falls back to defaults, which are all disabled, so the failure is safe.
- Backup export writes every relay 1..4 with effective values. `hyst_c` is printed with `%.9g` so it round-trips exactly. A quarantined store is omitted (`http/backup_export.c:195-232`).
- Backup import:
  - validates relay 1..4, ranges and `aux_outputs_cfg_entry_valid()`;
  - checks conflicts against the restored zones' relay masks, not only the live ones;
  - refuses the whole restore on a conflict or a quarantined store;
  - treats an absent key as preserve;
  - keeps an undo record (`http/backup_import.c:2961-3200`).
- On the PC side, `tools/PcTools/src/kilnctrl/cfg_convert.py:456-459` carries the `aux_outputs` array through unchanged and reports it as "kept" at any target version.
- `tools/PcTools/src/kilnctrl/config_convert.py` has no converter for the raw `aux_out_cfg` blob or `aux_out.dat`. With a single blob version there is nothing to convert yet. When version 2 is added, that store needs an entry there.

For the profile store, a down-convert below PROFILE_VERSION 4 drops on/off rules, aux rules included, and reports the drop (`config_convert.py`, `convert_profile_blob`). The drop is not silent.

### F3 (LOW): RAM is not rolled back on a failed save

**Fixed in 49ced2d0.**

`persist/aux_outputs_cfg.c:268-285` updates `s_entries` and `s_enabled_mask` before `pref_cfg_fs_commit()` and leaves them changed when the commit fails. A failed enable therefore stays live in RAM:
- the relay is driven by rules;
- it is hidden from the Pico;
- it disappears at the next reboot.

The HTTP caller gets an error. This is config divergence, the class this repository treats as a fault.

Fix: apply the RAM update only after `ESP_OK`, or restore the previous entry and masks when the commit fails.

### F6 (LOW): rollback leaves aux rules silently inert

`persist/profile_rule_target.h:11-12` states this. After a rollback to firmware that predates aux, the freed zone runs as an ordinary HEATER with an empty relay mask. It gets PID and guards 1-4, but nothing can heat it, so it is likely to trip a heat-rise guard. That fails safe, but it can be confusing. I could not determine how a specific older build behaves without that build.

Fix: note this in the rollback hazard text in `firmware/CommonFW/docs/UPDATE_PROTOCOL.md`.

## (e) Are the relay and aux-slot index spaces consistent?

They are consistent everywhere I checked:
- Store: entry i is relay i+1 and bit i is relay i+1 (`persist/aux_outputs_cfg.c:254`). `get(relay)` takes relay 1..4.
- Rule target: wire byte = 8 + relay - 1 (`persist/profile_rule_target.h`).
  - The executor looks up `PROFILE_RULE_TARGET_AUX_BASE + aux_idx`, with `aux_idx` 0-based (`profile_executor_relay_io.c:670`).
  - Run-start checks use `profile_rule_target_aux_relay()` (`control/profile_executor_run.c:582-590`, `:867-868`).
  - The save validator does the same (`http/profiles_http.c:1363-1368`).
  - The converter and destination writers use `profile_rule_target_from_aux_relay()` (`http/profiles_http.c:2024`, `:2130`, `:2197`).
- Zone-to-aux convert: the relay is the 1-based lowest set bit, and exactly one bit is required (`zone_aux_convert_core.c` around lines 261-271). The verify step uses `1u << (relay - 1u)` (`:108`).
- RELAY_IO segment refusal: the target is 1..4 and the bit is `t - PROFILE_IO_TARGET_RELAY_BASE` (`persist/profiles_types.h:81`, `control/profile_executor_run.c:559`).
- Pico mask: bit i = relay i+1 in both operands (`safety/safety_pico_relay_mask.h`).
- Backup: `relay` is 1-based and `tc_zone` is -1..2, stored as `tc_zone + 1` (`http/backup_import.c:3024-3061`, `http/backup_export.c:217-227`).
- Web UI:
  - `AUX_RULE_BASE + a.relay - 1` (`http/profiles_page.html:464`)
  - `RELAY_NAMES[relay - 1]` (`http/zones_page.html:2975`, `:3120`, `http/main_page.html:4367`)
- PcTools: `dest = 8 + relay - 1` and `zmask & (1 << (relay - 1))` (`tools/PcTools/src/kilnctrl/mcp_server_aux.py:127`, `:319`, `:492`).

There is no finding here.

## Not covered

- Physical wiring of aux loads relative to K4: could not determine. It decides how much F1 and F2 matter.
- The behaviour of specific older firmware builds after a rollback (F6): could not determine without those builds.
- No host test or bench run was done for this review. The findings come from reading the code only.
