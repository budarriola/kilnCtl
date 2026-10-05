# Spare-relay on/off ("aux outputs") -- plan

Status: 2026-10-05. WP-0, WP-1 (`1f70c419`), WP-2 (`622f0539`) and WP-9 (`bc21b218`) and WP-4 (`5dcf89f3`) landed; WP-3 in progress, then WP-5..WP-8. Pending work only.

Owner requirement 2026-10-04 (overturns D1 of
`docs/audits/on_off_zone_decisions_2026-09-14.md`): "we have 4 relays, the 4th
was always intended for this and the ones not assigned to a zone where always
intended to be used for things like this". An on/off device (vent, damper,
fan, water feed) must bind any relay that no heating zone uses, WITHOUT
consuming one of the 3 thermocouple-backed zone slots.

Background read: `docs/audits/on_off_spare_relay_binding_2026-09-14.md`
(why widening zones[] is L), `docs/ON_OFF_ZONE_PLAN.md` (the evaluator and
rules this plan reuses), `docs/CONFIG_MIGRATION_CHAIN_PLAN.md`,
`docs/CONFIG_FILESYSTEM.md`.

## 1. Design in one page

Do NOT widen `zones[]`, do NOT bump `ZONES_CFG_VERSION` (26), do NOT bump
`PROFILE_VERSION` (4) or `KILN_CFG_STORE_VERSION`. Add a small, separate,
parallel store of "aux outputs" and let the existing pure evaluator drive it.

1. **Aux store.** New persisted blob `aux_outputs_cfg_t`, version 1, CRC32 tail,
   one entry per relay (`KILN_IO_RELAY_COUNT` = 4 entries, entry i = relay i+1,
   so "one aux per relay" holds by construction). NVS key `aux_out_cfg`
   (11 chars, `NVS_KEY_LEN_CHECK`), cfg mirror `aux_out.dat`, same dual-write
   shape as `relay_names.dat` (NVS authoritative). Own file pair under
   `persist/`; it is NOT a field of `zone_cfg_t`.
2. **Entry fields** (all-zero = disabled, today's behaviour):
   `enabled u8`, `tc_zone u8` (0..2 = zone whose TC feeds temperature rules,
   `0xFF` = none; note 0 is a valid zone, so the zero-init default must be
   normalised on read, or store `tc_zone_plus1` with 0 = none -- DECIDED 2026-10-04: plus1),
   `hyst_c f32` (0 -> 2.0 at read), `min_on_s u16`, `min_off_s u16` (0 -> 30),
   `reserved[]`. Fail-safe state is fixed OFF (no field; see Q3). Relay label and
   relay_type stay in the existing `relay_names` store; do not duplicate them.
3. **Rules.** Reuse `profile_on_off_rule_t` and the 8-slot
   `profile_t.on_off_rules[]` array unchanged (same layout, so no
   `PROFILE_VERSION` bump). Its `zone_index` byte already is a `uint8_t`; define
   `PROFILE_RULE_TARGET_AUX_BASE = 8` so `zone_index` 8..11 addresses aux relay
   1..4. Values 0..2 stay zones. Old firmware importing such a profile rejects
   it (`validate_on_off_rules()` refuses a non-on/off zone index) -- fail loud,
   not silent. `temp_source == 1` ("this zone's TC") means the entry's
   `tc_zone` for an aux target; `temp_source` 2/3 stay reserved.
4. **Evaluator.** Reuse `on_off_trigger_decide()` +
   `profile_executor_on_off_actuation_gate()` +
   `profile_executor_on_off_cap_denies()` verbatim. The only new code is the
   per-aux runtime state, the input builder (shared with the zone path), and
   the tick call site (sec 6).
5. **Conflict rule.** A relay bit may be in at most one of: any heating zone's
   `relay_mask`, or an enabled aux entry. Enforced on every write path to
   either side (sec 3).
6. **K4 independence.** Aux outputs do not take a `heat_enable` claim and do
   not depend on K4 (sec 4).
7. **Safety.** Aux relays go OFF on every condition a zone relay goes OFF on
   (global fault sources, run end, abort), via the same chokepoint (sec 5).
8. **Surfaces.** Extend existing routes only (sec 7), MCP tools (sec 8),
   firing record (sec 9), optional migration of existing ON_OFF zones to aux
   (sec 10).

Why this beats the alternatives: widening `zones[]` touches guards, the
coupling matrix, firing records, the persisted blob, and needs a v26 freeze +
converter on the family under migration scrutiny (D1's M-L). A parallel
small store has no migration (new key, absent = disabled) and no consumer in
the thermal model: the aux path never enters guards, coupling, ramp-lock,
feasibility, autotune, or firing stats, because it is not a zone.

## 2. Hardware facts this rests on (verify in WP-0, do not assume)

- All four relays are PILOT relays "for galvanic isolation only -- None
  carries element current; they drive external contactors and SSRs"
  (`firmware/SaftyFW/README.md`). `IO_RELAY_COUNT` = 4
  (`uart_task_ids.h:445`), `KILN_IO_RELAY_COUNT` = 4 (`kiln_io.h:65`).
- K4 is a separate, Pico-owned relay whose contact sits in the external line
  contactor coil circuit; "K4 must sit in series with whatever energizes the
  load-switching stage, wired so that de-energized K4 = elements dead"
  (same README; `firmware/SaftyFW/docs/HARDWARE.md`).
- ESP-side, a relay command is gated only by `relay_authority_on_blocked()`
  (`owners/relay_authority.c:15`): blocked iff safety link absent or
  `safety_link_get_fault_sources() != 0`. It is NOT gated on K4 state or on a
  `heat_enable` claim. So the ESP can energize a pilot relay with K4 open.
- Therefore: whether a zone relay (or aux relay) "switches anything" when K4
  is open depends on how its external contactor/SSR supply is wired, not on
  firmware. A heater's supply is behind K4 by rule. A vent/fan/damper supply
  may be on its own ahead-of-K4 feed (runs with heat off) or behind K4 (dead
  when K4 opens). Firmware cannot tell which. See Q1.

## 3. Relay-conflict validation (both write paths, plus the others)

Single predicate in one place, in the new aux module:
`aux_outputs_relay_conflict(zones_relay_union, aux_enabled_mask)` -> mask of
conflicting relay bits (pure, host-testable). Union of relay bits across ALL
zones' `relay_mask`, including zones that are disabled or typed ON_OFF
(an ON_OFF zone keeps its relay; moving it to aux is the sec 10 flow, not an
implicit overlap).

Enforcement points, each refuses with HTTP 400 naming the relay and the
other owner (never silently masks):

1. Aux write (`POST` aux fields, sec 7): enabling relay N is refused if N is
   in any zone's `relay_mask`.
2. Zones write: `POST /api/zones` (`zones_http_post_parse.c`) and
   `zones_config_json_validate()` (`zones_config_json.c:316` already checks
   `relay_mask` against valid bits) refuse a `relay_mask` that includes an
   enabled aux relay. Put the check in the validate function so every caller
   inherits it.
3. Writers that do not go through POST /api/zones, to be enumerated by grep
   in WP-1 and each given the same check (this is the reset-one-side class:
   two stores joined by an invariant neither owns):
   backup import (`backup_import.c`), kiln package apply
   (`kiln_cfg_swap.c`, `kiln_cfg_apply` path), `load_config_preset()`,
   UART-bridge/setup-wizard zone writers. A package whose zones blob
   conflicts with the live aux set is refused at apply time, before the
   two-processor transaction starts.
4. Boot/load: if the persisted pair is in conflict (corrupt, or a path was
   missed), the aux entry is forced disabled in RAM and a sticky,
   operator-visible `aux_conflict` flag is reported; a zone is never
   silently stripped of its relay. Never both drive one relay.
5. Mechanical guard: `tools/check_aux_relay_conflict_sites.ps1` fails if a
   new caller of the zones commit/setter appears that is not on the
   allowlist of validated writers. Negative-test it (COMMON.md).
6. Run start: `profile_executor_run()` refuses if a rule targets an aux that
   is disabled/conflicted, and re-checks the conflict at start (config could
   change between save and run).

Manual relay path (`/api/relay` Danger Zone, UART SET_RELAY): ownership tag
handling in sec 6.

## 4. Does an aux output need K4 granted?

Recommendation: NO. A vent must be able to run with heating off (cooling
phase, a pause, a cool-down vent), and on this firmware's path nothing about
relay energization requires K4. So:

- Aux entries take no `heat_enable` claim (`HEAT_ENABLE_CLAIMANT_*` untouched)
  and are not counted as a heat owner toward the Pico
  (`heat_owner_active_decide()`/held mask unchanged). Energizing a vent must
  never tell the Pico a heat owner is active.
- Aux evaluates every executor tick the run is RUNNING/PAUSED (rules on
  PAUSE: hold last, same as zones; `failsafe_on_pause` false, Q4), regardless
  of whether `heat_enable_is_granted()`.
- Whether the device actually gets power while K4 is open is wiring (sec 2).
  Document it on the settings page and in `docs/SAFETY_CASE.md`; do not
  pretend firmware can fix it.

Pico-side check (WP-0, read-only): confirm no SaftyFW guard reads an ESP
relay state in a way that makes "relay commanded on while no heat owner" a
fault, and that an aux load current on a CT channel mapped for heater
current (S14/S15 style, `ct_channel_map`) cannot false-trip. If it can,
that is a wiring rule for the owner (aux loads must not pass through a Pico
CT), not a Pico change: the standing rule is that Pico limits are never made
conditional on ESP config.

## 5. Safety interactions

- **Drop on trip / E-stop: yes, default.** The aux path calls the same
  `apply_relay()` chokepoint as zones, which forces OFF whenever
  `relay_authority_on_blocked()` is true (link loss, Pico trip/fault source,
  PC fault). An E-stop or Pico trip therefore drops aux relays on the ESP
  side as well as opening K4. No new relay write path (the existing
  `tools/check_relay_authority_paths.py` must pass with no allowlist entry).
  Use the GLOBAL check for aux (there is no zone index; do not index
  `relay_authority_zone_blocked()` with an aux target). Per-zone latched
  blocks must NOT stop an aux vent. WP-0 CORRECTION: this holds ONLY for the
  per-zone guard classes with `continue_on_zone_trip=true`. A GLOBAL-class
  trip (RUNAWAY, MAX_TEMP, MIN_TEMP, SENSOR_INVALID) faults the whole run and
  asserts the safety-link fault source, and the default
  `continue_on_zone_trip=false` faults every active zone on any trip, so aux
  goes OFF in both (section 15 item a/b/e). Test the three cases separately.
- Run end/abort/FAULTED/halt/DONE: fail-safe state = OFF for all aux, bypassing
  both hold layers (same as zones, `bypass_hold`).
- Fail-safe ON unsupported (Q3): the hardware cannot honour it behind K4 and
  `apply_relay()` forces OFF under a block anyway. Offering it would be a
  setting that lies (D2).
- "No thermal-guard disable" owner decision stands: aux adds NO guard
  disable and no new guard exemption. Aux has no TC of its own and no guard
  runs on it; the zones' guards are untouched. Aux never relaxes
  `max_temp_c`.
- Load cap: aux counts toward `max_simultaneous_relays` (coil/supply current,
  same reasoning as ON_OFF_ZONE_PLAN sec 6) and is the LAST victim after
  heaters. Run start refuses if heaters' needs plus aux count make the cap
  structurally unsatisfiable (extend the existing on/off count check).
- Executor stray-relay check (`profile_executor_relay_io.c` ~line 1027:
  shadow & claimed & ~owned) must include aux bits in `claimed_relay_mask`,
  or a healthy aux relay is reported as a stray. Add a host test.
- Aux wiring rule (owner 2026-10-04): aux loads must be outside the CT path; the
  Pico keeps S3/S4 fully active and the ESP strips aux-bound relays from the
  masks it sends (WP-9, sec 15a). A miswired aux load gives a nuisance S3/S4
  trip that fails safe (SAFETY_CASE item 14).
- Welded-contact detection for aux outputs: none on the ESP (same accepted
  gap as ON_OFF zones, SAFETY_CASE item 12). Add a SAFETY_CASE row.
- Relay-cycle accounting: `relay_cycles_add()` is keyed by relay index, so
  aux transitions count for free if `cycle_count`/`relay_on` are updated on
  actuation as in the zone path (`profile_executor_relay_io.c`).

## 6. Profile-segment rule binding and the executor loop

Binding: rule `zone_index` in 8..11 = aux relay 1..4 (sec 1.3). Validation in
`validate_on_off_rules()` (`profiles_http.c`): target 0..2 -> existing
"zone typed ON_OFF" check; target 8..11 -> aux entry for that relay is
enabled and not conflicted; a temperature axis requires a valid `tc_zone`;
anything else (3..7, >11) -> 400. UI/MCP address aux by relay number; only the
wire byte uses the offset, via one shared helper
(`profile_rule_target_is_aux()`/`_aux_relay()`), used by the executor, the
validator, the editor JS and the MCP client so they cannot drift.
`profile_resolve_on_off_rule()` already keys on (segment, target); extend its
lookup, no layout change. Export/import JSON `on_off_rules` keeps the same
keys (the numeric target carries the meaning); document the 8..11 range in
`profiles_export_http.c`.

Executor (`control/profile_executor.c`, `profile_executor_relay_io.c`):

1. New `s_exec.aux[KILN_IO_RELAY_COUNT]`: `on_off_trigger_state_t`,
   `actuated_on`, `held_s`. Reset in `profile_executor_run()` exactly where
   zone on/off state resets (a resume re-zeros, per ON_OFF_ZONE_PLAN sec 5;
   never persist quasi_dwell).
2. After the zone loop each tick, `for each enabled aux`: resolve rule ->
   build `on_off_trigger_input_t` (shared builder extracted from the zone
   path; extraction must leave heater and ON_OFF-zone behaviour
   bit-identical, proven by the existing host suite unmodified) ->
   `on_off_trigger_decide()` -> actuation gate -> cap check ->
   `apply_relay()`-equivalent write for the aux relay mask via the same
   chokepoint (claim into `claimed_relay_mask`, `relay_authority_claim_mask`
   as RELAY_OWNER_PROFILE).
3. Temperature input: the measurement of `tc_zone`'s TC, read from the
   snapshot already taken outside the lock (never hold a module lock across a
   producer call). Invalid/missing reading with a temperature axis -> treat as
   fail-safe OFF and report `rule_reason`.
4. Segment index, phase, direction, quasi-dwell: from the same executor values
   the zone path uses; aux must not feed anything back into `dwelling`,
   firing stats or credit (reset-one-side class).
5. Aux does not make a run "alive": the "every active HEATER zone faulted ->
   FAULTED" rule is unchanged; a profile with `zone_mask` containing no heater
   is still refused as today. Aux is not in `zone_mask`.
6. Stack: the executor task is 6144 B (raised from 4096 on 2026-09-24,
   `profile_executor_start.c:100`; WP-0 section 15 item c) and has had four
   stack-smash panics. No new large locals; aux state lives in `s_exec`; measure and register per
   `check_stack_margin_registration.ps1` (no new task is expected).
7. Manual relay override: an aux relay with an active run is owned
   `RELAY_OWNER_PROFILE` -> manual refused, same as zones. With no run, aux
   relays are manual-reachable like any unowned relay. Q5 decided 2026-10-04:
   v1 also ships an admin-only manual on/off toggle for an aux relay outside
   a firing, gated by the same `system_mode_gate` as other relay writes (409
   while a profile is running). It rides the existing manual relay route if
   WP-0/WP-2 confirm that route is ADMIN tier and mode-gated for aux relays;
   otherwise WP-2 adds the minimum change and the URI cap rule above applies.
   WP-0 finding (section 15 item f): no new gate is needed. The toggle hooks
   `dashboard_set_relay()` -> `kiln_io_owner_command_set_relay()`, whose
   `relay_on_blocked()` (`kiln_io_owner.c:238-306`) already applies
   `SYS_ACTION_RAW_RELAY_DEBUG_WRITE` and refuses relay-ON during a firing or
   autotune with the "409 Conflict" from `system_mode_gate_http_send_refusal`.
   Danger Zone and the LCD Temperature page are the only callers today; the
   LCD must learn aux ownership. No manual hold: the manual toggle is idle-only (sec 14 item 12).

## 7. HTTP surface, web UI, LCD

URI cap: `check_uri_handler_cap.ps1` = 163 used of 170 (7 spare). Plan adds
ZERO new routes:

- Aux config rides `GET/POST /api/zones` as a top-level `aux_outputs` array
  (GET echo; POST fields `aux{N}_enabled/_tc_zone/_hystc/_minons/_minoffs`,
  N = relay 1..4), parsed in `zones_http_post_parse.c`, composed in
  `zones_http_get.c`. Alternative if the zones GET json_cap (7360 B, 895 B
  headroom at last count) cannot hold ~4x50 B. WP-0 CORRECTION: the 895 B
  figure predates ZONES_CFG_VERSION 22->26, the +24 B `relay_types` array and
  the `%.4f`->`%.9g` precision change, so it is stale and unmeasured at HEAD
  (section 15 item d); WP-4 must re-measure first: put it on
  `GET/POST /api/relay_names` family or `/api/settings`; decide in WP-4 by
  measuring cap headroom first (the httpd stack-blob rule applies: stream
  or measure, never grow a task-stack buffer blindly). Do not add a route
  unless measurement forces it; if it does, bump
  `config.max_uri_handlers` in `wifi_provision_http.c` in the same change
  and add the `ROUTE_TIER` row (admin tier; `check_route_tier_coverage.ps1`).
- Status: `/api/status` gains per-aux `commanded_on`, `on_time_s`,
  `switch_count`, `rule_reason`, mirroring the zone on/off fields.
- Profile rules: unchanged routes; `rule%u_zone` carries the 8..11 target.
- Web UI: `zones_page.html` gets an "Aux outputs (spare relays)" card listing
  only relays that no zone uses (disable a relay row with the reason if a
  zone has it; the zone card likewise greys a relay an aux uses). Fields:
  enable, TC zone, hysteresis, min on/off, live projected cycles/hr. Plain-sight
  wiring warning (sec 4: "runs with heat off only if wired ahead of K4").
  `profiles_page.html` rule editor: zone picker lists on/off zones PLUS enabled
  aux relays (labelled with the `relay_names` label); stale-target orphan
  handling already exists (`ooZoneOptionsHtml`/`ooHasStaleZoneRow`) -- extend it
  for targets 8..11, do not fork. Must pass `check_lint_pages.ps1`,
  `check_ui_responsive_sweep.ps1`, `check_page_js_tests.ps1`
  (`test_on_off_rules_editor.js` extended).
- LCD (480x320, no scrolling, no new colours, no new page): the Home rail
  already shows `s_ui_home_rail_relay_pill[KILN_IO_RELAY_COUNT]` (one pill per
  relay); an enabled aux relay's pill shows its label and ON/OFF from the
  existing relay-state colours. No new element; verify with
  `capture_lcd.ps1` and numeric sampling (CLAUDE.md), not by eye. The profile
  builder UI (`ui_page_profile_builder_*`) is out of scope for v1 (Q6).

## 8. MCP tools (kilnctrl facade; count 207 -> 210)

- `control_get_zones` also prints the aux block (read-only).
- `control_set_aux_output(relay, enabled, tc_zone, hyst_c, min_on_s, min_off_s,
  confirm=False)`: narrow writer, GET-merge-POST, refuses unless
  `confirm is True`, refuses mid-run (`system_mode_gate` 409), refuses on a
  zone-relay conflict, reads back and fails loud if anything else in
  `/api/zones` changed (same discipline as `control_set_zone_coupling`).
  Never use `load_config_preset()` for this.
- `control_set_aux_manual(relay, on, confirm=False)` (Q5, decided 2026-10-04):
  admin-only manual on/off of an aux relay outside a firing; refuses unless
  `confirm is True`, refuses mid-run (`system_mode_gate` 409) and refuses a
  relay that is not an enabled aux; reads the relay state back and fails loud
  if it did not change.
- Profile tools that author rules (`profile_live_*`, profile import) accept
  `aux_relay=N` and translate to the wire byte through the shared helper.
- Update the CLAUDE.md tool count/narrative and `docs/MCP_SERVERS.md`.
  `capability_preflight` and tuning tools must not report aux as untuned.
- `backup_export`/`backup_import`: see sec 11.

## 9. Firing records, backup, migration

- Firing record (`profile_firing_run_record_t`): do NOT widen
  `zones[MAX31856_CHANNEL_COUNT]`. Aux outputs need no IAE/lag data. If an
  on-time/switch-count summary is wanted, append a small tail
  (`aux_on_time_s[4]`, `aux_switches[4]`) under that record's own versioning
  rules (Q7: recommend skip for v1; status endpoint suffices).
- Backup export/import (`backup_export.c`/`backup_import.c`,
  `backup_json.c`): add an `aux_outputs` section. Import order: zones first,
  then aux, with the sec 3 conflict check on the combined result; a conflicting
  backup is refused as a whole (no partial write), reported as a failure
  (the existing 500-partial-write convention). Old export imports as
  "aux absent -> all disabled"; new export imported by old firmware has the
  unknown key ignored (same precedent as `on_off_rules`). Add
  `aux_out.dat` to the `GET /api/cfgfs` items (`diagnostics_http.c`
  `cfgfs_add_item`, ~line 1641) and to `docs/CONFIG_FILESYSTEM.md`.
- Migration chain: the new store is governed by
  `docs/CONFIG_MIGRATION_CHAIN_PLAN.md` from its first release. v1 has no
  step (nothing older). Add a row to that plan's 0.1 governed-store table and
  the mechanical enforcement in its sec 5 (version symbol
  `AUX_OUTPUTS_CFG_VERSION`). Reader refuses newer-than-known (quarantine
  semantics, not defaults). Rollback to older firmware: the key is unknown to
  it and is ignored; aux relays then simply do nothing, with no zone impact.
  An aux relay left energised by a rollback is impossible: no run, no owner,
  relays drop on boot.
- Kiln packages (`kiln_cfg_store`): NOT included in v1 (would force a
  `KILN_CFG_STORE_VERSION` bump). Consequence: applying a package never
  changes aux; applying one that conflicts is refused (sec 3.3). Q8.
- Factory reset (`scope=KILN` and full): clears aux (set all-disabled).
  Add to `docs/CONFIG_FILESYSTEM.md` "What factory reset does to it".

## 10. Moving existing ON_OFF zones to aux (offered, never automatic)

Today a vent is a `ZONE_TYPE_ON_OFF` zone in `zones[]`, holding `relay_mask`,
`hyst_c`, `min_on_s`, `min_off_s`, `failsafe_state`, and profile rules keyed to
its zone index. Do not auto-migrate (rules, UI and firing history reference
the zone). Offer a one-shot, confirm-gated operator action, implemented as an
HTTP POST field on the existing zones route (`move_zone_to_aux=Z`) plus the MCP
tool `control_convert_onoff_zone_to_aux(zone, confirm=False)`:

1. Preconditions: not running; zone Z is ON_OFF with a single-relay
   `relay_mask`; no other aux on that relay.
2. Atomically: create the aux entry on that relay copying `hyst_c/min_on_s/
   min_off_s`, `tc_zone` = Z's TC zone if it had one (else none); rewrite every
   stored profile's rules with `zone_index == Z` to target 8+(relay-1);
   then retype zone Z back to HEATER, disabled, `relay_mask = 0`, defaults.
   Rules rewrite touches every profile blob, so it must report counts and
   fail closed (all or nothing, read-back verified), like `backup_import`.
3. Zone Z's thermocouple slot is thereby freed for a real heater. A zone with
   a non-zero `failsafe_state` (ON) is refused with the reason (unsupported
   on aux, Q3).
4. The reverse is not provided (a zone slot is the scarcer resource).

Existing ON_OFF zones keep working unchanged until the operator converts.

## 11. Host tests (all host-only; no board)

- `test_aux_outputs_store.c` (new): defaults on all-zero blob, CRC reject,
  newer-version refuse, key length (`NVS_KEY_LEN_CHECK`), dual-write resync,
  conflict predicate truth table.
- `test_zones_http.c`: relay_mask touching an enabled aux relay -> 400 on
  POST /api/zones and in `zones_config_json_validate()`; aux POST on a zone
  relay -> 400; omitted aux fields preserve; json_cap headroom assertion.
- Backup round trip and conflict refusal; kiln-package apply conflict refusal;
  `load_config_preset()` conflict.
- `test_profile_executor_prestart.c` aux section: rule ON/OFF through the real
  chokepoint; every run-ending path drives aux OFF; global fault source drops
  aux; a zone-guard trip on zone A does NOT drop aux; aux not a heat owner and
  no `heat_enable` acquire for aux-only demand; min on/off both layers;
  chatter sweep; cap suppression last after heaters; stray-relay check clean
  with aux; invalid tc_zone reading -> OFF; resume re-zeros state. Heater and
  ON_OFF-zone outputs bit-identical with aux all disabled (existing suite
  unmodified).
- `test_profiles_http.c`: target 8..11 valid/invalid matrix, export/import round
  trip, old-format profile unchanged.
- JS: `test_on_off_rules_editor.js` aux targets incl. orphan/stale.
- Negative tests (restore by hand, force a full rebuild per CLAUDE.md): disable
  the conflict check in zones validate; disable the aux global-blocked drop;
  each must go RED. Run the full `run_all_checks.ps1 -Fast`, not `-Only`
  subsets.

## 12. Bench test (4W fixture; owner need not be present)

Relay 4 (mask 0x08) is free on the bench zones config. Dry contacts or an LED
jig on relay 4 terminals; the bench cannot validate anything kiln-scale.

1. Preflight: `get_readiness`, `safety_get_status`, no crash report, firing idle.
2. `control_set_aux_output(relay=4, enabled=True, tc_zone=0, confirm=True)`;
   read back `control_get_zones`.
3. Run a short profile with an aux rule (e.g. ON while COOLING, or ABOVE
   threshold within the fixture's 40 C rise) via `profile_live_*` or a saved
   profile with `rule0_zone=11`.
4. Verify by `io_read`/relay shadow (primary) that relay 4 toggles with the
   rule and honours min on/off; verify with CT only if a real load is wired
   through a CT channel (record which channel; otherwise CT is N/A, say so).
5. Conflict: try enabling aux on relay 1 -> expect 400; try zone relay_mask
   containing relay 4 -> expect 400.
6. Safety: with relay 4 on, inject a Pico trip by the existing
   sanctioned bench trip path (not the E-stop jumper) -> relay 4 drops;
   clear per procedure (trip_mask is `1 << (trip_reason - 1)`).
   Separately: a zone guard trip does not drop relay 4 (host-proven; bench only
   if a provocation exists in `docs/audits/guard_bench_provocations_*`).
7. K4 independence: with no heat granted (idle between segments / PAUSE), relay
   4 still follows its rule per io_read; record that this proves the ESP path
   only, not the external supply wiring.
8. Record in `docs/BENCH_TEST_LOG.md`; add a case to the bench suite
   (`docs/BENCH_TEST_SYSTEM_PLAN.md` style) once WP-3 lands.

## 13. Work packages (file ownership; no overlap between concurrent WPs)

Order: WP-0 -> WP-1 -> {WP-2, WP-4} -> WP-3 -> {WP-5, WP-6} -> WP-7 -> WP-8.
"Owns" means only that WP edits the file; a later WP may touch it after the
earlier one merges.

- **WP-0 Verify premises (read-only, S).** Sec 2 hardware facts, sec 4 Pico
  check, enumerate every `relay_mask` writer (sec 3.3), stack margins of the
  executor. Output: a short addendum to this plan. Owns: this doc only.
- **WP-1 Store + conflict core (M).** New `persist/aux_outputs_cfg.c/.h`
  (+ `_cfg_fs` bridge, mirrored from `relay_names`), conflict predicate,
  `nvs_key_check`, factory-reset hook, cfgfs status item, host tests
  `test_aux_outputs_store.c`, check `check_aux_relay_conflict_sites.ps1`.
  Owns: those new files, `persist/zones_config_json.c` (validate hook only),
  `diagnostics_http.c` cfgfs item list, `check_*` script.
- **WP-2 DONE.** Added `aux_outputs_http.c/.h` and `aux_outputs_http_core.c/.h`: GET/POST `/api/aux_outputs` and POST `/api/aux_outputs/manual` (all ADMIN tier; 166 of 175 URI slots, cap bumped 170 to 175). The pure core decides mode gate 409, field 400, quarantine 409, zone-claims-relay 409. The manual toggle is idle-only and refuses 409 mid-run via `system_mode_gate`. `POST /api/zones` and `backup_import` carry explicit aux conflict checks (`zones_config_json_aux_conflict_mask`, `zones_config_json_aux_enabled_mask`), and the zones validate provider is registered after `kiln_cfg_store_init`. PENDING-WP2 markers are removed. Not done: the `kiln_cfg_swap.c` pre-check was skipped. WP-3 hand-off: a firing start must hand aux control to the profile rule, and a run end must turn aux off; the manual route only refuses mid-run today.
- **WP-2 HTTP + zones write paths (M).** Also the manual aux toggle's route/mode-gate check (Q5). `zones_http_post_parse.c`,
  `zones_http_get.c`, `zones_http_internal.h`, conflict hooks in
  `backup_import.c`, `kiln_cfg_swap.c`, `load_config_preset` path; json_cap
  measurement; route cap decision. Owns: those files + `test_zones_http.c`.
- **WP-3 Executor (M-L, riskiest). IN PROGRESS.** `profile_executor.c`,
  `profile_executor_relay_io.c`, `profile_executor_internal.h`,
  `profile_executor_run.c` (start checks/reset), status fields in
  `profile_executor_status.c`; shared input-builder extraction; tests in
  `test_profile_executor_prestart.c`. Owns: all `control/profile_executor*`.
  Nothing else touches these files while WP-3 is open. Flash gets its own
  bench observation before anything is bound in a real firing.
- **WP-4 DONE (`5dcf89f3`).** Profile rule targets for aux, validated on every entry point (`profiles_http.c`, edit/catalog/export, import).
- **WP-5 Web UI (M).** Also the dashboard manual aux on/off toggle (Q5, refused 409 mid-run). `zones_page.html`, `profiles_page.html`, page JS + JS
  tests, lint/responsive checks. Owns the pages. Needs WP-2 and WP-4 wire
  formats frozen.
- **WP-6 LCD + status (S).** `ui_page_home_rail.c`/`ui_page_home_refresh.c`,
  `dashboard_http.c`/`.h` status fields (shared with WP-2: land `dashboard_http`
  edits in WP-6 only). Owns those. Camera verification via
  `capture_lcd.ps1`.
- **WP-7 Backup, MCP, tools, convert flow (M).** `backup_export.c`,
  `backup_json.c`, `tools/PcTools/src/kilnctrl/zones_http_client.py` + MCP
  server module, `control_set_aux_output`, `control_set_aux_manual`, `control_convert_onoff_zone_to_aux`,
  CLAUDE.md/MCP_SERVERS.md count. Owns those. Backup edits after WP-2's
  `backup_import.c` hooks merge.
- **WP-8 Docs + bench (S).** `docs/SAFETY_CASE.md` rows (aux relay: no welded
  detection; aux not behind K4 wiring note; no guard change),
  `docs/ON_OFF_ZONE_PLAN.md` cross-link, `docs/CONFIG_FILESYSTEM.md`,
  `docs/CONFIG_MIGRATION_CHAIN_PLAN.md` governed-store row, `ROADMAP.md` row
  (mark D1 overturned, link here), supersede note on the two 2026-09-14
  audits, bench session sec 12. Owns those docs.
- **WP-9 ESP strips aux relay bits from the masks sent to the Pico (S, ESP only). DONE 2026-10-04.**
  Owner decision 2026-10-04 (sec 14 item 11, refined same day): the ESP removes
  aux-bound relay bits from the relay masks it sends the Pico. No new kilnlink field,
  no protocol bump, no Pico change, no Pico flash. Today `safety_link_frames.c:391-415`
  sends the whole relay shadow (`kiln_io_get_relay_shadow()`) as
  `relay_now_mask`, and derives `relay_recent_mask` from it via
  `safety_context_update_relay_recent()`. Change: compute
  `relay_now_mask = shadow & ~aux_bound_mask` BEFORE the recent-mask update, so the recent
  mask never sees aux transitions either. `aux_bound_mask` comes from WP-1's aux
  accessor (enabled aux bindings only). The Pico then sees an aux relay as never
  commanded, so S3 stays fully active and aux current on a CT is NOT explained away
  (S3/S4 nuisance trip, fails safe, SAFETY_CASE item 14). Same stripped masks feed S4,
  so an aux vent no longer raises the S4 warn. Host tests (ESP side, in the
  safety_link_frames tests): aux bit set in the shadow never appears in either sent mask;
  a heater bit still does; aux bound but disabled is not stripped. Pre-work: grep every
  SaftyFW consumer of `relay_now_mask`/`relay_recent_mask` (`safety_core.c:1030`,
  `link_task.c:1359`, relay-feedback checks) to confirm none needs aux bits. Owns:
  `safety_link_frames.c` mask build and its host test only. Needs WP-1's accessor.
  DONE 2026-10-04: single helper `safety/safety_pico_relay_mask.h` (`safety_pico_relay_mask()`, live read of
  `aux_outputs_cfg_enabled_mask()` every PUSH_CONTEXT, never cached), the only mask-construction site
  (`safety_build_and_send_context()`); host tests in `test_aux_outputs_store.c`. Pico side checked: no relay
  feedback input exists; S3/S4/S14/S15 and CT map read only these masks, so an aux relay is invisible to them
  (an aux load on the CT is a nuisance S3 trip, by design). Residual: PUSH_CONTEXT per-zone fields still derive
  from zone relay_mask only.
  Rejected alternative: a new trailing `aux_relay_mask` byte on PUSH_CONTEXT so the Pico
  masks aux itself. It needs a protocol-version decision, a decoder accepting both
  lengths, a SaftyFW change and a Pico flash, and the Pico flash path is currently
  down (no CMSIS-DAP probe has enumerated since 2026-10-03). The ESP-side strip
  achieves the same exclusion with none of that. It does depend on the ESP being
  the only source of the masks, which it is.

Every WP: run the full `tools/run_all_checks.ps1 -ExecutionPolicy Bypass -Fast`
(not an `-Only` subset), host tests via the gate, and a
`check_00_kilnfw_target_build.ps1` target build (host tests are not a target
build). Negative-test each new check.

## 14. Owner decisions (2026-10-04)

1. **Aux supply vs K4:** varies per install; document both wirings. Ahead of K4 the load is always live; behind K4 it has no power while K4 is open. Firmware stays K4-independent (sec 4).
2. **Aux outputs:** one per relay, max 4, only relays no zone uses (recommended default accepted).
3. **Fail-safe:** fixed OFF, no fail-safe ON (default accepted).
4. **PAUSE:** hold last state.
5. **Manual aux toggle outside a run:** YES in v1 (overturns the earlier default). Admin-only, gated by the same mode gate as other relay writes, so refused with 409 while a profile is running. In scope for WP-2, WP-5 and WP-7; MCP tool `control_set_aux_manual` (sec 8).
6. **LCD profile builder rule editing for aux:** web and MCP only in v1 (default accepted).
7. **Firing-record on-time/switch counts for aux:** skipped in v1 (default accepted).
8. **Aux in saved kiln packages:** no (default accepted).
9. **Convert existing ON_OFF zones to aux:** offer the confirm-gated one-shot (sec 10), never automatic (default accepted).
10. **Bench relay 4 free of other duty:** default accepted; a profile's RELAY_IO segment targeting a relay bound to an enabled aux is refused at save.
11. **S3 blind spot (aux current on the CT):** "require aux outside CT" (owner 2026-10-04). Commissioning assertion that aux loads are wired outside the CT path; no S3 or correlation suppression for aux; the Pico must not treat an aux relay as explaining CT current; a miswire is a nuisance S3/S4 trip that fails safe. The mask change is ESP-side only: the ESP strips aux bits from the masks it sends (WP-9, sec 13, 15a).
12. **Manual versus rule:** the manual toggle applies only while idle; at firing start the profile rule takes over; at run end aux goes OFF. Resolves the "manual hold" question (sec 15f): no hold flag.
- **tc_zone encoding:** `tc_zone_plus1`, 0 = none.

## 15. WP-0 findings (read-only premise check, 2026-10-04, origin/main 78dc15b2)

No board access, no code changes. Evidence is file:line at 78dc15b2; paths are under
`firmware/` unless stated.

### a. K4 and Pico CT assumptions

- Sec 4 holds: no SaftyFW guard faults on "ESP relay commanded while no heat
  owner". `HEAT_OWNER_ACTIVE` (`KilnFW/App/drivers/safety/safety_link_frames.c:524`,
  `heat_owner_active_decide`) and `HEAT_REQUESTED` (`:502`, `:533`) are zone-derived;
  an aux relay feeds neither. K4 stays Pico-owned, so an aux load behind K4 is
  unpowered with no heat owner (wiring fact, sec 2 unchanged).
- BUT the relay masks sent to the Pico are the whole shadow, not zone-derived:
  `relay_now_mask`/`relay_recent_mask` come from `kiln_io_get_relay_shadow()`
  (`safety_link_frames.c:391-415`). Consequences the plan did not list:
  1. S3 (`SaftyFW/src/safety_guards.c:862-885`, trips on
     `any_current_present && !relay_commanded_recently`) is suppressed for
     `correlation_window_s` after any aux relay energizes
     (`SaftyFW/src/tasks/safety_core.c:1030`, `relay_recent_mask != 0`). A welded
     heater contactor with real stuck-on current would go undetected while an aux relay
     is cycling. OWNER DECISION 2026-10-04 ("require aux outside CT"): this blind spot
     is closed by wiring plus one Pico change, not by weakening S3. See the decision
     paragraph after item 4 below.
  2. S4 (warn only, `safety_guards.c:887-891`) fires when an aux relay is on
     continuously (`SaftyFW/src/tasks/link_task.c:1359`, `relay_now_mask != 0`) and
     the aux load is not on a fitted CT channel. Warn, never a trip. Resolved by the
     same decision: once the ESP strips aux-bound relays from the masks it sends (WP-9),
     an aux relay no longer drives S4 either. Until WP-9 lands, expect a spurious
     S4 warn on a vent.
  3. No false trip from aux current on a CT: S3 sees `relay_recent` true; S14/S15 are
     keyed to commanded heater relays via `ct_channel_map` (`safety_core.c` ~729,
     ~1193, masked to fitted channels). With `ct_topology=summed`, aux current adds to
     the total and can mask a dead heater element (S15 under-current). The wiring rule
     stands: aux loads must not pass through a heater CT. Not a Pico change.
  4. S9 (`TRIP_INEFFECTIVE`, `safety_guards.c:345-430`) is evaluated only after a
     trip: not affected by an aux relay.
  **Owner decision 2026-10-04, S3 blind spot: "require aux outside CT".**
  - Commissioning assertion: aux loads must be wired OUTSIDE the CT path (not
    through a heater CT or the summed CT). Add it to the commissioning checklist
    and the bench session (sec 12); it is a wiring rule, firmware cannot verify it.
  - The firmware keeps S3 (and S14/S15/S4) fully active. NO S3 suppression and NO
    correlation allowance for aux relays: the Pico must not treat an aux relay as
    explaining CT current.
  - A miswired aux load (on a CT) therefore gives a nuisance S3 or S4 trip/warn
    while it runs. That fails safe (it stops heat, never permits it) and is the
    intended signal that the wiring rule was broken. SAFETY_CASE item 14 records this.
  - Pico-side work found by WP-0 and now required: `safety_link_frames.c:391-415`
    builds `relay_now_mask`/`relay_recent_mask` from the whole relay shadow, so an
    aux relay currently counts as "a relay was commanded" and masks S3. The Pico
    must not see aux-bound relays in the heat/CT correlation masks. Decided approach: the
    ESP strips aux bits from the masks it sends, so no kilnlink field, no protocol bump and
    no Pico flash. See WP-9 (sec 13).
- "H9 CT alarm" (owner decision 2026-10-04: current seen while every relay is
  commanded off): NOT implemented anywhere. Searched origin/main and, read-only,
  `C:\wt\safedec_yldzsu` and `C:\wt\safedec_ht1`: no source, test or doc hit beyond
  unrelated SAFETY_CASE "H9" rows. S3 is the nearest existing guard. Design
  requirement for whoever builds it: define "commanded off" from the full relay
  shadow (`relay_now_mask`/`relay_recent_mask`), not zone relays, or exclude channels
  an aux load can reach. Keyed on zone relays only, an energized aux load on a fitted
  CT would false-alarm; keyed on the full mask it cannot. Cross-reference this into
  the H9 task before it lands.

### b. Writers of `relay_mask` and zone relay ownership (exhaustive at HEAD)

Persistent zone `relay_mask` (the field the sec 3 conflict check guards):
1. `KilnFW/App/drivers/http/zones_http_post_parse.c:158` (POST /api/zones; also what
   PcTools `load_config_preset()` and the narrow `control_set_zone_*` writers hit via
   GET-merge-POST).
2. `http/backup_import.c:1211` (parse) and `:2241` (commit via
   `zones_config_set_relay_mask_no_save()`, `persist/zones_config_accessors.c:311`).
   `zones_config_set_relay_mask()` (`:325`) has no non-test caller.
3. Whole-blob paths: `persist/kiln_cfg_store.c:925,1594`, `persist/kiln_cfg_swap.c:382,666`,
   `persist/zones_config_store.c` load/restore (including cfg_fs restore), and
   `persist/zones_config_migrate.c:700` / `zones_config_convert.c` (version copies, no
   new values). Defaults are all-zero.
4. Validation hook point: `persist/zones_config_json.c:316`.

Runtime caches, not authorities (must stay in step; reset-one-side class):
`control/profile_executor_run.c:816-823` (run start, also seeds `claimed_relay_mask`),
`control/profile_executor_config_reload.c:51` (mid-run mask edit; forces the old mask
off), `control/profile_executor_relay_io.c:42,572,639,935` (`claimed_relay_mask`).

Ownership predicate: `persist/zones_config_store.c:1571` `zone_owned_relay_mask`, recomputed
by `profile_relay_is_zone_owned` (`http/profiles_http.c` ~1173-1260) and by the LCD
(`ui/ui_page_temperature.c:206`). All three must learn aux, or a RELAY_IO segment and the
LCD toggle can claim an aux relay. Readers that infer "heater commanded" from zone masks
only and would therefore not see aux: `http/ota_http.c:532` (interlock `heater_commanded`;
its separate any-relay-energized fact does include aux), `bridge/uart_bridge_ext_control.c:55`.

Live relay writers (all through `kiln_io_owner`, MANUAL or AUTHORIZED):
`control/profile_executor_relay_io.c` lines 97, 575, 624, 938, 1037 (AUTHORIZED);
`control/zones_current_sweep_engine.c:1114` (mask write) and `:869` (all-off);
`bridge/uart_bridge_io.c:201` (SET_RELAY), `:287` (SET_RELAY_MASK), `:392` (all-off);
`http/dashboard_http.c:721` `dashboard_set_relay()`, called by `http/diagnostics_http.c:1265`
(Danger Zone) and `ui/ui_page_temperature.c:358` (LCD); `safety/danger_mode.c:284,332` and
`safety/safety_ceiling_sync.c` (all-off); `main_control_bringup.c:46,154` (boot all-off).
`kiln_io_set_relay*` is called only inside `owners/`. `/api/relay` was removed
(`dashboard_http.c:774`).

### c. Executor stack margin

- Allocated 6144 B, not 4096: raised 2026-09-24 after a measured 468 B free of 4096
  mid-firing (`control/profile_executor_start.c:87-100`, registered `:115`). Sec 6 item 6
  corrected above.
- Latest data (fw `eb83c1ac`, 2026-10-01, `docs/stack_margin_baseline/`): minimum free
  (`hwm_bytes`) idle 4820 B, web-UI-open 4820 B, mid-firing 3428 B of 6144 (55.8 percent,
  level OK). The mid-firing capture was taken ramping 51 s in: not dwelling and not during
  a guard trip, so it excludes the deepest paths.
- Headroom for the aux tick: about 3.4 KB measured. Keep aux state in `s_exec` and the tick
  free of large locals; re-capture a mid_firing baseline with an aux rule active before WP-3
  closes. No new task, so no new `stack_margin_register()`.

### d. GET /api/zones json_cap headroom

- `http/zones_http_get.c:95` `json_cap = 7360` (PSRAM heap buffer, not stack). The 895 B
  headroom (6465 B worst case) is documented as of ZONES_CFG_VERSION 22 and is NOT
  verifiable as current: `ZONES_CFG_VERSION` is 26 (`persist/zones_config_json.h:63`),
  `relay_types` added 24 B (`1baa828c`), and `04fb2afe` widened gains/model/coupling from
  `%.4f` to `%.9g`. Real headroom at HEAD is unknown and likely smaller than 895 B.
- The authoritative measurement is `test_zones_get_handler_max_width_response_fits_json_cap()`
  (`App/test/test_zones_http.c:6548`), which prints the measured headroom. I did not run the
  host build (heavy MSVC build; WP-4 owns the measurement). Do not trust 895 B.
- For WP-4: a compact per-relay array like `relay_types` (about 6 B per relay) is cheaper
  than 4 x 50 B objects.

### e. RELAY_IO and Danger Zone interactions

- `validate_io_segment` / `profile_relay_is_zone_owned` (`http/profiles_http.c` ~1173-1260)
  reject RELAY_IO on zone-owned relays only. An aux-enabled relay must be added to that
  refusal (matches the sec 10 proposed rule).
- `apply_relay` (`control/profile_executor_relay_io.c:22-120`) has no K4 gate, matching sec 4.
- Danger Zone (`http/diagnostics_http.c:1214-1300`) calls `dashboard_set_relay()` after a
  `danger_mode_active()` check and returns 409 on `ERR_RUNNING` and `ERR_OWNED`; an aux relay
  owned by a running profile already gets the OWNED 409.
- Terminal-state off paths iterate ACTIVE ZONES only (`force_all_relays_off`,
  `exec_enter_terminal_state` comment, `profile_executor_relay_io.c` ~700-760). Aux relays
  are in no zone, so aux OFF on FAULTED/DONE/halt needs an explicit aux force-off; it will
  not happen by inheritance. `release_profile_relay_claim()` must also release aux ownership.

### f. Manual aux toggle outside a firing (owner: YES)

- Machinery: `App/drivers/owners/kiln_io_owner.c` `relay_on_blocked()` (lines 238-306) is the
  single choke point for manual relay-ON, used by `handle_set_relay` and
  `handle_set_relay_mask`. Order: `relay_authority_on_blocked` (safety/global), OTA in
  progress, unacknowledged crash, then `system_mode_gate_blocks_relay()` which applies
  `SYS_ACTION_RAW_RELAY_DEBUG_WRITE` (`safety/system_mode_gate.c:54-77`): refuse while a
  profile or autotune runs or a backup restore is in flight. Relay-OFF is never gated.
  HTTP maps the refusal to "409 Conflict" through `http/system_mode_gate_http.c`
  (`system_mode_gate_http_send_refusal`).
- So a manual aux toggle outside a firing needs NO new gate: route it through
  `dashboard_set_relay()` -> `kiln_io_owner_command_set_relay()`. During a firing it is
  already refused by the mode gate (409) and by `relay_authority_manual_blocked_by_owner()`
  (`owners/relay_authority.c:93`; owners PROFILE/RULE/AUTOTUNE).
- Callers today: only Danger Zone (`/api/diagnostics/danger/relay`, ROUTE_TIER_ADMIN,
  `http/route_tier_table.h:338`) and the LCD Temperature page, which already toggles any
  NON-zone relay (`ui/ui_page_temperature.c:206,358`). An aux relay has no zone, so today the
  LCD would let an operator toggle an aux relay and the aux rule would then re-command it
  (two owners fighting, the case the 2026-08-27 owner note forbids). The LCD ownership test
  must treat an enabled aux relay as aux-owned, unless the manual toggle is deliberately
  routed through the aux module.
- Recommended hook for the admin-only toggle: a handler on an existing admin route family
  (no new URI; 7 spare under the cap) that calls the same `dashboard_set_relay()` path and
  reuses `system_mode_gate_http_send_refusal`.
- RESOLVED (owner 2026-10-04, manual versus rule): the manual toggle applies ONLY while
  idle. At firing start the profile rule takes over (the mode gate already refuses manual
  ON mid-run, 409), and at run end aux goes OFF (sec 5). So there is no persistent
  "manual hold" and the idle evaluator needs none: a manual state is simply dropped when
  a run starts or ends. Do not build a hold flag.

### Blocks WP-1

Nothing hard. Both former WP-1 questions are now decided (owner 2026-10-04): the manual
toggle applies only while idle and the rule wins from firing start (sec 15f), and the S3
blind spot is closed by "aux outside CT" wiring plus WP-9 (sec 15a). The 895 B cap claim
must not be relied on until re-measured (WP-4).

### g. WP-1 review corrections (2026-10-04)

- Sec 3 item 2's premise ("every caller inherits it via validate") is FALSE for two paths.
  `POST /api/zones` assigns `s_zones.cfg = tmp` (`zones_http_post.c:~499`) without calling
  `zones_config_json_validate()`, and `backup_import.c` commits through
  `zones_config_set_relay_mask_no_save()` / `zones_config_restore_snapshot_no_save()`, neither
  of which validates. Only `zones_config_json_decode_blob` and `kiln_cfg_store.c` call validate.
  WP-2 must call the aux conflict check explicitly on both paths, then delete the
  `PENDING-WP2` markers in `tools/check_aux_relay_conflict_sites.ps1` (it WARNs while they exist).
- Boot order: `aux_outputs_cfg_start()` and the zones provider registration must run AFTER
  `kiln_cfg_store_init()`, not next to `relay_names_load()` (zones union is 0 there, and the
  package re-import can still change `relay_mask`; a provider registered earlier would fail
  that import and clear the active package id).
