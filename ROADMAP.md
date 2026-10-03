# kilnCtl Roadmap — both processors

> **Status:** planning · **Last reviewed:** 2026-10-02. The dated status log
> that used to fill this block (2026-08 through 2026-09-28, about 2300 lines) was
> moved verbatim to `docs/ROADMAP_STATUS_LOG_2026-09.md`, together with every
> index row this pass removed or replaced; nothing was deleted. Landed since
> 2026-09-28, in git order: the `network_get_ip_config`/`network_set_ip_config`
> MCP tools; `debug_reset` verifies the ESP answers afterwards; the first live
> `ota_matrix_run` (OT-B01 PASS on rerun, `sw_reset` of both processors does not
> latch S6a); stack-margin raises for `info_uart_bridge` and `lvgl` plus new SK
> baselines; the LCD Edit-firing page and the LCD Discard/Save as/Overwrite
> decide page (LCD-22..25 PASS on the bench); the Pico's reported active slot now
> decides the next auto-update slot; `flash_recovery` (JTAG write of the
> `recovery` partition only); and the recovery image rework waves W1-W4
> (hold-off relays, LCD status page, streamed ESP upload, Pico relay from a PSRAM
> copy) plus the recovery-image MCP tools (`40c04dfd`). **Pending right now:**
> W5, the recovery bench verification with the owner present (in progress), and
> everything hardware-gated in the index below. The completed plans
> `docs/CT_CHANNEL_MASK.md` and `docs/SCENARIO_SIMULATION.md` lost their `_PLAN`
> suffix 2026-10-02.

The system is two firmwares that must agree with each other:

- **`KilnFW`** — ESP32-S3 main controller. Thermocouples, SSR heater outputs, PID,
  profiles, Wi-Fi, web GUI. Partly built and partly verified on hardware.
- **`SaftyFW`** — RP2040 safety processor (A1). Independent overheat and fault
  detection, owns the mechanical pilot relay K4. **Built and running on real
  silicon**; every guard input is now produced, and what remains is
  commissioning values plus the hardware-gated trip proofs.

They talk over an isolated UART (a digital isolator, U6, as of 2026-08-25;
previously an optocoupler pair). That link, and the rule that **the safety
processor must be alive for the main processor to heat**, is what makes this one
project rather than two.

---

## Index of what is left, by complexity

Every open item in this file, in one table, so the size of the remaining work
is visible without reading 1000 lines. **This is an index, not a second copy of
the plan** — each row points at the milestone that owns the detail, and when
the two disagree the milestone is right. Complexity is effort *once the item is
unblocked*; an XL that is blocked on a decision is still one sentence of your
time away from being startable.

| Size | Means |
|---|---|
| **S** | An hour or less. One file, or one number, or one question answered |
| **M** | A session. Several files, or a bench procedure with a known script |
| **L** | Multiple sessions. Touches persisted data, or a subsystem, or needs its own test pass |
| **XL** | A project. New hardware in the loop, or an unretired risk with no reproducer yet |

### Blocked on you — nothing in the code can answer these

| Size | Item | Where |
|---|---|---|
Six of the nine questions from 2026-08-28 became work rather than questions —
see [M12](#m12--commissioning-the-operator-can-actually-do--opened-2026-08-28).
Eight more were decided by the owner on 2026-09-05. What is still genuinely
open is short:

| Size | Item | Where |
|---|---|---|
| **M** | ~~CTs — deferred, 2026-09-05~~ — superseded: a summed CT was fitted on GPIO28, 2026-09-05. **CT commissioning, 2026-09-06** — owner wants user-entered probe rating (any rating; real probes 10-100 A, bench probe 1 A), user-entered or auto-measured idle offset, real-amps readout, and a `ct_topology` (per_zone / summed) so S14 works on the one summed CT. Plan with steps 0-6: `firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md`. **Steps 1-5 done** (`51c084f`, `7175078`, `9374e8a`, `8a124c4`, `b8f0f47`, `8239b87`: editable calibration fields, auto idle-offset over the wire, `ct_topology`/S15/summed sweep on both Pico and ESP, real-amps display on web/LCD/PcTools, docs). **Step 0 done, corrected 2026-09-18: this row was stale.** The noise-floor capture was actually taken and recorded 2026-09-06 (`firmware/SaftyFW/docs/CURRENT_SENSE.md`'s "Measured noise floor — RUN 2026-09-06" section: 262 samples/60.1 s via `safety_capture_ct_counts()`, fitted channel std ≈ 4.678 counts ≈ 3.8 mA), and that doc's own completion checklist already marked step 0 closed — this row alone had not been updated to match. ~~**Step 6** (bench run with the owner to actually arm S14/S15)~~ — **CLOSED by owner decision, 2026-09-19: "software walkthrough only."** The CT-lead-pickup investigation (`8ae0ca6c`) found the fitted channel's idle noise (std 9.329 counts vs. 0.180/0.223 on the two unfitted channels) traces to CT-lead pickup, not sampling rate, and would need a bench oscilloscope to chase further — moot now that the owner has decided not to attach a real load to this fixture. The software walkthrough ran end to end on the live board 2026-09-19: commissioning read back (`ct_installed=1`, `ct_topology=1` summed, channel 2 calibrated `k_ct_v_per_a=1`/`zero_counts=63`/`gain=0.715`, `i_normal_a` unmeasured on all channels), live status confirmed S14/S15 both DORMANT from the board's own description, and the current sweep (`zone_current_sweep_start`) ran all three zones and returned its expected **INCONCLUSIVE** verdict (`summed_unmeasured_mask=7`, no `i_normal_a` pushed) because this fixture's ~23 mA/zone sits below the firmware's 45 mA noise floor. A post-sweep re-read confirmed nothing changed (commissioning byte-identical, no trip latched, relays off). Full detail: `firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md`'s "Step 6 CLOSED" section. **This closes the whole CT commissioning step list (0-6).** A real load (or a different bench fixture) is the only thing that can ever let S14/S15 arm — that remains a hardware-gated, one-line follow-up, not further code. | `firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md`; `docs/CONTACTOR_FEEDBACK_OPTIONS.md`; `docs/audits/ct_sampling_mains_aliasing_review_2026-09-18.md`; M5 |
| — | ~~LittleFS for flash writes, 2026-09-06~~ — assessed **not adopted** for log retention, then **superseded 2026-09-07**: endurance review confirmed no wear problem exists either, but the owner directed the migration anyway for architectural reasons (structured, inspectable, backup-able user data). In progress — zones config, profiles, and prefs dual-write to a new `cfg` partition; see `docs/CONFIG_FILESYSTEM.md` for state and open items, `docs/LITTLEFS_ASSESSMENT.md`/`docs/FILESYSTEM_USER_DATA_PLAN.md` for the history. **RP2040 `config_store` A/B sectors (flash_endurance_review_2026-09-07.md R2): implemented AND FLASHED, `b7af9ebe` 2026-09-08** (bench-verified: commissioning config read back byte-for-byte across the migration, CRC unchanged) — this row previously read "NOT YET FLASHED" and was stale by a day. **The separate in-RAM-cache defect found 2026-09-09** (`s_cached_record` read with no synchronisation across cores, `safety_config_version` observed changing mid-read during a live heating run), fixed by a seqlock (`b202fe56`, barrier fix `5671ee03`, then a writer-owned-fallback correction `cb1ba325` after review found the first fix's own fallback snapshot could itself race two readers, 169+ host tests including a torn-read reproduction: 20,945/190k writes without the fix, 0 with it) — **IS now flashed** (`ae23aba4` 2026-09-09, `b88ea6ba` 2026-09-10; corrected 2026-09-14 roadmap truth-up — live `safety_get_fw_version` reads Pico build `d957d5fd`, far newer than either landing commit, commissioned). This row previously said "none of the three seqlock commits have been flashed" and was stale. See `docs/CONFIG_FILESYSTEM.md` for detail. | `docs/CONFIG_FILESYSTEM.md` |
| **XL** | **Whole-kiln setup wizard — NEW, owner request 2026-09-08.** One web page, `/setup`, guiding a new owner from a blank board to a kiln `/api/readiness` reports ready: network/time/units, zones + thermocouples + types, zone type (HEATER vs ON_OFF_DEVICE), relays and names, zone commissioning limits, the safety processor's own commissioning, current sensing + CT verification under load, autotune and the coupling matrix. Modelled on `safety_commissioning_page.html`'s guided flow (stepper, consequence-bearing radio cards, read-back-verified commit) and backed by the existing `/api/readiness` checklist rather than a new model. **DONE (2026-09-09).** All 13 wizard steps and all 11 implementation steps shipped; progress persisted in NVS (not `cfg`) so a filesystem problem cannot lose it. Two steps apply heat (CT sweep, autotune) and five need the owner present. **Follow-up (2026-09-19) resolved (2026-09-21):** the coupling-matrix-step-removal rewrite (`3e9ee5f5`) landed and merged into `origin/main`, and `c339ad16` dropped the temporary `check_no_bench_text_in_ui.ps1` whole-file allowlist for `setup_wizard_page.html` in the same rebase; no allowlist entry for that file remains today. | `docs/SETUP_WIZARD.md` |
| — | **On/off device zones — owner request 2026-09-07, decisions settled 2026-09-14.** A zone may drive a non-heater on/off device (vent, damper, fan, water feed) instead of a heating element, switched by per-segment rules on ramp phase / direction / temperature / time, with a stalled ramp counting as a dwell. **Not "design only" — steps 1-8 of the 9-step plan are shipped and host-tested** (`d58492c9`, `3d740f78`, `dd1d6ada`, `172e3081`/`b46c120c`, `bf1db47f`, `83c8b28b`/`e8e32c7a`, `be27d461`); this row previously understated remaining work by ~8 steps. Safety core: guards 1/2/3/4/9 disabled for such a zone, per zone (`docs/ON_OFF_ZONE_PLAN.md` sec 1's guard table) — guard 1 (HEATING_FAILED) would otherwise false-trip on a *correctly working* vent, since "duty high, temperature flat" is both its trip condition and the device's normal signature. **2026-09-14: owner asked to decouple an on/off device from the 3-slot heating-zone array so it binds a spare relay instead — investigated and found genuinely large** (the zone array is hard-sized at `MAX31856_CHANNEL_COUNT` = 3 everywhere: guards, coupling matrix, firing records, persisted `zone_cfg_t`, HTTP surface; widening it needs a `ZONES_CFG_VERSION` schema bump, a frozen prior struct, a converter and a CRC check — `docs/audits/on_off_spare_relay_binding_2026-09-14.md`); **not implemented**, per D1 of `docs/audits/on_off_zone_decisions_2026-09-14.md`, which the owner has not yet reconsidered against this new request. Two engineering gaps from that decisions doc closed 2026-09-14: `adaptive_tune`/`firing_score`'s firing-stats snapshot now skip on/off zones as training data (`adaptive_tune.c`, `profile_executor_firing_stats.c`), and `docs/SAFETY_CASE.md` now carries the guard-3 coverage gap and the `max_temp_c == 0` relaxation. **Remaining open item: step 9, a supervised bench session with dry contacts — no on/off zone has ever actuated a physical relay.** **2026-09-20: step 5b added** — `profiles_page.html` previously had no editor for a profile's on/off rules at all (it only echoed `on_off_rules` back unchanged on save); an "On/off devices" section now lets an operator add/edit/remove per-segment rules from the browser, wired into save/load/preview, plus a real fixed gap (`rule%u_temp_source` was never sent, so any saved temperature condition was silently inert). **Follow-up 2026-09-20:** fixed a silent rule-destruction defect in that same editor -- a stale/absent zone used to serialize as an empty value and get silently dropped on save; it now round-trips via a flagged orphan option and save is refused client-side while any row is stale, plus a widened JSON byte budget and a new `check_page_js_tests.ps1` standing check; `4fe0a38d` then closed the Opus review nits on that pass (bounded temp_c import, stderr-safe check wrapper). | `docs/ON_OFF_ZONE_PLAN.md` |
| S | S8 sanity rate — tool added `c49bb0e9`: `safety_set_rate_guard()`/`safety_get_rate_guard()` now expose config_store 0x0204/0x0205 over `POST`/`GET /api/safety/commissioning` (mirrors `safety_set_ct_cal`'s confirm-gated, read-back-verified pattern; refuses off `confirm`, a running firing/autotune, or an ARMED relay). **Corrected 2026-09-14 roadmap truth-up: this row said the guard "remains DORMANT (0)" — live `safety_get_rate_guard()` reads `max_rate_c_per_min=20 (ARMED)`, `rate_window_s=60`, i.e. already armed at a hand-set bench value, not the docs' 33.3 C/min (2x-fastest-rule) default.** **Open owner question, not resolved here: 20 C/min is tighter than the documented 2x-fastest rule and could nuisance-trip a 900 C/hr zone** — whether to raise it to 33.3 C/min (or the auto-derived value once the coupling matrix is re-identified, see the ninth/tenth-sweep notes above) is an owner decision, left open. | M3 |
| — | High-temperature validation firing — closed 2026-09-05, `94b1a2a` confirms ff_hold infeasible above 62 °C on hardware. | `PID_EXPANSION_PLAN.md` §3.6i |
| **S** | **Display items needing the owner's own hands/eyes, 2026-09-04.** Three separate (touch corner accuracy CLOSED `f028e2f` — see M1): (1) a residual blue tint on the ST7796 panel with every firmware cause eliminated by measurement — needs the owner's eye, or a colorimeter, or a second unit; (2) wake-on-touch, first-touch-swallow and error-dismissal behaviour on display power, which need a finger on the actual glass; (3) the STOP-block 5V I2C hazard measurement at meter-module pins 10/12, still not taken. | `DISPLAY_ST7796_PLAN.md` §4 |
| **M** | ~~Field-update hardware exercise~~ — **ESP half done 2026-09-05**: OTA into `ota_0` + rollback both verified on the bench (PID gains byte-identical before/after, no heat, no firing). **Pico half attempted 2026-09-06**: a raw `.bin` (`arm-none-eabi-objcopy -O binary` on `SaftyFW_slotA.elf`, no header-packaging step needed — the ESP builds `UPDATE_BEGIN`'s header itself) staged and the relay started, but the Pico refused `UPDATE_BEGIN` ("a safety trip is pending") before any flash write — a real Pico-side interlock the ESP's own cached status did not show. The actual over-the-wire transfer is still unexercised. **Updated 2026-09-18: the bootloader/metadata gap is closed** — the bench Pico now boots through its two-slot bootloader (slot A active, `KLN1` metadata present) — **and a fresh attempt got further and failed differently**: the RP2040 hardware-watchdog-reset while erasing the destination slot, so the ESP failed the relay at its 15000 ms erase timeout. The failure was safe (relays off throughout, no trip latched, configuration unchanged). See the M8 item below and `docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md`, plus `firmware/CommonFW/docs/UPDATE_PROTOCOL.md` "Hardware exercise 2026-09-05". **2026-09-20, in source (not yet exercised on the wire):** the SaftyFW build now emits per-slot raw images `SaftyFW_slotA.bin`/`SaftyFW_slotB.bin` (position-dependent, slot A 0x00011000 / B 0x000E1000), and the Pico's `update_task` rejects an image whose reset vector does not point inside the destination slot (`update_task_slot_linkage_check()`, state `UPDATE_TASK_STATE_REJECTED_SLOT_LINKAGE = 8`, no protocol bump) — `5310dd78`, `7fb0b15f`, `6d3d0ada`. The ESP-side mirror of state 8 and the embedded-image boot path are in flight under the M-row below. | M8 |
| XL | **Two accepted risks in `docs/SAFETY_CASE.md`, new 2026-09-04: nothing currently mitigates either.** (1) Whether the two processors' independently-"healthy" verdicts are actually *correct* rather than merely self-consistent — e.g. both could be reading a shared, physically-faulted thermocouple wire. (2) Whatever sits downstream of both relays (a mechanical failure past K4) has no mitigation beyond K4 itself. Not a code gap — no reproducer exists and none is proposed; owner decision on whether/how to mitigate | `docs/SAFETY_CASE.md` H5, H9 |
| — | **Guard evidence is mostly host-tested, not hardware-verified.** Of ~20 tracked guard-level claims, 19 are host-tested and only **3** are hardware-verified (**corrected 2026-09-15 roadmap claim audit** — the three rows are S5's *hardware fit*, S5's *masking-before-fit* finding, and KilnFW thermal_guard guard 6. The E-stop polarity fix was named here as the third and is **not** one: `SAFETY_CASE.md` §4 classes S7 as host-tested and negative-tested. The count was right, the attribution was wrong, and it credited the E-stop path with evidence it does not have) — everything else, including all of S1–S4/S6–S14's trip logic and KilnFW guards 1/2/3/4/5/7/9, has never been provoked on real silicon | `docs/SAFETY_CASE.md` §4 rollup; `GUARD_TEST_MATRIX.md` §3 |

**Current owner-dependent items, 2026-09-09 sweep** (nothing in the code can close these; listed together so they don't have to be re-derived per session):

| Item | Blocks | Where |
|---|---|---|
| Bench webcam re-aim + LCD colour verification (numeric pixel sampling, not eyeball) | Display power / colour items above | `CLAUDE.md` "Camera aim (2026-09-06)"; `DISPLAY_ST7796_PLAN.md` §4 |
| ~~`iter_tune.c` wire-vs-delete decision~~ — **decided 2026-09-08: keep it, redesign it.** Three open questions for the owner in `ITER_TUNE_REDESIGN_PLAN.md` §9 are settled by that section itself (auto-snapshot anchor, 6-trial budget, bench-fixture-only scope). **Steps 1, 2 and 5 landed 2026-09-09** (`8f80a4de`, three latent defects found in review fixed same day, `249ce287`): `control/firing_score.c`/`firing_compare.c` plus a rewritten `iter_tune.c` decision core (old whole-firing IAE path deleted, not left dual), validated by a Monte-Carlo sim harness (`sim_iter_tune.c`) — 24/24 converged, 660 null comparisons 0% false-accept, 660 mismatched-plant runs 13 better/0 worse/0 cage violations. **Corrected 2026-09-14 roadmap truth-up: step 3 (the G1-G4 sim-harness gaps, `e0d2e006`) and step 4 (the §6.5 credibility gate, `225d4b91`) also landed 2026-09-09, and step 7's write-surface guard (`check_iter_tune_write_surface.ps1`, `f3fcd597`) too** — `docs/ITER_TUNE_REDESIGN_PLAN.md` itself was corrected 2026-09-10 to say so; this row never followed. **Still open: the credibility gate FAILS against a real recorded firing for a currently-unknown reason** (two explanations investigated and retired — see the 2026-09-11 sweep note above, do not re-propose either), plus the noise-floor artifact and step 9's first hardware trial (owner present). Step 7 (persistence/HTTP surface) landed (`7e754997`, opus-review fixes `bb6d3947`); acceptance is now MET as of the 2026-09-23 schema-migration follow-up pass (`f3925704`, `5cc04518`) -- see docs/ITER_TUNE_REDESIGN_PLAN.md row 7 for detail. It bumped `wifi_provision_http.c`'s `max_uri_handlers` 165 -> 170 for headroom in the same commit, per `check_uri_handler_cap.ps1`. **CLOSED 2026-09-23**: `restore_commissioned`'s check-then-apply race against a concurrent autotune start is fixed by a per-zone external-write reservation (`f82ca846`), the same reservation now also gates `autotune_engine_accept()`'s own write path and is test-covered end to end (`0f6dd8f9`, `50769ef0`) -- `adaptive_tune_model.c`'s SIMC-refine write and `uart_bridge_ext_control.c`'s `CONTROL_CMD_SET_ZONE_PID` are deliberately left ungated (different lock domain / no autotune awareness). **Shadow mode (step 8): wired, awaiting five firings** (`control/firing_shadow.c/.h`, pure module hooked into `profile_executor_firing_stats.c`'s existing per-tick/per-firing calls, never calls `iter_tune_*`, no gain-write API, own `shadow_tune`/`sdwblob` NVS namespace, summary surfaced read-only on `GET /api/iter_tune/status`) — see `docs/ITER_TUNE_REDESIGN_PLAN.md` row 8 for detail. | `docs/ITER_TUNE_REDESIGN_PLAN.md` §8/§9 |
~~CT commissioning steps 0 and 6 (noise-floor capture, bench run with the owner)~~ — **both closed.** Step 0 was closed 2026-09-18; step 6 closed 2026-09-19 by owner decision as a software walkthrough only (see the M-size CT commissioning row above). S3/S4/S9/S14/S15 stay DORMANT (`i_normal_a not measured`) — that is now expected to stay true on this bench permanently, not pending further work; only a real load can change it. | `firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md`; M5 |
| `abs_max_temp_c` must be raised **Pico-first, then ESP**, before a real (non-bench) firing — and the Pico's ceiling must never end up tighter than the ESP's | Real-kiln firing readiness | `docs/SETUP_WIZARD.md`; `docs/ON_OFF_ZONE_PLAN.md` |
| S8 is ARMED at a hand-set 20 C/min bench value, not the docs' 33.3 °C/min default (corrected 2026-09-14, see M3 row above) — whether 20 is right for a real kiln, or should move to 33.3 or an auto-derived value, is an open owner decision | S8 (rate-of-rise) real-world accuracy | M3 row above; `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md` |

### Software, doable now — no hardware, no decisions

| Size | Item | Where |
|---|---|---|
| M | **KilnFW web POST handlers off the shared httpd worker** (owner approved 2026-09-25; KilnFW TODO.md 10.14 "Web side"). A1 landed 2026-09-25: `http_async_job` single-flight helper (`httpd_req_async_handler_begin`/`_complete`), first used by `ct_auto_zero_post_handler()` (about 10-15 s today), host-tested and negative-tested. **2026-09-25 fix-then-push review**: every existing body and status is unchanged, plus one new synchronous refusal for a concurrent second POST; corrected the stack ceiling to a real measured 3312 B (was a wrong hand-borrowed 2736 B, stack raised 4096 -> 6144 B); serialized `commissioning`/`relay_type`/`ct_cal`/`ct_trim`/`rate_guard_auto` POST handlers and `backup_import` against A1's async window; fixed a `s_task_handle`/`s_busy` clear-order race. W1 (`wifi_prov_owner` replying before its blocking scan and join) landed, pending bench verification. A2 (`bench_preset` onto A1's helper) landed 2026-09-25: same `http_async_job` pattern, byte-identical response bodies, stack unchanged (6144 B declared / 3312 B measured lower bound), `.dram0.bss` 99672/101000 B unchanged (the feature is `#if CONFIG_KILNCTL_DEV_TOOLS`, off on the real board), host-tested (271/271 checks, previously uncompiled/untested by the host suite -- reached via a `CONFIG_KILNCTL_DEV_TOOLS` host-stub override) and negative-tested. A4 (`backup/import` onto the same helper) landed 2026-09-28, driven by a real bench A4 finding (all three restore attempts timed out client-side, board unresponsive to other requests for several polls after each): mode-gate/interlock/busy checks and header reads stay on `httpd_worker`, the body read plus `backup_import_apply()`'s two-pass validate-then-commit move to `backup_import_job()` on the same `http_async_job` helper (6144 B), byte-identical response bodies/status codes (`classify_refusal()` needed no changes), `.dram0.bss` unchanged (99720/101000 B, ctx is heap-allocated), URI route count unchanged (163/170, no new route -- this is A1/A2's same-connection-reply shape, not a job-id/poll shape). Fixes the "other requests starve" symptom, not the restore's own duration, so `backup_import_http_client.py`'s client-side timeout was separately raised 30s -> 90s; neither the client nor the MCP tool retries a timed-out import (the bench's first attempt had, in fact, already committed despite the client timeout -- a caller must read the config back, never re-POST blindly). **Bench responsiveness check done 2026-09-28** (was the plan's own open item): ESP flashed `c021ad96` (verified); two no-op `backup_import` merge round-trips (~61 s each, "ok - restored") ran concurrently with a `GET /api/readiness` poller at 1 Hz for 90 s -- 66/66 OK, max 715.8 ms, avg 359.5 ms; zones/readiness/crash/trip state unchanged. The ~61 s is the ~48 per-setter `nvs_save()` calls in the commit loops plus the Pico round trips (safety ceiling guard, `i_normal_a` stage/COMMIT_CONFIG/read-back), not a symptom of the A4 move. **Batching those saves landed 2026-09-28** (`7b107411`, with stack/check follow-ups in `b4bad2c7`/`c021ad96`/`5d0a2756`/`c22ff081`): `backup_import_apply()`'s zone/timing-profile commit loop now uses the `_no_save` setter variants and a single `zones_config_save_now()` call at the end instead of one `nvs_save()` per field, with `zones_config_get_full_copy()`/`zones_config_restore_snapshot_no_save()` giving an atomic RAM rollback on a mid-batch failure; the async job also moved off `httpd_worker`'s stack onto `http_async_job`'s own task (8192 B declared after a stack-budget-checker fix). Host-tested (single-save-on-success, full-snapshot-restore-on-mid-batch-failure, round-trip save count) and negative-tested by hand. A bench re-measurement of the resulting restore time has not been done. Safety-link `link_reply_us` timeouts (15) accumulated during the window with no trip -- benign, not an open finding: that counter counts status-push gaps, not failed replies (redefined 2026-09-10, `docs/audits/safety_link_get_status_timeout_counter_2026-09-10.md`), and the import's `safety_exchange` calls contend for `xact_lock`, so a few gaps are expected under this load. One of the earlier restore attempts that timed out client-side had zeroed the bench's cross-zone coupling matrix (`fee835fa`/`b4bad2c7`); it is now restored via the new narrow `control_set_zone_coupling` writer to z0=[0,25.42,24.52], z1=[12.44,0,28.69], z2=[8.08,10.81,0]. A3 `crash_report/clear` landed 2026-10-02 (bench measurement: the synchronous coredump erase stalled httpd about 3.4 s, concurrent `GET /api/status` 2067 ms and `GET /api/readiness` 1378 ms; after-change stall unmeasured on bench, expected near zero since the erase now runs on the `http_async_job` task). Same one-POST/one-response wire contract, new 503 busy reply, `GET /api/crash_report` gains `clear_in_progress` on its `present:false` reply; no new route. All slices of this line are done. No route, API or auth-tier changes beyond that. | [`docs/HTTP_POST_OWNER_MIGRATION_PLAN.md`](docs/HTTP_POST_OWNER_MIGRATION_PLAN.md) |
| L | **Every fault says what was detected and what to do** — a standing rule, not a closing milestone, so it never fully closes: applies to every fault surface added from here on. All of S6a's own checklist items landed 2026-08-28 | M13 |
| L | **CT clamp attribution — built and verified under both topologies (individual per-zone CTs and shared/summed), re-confirmed 2026-09-19** (host suite 2499/2499, `run_all_checks.ps1` 113/0/0). Nothing software-only remains. Pending, and hardware-gated only: any real PASS or FAIL verdict, and the true envelope settling behaviour at real current — both require a real kiln; this ~4 W bench can only ever produce INCONCLUSIVE, by design. See `docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md`'s status line. | `docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md`; `docs/CT_CHANNEL_MASK.md`; `firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md` |
| XL | **Source layering + hardware abstraction** — `drivers/` reorg applied in `9f18ca5` (2026-09-05); HAL Phases 0-4 all done (every interface has a real backend + host fake, every named consumer migrated, include-boundary enforcement is strict, `esp_random.h` classified 2026-09-06). **Closed 2026-09-16: the hardware timing re-check.** All three named measurements (safety-link reply, display frame time, thermo read latency) now have hardware numbers — safety-link reply against the pre-existing 345 ms budget (max 340 ms observed, thin margin, see the 2026-09-14 composition correction on what that counter actually measures); display/thermo have no prior figure to compare against and are recorded as fresh baselines against the ESP32-S3's 300 ms interrupt-watchdog ceiling (max 81 ms / 57 ms observed, comfortably under). No measurable cost from the HAL indirection against any of these bars. Full detail: `docs/HW_ABSTRACTION.md` | M16; `docs/HW_ABSTRACTION.md` |
| **L** | **`iter_tune` redesign — see the M-row above (this table, "iter_tune.c wire-vs-delete decision") for current step status; consolidated here 2026-09-14 roadmap truth-up to remove a duplicate that had drifted (this row still said "steps 3-4 and 6-9 remain design-only" after both plan doc and code had moved past it).** Design background kept: owner decision 2026-09-08 to keep and redesign rather than wire `control/iter_tune.c` as-is (`3bf773af` superseded); score measures matched profile segments (ramp lag in seconds, dwell-entry overshoot, steady dwell RMS) against the target profile rather than whole-firing IAE, validated first in simulation by extending `firmware/KilnFW/App/test/sim_plant.c` with the four gaps (PWM window, actuation lag, MAX31856 quantisation, real measured plant/coupling constants) — *not* the deleted `SimFW`/`kilnsim`. 10 ordered steps (0-9), no kiln time before step 8. See `docs/audits/consumer_without_producer_2026-09-06.md` for how the module got here | `docs/ITER_TUNE_REDESIGN_PLAN.md`; `PID_EXPANSION_PLAN.md` |
| L | **ESP32-S3 OTA: single 8 MiB `app` slot plus a ~1.9 MB non-firing `recovery` image.** Steps 1-3 landed (recovery project builds, `partitions.csv` carries `app` and `recovery`, `flash_firmware()` targets `app`, `flash_recovery` writes `recovery`); the bench board already boots from this table. The plan records no result for the migration-flash verification (step 4). Pending: step 5 (exercise recovery deliberately; this is recovery W5), step 6 (the esptool last-resort path, never run on this board), step 7 (delete in-app RECOVERY MODE and retarget the `boot_guard` counter to boot recovery, which must not precede 5 and 6), step 8 (documentation cutover). | M8; `docs/OTA_SINGLE_SLOT_PLAN.md` |
| M | **Recovery image rework -- owner decisions 2026-10-02.** W1-W4 landed (build trims and optional PSRAM, relays held low and LCD status page, validated streaming ESP upload with status/exit/wifi_reset routes, Pico relay from a PSRAM copy, all host-tested and target-built only), and the recovery-image MCP tools landed in `40c04dfd` (plus `flash_recovery`, `e7e8724f`). **Pending: W5**, bench verification with the owner present: `flash_recovery` dry run, real write, a proven recovery boot, then the Pico relay DATA pace and target-slot guard. In progress 2026-10-02. | `docs/RECOVERY_IMAGE_PLAN.md`; `docs/OTA_SINGLE_SLOT_PLAN.md` |
| **M** | **ESP application auto-updates the Pico's firmware on boot -- owner requirement 2026-09-16.** In source and flashed: both `SaftyFW` slot images embedded in the ESP build, boot-time identity comparison, persisted 3-attempt budget (exhaustion is a `/readiness` warning, only NO_IMAGE and CHAIN_GAP refuse firing), the Pico's reported active slot wins slot selection (2026-10-02), `check_embedded_pico_image_fresh.ps1`, identity scoped to the SaftyFW source paths. The erase-watchdog and CRC defects that blocked the relay are fixed. Pending: a real ESP-driven Pico OTA on hardware end to end (the bench Pico runs a flat image; installing the two-slot bootloader there is NO-GO until the three items in plan section 11 resolve), so the auto-update path is inert on this bench unit and the wire-driven slot selection is bench-unverified. | `docs/PICO_AUTO_UPDATE_PLAN.md`; M8 |
| **L** | **One-step-at-a-time config migration — owner requirement 2026-09-16:** "Each new fw should support migration of the nearest configuration forward allowing a one way one step at a time config update path". From the next schema bump onward, a release that bumps a persisted config version ships exactly one new step (N-1 -> N) and carries **only** that step, so a board more than one version behind cannot read its own config and must be upgraded one release at a time (settled by the owner 2026-09-16 as "one way one step at a time"). **Settled by the owner the same day: forward-only, NOT retroactive** — `convert_versioned_blob_to_current()` (`firmware/KilnFW/App/drivers/persist/zones_config_migrate.c`) stays as the pre-v26 tail, unchanged, and the chain's input floor is v26, so the step table is empty until `ZONES_CFG_VERSION` moves to 27. `ZONES_CFG_VERSION` is **not** bumped by this work. Governs the zones config, the kiln-config slots (`KILN_CFG_STORE_VERSION`, already a real two-step chain and the shape to copy), fire profiles, and the RP2040's `CONFIG_STORE_FORMAT_VERSION`; not the boot-critical NVS items nor the `cfg` partition bridges, which re-use the same versioned blob. Carries three dependent pieces: a per-step `calibration_missing`/`fields_set` policy on the Pico — the actual mechanism that lets a `CONFIG_STORE_FORMAT_VERSION` bump carry the CT normals `i_normal_a` forward instead of forcing recalibration; a firing-blocking quarantine for a newer-than-known blob, closing the "runs on firmware-default PID gains after a rollback, unannounced" hazard; and `tools/check_config_migration_steps.ps1` (landed 2026-09-17 for the zones store; **extended 2026-09-19** to also require a current-version step/macro for kiln-config slots, fire profiles, and the RP2040 store — narrower than the zones rule set for those three, see `docs/CONFIG_MIGRATION_CHAIN_PLAN.md` §5.1 for exactly what's still deferred per store; **extended again 2026-09-19** to teach the fire-profiles rule the frozen-input `_Static_assert`(sizeof)/`crc32`-last-field discipline that store's code already had but the check did not yet inspect — `test_check_config_migration_steps.ps1` now 22 assertions, up from 18), failing a build that bumps `ZONES_CFG_VERSION` without its step, its frozen-struct asserts and its captured-blob test. Testing is deliberately asymmetric: the existing tail keeps the coverage it has, every new step owes a real captured blob at its input version from the day it lands. All four owner decisions are now settled (one step only; steps expire past a fixed age; quarantine firing; mandatory pre-bump blob capture). **The blocking prerequisite (a migrated blob was never written back on the ordinary `nvs_load()` load path) is CLOSED, corrected 2026-09-17** — `d3f74d67` persists a migrated blob immediately, read-back verified, and `6985c89b` surfaces a write-back verify failure to the operator (`zones_cfg_migration_persist_fault_t`, wired through `/api/status` and the LCD trip strip). See `docs/CONFIG_MIGRATION_CHAIN_PLAN.md` §1.6, itself corrected the same day. The step table itself is still empty pending the first schema bump past v26 | `docs/CONFIG_MIGRATION_CHAIN_PLAN.md`; `docs/PICO_AUTO_UPDATE_PLAN.md`; `docs/OTA_SINGLE_SLOT_PLAN.md` |
| **S** | **PC-side arbitrary-jump config converter — owner request 2026-09-17.** Firmware stays one-step-only (row above); this is the separate PC-side tool that jumps any version to any other, best-effort, file-only, never touches a board. Landed: `tools/PcTools/src/kilnctrl/config_convert.py` (CLI `tools/PcTools/scripts/config_convert.py`, MCP `convert_config`), covering the `kilnctl_backup` document, the `kilnctl_profile_blob` raw-NVS wrapper (v1-v4), and (2026-09-23) the `kilnctl_safety_config_blob` wrapper for SaftyFW's raw `config_store_record_t` (v1-v3 decode, v3-only encode — firmware has no pack path for v1/v2), plus a mirror-drift check (`tools/check_config_convert_mirror.py`, negative-tested, now also covering `CONFIG_STORE_FORMAT_VERSION`). Review before landing caught the CRC being computed over the body only, where firmware's `compute_profile_crc()` covers the whole struct with `crc32` zeroed; fixed, decode now verifies the CRC too. **2026-09-24:** added the ESP's raw `zones_cfg_t` blob (`kilnctl_zones_blob`) and `kiln_cfg_store`'s `kilnpkg.json` package format (`kilnctl_kiln_package`) in three stages. Stage 1 decodes/encodes byte-exactly the CURRENT `ZONES_CFG_VERSION` (26, 896-byte blob) only, its layout hand-derived from `zone_cfg_t`/`zone_timing_profile_t` (natural C alignment, no `#pragma pack`) and cross-checked against firmware's own frozen-struct `_Static_assert(sizeof/offsetof)` lines for `zone_cfg_v23_t`/`v24_t`/`v25_t` plus an independent match against `ZONES_CONFIG_BLOB_MAX_SIZE`; `tools/check_config_convert_mirror.py` now also mirrors `ZONES_CFG_VERSION`/`ZONE_NAME_MAX_LEN`/`TIMING_PROFILE_NAME_MAX_LEN`/`SRC_GROUP_COUNT`/`ZONES_CONFIG_BLOB_MAX_SIZE`, negative-tested. Stage 2 (porting `zones_config_migrate.c`'s older-version chain, 25 historical versions) was assessed and explicitly NOT attempted this pass — firmware converts an arbitrary historical version straight to current in one function rather than a chained N-1→N step, so each version needs its own firmware-derived test vector; `decode_zones_blob`/`convert_zones_blob` refuse any non-current version by name rather than guessing. Stage 3 (`kiln_package`/`kilnpkg.json`, `convert_kiln_package`) landed on top of Stage 1, mirroring `kiln_package_compute_hash()`'s byte-buffer/CRC32 algorithm in Python; also fixed a pre-existing doc bug (the module's old comment named the kind `kilnctl_kiln_cfg_package`, firmware's real `KILN_PKG_KIND` is `kilnctl_kiln_package`) (61 tests in `test_config_convert.py` as of 2026-09-24). See `docs/CONFIG_MIGRATION_CHAIN_PLAN.md` §7. | `docs/CONFIG_MIGRATION_CHAIN_PLAN.md` |
| **L** | **LCD dashboard and profiles rework -- owner request 2026-09-19.** All seven items are in source and flashed. Bench-verified by webcam 2026-09-20: items 5, 6, 7. Item 1 partial (name bar present, text not legible at camera resolution). Pending verification: item 1's tap, and items 2, 3, 4 (profiles page list and New button, safety relay on the Temperature page, relay-life reset removed). Detail: `firmware/KilnFW/docs/UI_PLAN.md` section 6. | `firmware/KilnFW/docs/UI_PLAN.md` |
| **L** | **Thorough OTA testing of both processors -- owner request 2026-09-19.** Case bodies for suite OT (OT-E01..12, OT-P01..05) and the `ota_matrix_run` MCP tool (hard `confirm is True` gate, `dry_run`, fail-closed run-level gate) are implemented; the matrix is unit-tested with a fake board. Real hardware so far: only OT-B01, PASS on 2026-10-01 (run `20261001T072647Z_ota`, `outcome=no_trip`, so `sw_reset` of both processors does not latch S6a). Every image-dependent case SKIPs without an `ota_*` image parameter. Pending: run the rest of the matrix on the bench with images (OT-E07/E08 only with `allow_heat=True`), and record each case PASS / FAIL / NOT RUN with the flashed commit. OT-E11 waits on recovery W5. | M8; `docs/BENCH_TEST_SYSTEM_PLAN.md`; `firmware/CommonFW/docs/UPDATE_PROTOCOL.md`; `docs/OTA_SINGLE_SLOT_PLAN.md` |
| **L** | **Standardized bench test system -- owner request 2026-09-19.** Waves 0-4 are implemented (`tools/PcTools/src/kilnctrl/bench_test/`, `bench_test_run`/`bench_test_list`/`bench_test_last`, `tools/bench_test.ps1`, case bodies for suites ST/FL/SK/OT/AT/HP/WEB/LCD/SP). Real-hardware record so far: heat suite HP-01..08 fixed and passing after harness fixes, LCD-08/09/14/16/19/21 and LCD-22..25 PASS, stack suite SK-01..04 PASS on `eb83c1ac` with idle, `web_ui_open` and `mid_firing` baselines committed, web suite render-only rows clean. No firmware defect found by any FAIL so far. Pending: OT-E11 and LCD-20 (wait on recovery W5), and suites OT/AT against real hardware. Per-run detail lives in `docs/BENCH_TEST_LOG.md`. | `docs/BENCH_TEST_SYSTEM_PLAN.md`; `docs/BENCH_TEST_LOG.md` |
| **M** | **Edit the running profile mid-firing, from the web UI -- owner request 2026-09-18. DELIVERED 2026-09-19**: `live_profile_page.html`, five ADMIN routes in `profiles_live_http.c`, fork-on-edit, HARD-mode validation, executor pickup, end-of-firing prompt, shared duplicate-name refusal. The LCD Edit-firing page and the LCD end-of-run Discard/Save as/Overwrite page also landed, bench PASS 2026-10-01 (LCD-22..25: live edit adopted by a running firing, decide page, heap floor). Pending bench items: delete the stray test profile "LiveEditTest" in slot 0 of the bench board, and decide which of the stored `kiln_auth` record and the bench env-var credentials is authoritative (the 2026-09-21 clean login returned 401). Plan: `docs/LIVE_PROFILE_EDIT_PLAN.md` section 10. | `docs/LIVE_PROFILE_EDIT_PLAN.md` |
| **L** | **100 user profile slots plus a live-edit slot -- owner request 2026-09-19.** Done: all 12 plan tasks, including the 2026-09-20 bench migration (new partition table, `cfg` grown to 0x250000, `profiles_http.c` fallback moved out of internal DRAM, guarded by `check_kilnfw_dram_bss_budget.ps1`). Pending: the `cfg` LittleFS volume still has its old 512 KiB geometry on the 2.3 MiB partition until reformatted via `cfgfs_format` / `POST /api/cfgfs/format_confirm`, which is plain admin-login gated since the 2026-09-29 HMAC retirement; no run is recorded. NVS stays authoritative, so this costs unused space, not data. | `docs/PROFILE_SLOTS_100_PLAN.md` |

### Blocked on hardware that does not exist yet

| Size | Item | Where |
|---|---|---|
| S | Time the firing abort (30 s) with a stopwatch during a real running firing — the 1.5 s staleness ceiling was bench-verified 2026-09-06 (`LINK_PROTOCOL.md` §8) with no firing needed | M6 |
| M | S9's welded-contactor escalation — by definition needs a welded contactor. **Checked 2026-09-03: SimFW cannot do this — SimFW itself no longer exists** (removed `8553244`, 2026-08-28; `firmware/UnitTestFw` took its place and is unrelated ESP32-S3 bench-instrument firmware — DAC/AD9833/OLED/PCF8575 — with no path to the safety processor's current-sense input at all). Even when SimFW existed, its own removal commit records that `ct_calibration` "needs the fixture to physically drive current into the CT" — S9 (`firmware/SaftyFW/src/safety_guards.c:363-389`) latches only on real `any_current_present`, gated by `in->context_valid`, `in->current_sensing_commissioned` and NOT `in->current_sensing_disabled`; that flag comes from the CT's analog current-transformer signal through `current_sense.c`, not a GPIO a simulator MCU could assert. What would actually be required: a fixture that injects genuine AC current through the CT sense loop while the K4 drive line is confirmed de-energized — i.e. a hardware jig, not firmware simulation — plus a CT actually fitted and commissioned (`ct_installed=yes`; this was `ct_installed=no` on the bare bench as of the checked date above). **Corrected 2026-09-18, then superseded the same day by the CT-summed-topology fix:** the board reads `ct_installed=1` (channel 2's summed CT fitted and calibrated, per the CT-commissioning bench check at the top of this file); `s_current_sensing_commissioned` used to require all three `k_ct_v_per_a` entries greater than zero regardless of topology — a deliberate decision at the time, but one that permanently blocked any SUMMED-topology board (only one CT, wired to channel 2) from ever reporting commissioned. It now instead requires `k_ct_v_per_a > 0` only on channels that are actually fitted for the board's topology (`config_store_current_sensing_commissioned()`, `firmware/SaftyFW/src/config_store.h`), landed together with masking `any_current_present` to fitted channels only (channels 0/1's idle ADC noise must not count) so the unclearable S9 latch cannot arm off noise. **S9's `TRIP_INEFFECTIVE` is now armable on this board for the first time** — this is a live change to the bench's safety posture, not only to source, once flashed: a welded-contactor exercise here can now actually latch S9, independent of the fixture-availability question above. | M4 |
| M | AP-fallback verified end to end (needs a router with correct *and* deliberately-wrong static config; the second AP itself is no longer the gap — `docs/BENCH_HOTSPOT.md`'s bench hotspot, 2026-09-24, provides one — the test has not been run yet) | M6 |
| M | Per-channel CT-to-jack commissioning and the ADC noise-floor measurement — see the M-size CT commissioning row far above (M5's table), `CT_COMMISSIONING_PLAN.md` steps 0 and 6 | M5 |
| M | **HW changes:** relay status LEDs for K1–K4/S9, distinct connector types for the thermocouple daughterboards, I2C broken out on an expansion connector. (LCD backlight control's flying wire is fitted and confirmed — see M1, closed 2026-09-04.) | M1 |
| S | **Blocking, before the MSP4031 touches J2 at all**: meter module pins 10/12 (CTP_SCL/CTP_SDA) at 5V — confirms or clears a hazard that can back-feed the SX1509/ESP32-S3 through the shared I2C bus. `DISPLAY_ST7796_PLAN.md` §4 | M1 |
| M | DEBUG header and GP16/GP17 access before A1 is soldered down | M0 |
| L | Field updates exercised against real hardware — see the M-size "Field-update hardware exercise" row above (consolidated 2026-09-14 roadmap truth-up, was a duplicate): ESP half done; the Pico half's 2026-09-06 interlock refusal is superseded by the 2026-09-18 attempt, which reached the erase phase and failed there on an RP2040 watchdog reset | M8 |
| L | `GUARD_TEST_MATRIX.md` §3 — every enabled guard's real trip, safe-state power-on, sensor open-circuit, current-mapping commissioning | M4 |

---

## Where each kind of task is planned

| Plan | Owns |
|---|---|
| [`ROADMAP.md`](ROADMAP.md) (this file) | Milestone order, cross-processor dependencies |
| [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md) | Main firmware: web UI, profiles, PID, thermal protection, storage |
| [`firmware/KilnFW/docs/PROJECT_STATUS.md`](firmware/KilnFW/docs/PROJECT_STATUS.md) | What in `KilnFW` is built vs. verified — the honest ledger |
| [`firmware/KilnFW/docs/UI_PLAN.md`](firmware/KilnFW/docs/UI_PLAN.md) | LCD + web UI usability/cleanup plan — no-scroll LCD audit, phone/tablet web audit, prioritized fix queue |
| [`firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`](firmware/KilnFW/docs/PID_EXPANSION_PLAN.md) | Per-zone control-algorithm choice (Cohen-Coon rule, fuzzy-PID layer), cross-zone coupling measurement (RGA) and feedforward |
| [`firmware/KilnFW/docs/DISPLAY_ST7796_PLAN.md`](firmware/KilnFW/docs/DISPLAY_ST7796_PLAN.md) | Second LCD panel (ST7796/MSP4031) support, runtime panel auto-detection, display SPI async/DMA |
| [`firmware/KilnFW/docs/FLASH_BUDGET.md`](firmware/KilnFW/docs/FLASH_BUDGET.md) | The 16 MB flash: partition table, image size, what has been reclaimed |
| [`firmware/KilnFW/docs/DRAM_PSRAM_STATUS.md`](firmware/KilnFW/docs/DRAM_PSRAM_STATUS.md) | Internal SRAM reclamation — allocator threshold, stack sizing, PSRAM relocation |
| [`firmware/KilnFW/docs/WEB_UI_RESPONSIVE.md`](firmware/KilnFW/docs/WEB_UI_RESPONSIVE.md) | Browser UI across display sizes: token consolidation, shell layout, the responsive sweep |
| [`firmware/KilnFW/docs/ARCHITECTURE.md`](firmware/KilnFW/docs/ARCHITECTURE.md) | Tasks, priorities, owner-task queues, single-writer ownership doctrine |
| [`docs/SETUP_WIZARD.md`](docs/SETUP_WIZARD.md) | The whole-kiln setup wizard: step order, dependencies, which steps need heat or the owner, and where progress persists |
| [`docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md`](docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md) | Commissioning-time proof that each CT clamp is on the conductor the configuration claims, and that each zone's normal current is what it should be: the pass/fail/**inconclusive** verdict, its configuration fingerprint and staleness rule, where it is stored and surfaced, and what this ~4 W bench can and cannot ever determine |
| [`docs/LIVE_PROFILE_EDIT_PLAN.md`](docs/LIVE_PROFILE_EDIT_PLAN.md) | Editing the profile a firing is currently running, from the web UI: what is editable past/current/future, the fork-on-edit working copy and where it survives a reboot, the end-of-firing save-or-overwrite prompt and its builtin refusal, the server-side temperature and ramp bounds and their re-check at pickup, and the shared factoring that keeps both profile editors on one implementation |
| [`docs/HW_ABSTRACTION.md`](docs/HW_ABSTRACTION.md) | KilnFW `drivers/` layering into role directories, and the `firmware/hwAbstraction/` tree (interface/esp/pico/host) for both firmwares — M16 |
| [`docs/HTTP_HANDLER_OWNERSHIP.md`](docs/HTTP_HANDLER_OWNERSHIP.md) | KilnFW TODO.md Phase 5, opened and CLOSED 2026-09-22 (`c2b5fe9d`): the 4 live HTTP-handler read-path bypasses of `thermo_owner`/`kiln_io_owner` (`dashboard_http.c` x2, `ota_http.c` x2; the `diagnostics_http.c` hit was comment-only) now go through the owner accessors, enforced by `tools/check_no_handler_direct_driver_calls.py` |
| [`docs/HTTP_POST_OWNER_MIGRATION_PLAN.md`](docs/HTTP_POST_OWNER_MIGRATION_PLAN.md) | KilnFW TODO.md 10.14 "Web side": which action-taking POST handlers move off `httpd_worker`, and how (the owner replies before the slow part, or an async-request handoff), with the response contract unchanged. It also lists what is already deferred and what is not worth moving |
| [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) | Safety firmware, phases 0–10 |
| [`firmware/SaftyFW/docs/SAFETY_MODEL.md`](firmware/SaftyFW/docs/SAFETY_MODEL.md) | What trips, why, and the anti-nuisance doctrine |
| [`firmware/SaftyFW/docs/ARCHITECTURE.md`](firmware/SaftyFW/docs/ARCHITECTURE.md) | Tasks, priorities, core affinity, logging transports |
| [`firmware/SaftyFW/docs/HARDWARE.md`](firmware/SaftyFW/docs/HARDWARE.md) | The traced board, pin map, bench connections |
| [`firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`](firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md) | Guard-by-guard test coverage and real-input reachability |
| [`firmware/SaftyFW/docs/CONFIG_REFERENCE.md`](firmware/SaftyFW/docs/CONFIG_REFERENCE.md) | Every safety tunable, its default, and whether getting it wrong is dangerous |
| [`firmware/SaftyFW/docs/COMMISSIONING.md`](firmware/SaftyFW/docs/COMMISSIONING.md) | How those values get set from the web GUI and persist on the safety processor across OTA |
| [`firmware/CommonFW/README.md`](firmware/CommonFW/README.md) | Shared `kilnlink` code, used by both firmwares |
| [`firmware/CommonFW/docs/LINK_PROTOCOL.md`](firmware/CommonFW/docs/LINK_PROTOCOL.md) | The wire, both ends — the contract neither side may break alone |
| [`firmware/CommonFW/docs/UPDATE_PROTOCOL.md`](firmware/CommonFW/docs/UPDATE_PROTOCOL.md) | Field updates for both processors: interlocks, one-password auth, ESP OTA partitioning |
| [`firmware/SaftyFW/docs/BOOTLOADER.md`](firmware/SaftyFW/docs/BOOTLOADER.md) | The RP2040 bootloader, flash layout and recovery mode |
| [`tools/PcTools/TODO.md`](tools/PcTools/TODO.md) | GUI, MCP, GPIO probe, debug and logging for **both** processors |
| [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md) §§12–13 | M10's instrumentation: HTTP route-table cap, internal-DRAM low-water alarm, task-stack high-water reporting |
| [`docs/SAFETY_CASE.md`](docs/SAFETY_CASE.md) | The cross-processor safety argument: hazard list, guard/measure/accepted-risk mapping, residual risks, evidence classification (argued/host-tested/hardware-verified). Written 2026-09-04, still thin on hardware evidence — see its own §5 |
| [`docs/SYSTEM_ARCHITECTURE.md`](docs/SYSTEM_ARCHITECTURE.md) | The system level spanning both processors: end-to-end command/trip paths, the board-to-board interface, power domains and GND crossings, cross-processor boot/shutdown ordering. Written 2026-09-04 |
| [`docs/REPO_LAYOUT.md`](docs/REPO_LAYOUT.md) | The hardware/software reorganisation and its blockers |
| [`docs/SETUP.md`](docs/SETUP.md) | Fresh-clone setup: what is machine-specific, and how `tools/setup.ps1` handles it |
| [`docs/RELEASE_HARDENING_PLAN.md`](docs/RELEASE_HARDENING_PLAN.md) | What has to be true before this controls a real kiln unattended: coredump readback for the open `profile_executor` panic, a release-gate audit against this repo's own vacuous-pass history, long-duration soak with a machine-checked verdict, guard provocation on hardware split into what the 4 W bench can and cannot ever close, failure injection, OTA/recovery mechanics, and the first-firing checklist (**closed** — [`docs/FIRST_FIRING_CHECKLIST.md`](docs/FIRST_FIRING_CHECKLIST.md), blocker 8). Risk-ordered, remaining blockers marked. **Corrected 2026-09-17 roadmap-upkeep sweep: this row's "starts once `docs/KILN_PROFILES_PLAN.md` is finished" was stale** — the plan's own doc shows it opened 2026-09-16 and several BLOCKER sub-items already closed (e.g. `bfa60679`, Blocker 6's schema-downgrade-hazard sub-item), regardless of `KILN_PROFILES_PLAN.md`'s status; read that plan's own status line for what remains, not this gating note |
| [`docs/WEB_AUTH_PLAN.md`](docs/WEB_AUTH_PLAN.md) | Username/password and roles for the web GUI plus a numeric PIN for the LCD: an always-open Dashboard tier, `user` (start/stop only) and `administrator` (everything else), the full three-tier classification of all 140 HTTP routes, hashed and salted credentials in NVS, a fail-closed enforcement point with a mechanical route-tier check, an inactivity lock with a ten-second stay-unlocked prompt on both interfaces, and an E-stop-gated physical credential reset. Authentication ships defaulted OFF on both interfaces, so a field-upgraded board is unchanged. **Corrected 2026-09-17 roadmap-upkeep sweep: "Follows `docs/RELEASE_HARDENING_PLAN.md`" was stale — both plans opened the same day (2026-09-16) and have been landing concurrently since (route tiers, credential storage, LCD PIN entry, the web login surface, admin password/settings page, and the web-GUI inactivity lock all host-tested per `docs/WEB_AUTH_PLAN.md`'s own status line), not sequenced as this row claimed** |

**Software backlog state, 2026-09-23:** every remaining unchecked box in `firmware/SaftyFW/TODO.md` (26) and `tools/PcTools/TODO.md` (4, two duplicates) is hardware-gated. `firmware/KilnFW/TODO.md`'s remaining open items are the two "POST handlers should post commands to owner tasks" items (~lines 1950/1958, architectural, scoped not urgent per `docs/HTTP_HANDLER_OWNERSHIP.md`). **6A.3's guard-disable ack gate closed 2026-09-28 by owner decision: thermal protection gets no disable switch, ever, so the item needs no design.**

---

## The dependency spine

Four facts set the order. Everything else can be shuffled.

1. **The safety UART pins are swapped in `KilnFW`.** Until that is fixed the link
   cannot pass a byte in either direction, so every link-dependent task is
   blocked behind it.
2. **The wire contract is shared code.** Writing it twice guarantees the two
   copies diverge, so `CommonFW` comes before either firmware consumes it.
3. **The relay is the last thing to wire up.** Guards get built and host-tested
   against synthetic inputs before anything can physically open a contactor.
4. **The liveness rule cuts both ways.** Once the ESP refuses to heat without
   safety telemetry, a main board with no Pico fitted cannot heat either — so
   that switch is thrown deliberately, with a documented bench escape hatch.

---

## What is actually left

The milestones below are the detail. This is the honest short answer, because
after M0 cleared, "what remains" stopped being a code question and became
mostly a hardware-and-decisions question. Reviewed 2026-08-28.

**The bench changed on 2026-08-28 and several long-standing "theoretical"
items became live.** The heating elements are now PHYSICALLY CONNECTED. Every
statement anywhere in this repo that reasons from "relays may be activated but
nothing will get warm" is now false, and that assumption is load-bearing in
more places than it looks.

Two consequences that need to be read together:

- **The fixture is limited to 80 °C, and that is a property of the FIXTURE,
  not of the kiln.** It is currently enforced by `max_temp_c = 80` on all
  three zones (set live over `/api/zones`, previous config saved). This is a
  TEST THRESHOLD and must come out before a real kiln — `SaftyFW/TODO.md`
  phase 9 already carries "confirm no test threshold was left in place", and
  this is now a concrete instance of it, not a hypothetical.
- **Only ONE processor is enforcing that limit.** The safety processor's
  `abs_max_temp_c` reads `set: true, value: 0`, and 0 on that field means
  *never trip*. **(Corrected 2026-09-15 roadmap claim audit: this 2026-08-28
  snapshot is superseded by work recorded higher in this file —
  `safety_ceiling_sync.c` pushes the ESP's configured ceiling to S1 on every
  link-up tick, and `c99356f8` makes that an exact hard cutoff with no added
  headroom, bench-verified in both directions. The present live value is a
  hardware reading this audit was not permitted to take.)** So the independent overtemperature guard is not armed, and the
  ceiling is enforced by the same processor that commands the heat. Note the
  reporting trap that hid this: `/api/readiness` says "all 58 safety
  parameters have values", which counts a zero as a value — the field next to
  it, `commissioned`, says `false`. Setting a Pico-side ceiling is offered and
  awaiting the owner's number.

**Blocked on you — mostly answered on 2026-08-28.** Six of the nine questions
that stood here were answered in one message, and the answers were largely
*"the operator should be able to enter that"*, which converted them from
questions into [M12](#m12--commissioning-the-operator-can-actually-do--opened-2026-08-28).
Summarised, because the answers matter beyond the milestone that implements
them:

- **An uncommissioned safety processor must NOT grant heating enable.**
  Unqualified NO. It grants it today, which is the only reason the bench can
  heat, so M12 lands this one **last** — see that milestone's ordering note.
- **Kiln maximum temperature** is entered on a safety web page, and feeds
  `abs_max_temp_c`. **Thermocouple maximum is inferred from the thermocouple
  type**, not asked for.
- **Every relay is rated for 100% duty and inrush is negligible — the board is
  designed for it.** This closes the SSR-vs-contactor-coil question and the
  2 A/125 VA duty-window check outright.
- **Breakers are assumed sized for the full kiln load at 100% duty.** Instead
  of per-zone element power, the operator enters maximum expected kiln power
  on the safety page, and the zones page gets a button that measures each
  zone's normal current by energizing them one at a time. That measurement is
  then used at runtime to check each CT is on the zone it is configured for,
  and to warn when it is not.
- The twelve stale `display_*` MCP tools were left to my judgement: delete.

All three items once listed here as still unanswerable are now closed: physical
zone arrangement (owner-confirmed 2026-09-03), the deferred S8 sanity rate
(decided 2026-09-05 — see the table above), and `hardware/UnitTestFixture/`
(owner said KEEP, 2026-09-05). **Scope firmed up 2026-09-05: `UnitTestFixture`
is a control device only** — a PcTools/MCP surface driving its I/O expanders
to flip relays, for shorting/opening thermocouples, opening heater
connections, and simulating SSR lock-ups. It stays excluded from the HAL
boundary and the `drivers/` reorg (M16/`HW_ABSTRACTION.md`). See
`docs/UNIT_TEST_FIXTURE_STATUS.md` for the relay inventory, wire protocol, and
the MCP control surface.

**Blocked on hardware that does not exist yet.** All of this is scripted and
waiting, not unwritten:

- `GUARD_TEST_MATRIX.md` §3's trip rows — every enabled guard's real trip,
  safe-state power-on, sensor open-circuit, current-mapping commissioning.
- S9's welded-contactor escalation, which by definition needs a welded
  contactor. **Checked 2026-09-03**: not a SimFW task — SimFW was removed
  (`8553244`, 2026-08-28) and its replacement, `firmware/UnitTestFw`, is
  unrelated ESP32-S3 bench-instrument firmware with no connection to the
  safety processor's current sense. S9 latches on real `any_current_present`
  from the CT's analog signal (`safety_guards.c:363-389`), which requires
  physically driving current through the CT — a hardware jig, not something
  any firmware simulator asserts over GPIO — plus a CT actually fitted and
  commissioned, since `ct_installed=no` switches S9 off on today's bare bench.
- CT hardware: split-core current transformer probes, 1 V output at full
  scale (amp rating = full-scale value); no coupling/isolation transformer
  is used. See `docs/CONTACTOR_FEEDBACK_OPTIONS.md` §7.
- Link-staleness *timing* — the 30 s firing abort only. The 1.5 s ceiling was
  bench-verified 2026-09-06 (`firmware/CommonFW/docs/LINK_PROTOCOL.md` §8):
  `debug_reset(peer="pico")` silenced telemetry and the device log recorded
  the fault firing at the coded 1500 ms threshold (observed at 2250 ms of
  silence, within 500 ms poll-granularity slack), self-clearing on the next
  good frame. The 30 s abort needs a real running firing to observe and is
  still untimed.

**Genuinely still software, and doable without you or the fixture.** This is
now a short list, which is the point:

- **Display power (brightness/idle-timeout/keep-on-while-firing/display-on-
  error), 2026-09-04.** New feature, not an M11 reopen — see
  `firmware/KilnFW/docs/UI_PLAN.md`'s "Display power" section for the full
  writeup. Pure decision core + persisted settings + HTTP API + settings-page
  UI are built and host-tested. **Brightness is no longer inert
  (`be02d34`):** the flying wire (GPIO15 → panel pin 8) is fitted, owner-
  confirmed by meter, `KILNCTL_BACKLIGHT_PWM_ENABLE` now defaults on, and ON
  duty is driven from the operator's brightness setting rather than a
  Kconfig constant — not yet confirmed by meter/eye that the panel actually
  dims, that's the next check. `keep_on_while_firing`/`display_on_error` now
  default **true** (`f028e2f`, owner decision from a real finger on the
  glass), so both are already right the day a timeout is chosen. The
  `screen_idle.c`/`lvgl_port.c` touch-swallow/timeout hookup landed the
  same day (`7fc17cc`), bound to real producers — the executor for
  `firing_active`, the RP2040 DIAG trip for `error_active`. Review then found
  both producers too narrow (`192eb7d`): keep-on-while-firing blanked the
  screen mid-autotune, since autotune holds relay authority with the executor
  IDLE, and display-on-error missed the ESP's own guard aborts, which never
  touch the RP2040's `diag_state`. Both widened. The 1-minute timeout is
  hardware-verified via `touch_get_state()` ("screen on" → "screen blanked"),
  as is NVS persistence across a reboot. Wake-on-touch, first-touch-swallow
  and error dismissal still need the owner's finger.
- **`screen_idle` held its own lock across the producer reads, 2026-09-04
  (`7a8594d`).** The policy tick called `dashboard_get_status()` (five
  MAX31856 SPI bursts), `kiln_io_owner_command_read()` (blocks up to 200 ms on
  another task) and four interrupts-disabled heap walks at 20 Hz, all under a
  lock the LVGL task takes on every tick and touch — against the module's own
  documented invariant. Reads moved outside the lock and throttled to 1 Hz,
  policy now reads a cached snapshot; five mutation tests, all red.
**Closed 2026-08-27 through 2026-09-04, no longer open** — DRAM/stack
reclamation, PID Cohen-Coon/fuzzy-layer landing, SNTP sync, the first
hardware coupling-matrix/RGA measurements, the withdrawal of coupled/Ki
adaptive-tuning hardware clearance (harvest-layer defect), the shelving of
dynamics-from-ramps, ramp assist landing end to end (default OFF, gated on a
real cone-temperature firing — see the GATED table below), a round of
relay-autotune/measurement-tooling/web-UI hardening, and the fuzzy-PID A/B
campaign's discovery-and-fix-and-second-discovery (`8906686` then `778ad64`,
see [Blocked on you](#blocked-on-you--nothing-in-the-code-can-answer-these)
for the resulting owner decision) plus the 2026-09-04 safety-case/guard-
coverage hardening pass (27 payload-decoder fuzz targets, S3/S4/S10/S9
coverage, `SAFETY_CASE.md`/`SAFETY_MODEL.md` corrections, and
`wire_protocol_fingerprint_check.py`). Full postmortem detail for all of the
above:
[`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#what-is-actually-left-closeout-narratives-moved-2026-09-04).

**What is currently GATED, and on what** (the short answer for planning):

| Item | Gated on | Where |
|---|---|---|
| DRAM/PSRAM allocator-threshold work | A full soak (cold firing through cooldown) plus a Pico OTA relay-path measurement that has never been taken | `DRAM_PSRAM_STATUS.md` §5/§6/§9 |
| Second LCD panel (ST7796/MSP4031) | The physical panel, and its pre-power STOP-block 5V I2C hazard check before the module ever touches J2 | `DISPLAY_ST7796_PLAN.md` §0/§4 |
| Ramp assist default (OFF → ON) | A real firing at cone temperatures — everything measured so far is bench-range (0–80 °C), well below where the cone table's heat-work weighting matters | `PID_EXPANSION_PLAN.md` §7 |

**Two operational facts a future session must not miss (verified by READING
THE BOARD, 2026-09-04 — not inferred from commit messages):**
- **Both zone-config migrations are ALREADY on the live board and have already
  run against its real config.** `GET /api/zones` returns
  `ease_off_window_mult: 2.0` and `approach_rate_cap_c_per_hr: 0.0` on all
  three zones; `GET /api/status` reports `fw_build: Sep 4 2026 14:53:53`.
  This was NOT deliberate: an agent authorised to flash while diagnosing a
  watchdog reset built from a shared working tree carrying another session's
  in-progress schema work, and the v16→v17→v18 chain rode along. It landed
  correctly — v16→v17 carried the prior global `ease_off_window_mult` of 2.0
  verbatim to every zone (the intended default; had it been left at 3.0 by the
  withdrawn A/B, every zone would silently have inherited 3.0), and v17→v18
  added the cap at 0/uncapped. PID gains, plant models and the 80 °C ceilings
  are intact. Note `d800a60`'s own commit message says it stayed unflashed —
  true of *that agent*, and wrong about the board. **Read the board.**
- **The accidental flash briefly broke every preset apply**, because the
  firmware emitted `approach_rate_cap_c_per_hr` while `zones_http_client.py`
  had no POST mapping, so `load_config_preset` refused rather than risk
  silently zeroing it — correct behaviour, and the reason the board still sits
  on the `fuzzy_ab_strength50_20260903` arm preset rather than the baseline.
  The mapping landed in `d800a60` (`z%u_approachratecap` /
  `_PRESET_ZONE_OVERRIDE_FIELDS`), so the tree is whole; a long-running MCP
  server started before that commit will still refuse until restarted. The
  trap for the next flash: never flash a build carrying the GET half without
  the matching client POST half. `7afd2e6`'s flash-provenance guard now
  refuses when the dirty tree touches config-schema files, which is what
  should have stopped this.

**What is done and should not be reopened:** the link itself, the wire
contract and its two independent version numbers, the PC-link acknowledgement
convention, every guard's input plumbing, and the instrumentation that now
reports DRAM, stack and link health. See the decisions table at the foot of
this file before re-litigating any of them.

---

## M0 — Unblock the link · *the only milestone with no alternatives*

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phase 0.

- [x] **Link confirmed working end-to-end (2026-08-23).** The bench
      measurement found the byte actually died on baud mismatch, not idle
      level, framing or opto polarity: the TCMT1109 optocoupler pair then
      fitted could not switch fast enough for 115200. Walking the rate down
      settled on 9600, hardcoded on both sides at the time, and
      `safety_get_status()` returned live telemetry. **That optocoupler pair
      was replaced by a non-inverting digital isolator (U6) on 2026-08-25;
      the 9600 figure was a property of the retired parts, not of either
      firmware, and the baud sweep is now complete — see
      `KILNCTL_SAFETY_BAUD_RATE` in `KilnFW/App/drivers/Kconfig` for the
      committed value.** See `firmware/KilnFW/docs/SAFETY_LINK.md` "Transport"
      and `firmware/SaftyFW/docs/HARDWARE.md` §1.
- [x] Tier 0 pin test settles ESP TX/RX by measurement (`HARDWARE.md` §1) —
      moot as a separate step: the Pico is attached and the link carries live
      telemetry both ways (`safety_get_status()` returns fresh frames,
      `tx_dropped 0`), which settles TX/RX by observation rather than by
      probing pins
- [x] ESP TX/RX GPIO assignment and pull-up fixed in code, docs corrected,
      `UART_PROTO_MSG_BROADCAST` added, stale netlist removed, bench path
      decided (Debug Probe SWD + UART bridge on GP16/GP17) — all 2026-08-16
- [ ] DEBUG header and GP16/GP17 access provided before A1 is soldered down

System-wiring decisions gating any bench trip test (not firmware work) are
recorded in the decisions table below, with reasoning in
[`firmware/SaftyFW/docs/HARDWARE.md`](firmware/SaftyFW/docs/HARDWARE.md) §3/§5.

## M1 — Tooling that makes everything after it cheaper

Owned by [`tools/PcTools/TODO.md`](tools/PcTools/TODO.md). Worth doing early precisely
because it is what turns later hardware questions into a script instead of a
soldering session.

- [~] **Known-good config presets, so a test always starts from the same
      board** (owner request, 2026-08-28): factory-default then load a named
      config as one step, so a run is reproducible instead of depending on
      whatever the last session left behind. Presets live as DATA under
      `tools/`, never compiled into firmware — the bench fixture's 80 °C
      ceilings must never be capable of being left behind in a real kiln build.
      **No longer scoped to KilnFW-side config:** the safety-parameter write
      path is trustworthy now (SET_PARAM/COMMIT_CONFIG confirmed by a live
      read-back), so a preset carries a `"safety"` section written over
      `POST /api/safety/commissioning` by `safety_cfg_http_client.py`, with
      `load_config_preset(safety_host=...)` reaching it. Applied for real to
      the bench 2026-08-28: `tc_source`, `tc_placement_mode`,
      `abs_max_temp_c`, `mains_voltage_v`, `tc_type`, `max_rate_c_per_min`,
      `borrowed_zone_index` all committed and confirmed by read-back.
      **What is left is hardware, not software:** `ct_channel_map[0..2]`
      states which relay each CT is clamped around, and no CT is fitted on
      this bench (M5), so it has no measured truth. Committing a guess would
      clear `calibration_missing` and make the safety processor report itself
      COMMISSIONED on a mapping nobody verified, so the assumed identity map
      sits in a separate `"safety_ct_channel_map_backup"` preset section that
      applies only on an explicit `use_ct_map_backup=True`. Readiness still
      reports "Safety processor commissioned" as not-done, correctly, until
      the CTs are fitted and the zone current-sweep derives the real map
- [~] **Live-bench regression suite built on that preset** (2026-08-28):
      `tools/PcTools/tests/bench_fixture_session.py` +
      `tests/conftest.py` turn "start from `bench_fixture.json`, confirmed
      loaded" into a pytest fixture, and
      `tests/test_live_bench_firing.py` runs a real, bounded (45 °C, ≤80 °C
      ceiling checked four independent ways) firing attempt through the same
      `POST /api/profile_exec/start` the dashboard's Start button uses. Ran
      green against the live bench 2026-08-28, 4/4. **What it proves today:**
      the start endpoint accepts (the gate is downstream of
      `profile_executor.c`), and with the safety processor not commissioned
      no relay ever energizes and `safety_heating_enabled` stays false — a
      regression test for `commissioning_gate.c` working, not for it
      existing. The test branches on the board's own live verdict, so the day
      the CTs are fitted and the sweep commits a measured `ct_channel_map` it
      begins exercising the real heat path with no edit. Skips entirely
      unless `KILNCTRL_BENCH_HOST` is set; no mock stands in for the board
- [x] Live-bench **step and PID-tuning** regression tests
      (`tools/PcTools/tests/test_live_bench_tuning.py`), on the same harness —
      green against the live bench 2026-08-28, 4/4 in 6m10s. A closed-loop
      setpoint step (35 → 45 °C on zone 0) with the PV trace sampled at 2 s,
      and the open-loop step autotune (`autotune_engine.c`'s
      `AUTOTUNE_METHOD_STEP`) driven end to end through
      `/api/autotune/{start,abort}` under a 300 s budget. **What it proves
      today:** the start is accepted, the engine steps, and with heat refused
      PV is flat (−0.03 °C over 61 s, no relay ever closed) and the engine
      aborts itself with *"response too small to fit (trace flat or
      noise-dominated)"* — the correct verdict, said out loud. Also a
      negative test that a second concurrent autotune is refused
      (`begin_run_locked()`). `/api/autotune/accept` is never posted: a
      regression test must not retune the bench.
      **Superseded 2026-08-29 — the flat trace had two causes, and both are
      now fixed.** The first was K4: nothing in the firing path ever asked
      for heat enable (`heat_enable.c`, above). The second was found the
      moment the first was: zone 0's `heater_window_ms` was 2000 ms against
      the 10 s minimum on-time, so `heater_output_duty()` quantized every
      on-time a duty could compute to OFF. Autotune commanded `step_duty
      0.4` for 40 minutes and the relay never closed once — the "response
      too small to fit" verdict was correct about the trace and said nothing
      about why it was flat. With the window at 60 s the same run measures
      relay 1 closing for **24.0 s of a 60.9 s period (duty 0.394 against a
      commanded 0.40)** and PV climbing ~2 °C/min — and the step **completed
      with a model for the first time**: `state=done`, `model_valid=true`
      after 390 s / 39 samples, PV 31.9 → 45.0 °C, fitted `K = 32.95
      °C/duty`, `tau = 166.9 s`, `dead time = 36.9 s`, SIMC proposal
      `kp = 0.0343 / ki = 0.000206 / kd = 0.633`. The gains were **not**
      accepted, on this bullet's own rule. This bullet's "what it proves
      today" describes the old, heat-refused behaviour and is superseded by
      the run above. The relay-feedback method is
      **not runnable on this fixture at all** —
      `AUTOTUNE_RELAY_SETPOINT_HEADROOM_C` (50 °C) under an 80 °C ceiling
      admits only setpoints below the bench's own 35 °C ambient
- [x] **Live-bench verification pass, 2026-08-29** — step tuning, PID tuning,
      zone interaction and a full profile firing, each turned from an
      observation into an assertion, plus the cooldown gate that lets them run
      back to back honestly.
      **The gate:** `BenchSession.wait_for_cooldown()` +
      the `cold_bench` pytest fixture. The target is derived from the board's
      **lowest live cold-junction reading** plus a tolerance, not hardcoded —
      the room here runs around 100 °F and a 25 °C gate would never open. It
      raises on budget expiry rather than proceeding onto residual heat, and
      its policy is split into two pure functions with negative tests
      (`tests/test_cooldown_policy.py`): no cold junction refuses rather than
      defaulting, and **NaN is not cool**. Deliberately *not* an MCP tool — a
      multi-minute blocking wait does not fit `kiln_batch`'s one-round-trip
      contract. Measured in use: 50.4 → 37.5 °C in 646 s.
      **Two harness defects found by asserting instead of observing.** The
      step test's profile carried a 1-minute dwell against a 900 °C/h ramp, so
      the run ended at ~105 s with PV at 38.6 °C and the remaining ~380 s of
      the sweep sampled a *cooling* jig — the trace read 0.38 °C/min, four
      times too slow, for a reason that is not the plant. And
      `sample_response()` indexed channels positionally in a list already
      filtered to valid ones, which would have relabelled every channel after
      a gap — the exact failure that turns a cross-zone measurement into
      fiction. Both fixed; every sample now carries `channels_c` for all
      zones.
      **Results.** Step response from an enforced cold start: PV
      37.51 → 44.37 °C over 487 s, executor `running` throughout, relay 1 and
      K4 closed, **driving-phase rise 1.82 °C/min**, arrival at t=135 s,
      worst post-arrival deviation 4.48 °C. Autotune: `state=done` at 450 s,
      **K = 31.36 °C/duty, τ = 184.7 s, L = 46.1 s**, SIMC
      `kp = 0.03196 / ki = 0.00017 / kd = 0.73614` — an independent second fit
      agreeing with the first (32.95 / 166.9 / 36.9) to within 5 % on K, which
      is the first time this board's plant model has been *reproduced* rather
      than merely measured. Gains still not accepted.
      **Zone interaction — the RGA's first run on real data.**
      `autotune_engine.h` said "no input cell has ever been filled on hardware
      ... this has never run on measured data"; it has now. Two cold-start
      autotune runs (zones 0 and 1, same power cycle — the matrix is RAM-only)
      filled `K = [[30.181, 5.519], [11.822, 23.266]]`, and the board computed
      `Λ = [[+1.1024, −0.1024], [−0.1024, +1.1024]]`, det 636.9. Every row and
      column sums to 1.0000 — Bristol's identity, which is what the test
      asserts, so it is a check on the implementation rather than on a number
      someone typed. **Verdict: the loops interact mildly and independent
      per-zone PID is legitimate on this jig.** Raw thermal coupling, measured
      alongside and **asymmetric**: firing zone 0 raises ch1 by 17.5 % and ch2
      by 8.7 % of its own rise; firing zone 1 raises ch0 by **43.5 %** and ch2
      by 17.7 %. Zone 1 leaks into zone 0 about 2.5× as hard as the reverse,
      which matters for any multi-zone schedule on this enclosure.
      **Superseded 2026-08-30 by a full 3x3 coupling matrix and RGA over all
      three zones, and again 2026-09-02 by a re-solved matrix** — current
      numbers live in `firmware/KilnFW/docs/PID_EXPANSION_PLAN.md` §2, not
      restated here.
      **Full multi-segment profile.** 42 °C dwell 6 → 52 °C dwell 6 → down-ramp
      to 46 °C, run end to end through `POST /api/profile_exec/start`: all
      three segments entered in order, all dwelled, peak 56.30 °C, K4 closed on
      **112/113 samples**, no heat commanded during the down-ramp (PV
      56.16 → 54.18 °C with relays open), ramp-lock never engaged, and every
      relay plus K4 released by the completion path rather than by teardown.
      *(NOT a test of multi-zone ramp-lock coordination — that needs two zones
      in closed-loop control, and zones 1/2 are `control_mode 0` here.)*
- [x] **Guard 1 aborted a healthy firing for settling.** Found by the profile
      run above, on its second dwell: zone 0 holding 50.7 °C against a 52.0 °C
      setpoint at full duty — the steady-state offset any finite-gain PID
      leaves — and `thermal_guard.c` tripped with *"heating but rose only
      −0.2C in 1min"*, aborting the whole firing. Guard 1 treated `error > 0`
      as "still climbing toward setpoint"; those are different questions, and
      once a loop arrives, high duty + positive error + not rising **is** the
      correct state. Every sufficiently long dwell on a sufficiently lossy
      zone would eventually abort. Fixed with an arrival band
      (`progress_band_c`, default 3 °C): outside it guard 1 is unchanged, so a
      dead element on a ramp — errors of tens of degrees — is still caught;
      inside it the zone must not *fall*, which is guard 2's rate test applied
      to a case that previously had no test at all. Five host tests, proven
      load-bearing by forcing the band to 0 (two fail, both pass at 3).
- [x] `pc_tools` moved to `tools/PcTools/`; GPIO probes built for both chips
      (ESP: deny-list incl. GPIO6; Pico: over SWD, GPIO6 read-only). **Pico
      probe bench-tested 2026-08-19, PASS.** ESP probe bench-tested 2026-08-19
      but blocked: the PC↔ESP command UART (COM9) is unresponsive independent
      of the M0 isolated link — see bench state below
- [x] Coordinated two-board test script (`tools/PcTools/scripts/
      coordinated_gpio_test.py`) — reaches each board by a path independent of
      the link under test; run 2026-08-19 confirmed the Pico half works and
      the ESP half hits the same COM9 fault above
- [x] OpenOCD wrapper covering both chips (program/reset/halt/read/write) —
      2026-08-17, `kilnctrl.debug_probe`
- [x] Per-processor console capture + interleaved log file
      (`kilnctrl-console-capture`) — host-verified only; the SAFETY log-relay wire path has since landed
      in firmware (`safety_link_service_log_relay()`, `safety_link_poll.c`,
      2026-09-20 per `tools/PcTools/TODO.md`)
- [x] **HW change: LCD backlight control — CLOSED 2026-09-04 (`be02d34`).**
      Flying wire (GPIO15 → module pin 8) is fitted, owner-confirmed by
      meter; `KILNCTL_BACKLIGHT_PWM_ENABLE` now defaults on and ON duty
      comes from the operator's brightness setting. Not yet confirmed by
      meter/eye that the panel actually dims — that's the next check, not a
      firmware gap. Full buildup history in `docs/COMPLETED_2026-09.md`.
- [~] **Second LCD panel (ST7796/MSP4031), auto-detection, display SPI
      async/DMA.** `firmware/KilnFW/docs/DISPLAY_ST7796_PLAN.md`, sequenced
      Phase 0 (bench facts/hazard measurement) through Phase 7 (UI). Phases 1
      (single-owner cleanup, bounded SPI-owner timeout — also closes
      `TODO.md`'s spi_owner unbounded-wait entry), 2 (panel codec extracted),
      3 (`panel_spi`/`st7796_panel` split, Kconfig-selectable) and 5 (FT6336U
      touch abstraction, unvalidated on hardware) landed 2026-09-01/02.
      Phase 4 (auto-detection) is done ahead of hardware: the detection logic
      is host-tested and wired into `panel_spi_blit.c` (`panel_spi.c`'s
      2026-09-04 split, see the 1500-line-rule item below), and is correctly inert
      today because both descriptors' `id_matches` stay NULL — the ILI9488's
      Sec.4 RDDID bytes were captured 2026-09-03 (`0x00 0x00 0x00`, MISO
      undriven, i.e. permanently unmatchable) and the ST7796's have never
      been read since the module has not touched J2. Phase 6 (SPI DMA) is
      also done ahead of hardware: 9.2/9.5/9.9 landed; 9.3 (PSRAM DMA), 9.4
      (hardware CS), 9.6 (async flush) and 9.7 (zero-copy flush) landed
      compiled-in but default-OFF behind their own Kconfig symbols; 9.1's
      instrumentation landed (`flush_last_us`/`flush_max_us`/`flush_count` on
      `GET /api/status`) though 9.1b's number is not yet recorded live; 9.8
      needed no code. **The owner connected the module to J2 on 2026-09-04 and
      it is now the panel in service** — the "has not yet touched J2" caveat
      above is superseded for everything except the STOP-block 5V I2C hazard
      measurement, which was never taken. Bring-up that day:
      `KILNCTL_DISPLAY_PANEL` was still pinned to ILI9488, so the board ran the
      wrong init table against real ST7796 silicon; and `main.c` only ever
      constructed an `NS2009Class`, so the fully written, host-tested FT6336U
      driver was never instantiated — a textbook consumer-without-producer.
      Both fixed (`16fe9ed`); touch answers at I2C 0x38. The real colour bug
      was neither inversion nor MADCTL: `panel_codec.c`/`panel_spi.c` sent
      LVGL's RGB565 little-endian where MIPI-DCS RAMWR wants MSB-first, and the
      code's own comment had the wire order backwards (`8174db5`). Every
      INVON/BGR experiment run before that fix was confounded and is not
      evidence. With byte order correct, MADCTL was re-measured and set to RGB
      `0x00` (`f493bf8`); **corrected to BGR `0x08` on 2026-09-23** after a
      strongly R/B-asymmetric colour sample showed the RGB setting was itself
      swapped (`st7796_panel.c`). INVON is deliberately absent (vendor
      transcription, guarded by `test_st7796_panel.c`). Touch axis mapping now
      lives on `touch_dev_t`, so panel and touch controller select
      independently (`KILNCTL_TOUCH_FT6336U`). ST7796 RDDID reads `0x00 0x00
      0x00` like the ILI9488 — MISO undriven — so `id_matches` stays NULL
      permanently on both and Phase 4 is inert by design, not by omission.
      **Still open:** a residual blue bias, with every firmware cause
      eliminated by measurement (camera response refuted by a neutral
      off-screen bezel sample; RGB565 field boundaries unit-tested against
      known values; LVGL double-swap ruled out; blit paths compiled out; SPI
      clock swept 20/15/10 MHz with the blue *fraction* flat at 0.62/0.61/0.58
      — `07cad60`, table in `DISPLAY_ST7796_PLAN.md` §4). Treat as a module
      characteristic needing the owner's eye, a colorimeter or a second unit,
      not more firmware. Touch corner accuracy CLOSED 2026-09-04 (`f028e2f`
      — Y was mirrored, `KILNCTL_TOUCH_CAP_INVERT_Y` now defaults on).
      Still open: the 5V I2C hazard measurement. Rendering can now be
      checked without a person at the bench
      via `tools/PcTools/scripts/capture_lcd.ps1` (`5fd9761`) — sample pixels
      numerically, never by eye, and always include an off-screen reference.
      The only hardware change required remains a custom
      harness; the main board itself needs no modification
- [ ] **HW change: relay status LEDs** for K1–K4, S9
- [ ] **HW change: distinct connector types** for the thermocouple daughterboards
      vs. main-board connectors
- [ ] **HW change: I2C broken out on an expansion connector** (owner request,
      2026-08-28). For a future board revision, not the current one. Worth
      deciding alongside it: whether the expansion header carries power and at
      what rail, and whether the bus is the same one the SX1509 and the
      MCP23017 expanders sit on or a separate segment — an expansion connector
      that shares the relay expander's bus lets an add-on wedge relay control

**Bench state (2026-08-20):** ILI9488 LCD, ESP32-S3 JTAG, and Pico SWD all
verified working. Three MAX31856 ICs + thermocouples now fitted on the ESP32-S3
board (channels 0/1/2 reading correctly, `thermo_owner.c` unblocked); the
safety processor (RP2040) still has none fitted.

**Bench state (2026-08-24):** both UARTs work — the two blockers named above
are cleared. The isolated Pi↔ESP link is up and carrying telemetry at 9600
(M0; that figure was specific to the optocoupler pair fitted at the time and
does not describe the digital isolator that replaced it on 2026-08-25 — see
M0's note), and the PC↔ESP command UART on COM9 is responsive. Both firmwares were
flashed over JTAG/SWD today and verified running.

**Later the same day the safety processor's MAX31856 and thermocouple were
fitted** (this paragraph's earlier revision said "still not populated", which
was true when written and stopped being true a few hours later — the two
statements are hours apart, not a contradiction). Verified live:
`safety thermocouple valid | 30.20 C (CJ 28.08 C)`. It required a Pico reset,
because SPI init runs once at boot. The E-stop net measures low (a contact IS
fitted, contrary to `HARDWARE.md` §5's older note) and S7 is now genuinely
evaluated rather than masked by S5 — and correctly stays quiet, which is the
first real test of the `discrete_task.c` polarity fix (`642dd54`): with the
old inverted read this healthy board would now be latched on S7.

## M2 — `CommonFW`, before either firmware depends on it · *done, 2026-08-19*

Owned by [`firmware/CommonFW/README.md`](firmware/CommonFW/README.md). `kilnlink` builds
under both pico-sdk and ESP-IDF, both firmwares consume the same codecs for
every `LINK_PROTOCOL.md` sec 4/6 command, `pc_tools` cross-checks the same
byte vectors, and a CI grep (`tools/check_no_duplicate_crc.ps1`) keeps a
second CRC/framing implementation from reappearing outside `CommonFW`. Detail
in `firmware/CommonFW/README.md` and `firmware/CommonFW/docs/LINK_PROTOCOL.md`.

## M3 — Safety processor to first trustworthy reading

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phases 2–4. Independent of the
link, so it can run in parallel with M1 and M2 once M0 is out of the way.

- [x] FreeRTOS SMP skeleton, GPIO6 driven low first, watchdog with latched trip
      reason — all done 2026-08-16, build-verified
- [x] **The SAFETY processor's MAX31856 and thermocouple are now fitted**
      (2026-08-24, by the user). Verified on hardware the same day:
      `link up; safety thermocouple valid | 30.20 C (CJ 28.08 C)`, status
      flags `0x21` = `LINK_UP | TEMP_VALID`. This unblocked the real-reading
      work below it and immediately changed guard reachability — S5 (sensor
      invalid) stopped latching, which had been masking every later guard
      because `safety_guards_tick()` early-returns while any trip is latched.
      **Bring-up trap worth keeping:** the IC must be present BEFORE the Pico
      boots, since its SPI init runs once at startup. Fitted under power it
      reads `safety TC invalid` with nothing pointing at the real cause; a
      `debug_reset` over SWD is the fix.
      **And it exposed a decision that needs you** — see
      `firmware/SaftyFW/TODO.md`, "An uncommissioned safety processor grants
      heating enable": with the sensor real and the stale S5 latch cleared,
      the board granted heating enable while still reporting
      `commissioned: false`, i.e. with S1's absolute temperature ceiling
      disabled for want of `abs_max_temp_c`
      line above. Kept for one revision so anyone mid-task on the old wording
      sees why it changed.
      **Read the word "safety" carefully** — this item is about the RP2040's
      own thermocouple, not the main board's. `KilnFW`'s three channels ARE
      fitted and working: verified 2026-08-24 with all three reading ~35 °C,
      `SR 0x00`, and open-circuit detection confirmed genuinely enabled
      (`CR0 = 0x90`, so `OCFAULT[1:0] = 01`) — which is what makes "no fault"
      mean "a thermocouple is attached" rather than "detection is switched
      off". An absent TC on those channels would fault. The two sets of
      thermocouples are easy to conflate from this line alone, and doing so
      leads to "correcting" a true statement
- [~] MAX31856 driver + config plumbing (tc_type via flash-backed
      `config_store`, commissioned over `SAFETY_CMD_SET_CONFIG`) built and
      wired end-to-end in code (2026-08-19). ~~The part itself is not
      physically populated~~ — **fitted 2026-08-24 and reading correctly.**
      **Still open**: there is no LCD/web commissioning surface yet, and the
      four no-default section-1 fields remain unset, which is what keeps
      `commissioned: false` and leaves S1's ceiling disabled
- [x] 13 of 13 guards (`SAFETY_MODEL.md` §4) implemented as pure functions and
      host-tested against synthetic inputs. **S8 (rate-of-rise), the last
      holdout, gained its pure-module implementation and integration on
      2026-09-03** (`safety_guards.c`'s S8 block, `safety_core_load_guard_cfg()`
      wiring gated on `CONFIG_STORE_SET_MAX_RATE_C_PER_MIN`, `test_s8()` +
      `test_safety_core_s8_wiring.c`, `SaftyFW/docs/GUARD_TEST_MATRIX.md`).
      It ships deliberately off — `max_rate_c_per_min` defaults to 0.0f,
      the same "no default by design" shape S1 uses for `abs_max_temp_c` —
      so **commissioning `max_rate_c_per_min` is now what enables it**,
      superseding the earlier finding here that the field would enable
      nothing. **Input wiring now complete (2026-08-24):**
      `safety_core_build_input()` populates every field the guards read —
      `context_valid`, `any_current_present`, `relay_commanded_recently`/
      `_continuously`, `zone_count`, the setpoint/measured reductions,
      `sample_counter_advancing`, both discretes and `reboot_grace_active`.
      The older "only S5/S6b/S7/S12 are reachable, the other 8 have no
      producer" finding is superseded: what still holds a guard dormant is a
      missing **commissioning value** (S1's `abs_max_temp_c` defaults to 0 =
      never trip; S13 needs `tc_source`/`borrowed_zone_index`), which is a
      different kind of gap from a missing producer. Two real producer bugs
      were found and fixed on 2026-08-24 — the E-stop polarity was inverted
      (S7 could not fire) and `current_sense_set_cal()` was never called, so
      `any_current_present` was permanently false (S3/S9/S11 and S6b's
      current-gated trip could not fire). ~~The reachability count in
      `GUARD_TEST_MATRIX.md` predates both fixes; re-establish it rather than
      trusting the old number.~~ **Re-established 2026-09-03**
      (`GUARD_TEST_MATRIX.md` §6c): old count was 6 of 13 structurally
      reachable (computed before either fix and before S8 existed); S8
      itself gained its implementation and integration on 2026-09-03 and is
      now the same class as S1/S13 — implemented and integrated but
      deliberately configured off pending commissioning, not unreachable.
      See `SaftyFW/docs/GUARD_TEST_MATRIX.md` for the current reachable-count
      recomputation (S14 is new since 2026-08-28 and tracked separately). S3,
      S6a, S7, S9 and S11 moved from blocked to reachable — S6a's own
      `main_fault_asserted` wiring is a third fix in the same window, beyond
      the two named above. S1, S13 and S14 remain deliberately blocked by
      commissioning gaps, not producer bugs — §6c distinguishes that from an
      unreachable guard explicitly. One stale claim inside the matrix itself
      was also caught and corrected in the same pass: S13's row said
      `sample_counter_advancing` had no producer at all; it does now
      (`safety_core.c:957-960`), and `tc_source` alone is what still blocks
      S13. Host suite re-run in the same pass: 1983/1983 checks pass (the
      matrix's old "452/452" checklist line was itself stale, from before
      the suite grew ~4.4x).
- [x] CI grep: `safety_core.c` never includes the link header — 2026-08-16,
      `firmware/SaftyFW/tools/check_isolation.ps1`

## M4 — Relay authority

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phase 5. First milestone that can
physically stop a kiln, and the first that can nuisance-trip one.

- [x] Relay owner task is the sole writer of GPIO6 — confirmed by whole-tree grep
- [x] Trip latches and requires an explicit `CLEAR_TRIP` to clear, refused if
      the tripping condition re-fires on a one-tick retest
      (`safety_guards_try_clear()`); wired end to end PC→ESP→Pico and
      ESP-side send (`safety_link_send_clear_trip()`) plus web/UART surfaces —
      all built and host-tested (2026-08-19). **Known scope limit** (documented,
      not a bug): a graduated/windowed guard's elapsed-time accumulator resets
      on clear, so it re-trips on its own timescale rather than instantly.
      **Hardware-verified 2026-08-23/24**: the physical link has carried real
      CLEAR_TRIP frames. Refused correctly against a latched S5 with the
      safety thermocouple genuinely absent, accepted once the tripping
      condition cleared, and the board never rebooted in either case — which
      it originally did, via a `log_task` stack overflow on the refusal path
- [x] **K4 now has a real energize path.** The "zero callers" finding of
      2026-08-20 is stale: `safety_core_request_enable()` calls
      `relay_owner_command_energize()` (`safety_core.c`), reached from
      `link_task`'s enable handler, and it refuses ON when the safety
      thermocouple is declared absent or a trip is latched.
      **Enable is now GRANTED on real hardware (2026-08-24)** — the
      thermocouple is fitted, the stale S5 latch cleared, and the board
      reports `heating enable granted`. ~~The bench cannot demonstrate a
      genuine heat-enable until the safety thermocouple is populated~~ — that
      blocker is gone.
      **Observed closing, 2026-08-29.** Not with a meter and not with an LED
      — with the element. Until this date nothing in the normal firing path
      ever sent `SAFETY_CMD_REQUEST_ENABLE` at all: `profile_executor.c` and
      `autotune_engine.c` had *zero* calls to `safety_link_request_enable()`,
      so every firing and every autotune this firmware ever ran closed K1 and
      left K4 open. That is what "40 minutes of commanded heat moved this jig
      0.67 C" was actually measuring. With `heat_enable.c` wired in, the same
      jig on the same profile went 32.1 → 47.5 C, `safety_relay_energized`
      true on every poll of the run and false again the moment it stopped.
      A contact that passes enough current to move a thermocouple 15 C is
      closed. What is still unobserved is the *mechanical* state under a
      fault — a welded contact reading closed while the request is released
      — which is M1's LEDs, not this.
      And note what granting it exposed —
      `SaftyFW/TODO.md`'s "An uncommissioned safety processor grants heating
      enable": permission is given while S1's absolute ceiling is disabled for
      want of `abs_max_temp_c`
- [x] Rule engine drives relays through the existing owner arbitration —
      `rules_task.c` claims `RELAY_OWNER_RULE` and never writes the SX1509
      directly, so precedence is PROFILE/AUTOTUNE > RULE > MANUAL. Fails safe
      on safety fault, down link, OTA in progress, and on its own stale-tick
      watchdog. **Rules may never command a zone-assigned (PID/thermocouple)
      relay** — owner's scope rule, enforced in the evaluator, the task and
      the POST handler; heater relays stay readable as rule conditions.
      2026-08-22, verified on hardware before the bench was disassembled.
      **The rules engine was deleted on 2026-08-28 (M11)** — this bullet is
      kept because the arbitration it describes is still live and is what
      firing-profile relay/IO segments now claim through. The owner's scope
      rule survived the deletion intact: a segment may not command a
      zone-assigned relay either
- [x] Dashboard/readiness no longer report the safety link as up merely
      because the driver object exists — both call sites now consult the real
      staleness-gated `link_up`. This was a live false positive: the board
      reported "safety=up" with the UART unplugged. 2026-08-22
- [ ] S9 trip-ineffective escalation proven with a deliberately welded contactor
      (hardware-gated). **Not a SimFW task, checked 2026-09-03**: SimFW is gone
      (removed `8553244`); even the tool it was checked against for this exact
      question required "physically driv[ing] current into the CT," per that
      removal commit's own audit. S9's latch needs the CT's real analog
      current signal (see the M4 table row above for the code path), which
      only a hardware jig can supply, plus a CT fitted and commissioned
- [ ] Every guard exercised per
      [`GUARD_TEST_MATRIX.md`](firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md) —
      §2's host-provocation table is fully audited (452+/452+ checks pass);
      §3's hardware-trip rows remain open (no bench hardware attached)

## M5 — The link carrying real traffic

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phases 6–8, contract in
[`firmware/CommonFW/docs/LINK_PROTOCOL.md`](firmware/CommonFW/docs/LINK_PROTOCOL.md).

- [~] Current sensing: load-active detection and a power estimate (not an
      over/under-current trip) built and wired onto `SAFETY_CMD_POWER`.
      **2026-08-24: the calibration was never actually loaded** —
      `current_sense_set_cal()` had no caller anywhere, so `amps[]` was
      permanently 0 and `any_current_present` permanently false, silently
      disabling S3/S9/S11 and S6b's current-gated trip while S4 warned
      forever. Now loaded at boot and on every `COMMIT_CONFIG`, and presence
      detection is decoupled from `k_ct_v_per_a` (deciding whether current
      flows never needed a volts-per-amp scale). `CURRENT_SENSE.md` §5 and
      `CONFIG_REFERENCE.md` both claimed a wrong `k_ct_v_per_a` could not
      affect guard behaviour; that was false and is corrected. `context_valid`
      is also computed now — see M3's guard bullet. **Still open**: the
      per-channel CT-to-jack commissioning check on real hardware, and a bench
      measurement of the ADC noise floor to confirm the uncommissioned
      presence fallback margin (25 counts) sits above it
- [x] ESP → Pico context frames (`SAFETY_CMD_PUSH_CONTEXT`, incl.
      `relay_recent_mask`) built from live board state — 2026-08-18.
      **Hardware-verified 2026-08-24**: the Pico is on the bench, the link is
      up, and `context_valid` is computed from frames that actually arrive.
      The "no Pico on this bench" caveat this bullet used to carry is retired
- [~] Pico → ESP telemetry (status, diagnostics, firmware version, trip events,
      power) — all five frame types have working codecs, send paths, and
      `KilnFW`-side decode/dispatch, plus PC-facing `GET_DIAG`/`GET_TRIP_EVENT`
      subcommands (2026-08-18–19). **Now hardware-verified (2026-08-23/24)**:
      M0 is cleared, status/diag/power frames cross the real wire, and the
      diag/power applied counters were observed climbing at the expected
      0.5/s over a 90 s soak. Getting there needed a UART TX self-start fix —
      an edge-triggered TX interrupt that never re-armed stranded a frame in
      the ring silently, with no counter tripped
- [x] Pico never blocks on the link — all five no-wait rules audited clean
      2026-08-18 (no ACK/retry, bounded non-blocking TX, `safety_core.c` never
      calls into the link, correct task priority/core affinity)
- [x] Mutual version handshake: `ANNOUNCE_VERSION` both ways, `min_compatible`
      checked in both directions, both firmwares on the shared codec
- [x] TX ring reserves capacity for telemetry; log frames dropped above a 50%
      watermark and the drops counted
- [x] Borrowed-thermocouple staleness split correctly across S11/S13/S6 —
      audit-confirmed 2026-08-18, no code fix needed
- [x] `SAFETY_CMD_COMMIT_CONFIG_REJECTED` (0x20) — a refused commissioning
      commit used to be indistinguishable from an accepted one. The Pico now
      names the offending `param_id` and a reason code (RANGE /
      CONTRADICTION), and `safety_cfg_http.c` surfaces it in the page's error
      text instead of "sent, awaiting confirmation". 2026-08-22, host-tested
      both ends; not hardware-verified (M0)
- [x] `SET_LOG_LEVEL` (0x1B) reachable from the ESP — the codec and the Pico
      consumer existed with no caller, so the feature was dead. Now
      `POST /api/safety/log_level`, deliberately API-only (a bench knob, not
      an operator control). 2026-08-22
- [x] LCD stopped showing a raw `reason 0x%02X` where the web showed decoded
      words — the two same-language copies are one shared table
      (`safety_trip_words.h`). 2026-08-22

## M6 — Throw the liveness switch

The point at which the two processors become one system. Deliberately separate,
because it changes what a bare main board will do.

- [x] Link staleness → fault at a fixed 1.5 s ceiling, 30 s silence aborts a
      running firing, boot-time `FW_VERSION` request retried until answered,
      and a documented bench escape hatch (`safety_link_fault_on_link_loss`) —
      all built 2026-08-18/19. Code-verified and flashed. **The Pico is now on
      the bench (2026-08-24) so the "no Pico" caveat is gone, but the TIMING
      half is still unverified**: nobody has held the link down with a
      stopwatch to confirm the 1.5 s ceiling and the 30 s firing abort fire
      when they should. That is a bench procedure, not a code gap
- [~] GUI (web + LCD) surfaces safety temperature, enclosure temperature, and
      power — built and wired to the same status cache the wire frames land
      in. **The reason for the blanks changed on 2026-08-24 and the
      distinction matters**: frames now arrive every 500 ms, so this is no
      longer "no Pico has ever sent them". Safety temperature reads null
      because the safety MAX31856 is genuinely not populated (M3), and the
      three current channels read 0.00 A because no CT is fitted. Both are
      honest reporting of absent hardware, not a code gap and no longer an
      M0 consequence
- [x] **AP-fallback end to end, PASSED 2026-09-29** (`docs/BENCH_TEST_LOG.md`
      Test B): home network made unreachable (forget + forced mode-cycle —
      `wifi_forget` on the associated SSID does not itself force a
      disconnect), the board's own SoftAP came back up, home was restored via
      `wifi_add_network`, and the board rejoined with the saved-network list
      matching baseline. Still open: a router with deliberately-wrong static
      config was not part of this run — that specific scenario is untested;
      see `KilnFW/TODO.md` Wi-Fi

**2026-08-20: a large batch of UI, Wi-Fi, and boot-stability bugs were found
and fixed during a full hardware test pass** — profile/readiness reporting,
the LCD no-scroll rewrite, captive-portal DNS hijack, gzip content
negotiation, the Digital Fire built-in schedules, a thermocouple-fault page,
web DHCP/static-IP toggle, and others. Full list, and the ongoing ledger, is
in `firmware/KilnFW/TODO.md` and `firmware/KilnFW/docs/UI_PLAN.md`; two hazards worth reuse
were promoted to the decisions table below (internal-SRAM exhaustion at task
creation, and the UART owner's per-transfer heap churn).

## M7 — Repo reorganisation · CLOSED 2026-09-05

Owned by [`docs/REPO_LAYOUT.md`](docs/REPO_LAYOUT.md). All items done,
including `hardware/UnitTestFixture` (owner decision: KEEP, 2026-09-05). Full
detail: [`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m7--repo-reorganisation-closed-2026-09-05).

## M8 — Field updates

Owned by [`firmware/CommonFW/docs/UPDATE_PROTOCOL.md`](firmware/CommonFW/docs/UPDATE_PROTOCOL.md)
and [`firmware/SaftyFW/docs/BOOTLOADER.md`](firmware/SaftyFW/docs/BOOTLOADER.md);
tracked as `KilnFW/TODO.md` section 9 and `SaftyFW/TODO.md` phase 10.

Last, and genuinely last: a bootloader is new code in the component with nothing
behind it, and it is only defensible because SWD sits underneath as the recovery
path. Two facts set the shape of this milestone:

- **The RP2040 mask ROM has no UART bootloader.** Updating the Pico over the
  isolated link means writing one. It is written once over SWD and never
  updates itself.
- **You cannot OTA your way into being OTA-capable.** The ESP needs a new
  partition table with two app slots, and a partition table can only be written
  over a cable.

- [x] **Measure the isolated link's real error rate — done, and 115200 did
      not work at all under the old optocoupler pair.** The optocoupler pair
      capped the link at 9600 (see M0); that pair was replaced by a digital
      isolator (U6) on 2026-08-25 and the ceiling no longer applies, so the
      committed baud is being re-measured (`KILNCTL_SAFETY_BAUD_RATE` in
      `KilnFW/App/drivers/Kconfig`). The update transfer's error rate at
      whatever the current baud is, over a sustained multi-megabyte run, is
      still unmeasured. Retry cost is still 200 ms × up to 10
- [x] Real flash size established (N16R8, 16 MB/8 MB PSRAM) and declared in
      `sdkconfig` — 2026-08-17. **Both follow-ups are now done**: the
      two-app-slot table exists (`otadata`/`ota_0`/`ota_1`/`factory`/
      `pico_img`/`coredump`, and `factory` moved to 0x810000 on 2026-08-21),
      and the bootloader + partition table were flashed and verified on the
      physical board over JTAG on 2026-08-24. **Superseded 2026-09-17:** the
      single-slot table (`docs/OTA_SINGLE_SLOT_PLAN.md`) replaced `ota_0`/`ota_1`/
      `factory` with one `app` slot plus a `recovery` image, and `flash_firmware()`
      now writes `app`, so a bench-flashed build exercises the rollback/boot-confirm
      machinery
- [x] `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, app confirms itself only after
      NVS + safety link + web server are up — host-build-verified, not yet
      hardware-flashed
- [~] Pico flash layout/metadata frozen (reserved `signature[64]`, `sig_required`,
      pubkey region) and implemented in `metadata.c`/`flash_layout.h` — design
      and code done 2026-08-19; not flashed or hardware-verified (no RP2040
      attached)
- [~] Pico bootloader: GPIO6 low first, per-boot CRC, `boot_attempts` fallback,
      and a real recovery-mode UART1 receiver (not beacon-only) — all built
      and host-build-verified 2026-08-19; not flashed or exercised over a live
      UART1 link
- [x] Mutual protocol-version check landing with M5 — both firmwares on the
      shared `kilnlink_announce` codec, GUI shows both sides' versions and
      names which is older. Not verified against real mismatched hardware
- [~] Compatibility floor (frame ids `0x00`–`0x0F` reserved, never gated on
      `peer_version_compatible`) — frozen and implemented; not verified
      end-to-end against a live mismatch
- [x] Image header validated before the first erase (2026-08-17)
- [x] Challenge–response on the AP password, never crosses the wire, 3-failure
      lockout (2026-08-17)
- [x] Both update paths refused unless idle and cool, with the specific
      blocker named (2026-08-17)
- [ ] Link-loss heating block **not** bypassed during a Pico update — now
      **2026-09-21 SKIP note:** hardware-exercise attempt checked the premise first -- the bench Pico runs a flat image (two-slot bootloader install is owner-gated NO-GO per `docs/PICO_AUTO_UPDATE_PLAN.md` section 1), and the board's own `ota_status()` history shows the last Pico relay attempt already refused structurally (`REFUSED_RUNNING_IMAGE_OVERLAP`) before reaching the data phase, so no update -- and therefore no link-loss window -- can be pushed on this fixture today. No board state changed. Full record: `docs/COMMISSIONING_TEST_MATRIX.md` "Link-loss heating block during a Pico update -- 2026-09-21 hardware exercise (SKIP)".
      pinned in CI on both sides (2026-09-04), still OPEN as a
      hardware-exercise item (a test suite is not a substitute for running a
      real update on a real board):
      - KilnFW side: `firmware/KilnFW/App/test/test_safety_link_compile.c`
        now links the REAL `relay_authority_on_blocked()` (App/drivers/
        relay_authority.c — previously stubbed everywhere else in the host
        suite) against a real `SafetyLinkClass`, and proves
        `safety_link_set_update_in_progress()` (the call `ota_pico_relay.c`'s
        relay task makes around a Pico relay) does not relax an asserted
        `SAFETY_FAULT_SRC_SAFETY_LINK` fault, and that a link going stale
        mid-update still denies heat. Negative-tested: temporarily made
        `safety_link_set_update_in_progress(true)` clear `fault_sources`,
        confirmed 4 checks fail by name, reverted.
      - SaftyFW side: `firmware/SaftyFW/test/test_update_task_relay_wiring.c`
        source-scans the real, non-host-compilable `update_task.c` (same
        precedent as `test_safety_core_s8_wiring.c`/
        `test_safety_core_polarity_wiring.c`) and fails closed if it cannot
        locate either file or `relay_owner_command_energize()`'s real
        signature; pins that `update_task.c` calls no relay_owner_* mutator
        and touches no relay GPIO. Negative-tested: added a call to
        `relay_owner_command_energize()` into `update_task.c`, confirmed the
        scan fails by name, reverted.
      - Still needed: an actual Pico OTA exercised on real hardware with the
        link deliberately dropped mid-update, confirming no relay ever
        energizes and the fault stays latched after the update ends.
        **Blocked, 2026-09-18, by the two serially-blocking defects in the
        next items — not by prerequisite P1, which is closed.** The test design
        is settled
        (drop the link by halting the ESP mid-relay and hold past the 120 s
        `link_dead_hard_s` backstop — `firmware/SaftyFW/src/config_store.c:900`
        — so `SAFETY_TRIP_LINK_DEAD` latches, polling the relays throughout)
        and the slot image is built. The only missing thing is that the
        update cannot reach the data phase at all.
- [x] **An ESP-driven Pico OTA cannot reach the data phase — observed on real
      hardware 2026-09-18, FIXED the same day by `e59b0328`; only the hardware
      exercise remains.** The fix erases the Pico's destination slot in 4K
      sectors instead of 64K blocks (`UPDATE_TASK_ERASE_CHUNK_SIZE`, pinned to
      `HAL_FLASH_ERASE_SIZE`, the smallest unit the HAL can express) and feeds
      the hardware watchdog between sectors through one shared owning
      function, `watchdog_task_feed_if_all_within_deadline()`, which
      `watchdog_task_fn()` now calls too rather than carrying its own copy of
      the gate-then-feed sequence. The feed policy is unchanged — it happens
      only if every registered task is within its own deadline, so a wedged
      safety processor still starves the watchdog and still reboots, including
      mid-update. **Still needed: a real ESP-driven Pico OTA on hardware,
      reaching and completing the data phase.** The original observation, kept
      because it is the evidence the fix is graded against: the ESP staged the
      image and started the
      relay; the RP2040 then hardware-watchdog-reset partway through erasing
      the destination slot, so it never confirmed `RECEIVING`, and the ESP
      failed the relay at its 15000 ms erase timeout with `Pico did not
      confirm RECEIVING within 15000 ms of erase` (`RELAY_ERASE_TIMEOUT_MS`,
      `firmware/KilnFW/App/drivers/net/ota_pico_relay.c:111`; the message is
      formatted at `:448`). Evidence: `safety_get_diag()` immediately
      afterwards reported `boot reason: watchdog | state grace | uptime
      24007 ms`, against an uptime of 2552017 ms and state `armed` moments
      before. **The failure was safe** — relays R1-R4 read 0 throughout, no
      trip latched, the configuration survived (`config_version` 162,
      `config_crc` 53177, both unchanged) and the board returned to `armed`
      on its own — but the ESP-driven Pico update path is non-functional on
      this hardware today. Observed facts only, no root cause asserted here;
      the diagnosis lives in
      `docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md`
- [x] **A second, independent defect blocked the same path: the ESP staged the
      Pico image with the wrong CRC-32 parameterization — 2026-09-18, FIXED
      the same day by `fabd270f`; only the hardware exercise remains.** The
      arithmetic moved into its own module,
      `App/drivers/http/ota_image_crc.c`, specifically so it could be pinned
      by a known-answer test (`test_ota_image_crc.c`) — HTTP handlers in this
      project are target-build-only and do not link into the host suite, so a
      test written against `ota_pico_do_stage()` would never have run. Both
      ends of the comparison are now pinned to one value: SaftyFW's half has
      been held to the standard CRC-32 check value by
      `test_bootloader_metadata.c` the whole time. Reproduced numerically
      before the fix over a real 114796-byte image — the firmware's
      parameterization yielded `0xBA38A716`, exactly what the board reported
      when staging, against `0x83C472EF` for true CRC-32/zlib; the file was
      never wrong. **Still needed: an ESP-driven Pico update on hardware
      passing the verify step it could never previously pass.** The original
      diagnosis, kept as the evidence the fix is graded against:
      `ota_http_pico.c` seeded the accumulator with `0xFFFFFFFF` and applied a
      final XOR on top of `esp_rom_crc32_le()`, which already performs both
      inversions internally, so the ESP sent a different CRC-32 variant of
      the same, correct bytes and the Pico's verify step could never agree. It
      failed closed — a corrupt image could not reach the active slot — but every
      ESP-driven Pico update ended in `ERR_CRC_MISMATCH` regardless of link
      quality. **The two defects were serial, not alternative: fixing the
      erase/watchdog failure alone only advanced the failure to the CRC check
      rather than producing a working update — which is why both fixes are
      prerequisites of the one remaining hardware exercise.** Diagnosis:
      `docs/audits/pico_ota_staged_crc_mismatch_2026-09-18.md`. This also
      identifies the cause behind `firmware/SaftyFW/TODO.md` Phase 10's open
      "reconcile host-side vs ESP-side UPDATE_BEGIN image CRC" item, which
      recorded the symptom in September
- [x] MCP tools for OTA (ESP update, Pico update, status; the challenge tool was
      deleted with the AP-password HMAC 2026-09-29),
      plus explicit ESP and Pico rollback and a web `/ota` page — built and
      unit-tested against mocked HTTP (2026-08-18–19); not yet exercised
      against a physical board
- [x] `SAFETY_CMD_ANNOUNCE_REBOOT` sent before a routine ESP reboot so a
      firmware update doesn't trip S6(b) — 20 s grace window, host-tested;
      not hardware-verified (no board attached to confirm a real reboot
      suppresses the trip)

## M11 — The UI the owner actually asked for · *opened and CLOSED 2026-08-28*

Owned by [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md) and
[`firmware/KilnFW/docs/UI_PLAN.md`](firmware/KilnFW/docs/UI_PLAN.md). **CLOSED
— all items landed and are hardware/migration-verified.** A batch of direct
owner requests that changed persisted data structures and deleted the rules
engine subsystem: relay control consolidated into the diagnostics Danger
Zone; board-health/thermocouple-fault pages folded into `/diagnostics`;
safety pages grouped into one nav group; shared per-zone timing profiles
(`ZONES_CFG_VERSION` 8→9); relay/IO segments added to firing profiles
(`PROFILE_VERSION` 2→3); the rules engine deleted in favor of segments; LCD
pages consolidated the same way, plus a planned-profile preview. Full
detail, including the durable `zones_cfg_t`/`profile_t` migration-hazard
note (persisted structs that embed arrays by value displace every element
after an insertion — read this before touching either struct again):
[`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m11--ui-consolidation-full-detail-moved-2026-09-04).

## M12a — The commissioning surface lied · *opened and closed 2026-08-28*

**CLOSED.** An opus audit found the safety commissioning page reported
`{"ok":true}` on writes the Pico had actually rejected (four independent
defects: an ACK that couldn't fail, a rejection race that got dropped, a
stale NVS cache presented as current, and an unconditional `"set": true`).
All fixed and hardware-verified the same day (`ddbd024`, `3149393`);
`abs_max_temp_c = 80` is committed and confirmed on the bench, and fixing it
exposed a second defect (config page 1 always timing out — the ESP had never
once fetched a config page in the project's history), also fixed the same
day. The write-window constraint this uncovered (config writes refused
outside the ~60s boot GRACE period) shaped M12's design below. Full
postmortem: [`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m12a--the-commissioning-surface-lied-full-postmortem--opened-and-closed-2026-08-28).

## M12 — Commissioning the operator can actually do · *opened and CLOSED 2026-08-28*

**CLOSED.** All items landed and are hardware/code-verified, including the
2026-08-28 same-conversation additions and the 2026-09-15 S14/S15 correction
(`b5cb83a4`, `c0729e1e`). Every row in this milestone was already ticked by
2026-09-15 — found stale-presented-as-live during the 2026-09-16
roadmap-upkeep sweep and collapsed here. Full detail:
[`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m12--commissioning-the-operator-can-actually-do-full-detail-moved-2026-09-16).

## M13 — Every fault says what was detected, and what to do · *opened 2026-08-28*

A standing requirement from the repo owner, not a one-off fix: **"all faults
reported to the user should come with instructions on how to fix them or more
importantly what was detected wrong."** Note which half he called more
important — the cause, not the remedy. Treat this as a rule that applies to
every fault surface added from here on, not a milestone that closes.

It came from S6a, which is the worst case and therefore the right example. S6a
is `mainFault` (GPIO10) LOW, debounced 200 ms. The safety processor sees ONE
BIT and cannot know why — that independence is deliberate and is not to be
traded away. But the ESP does know: `fault_sources` is a bitmask of MANUAL /
PC_LINK / THERMO / SAFETY_LINK / APP / THERMAL_SANITY (`safety_link.h:139-142`),
and `dashboard_http.c:261` already reads it. So the cause was measured, held,
and simply never shown next to the trip.

**Landed 2026-08-28, all of it — S6a's fault-source decode (shared
`safety_trip_words.h` table across web/diagnostics/LCD, captured AT TRIP
TIME not read live), real numbers on every `safety_trip_t` cause line,
KilnFW-side fault coverage, and an explicit non-clearable notice for S9.**
Full postmortem: [`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m13m14--fault-reporting-and-verification-findings-full-detail-moved-2026-09-04).

**Second sweep, 2026-09-16** (following `6985c89b`'s zones-config
migration-persist fix, which explicitly did not review the rest): widened the
hunt for give-up paths whose only output is `ESP_LOGE` with no web/LCD/API
surface. Found and fixed two gaps, both in `kiln_cfg_swap.c`'s
`kiln_cfg_swap_boot_recover()` (the two-processor config-swap boot-recovery
state machine) — its seven failing branches (unreadable/CRC-bad pending
record, no safety link for PICO_OPEN/PICO_DONE, rollback-failed,
ESP_DONE re-read-failed with fallback-rollback-failed, ESP_DONE
esp/pico-mismatch, unrecognised marker) only ever logged and left heaters
alarmed with no operator-visible cause or remedy. Added
`kiln_cfg_swap_boot_fault_t`/`kiln_cfg_swap_get_boot_fault()` (same
first-one-latched-wins convention as `zones_cfg_load_fault_t`), wired through
`dashboard_http.c` -> `dashboard_status_http.c`'s `/api/status` JSON ->
`main_page.html`'s new banner -> `ui_page_home_refresh.c`'s LCD trip strip.
Also found, while wiring this, that `6985c89b`'s own
`zones_config_migration_persist_fault` fields were populated in
`dashboard_http.c` and consumed by `main_page.html`'s JS but were **never
serialized** into the `/api/status` JSON in `dashboard_status_http.c` — that
banner was dead code since it landed; fixed in the same pass. Host tests:
two new cases in `test_kiln_cfg_swap.c` (fault latches with an actionable
reason; first-one-wins across two different failures), both negative-tested
by hand against the production latch guard. Other candidate surfaces
reviewed and judged already adequate or out of scope this pass:
`estop_verification_clear()`/`safety_cfg_write.c` (already returns an
actionable reason to the HTTP caller), `ota_pico_relay.c`'s retransmission
give-up (already surfaced via OTA status), `safety_cfg_store_flush_if_dirty()`
(self-heals on next refetch), and `thermo_task.c`'s SWD-only
`s_reconfig_gave_up` counter, ~~not conclusively checked against the generic
TC-invalid path this pass — left open for a future sweep~~. **Closed the same
day, `9d600ac8`: `thermo_task_reconfig_gave_up()` is now wired into
`kilnlink_diag_t` bit4 (`link_task.c`) and surfaced on the ESP side as
`safety_tc_reconfig_gave_up` through `dashboard_http.c` ->
`dashboard_status_http.c`'s `/api/status` JSON -> `main_page.html`'s banner.**
This remains a
standing rule, not a milestone that closes.

**The clearing semantics, recorded here because they were only discoverable by
reading `safety_guards.c`:** an S6a trip LATCHES. It does not clear on its own,
a new firing does not clear it, and an ESP reboot does not. Only an explicit
CLEAR_TRIP does — and that clear is REFUSED while the cause persists, because
`safety_guards_try_clear()` (`safety_guards.c:209`) clears the state and
immediately re-runs the guard, and an unwindowed guard with the line still LOW
re-trips on that same tick. So the operator sequence is: identify the source,
remove it, then clear. None of that is currently told to the operator, and the
owner had to ask.

**"Identify the source" was not possible after the fact until 2026-09-24.** A
transient ESP-side fault source (guard 9 stale-tick, or a 1.5 s safety-link
staleness blip) asserts the mainFault line, the Pico latches S6a, and the ESP
then self-clears its own bits at profile stop, leaving no record of which source
fired (incident of that day, `docs/BENCH_TEST_LOG.md`, cause still unproven).
`5754603a` adds a 16-entry fault-source transition ring plus per-source rising-edge
counters in `safety_link.c`, reported on the existing `GET /api/safety/commissioning`
route (`fault_source_edges`, `fault_source_counts`, no new URI) and rendered by
`safety_get_commissioning`; read it before clearing any S6a that did not follow a
dual reflash. Not yet flashed to the bench board.

## M14 — Verification you can trust · *opened and CLOSED 2026-08-28*

Not a feature milestone. It exists because on 2026-08-28 the sentence "tests
pass, build clean, flashed and verified" could be true and worthless, and
almost every defect found that day was something reporting success it had
not earned. **CLOSED, all findings landed** — a `build_kilnfw` wrapper that
reported OK on a failed build, a `flash_firmware` that didn't check the
binary matched HEAD, two stack overflows found via unregistered margins, and
two new CI guard scripts that both caught real violations on their first
run. Full postmortem: [`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m13m14--fault-reporting-and-verification-findings-full-detail-moved-2026-09-04).
**The same bug class recurred 2026-09-09** (see the header of this file):
`profile_executor` (KilnFW, `379f3fe6`) and `current_task`/`discrete_task`
(SaftyFW, `3afc5ea6`) both overflowed real task stacks with no budget check
registered, same shape as the two 2026-08-28 findings — each fix landed
alongside the missing check (`379f3fe6`'s own budget row; `5b8fc53d` for
SaftyFW). `316967b7` closed the KilnFW-wide gap (28 previously-uncovered
tasks got budgets in one pass), then `c726748e` found even that coverage
overstates its own confidence for indirect-dispatch tasks — read that commit
before treating a green stack-budget run as proof.

## M10 — Instrumentation: make the board tell you when it is wrong · *CLOSED 2026-09-04*

Not a feature milestone. It exists because four separate defects in this
project were invisible for weeks not because they were subtle, but because
nothing on the board was counting the right thing — and in three of the
four, something *was* counting and reported the comfortable answer.
**All findings landed and are hardware-verified.** Full postmortem for each
(route-table overflow, safety-poll false timeouts, DRAM/stack-margin
instrumentation and its own blind spots, truncated-JSON readiness checks,
the heartbeat-contract guard, and the HTTP-concurrency-reset root cause):
[`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m10--instrumentation-findings-full-detail-moved-2026-09-04).

Owned by [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md) §§12–13.

## M15 — Architecture hardening · *opened and CLOSED 2026-09-04*

**CLOSED.** All 22 findings from the four-agent architecture review landed
the same day (12 files over the 1500-line rule split move-only, five new CI
drift/lint checks added, the SX1509/DRAM/duty-struct/mode-state/JSONL/
quantize/mcpkit/frame-A findings all fixed) — `build_kilnfw` + 21/21 host
tests green throughout. Informational carry-forward: the Pico is still on
protocol v8, which makes S13's BORROWED-zone indicator unreachable in
practice (fails closed and visibly, not a defect). Full per-item detail,
file maps and the "patterns worth copying" list:
[`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m15-architecture-hardening-findings-full-detail).

---

## M16 — Source layering and hardware abstraction · *opened 2026-09-05, CLOSED 2026-09-16*

**CLOSED.** Both reorganisations landed: the `drivers/` directory move
(`9f18ca5`, 2026-09-05) and all five HAL phases, and the one item that stayed
open past that — the hardware timing re-check for safety-link reply, display
frame time, and thermo read latency — got its last two live-board numbers on
2026-09-16 (`link_reply_us` measured 2026-09-14; `display_flush_us`/
`thermo_read_us` measured 2026-09-16). Everything named as unmigrated in the
plan (Wi-Fi/httpd/LVGL, OTA partition writes, the SaftyFW bootloader,
`firmware/UnitTestFw`) is an owner-decided permanent holdout, not open work.
Full detail, numbers and bars: [`docs/HW_ABSTRACTION.md`](docs/HW_ABSTRACTION.md).

---

## M17 — The zone graphic: configuration you can look at · *opened 2026-09-18; stages 1 and 3–5 landed*

An owner request, and a specific kind of instrument rather than decoration. At
the top of the **web** zones page sits a cartoon of a stacked kiln — octagonal
brick ring sections on a tube-steel stand with a hinged lid, no controller box
and no branding of any kind — drawn with exactly as many rings as
`thermo_count`, annotated per ring with the heaters, thermocouple and current
sensor that zone is configured for, in icons *and numbers*, plus icons for any
extra relays. Errors and warnings appear as badges that open detail on click.
A second, smaller piece: extra relays gain a **device type** — damper, outlet,
valve, fan, light, other — a fixed code-defined enum, one byte per relay,
rendered as a dropdown.

The point is at-a-glance confirmation that the right settings exist, which sets
an unusually harsh acceptance standard: **a graphic that renders plausibly
while the configuration is wrong or unknown is worse than no graphic**, because
the operator stops checking the fields under it. The plan's controlling section
is the one that decides how "unknown" and "not reported" are drawn so neither
can ever read as "configured and healthy", and its negative test is the one
that guards it — feed the renderer a response with the sensor associations
deleted and assert the unknown glyph appears and no configured-state icon does.
A well-meaning "sensible fallback" is exactly how this feature turns into a
confident lie.

**Decided, so they are not re-opened:** web only — the LCD zones page is
explicitly out of scope, it is 480x320 and must not scroll. The artwork is
inline SVG generated from the zone count inside `zones_page.html` itself, which
is one `EMBED_TXTFILES` blob, so a separate asset would cost new embed, gzip
and route plumbing to save bytes it would not save. The data contract extends
`GET`/`POST /api/zones` by a single `relay_types` array rather than adding a
route; **no httpd stack buffer and no zones JSON buffer is enlarged**, and the
graphic contributes zero bytes to any response because it is built in the
browser from numbers already on the wire. The relay device type lives in
`relay_names_cfg_t` with its own `RELAY_NAMES_CFG_VERSION` bump — **not** in
`zones_cfg_t`, even though a zones-schema bump is now authorized for the
parallel CT-channel work, because cosmetic per-relay data should not share the
PID gains' rollback fate.

**The one blocking hazard, and it was a data-loss one — CLOSED by stage 1,
2026-09-18.** `relay_names_validate()` silently discards the entire blob on any
version mismatch, resetting every relay name to blank, and only one version of
that blob had ever existed so no migration function existed for it. Bumping it
without writing one would have wiped every operator-entered relay name on the
next firmware update, with no notice. The migration landed in the same commit
as the bump, with the host test that proves a real v1 blob (frozen v1 layout,
v1 CRC) still yields every name under v2 firmware — negative-tested by breaking
the name copy and confirming that test goes red.

A second instance of the same hazard, not in the original plan, was found and
closed alongside it: the generic `pref_cfg_fs` bridge to the `cfg` filesystem
is parameterised by one fixed item size, so a v1-length *file* would have been
dropped silently rather than migrated. `relay_names_load()` now upgrades such a
file in place at the same rev before the divergence tie-break runs, with its
own host test. Inert on the bench at the time this was written — no board
mounted `cfg` yet; **superseded 2026-09-21**, the bench board (`8ab3b81a`)
now has `cfg` mounted and populated (7 files, confirmed via `GET
/api/cfgfs`), NVS still authoritative.

**Stage 1 landed**: `relay_device_type_t` (with `unset` as enum 0), the
`types[]` array, `RELAY_NAMES_CFG_VERSION` 1 → 2, the frozen
`relay_names_cfg_v1_t`, both migrations, and the accessors. **Note the
accessor names**: `zones_config_get/set_relay_device_type()`, because
`zones_config_get/set_relay_type()` already exists and means something else
entirely — the zone's *switching hardware* (SSR/contactor/mercury) behind the
relay-life budget. Two meanings of "relay type" now coexist; the collision
surfaced only as a duplicate-definition compile error.

Two faults the front end genuinely cannot see are recorded rather than faked:
there is no per-zone heater-load fault (only `ct_warn_mask`, which is silent on
any zone whose normal current was never measured, and which cannot assert at
all on this ~4 W fixture), and there is no live per-channel CT presence or
calibration-health flag as distinct from the config fields. The three questions
that were open for the owner are all answered as of 2026-09-18 and recorded in
the plan's section 10: an un-set relay type **does** get its own enum value 0,
`unset`, so a migrated board says "nobody has told me what this relay does"
rather than quietly claiming "other" (built in stage 1); a global safety trip
is drawn as **one banner across the whole graphic**, never per-zone badges; and
the per-zone heater-load badge **ships dormant and labelled "not measured"**
rather than being deferred.

Full detail, five-stage landing sequence and test strategy:
[`docs/ZONE_GRAPHIC_PLAN.md`](docs/ZONE_GRAPHIC_PLAN.md).

**Stages 3–5 CLOSED 2026-09-18 — the graphic itself now exists.** A cartoon
kiln sits at the top of the web zones page, drawing one glowing ring section
per configured zone from `thermo_count` — z0 top, z2 bottom — annotated with
each zone's heater relays, thermocouple channels and CT, plus icons for any
extra relay (derived as any relay bit no zone's `relay_mask` claims, never a
declared list). Every annotation has three renderings, not two: configured,
deliberately none, and not reported. Badges carry click-popups saying what they
mean and at what scope. A safety trip draws ONE banner for the whole kiln,
keyed to the live latched `diag_state` rather than the last trip event — the
bench board reports a ~39-minute-old `trip_reason` alongside a healthy live
state, which an event-keyed banner would have painted as a permanent false
trip.

**Owner corrections, 2026-09-18.** Two things the drawing got wrong once it
was in front of the owner. The large orange slabs under the top rim and at the
foot of each upper ring — the per-ring `kg-glow` ellipses, mostly painted over
by the ring bodies so only their top arcs showed — are gone; the small orange
heater indicator circles stay, one per ring. The lid was drawn before the rings
and floated above them on its own geometry, reading as an open ring rather than
a lid: it is now emitted last, in the body's own projection (same centre axis,
same `rx`/`ry` arc as the ring rim), filled with the card background so it
reads as a solid cap. `tools/check_zone_graphic_render.ps1` grew assertions for
all three facts and each was negative-tested by sabotage and a hand restore.

The same pass answered a second owner request about the settings below the
graphic: each per-group inheritance selector now shows or hides a bordered
frame containing exactly the fields that selector governs, visible only while
it says "Custom settings for this zone". Hiding is the element's own `hidden`
property, never an inline display style, and submission semantics are
unchanged — an inherited zone already saved through the terminal zone's stack.
`firmware/KilnFW/App/test/test_zones_group_frames.js` covers the rule,
including that zone 0's frames never hide.

**Stage 2 CLOSED 2026-09-18 — the type is on the wire and settable.**
`"relay_types"` is reported by `GET /api/zones` as one small integer per relay,
dense and 0-based like `relay_names`; `relay_type_N=` on the `POST` stores it,
refusing an out-of-range or non-numeric value with a 400 rather than coercing it
to `UNSET`; and each unowned relay gained a device-type dropdown beside its name
field, with `UNSET` ("not set") a real selectable choice that renders as the
unknown glyph. Measured cost: 24 bytes on a max-width response (6624 → 6648
against `json_cap` 7360), so no zones JSON buffer and no httpd stack buffer
grew. Web only; the LCD gained nothing. Not yet verified on hardware — the
set-over-HTTP/reboot/read-back check still needs a board flashed with it.

The controlling requirement is the plan's section 6: a graphic that renders
plausibly while the configuration is unknown is worse than no graphic. So the
render function is a pure JSON → HTML-string function with DOM insertion kept
separate, and `tools/check_zone_graphic_render.ps1` drives it under node
against fixture JSON — asserting that ring count follows configuration, that
zone order is top-down, that a missing field renders as unknown rather than
taking the page's own form default (`thermo_mask = 1 << i`), and that an
invalid config emits **no `<svg>` at all**. Both of those last two were
negative-tested by sabotage, hand restore and a re-run.

Colour never carries a badge's meaning alone: each badge gets a shape-distinct
glyph (octagon for a fault, triangle for a warning, dashed hollow circle for
unknown) plus an SVG `<title>` and the click-popup text. The two new badge
rules are therefore recorded in
`firmware/KilnFW/App/test/ui_status_color_allowlist.json` with that call-site
cue written out, which is what `check_ui_status_color.ps1` asks for — it can
see the CSS shape but not the JS-injected glyph.

Cost: +29,273 bytes raw, **+9,480 gzipped** on the embedded page — over the
plan's own 12 KB / 4 KB budget, recorded as a measured overrun in
[`docs/ZONE_GRAPHIC_PLAN.md`](docs/ZONE_GRAPHIC_PLAN.md) §5 rather than
restated. No httpd stack buffer and no zones JSON buffer grew; the live half
rides the page's existing 3 s `/api/status` poll instead of adding a fetch.
Not hardware-verified: another task owned flashing, so the board runs firmware
without this page — what is verified is the render function against real
captured board JSON.

---

## M18 — Full commissioning of the dev board · *opened 2026-09-21*

Owner instruction, 2026-09-21.

- Fully commission the bench board (both processors at HEAD, diverged
  config/filesystem items reset) and test every web function possible with
  the current dev hardware.
- Enumerate all web functions/buttons/options (every page, every control)
  into `docs/COMMISSIONING_TEST_MATRIX.md` (being written in parallel; link
  it) and mark each: testable on dev HW / hardware-gated.
- Test order, owner preference: (1) backend first via HTTP API and MCP
  facade, (2) then through the web interface (headless Chrome), (3) then LCD
  functions (touch injection + camera numeric sampling).
- Record results per function in the matrix; a failure becomes its own
  ROADMAP row.

**2026-09-21 bench findings, blocked/owner-gated:**

1. Web auth record rewrite: board already has an admin record; only paths
   are old password (unknown) or physical reset gesture (E-stop asserted
   then four LCD corner taps, `docs/WEB_AUTH_PLAN.md`). Owner-gated, needs
   hands at bench.
2. E-stop false verification record: CLEARED 2026-09-21 by
   `debug_reset(peer="pico")` — `get_readiness()` now correctly reports
   `estop_verified` as `not_done` (board-state fact, no commit).
3. Live-edit bench exercise: kilnctrl MCP tools landed (`bb61aac9`, fixes
   `c7d57ecc`/`75641b82`) — `profile_live_get`/`fork`/`edit`/`decide` for
   `/api/profile/live`'s five routes. **Bench exercise PASSED, 2026-09-29**
   (`docs/BENCH_TEST_LOG.md` Test A): short 45/48C firing, fork, in-bounds
   edit accepted, out-of-bounds edit refused 400, `zone_mask` change on the
   running segment refused 409, discard, stop — no trip, no reboot.
4. LCD blue profile button: CLOSED by owner decision, 2026-09-21 — code
   review of `UI_THEME_ACCENT_BLUE` accepted as sufficient given the
   camera's specular-glare limit on this region. **Addendum, 2026-09-23:**
   numeric sampling of this same button found and fixed a global MADCTL
   R/B swap (RGB `0x00` -> BGR `0x08`, `st7796_panel.c`); the glare
   confound on this region is still noted and unresolved. **Confirmed same
   day** with a second, independent R/B-asymmetric colour read away from
   glare, both directions matching BGR.
5. Zero-caller sweep deletion: DONE (`51effca5`) — 11 keep, 12 delete.

**2026-09-21 landings:**

- New kilnctrl MCP tools on main: `get_readiness` (`d7c32aac`, read-only
  `GET /api/readiness`); `profile_live_get`/`fork`/`edit`/`decide`
  (`bb61aac9`, `c7d57ecc`, `75641b82`); `flash_firmware()`
  `erase_partitions`/`confirm_erase` for the commission-flash NVS reset
  (`050baac7`).
- Web auth credential recovery, owner decision: erase the `nvs` partition
  via `flash_firmware(erase_partitions=...)` (`050baac7`) during the
  commission reflash, then bootstrap `web_auth_setup` from env vars.
  Done: `web_auth_setup` MCP tool landed and review-fixed (`2b46b1a8`).
- Login backoff ladder (owner decision 2026-09-21: fast first login, then
  5/10/30/60/300 s per IP, off-subnet clients pooled) — Done, landed and
  review-fixed (`8ab3b81a`).
- Done: `docs/COMMISSIONING_WEBUI_RUNBOOK.md` (`bb95e3ed`, review fix
  `58914003`) and `docs/COMMISSIONING_BACKEND_RUNBOOK.md` (`2c3d9787`,
  review fix `f2432e0c`).
- Done: reflash + NVS erase + bootstrap, 2026-09-21. ESP flashed to
  `8ab3b81a` with only the `nvs` partition erased (`flash_firmware(erase_partitions=...)`;
  `wifi`/`kiln`/`profiles` NVS namespaces untouched); `web_auth_setup` bootstrapped
  the admin record from env vars (web auth enabled, LCD auth off); login now
  answers 200 in ~0.9 s. Session timeouts (web 30 min / LCD 10 min) were
  restored to their correct values by `0edb60c9`/`9c4938fe` after
  `web_auth_setup` had persisted the `-1` never-expire sentinel from the fresh
  NVS erase. Pico unchanged at `a57d0138`. Board reachable at `192.168.1.156`.
- **Original blocker's two root causes both fixed on `origin/main`,
  2026-09-21:** `22b080bd` makes SaftyFW refuse an erase/program that
  overlaps its own running flat image (new state 9,
  `REFUSED_RUNNING_IMAGE_OVERLAP`); `27c25d44`/`68e0a4db`/`987050f6` scope
  SaftyFW's embedded build identity to the trees it actually compiles
  instead of repo HEAD, which is what made an ordinary KilnFW-only commit
  look like a SaftyFW change and trigger a spurious auto-update in the
  first place; `c48c9b4a`/`581f2679`/`cba52447` add the ESP-side state-9
  handling, a readiness text fix, and (compile-time, default **off**)
  `PICO_AUTO_UPDATE_ASSUME_BOOTLOADER_PRESENT`. With the kill switch at its
  default of 0, auto-update does not run at all in a normal build, so the
  4096 B `pico_auto_updat` task-stack overflow that started this was
  unreachable with the kill switch off. The stack itself is now raised to
  8192 B, 2026-09-21, owner-authorized (see the owner-decisions block
  below). None of this has been re-verified on hardware yet (no reflash
  since these landed).
- **M18 read-only sweeps done, 2026-09-21** (see the top-of-file entry
  above and `docs/COMMISSIONING_TEST_MATRIX.md`): 48/48 backend Class A
  rows PASS, 19/19 web-UI read-only rows PASS, against the still-running
  `8ab3b81a`/`a57d0138` pair. Remaining, in owner's stated order: reflash
  both boards to HEAD (needs the owner decision below first, since the
  stack overflow this uncovered is still live in source), acknowledge the
  standing `pico_auto_updat` crash report, then Class B/C backend rows, web
  UI write rows, and LCD rows.
- **M18 Class B backend rows done, 2026-09-21** (see the top-of-file entry
  above): 14/32 rows PASS initially, then a continuation pass ran the 9
  remaining rows — 1 more PASS (B30), 3 BLOCKED (B6, B9, B17), and one FAIL
  finding across 5 rows (B19–B23: kiln_configs store quarantined at boot).
  All 32 Class B rows are now either PASS, BLOCKED, N/A, or a diagnosed FAIL
  — none left NOT RUN. Class C (28 rows) deliberately deferred to the owner
  for row-by-row authorization rather than run as a blanket batch; most rows
  fall under this project's standing hard safety rules (estop-verify,
  reflash, real firing, unsafe relay drive). Remaining: owner authorization
  for Class C scope, a fix for the quarantined kiln_configs store, then web
  UI write rows and LCD rows.
- **M18 Class C backend rows, owner-authorized subset, 2026-09-21** (see the
  top-of-file entry above): ran the 16 owner-named rows (C1, C2, C9, C10,
  C11-C20, C27, C28). 2 PASS, 1 partial (C28 write-half blocked by tooling
  permission, not the board), 12 BLOCKED — 9 from the never-verified E-stop
  interlock (needs C5, still owner-gated), 4 from the safety processor's
  ARMED/GRACE config-write gate (needs a Pico reset to open, which this run
  could not do). One rule violation occurred and is fully disclosed in
  `docs/BENCH_TEST_LOG.md`: an unintended `debug_reset(peer="pico")` call
  during gate investigation reset the Pico despite an OpenOCD-level error
  (`boot_id` 159→178); no trip or config change resulted, and the opened
  grace window was deliberately not used to push through the blocked C17-C20
  writes. Remaining: C3-C8/C16/C21-C26 stay owner-gated; C17-C20 need either
  an owner-authorized Pico reset in a future session or a different
  safety-processor write path.
- **M18 Class C owner-authorized rows, 2026-09-21 (continuation)**: owner
  authorized C5, a deliberate Pico reset, and C3-C8/C16/C21-C26. C5 PASS,
  unblocking C9-C15 (all PASS: profile/live fork/edit/decide chain,
  firing stopped clean); C17/C19/C20 PASS (GRACE window reopened via a
  second deliberate reset); C18 BLOCKED (`CONFIG_KILNCTL_DEV_TOOLS` off on
  this build); C10 PARTIAL (aborted before the 4-hour full accept, to keep
  the heat run short); C3/C4 PASS (danger mode in/out clean); C25 PASS
  (backup export/import no-op round trip); C2 read-half PASS; C1/C9/C6/C16
  NOT ATTEMPTED (time-boxed out, not declined). C7/C8/C21/C22/C23/C24/C26
  DECLINED with disclosed reasoning: C7/C8/C21/C22 conflict with this same
  authorization's own "still forbidden: reflashing / acknowledging crash
  reports"; C23/C24 have no safe restore path while reflashing is
  forbidden; C26 rests on a stale runbook premise (`cfg` is now mounted and
  populated with 7 files, not inert). No board defect found; every
  BLOCKED/DECLINED outcome traces to build config, rule-text conflict, or a
  stale premise. Remaining: C1/C9, C6, C16 for a follow-up session; C7/C8/
  C21/C22/C23/C24/C26 need an owner ruling on the authorization conflict
  before any future attempt.
- **M18 web-interface class, first LIVE run, 2026-09-21**: ran
  `web_commission_row.py` (`tools/PcTools/src/kilnctrl/web_commission_row.py`)
  in live mode against the real board for the first time — previously only
  exercised by its own pytest suite. One login only, cookie reused for the
  whole class. All 12 wired rows run (W2/W6/W15/W16/W28/W48 PASS; W1
  PARTIAL — the driver's own login POST/session read-back passed but the
  login *form* was not submitted, see the matrix row; W3/W4
  FAIL-EXPECTED — no last-run/trip state to act on; W5/W29 skipped, both
  real writes with no safe restore path or forbidden by rule; W30 found a
  real driver defect — see below). Also drove 10 additional read-only
  page-load rows by hand through the same CDP path ahead of their `Row()`
  entries being added (W7/W21/W23/W25/W37/W39/W41/W43/W46/W49), all PASS.
  Three driver defects found and fixed (the first two during the run, the
  third in review of it): (1) `run_row_live()`
  logged in on every call — a class-wide sweep would have logged in once
  per row, against the one-login rule; added an optional `cookie` parameter
  so a caller can log in once and reuse it (new tests in
  `test_web_commission_row.py`). (2) `_web_commission_cdp.mjs` had no
  handling for native `window.confirm()` dialogs, which several controls
  route through (`app.js`'s `kcConfirm` is literally `window.confirm`) —
  the click's `Runtime.evaluate` hung for the full 20 s timeout with the
  renderer frozen on the dialog. Fixed by handling
  `Page.javascriptDialogOpening`: answered OK only for a row this driver
  classifies as a write/owner-gated action (`--accept-dialogs`), dismissed
  otherwise, and logged with its message either way — the same dialogs gate
  Danger Mode and the relay lifetime-cycle reset, so a blanket accept was
  not safe for a driver that must stay read-only unless the row is
  authorized. (3) W30's actual write (`POST /api/watchdog_cfg`) lost a race
  against the script's fixed post-click wait and immediate Chrome teardown,
  so the value never landed (confirmed unchanged via read-back); fixed in
  review by giving a row an `expect_post` path and having the CDP script
  wait, bounded, for that request to complete
  (`Network.requestWillBeSent`/`loadingFinished`) before screenshotting and
  teardown, with a bounded network-quiet wait for rows declaring none.
  Unit-tested only — **W30 needs a live re-run to confirm the write now
  lands.** No board defect
  found; uptime rose monotonically with no trip/reboot/firing throughout.
  Full detail, per-row evidence, and defect writeups:
  `docs/BENCH_TEST_LOG.md`'s "M18 web-interface class, first LIVE run"
  section; matrix rows annotated in `docs/COMMISSIONING_TEST_MATRIX.md`.
  **2026-09-21 update**: those 10 by-hand rows (W7/W21/W23/W25/W37/W39/W41/
  W43/W46/W49) plus one new write row, W31 (ramp-assist toggle, a single
  click on a stable id with no text entry, `expect_post="/api/ramp_assist"`,
  same restore-before-leaving shape as W30), now have `Row()` entries in
  `web_commission_row.py`, each with its own unit test in
  `test_web_commission_row.py` (73 tests passing, up from 41). `ROWS` now
  covers 23 of 51 runbook rows. Remaining unwired rows all need in-page
  text entry the CDP driver cannot do (click-and-screenshot only, no form
  fill) or are owner-gated/destructive: W8/W9/W10 (profile
  create/delete/favorite), W16-style edits with a real new value, W22/W38/
  W42/W50 (field-edit-then-save rows), W45/W51 (destructive
  import/Wi-Fi-forget), and the heat/E-stop/OTA/auth/Pico-reset rows named
  in the task's owner-gated list.
  **2026-09-21 further update**: added a `fills` primitive to
  `_web_commission_cdp.mjs` (a list of `{selector, value}` pairs applied via
  `Runtime.evaluate` setting `.value` and dispatching `input`/`change`
  before the click) and a matching `fills` field on `Row`, then wired W22
  (safety config's `#pcLink`), W38 (display brightness `#kcDpBrightness`)
  and W42 (kiln config save-as-new + delete, a dedicated
  `_run_kiln_config_create_delete` shape) — each reads its current value
  via `verify_endpoint` first, fills a distinct test value, confirms the
  change, then restores the original value (or, for W42, deletes the
  throwaway config it created) with a second POST plus read-back, all
  inside the row so nothing persists on the board. `ROWS` now covers 26 of
  51 runbook rows (89 tests passing, up from 73). W8/W9/W10 stay unwired:
  profile delete/favorite act on dynamically-rendered per-row buttons with
  no stable id, so a single fixed-selector fill risks hitting the wrong row
  or leaving stray data; the segment-builder shape needed for a real
  profile create is out of scope for one fills list. W50 stays unwired for
  a different reason than the others in its old group: its fields
  (`/api/unit_pref`/`/api/settings/tz`) only exist after client-side
  setup-wizard step navigation, which `fills` cannot drive (not a
  Wi-Fi-related skip). Results for the three new rows are NOT RUN — no
  bench access this pass; matrix updated in
  `docs/COMMISSIONING_TEST_MATRIX.md`.
  Review follow-up, same day: the CDP `fills` primitive now reads the
  assigned `.value` back before dispatching events and fails hard if the
  element rejected it (an unpopulated `<select>` keeps `''`, so W42's
  delete would otherwise have clicked on whatever the page's own fallback
  selection landed on), and `Row` gained `guard_fields` -- the other keys a
  whole-form Save posts alongside the edited one, snapshotted before the
  first Save and re-checked after the restore, since `restore_from_field`
  only ever puts the edited field back (95 tests).
  **Live run 2026-09-21 at 33124aa8 (fa8b449b):** W22 PASS, W38 PASS, W42
  FAIL. Root cause (docs/audits/w42_kiln_config_create_2026-09-21.md): the
  runner generated a 37-character throwaway config name against firmware's
  23-character `KILN_CFG_NAME_MAX_LEN`, so the create POST was correctly
  refused 400 and the runner only saw the generic "write did not land";
  page and firmware behave correctly. **Runner-side fix landed 2026-09-21**
  (`79f70f04`, `ea05886d`, `9c02787d`): the throwaway name is now
  `kc_test_<epoch>` (18 chars, under the 23-char limit) and the create step
  checks the POST status via the CDP driver's `post` field. W42 stays FAIL
  until re-run live (a re-run is in progress).
  **Runner redesigned 2026-09-21** (`e862a35a`): W42 now matches
  firmware's active-slot behavior -- `/api/kiln_configs/apply` is a real
  two-processor swap, not bookkeeping -- rather than the create/delete-
  same-slot shape that could never pass against the intentional H5
  backstop (deleting an active config is refused 400; see the delete-half
  FAIL below). **Live re-run 2026-09-22 on ESP `08f1c451`: board panicked**
  ("A stack overflow in task kiln_cfg_swap has been detected"), nothing
  moved, crash report left unacknowledged (`b024b33c`); firmware fix in
  flight, not landed. **W42 stays FAIL.**
  **Correction, 2026-09-21** (`459f692b`): C1/C9/C16 were already PASS
  earlier the same day; the doc's stale NOT-ATTEMPTED premise for those
  three was wrong, not the board -- corrected, no re-run needed. Board
  left with one harmless, byte-identical, currently-undeletable
  throwaway config (id=4, `kc_test_<ts>`).
  **W50 wired 2026-09-21** (`d19620aa`, `14e2324b`): setup wizard step 1
  deep-link (`#step=1`) now automated (flips `temp_unit`, restores it,
  verifies no collateral tz drift). W8/W9/W10 (profile create/delete/
  favorite) stay unwired -- no stable per-row ids the CDP driver's
  selector kinds could target -- **wiring now in progress** in a
  separate, not-yet-landed change now that `ccec2150`/`eec3e04b` added
  aria-label/css selector kinds and a `--steps` runner to
  `_web_commission_cdp.mjs`, plus a new standing check
  `check_web_commission_cdp_driver.ps1` (node/Chrome-gated, SKIPs
  cleanly without them) wired into `run_all_checks.ps1` phase 3;
  discovered check count 128 -> 129. `ROWS` covers 27 of 51 runbook
  rows as of this entry.
- **M18 web-interface class update, 2026-09-22**: W8/W9/W10 (profile
  create/delete/favorite) wired into `web_commission_row.py`
  (`23434e2d`, `f5a793ce` -- the latter fixing a W9 cleanup gap where a
  failed-looking create can still land on the board). `ROWS` now covers
  30 of 51 runbook rows. W8/W9/W10/W50 are wired but not yet run live.
  W42 stays FAIL, pending the `kiln_cfg_swap` stack-overflow fix
  (`76b78802`, in review) -- see the kiln_cfg_swap entry below.
- **kiln_cfg_swap stack overflow, found live 2026-09-22**: a live W42
  re-run on ESP `08f1c451` panicked the board ("A stack overflow in task
  kiln_cfg_swap has been detected") on `POST /api/kiln_configs/apply`;
  crash report left unacknowledged (`b024b33c`). Fix `76b78802` is in
  review, not yet flashed -- no `/api/kiln_configs/apply` call until it
  lands and is confirmed on hardware.
  **Fixed and flashed 2026-09-22** (`7e659e55`, `faae8492`, `987b84a6`;
  audit `docs/audits/kiln_cfg_swap_stack_overflow_2026-09-22.md`): the
  three scratch buffers causing the overflow moved to per-job internal-heap
  allocations, dropping internal DRAM `.dram0.bss` by 8496 B versus the
  static-buffer draft. ESP reflashed from a clean worktree to `7dcde0dd`
  (`flash_firmware`, verify passed, `boot_guard_reset` verified,
  internal heap free 25187 -> 35487 B); Pico untouched at `05f1ab1f`
  (stamp `987050f6`). The pending crash report was acknowledged
  (`crash_report_ack`). W42 re-run live: **PASS**, no reboot. See
  `12762a50`.
- **M18 web class, 2026-09-22 live results**: `41047846` wired W11/W18/
  W20/W33 plus a guarded-click helper and a W42 pre-read guard; `ROWS`
  now covers 34 of 51 runbook rows. Live run against `7dcde0dd`: W42,
  W50, W11, W18, W20, W33 all **PASS**. W8 (profile delete, first live
  run) **FAIL** -- the CDP driver's delete-button aria-label selector was
  not found on `/profiles`; a runner/page selector mismatch, not a
  firmware defect, fix in flight. W9 and W10 were correctly refused
  (not run) since they detected W8's leftover config first. 17 rows
  remain unwired by rule: W12/W13/W14 need a live firing; W17, W19, W24,
  W26, W27, W32, W34, W35, W36, W40, W44, W45, W47, W51 are the
  remaining owner-gated/destructive/no-stable-selector set. Full detail:
  `docs/COMMISSIONING_TEST_MATRIX.md`'s 2026-09-22 entry.
- **M18 web class update, 2026-09-22 ~04:00Z**: W8 root cause was the CDP
  driver clicking before the page's async `refreshAll()` had rendered, not
  a firmware defect; fixed with `clickWithRetry`. Re-run live against ESP
  `63a48ab3`: W8 **PASS**. Web class now 35/51 PASS wired. W9/W10 (profile
  delete rows) still never run live: a permission classifier refused the
  previous agent's retry, waiting on the owner's explicit go-ahead.
  Re-run 2026-09-22 at harness `63b62945` after the W20 idle-gate fix
  (done/aborted count as not running) and W50 POST-status grading: W20,
  W50, W8 all **PASS** live, no reboot, board state restored. Also landed:
  `kiln_config_apply` MCP tool (`2b9752ff`, kilnctrl facade now 180 tools,
  distinguishes the interlock 428 from the hardware-differs 428) and
  OT-P05 unreadable-vs-absent trip state (`4499314f`).
- **Owner requests 2026-09-22**: (1) 401 UX -- landed `87d3017b`: no
  "authentication required" page; an unauthenticated page GET 302s to
  `/login?return=` (open-redirect guard covers backslash); insufficient
  role on a page 302s to `/?admin_required=`, an API call gets 403 with
  `X-Kiln-Auth-Reason: insufficient_role`; `app.js` shows an admin-login
  modal and retries once on success; a 429 shows `Retry-After`. Follow-up:
  modal accessibility -- closed (this session): a shared `kcModalStack`
  (push/pop/isTop by id) now covers the confirm/alert modal, the login
  modal, and the forgot-password modal uniformly, so Escape only ever
  closes/cancels the topmost one regardless of which pair is stacked or
  which modal's `document` keydown listener registered first (previously
  only login-over-forgot-password was handled); a shared
  `kcFocusFirstEmpty` helper focuses the first empty field on open
  (username else password on login, username else the TOTP code field on
  the reset step, which now also carries over the login username).
  Focus trap and focus-restore-on-close were already correct and already
  covered by tests. Retrying through the safety-ack wrapper is still
  open. (2) Session/LCD timeouts -- already implemented
  (`web_auth_session.c`, `lcd_auth_state.c`); web timeout verified live
  PASS 2026-09-22, LCD timeout SKIP (no PIN set on bench); see `b3426467`.
  (3) `kiln_configs` apply reported `diverged=true` on a successful
  no-op self-apply: the post-swap clause read the ESP's own zeroed crc
  cache tag (`docs/audits/kiln_config_self_apply_diverged_2026-09-22.md`,
  `c3639d07`); fix in review with two required fixes being applied (a
  mutex creation race, and a "heaters disabled" log overclaim), then flash
  and re-apply on the bench.
- **check_kiln_auth_config_isolation.ps1 false positive, fixed `4a07a4ef`**:
  the check misread `diagnostics_http.c`'s refusal lines as a violation;
  suite was red at origin/main from `f564bf03` until this fix landed.
- **Bench reflash, 2026-09-22 ~04:00Z**: ESP reflashed to `63a48ab3` (flash
  verified, ELF archived as `KilnCtrl-da119321dcbb.elf`); Pico unchanged at
  `05f1ab1f` build.
- **C6 Wi-Fi factory-reset driver-storage self-check, 2026-09-22**: ran on
  hardware at `63a48ab3` -- serial log showed `nvs`/`net80211` key count 0
  before `esp_wifi_restore()` and 0 after. Verdict PASS for the leak, with
  the caveat that before=0 means the erase path was not exercised (RAM
  storage mode in `wifi_prov.c`). Detail:
  `docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md`. Do not
  re-dispatch.
- **Silent host-test build regression, 2026-09-22**: `0edfb313` dropped
  `nvs_entry_find`/`next`/`release_iterator` stubs, breaking the KilnFW
  host-test build silently; fixed in `032651c0`. A `run_all_checks` gate
  for the KilnFW host-test build landed 2026-09-22 (`a7088abc`,
  `check_00_kilnfw_host_tests.ps1`, phase 2, isolated per-PID build dir,
  excluded under `-Fast`; 130 checks discovered, 56/56 executables).
- **Process note, 2026-09-21/22 evening:** every code review this window
  was done by a sonnet reviewer because opus returned HTTP 500 on every
  attempt. **Opus re-review complete, 2026-09-22**: `978a5a2a`/`3db48d8a`,
  `bea1d8b1`, `acb1f970`/`0f96c570`, `17a4dff0`, and the kiln_cfg_swap fix
  chain above are all clean except one real defect -- the 428
  hardware-differs message lacked a sentence separator and filled its
  192-byte buffer exactly -- fixed with 256-byte buffers in all three
  callers, pushed as the `81a95b03` range.
- Tooling: `d473811a`..`502e69a5` centralized the default HTTP host
  (`KILNCTL_HOST` env var, opt-in last-seen cache, guaranteed AP fallback,
  atomic settings writes); the kilnctrl MCP server was restarted at
  `502e69a5`; `KILNCTL_HOST=192.168.1.156` is now set in User scope.
  `0956c0d1`/`06ac13cd` add a device-side log line when the login endpoint
  refuses with 429.
- `KILNCTL_AP_PASSWORD` is still unset, so `flash_firmware()` cannot clear
  `boot_guard` (count sits at 2).
- **2026-09-22 correction:** `KILNCTL_AP_PASSWORD` is set in User scope (verified via `[bool][Environment]::GetEnvironmentVariable(...)`); the "still unset" line above is stale. Log evidence: the `7dcde0dd` reflash this same day reported `boot_guard_reset` verified (see the kiln_cfg_swap entry above), so the counter has cleared at least once; whether it is clear as of the latest reflash (to `63a48ab3`, which did not report a boot_guard result) is not yet observed.

**Owner decisions open, 2026-09-21:**

1. **Decided 2026-09-21:** `pico_auto_updat` task stack raised 4096→8192 B
   (this commit) — the measured ~3104 B static ceiling excluded flash/NVS
   internals the embedded-staging path calls into, which is what actually
   overflowed on hardware. Owner: "Raise the stack and dont ask for
   permission in the future."
2. **Decided 2026-09-21:** readiness status for the auto-update gate while
   it is compiled off stays `ok` with the existing "deliberately_off" text
   — owner kept it as is.
3. **Moot as of 2026-09-22:** `KILNCTL_AP_PASSWORD` is now set in User scope. Owner directed a dedicated bench AP test password be created and set in User scope; that has been done. Whether `boot_guard` is clear as of the most recent reflash is not yet observed (see the 2026-09-22 correction above).
4. Whether the next commission reflash should reset NVS more broadly than
   just the `web_auth`-recovery erase already done 2026-09-21, given the
   `zones_cfg`/config-schema rollback hazards documented in CLAUDE.md. Still
   open.

**2026-09-23 pending, all blocking the bench reflash + commission pass:**

- [x] Pin the bench's hand-set `sdkconfig` values in `sdkconfig.defaults`
  (Wi-Fi static RX buffers 10, BA window 6, lwIP OOSEQ pbufs 4;
  `GPIO_PROBE` pinned unconditionally, no `DEV_AFFORDANCES`-style symbol
  exists to gate it on) — done, 2026-09-23 (`a513aa75`, watched-key coverage
  `0a8db924`).
- [x] Stale published `build/`/`sdkconfig` sibling guard in the stack-budget
  checkers, plus a `build_kilnfw()` refresh — done, 2026-09-23
  (`91477e4c` fix, `b9f574d3` wired the regression test into
  `run_all_checks.ps1`, `5a72eb50` skips the refresh when ninja did not
  relink).
- [x] `check_00_kilnfw_target_build.ps1` publishes `bootloader.bin`/
  `partition-table.bin`, not just `KilnCtrl.elf`/`.bin` — done, 2026-09-23
  (`d23d4eae`, freshness-gate fixes `51ae1ce0`/`ad2ee170`) — closes the
  second bench-commission gap noted in the twenty-ninth sweep above.
- [x] OTA transient task handles: `vTaskDelete` on the ESP rollback task,
  idempotent `stack_margin_register` — done, 2026-09-23 (`95be3327`,
  opus-review fixes `22fcc257`/`24e59726` comment-only follow-up).
- [x] Bench ESP reflash — done, 2026-09-23: `9c26dd91`, `nvs` erased, web
  auth re-bootstrapped, readiness 16 ok / 2 not_done / 3 other. See the
  top-of-file entry above for findings (D2-D5).
- [x] Bench Pico reflash — done, 2026-09-24: `6bb41fe1` (SaftyFW build
  `d6309f4a`) via `debug_program(peer="pico")` after the owner reseated the
  CMSIS-DAP probe.
- [x] `estop_verified` closed end to end, 2026-09-24 — S7 latched, verified,
  cleared, and recorded via the new `estop_verify` MCP tool (`fc16c186`,
  `055703af`). Class C heat/firing commissioning rows are now unblocked —
  not yet run.
- [x] First real-hardware bench_test heat/LCD/stack/web suite runs, 2026-09-24
  — see the top-of-file entry above and `docs/BENCH_TEST_LOG.md`. Every FAIL
  traced to the runner, not firmware; fixes landed same day.
- [x] Fix LCD-01/08/09/14/16 judge/navigation defects — done, 2026-09-24
  (`0df96d5d`). Further review-fix chain landed 2026-09-25: `d4e7ff29`/
  `f40e8d37` (harness, `cases_lcd.py`) and `0ef18917`/`466b29b2` (firmware,
  PIN keypad key-height regression and an LVGL-off-task call fix -- **not
  yet flashed to the bench**, ESP still `111b1b6f`). **Rerun done
  2026-10-01** (`20261001T155811Z_lcd`, ESP `eb83c1ac`, defaults, no heat): LCD-08/09/14/16/21
  PASS, LCD-01 and LCD-19 INCONCLUSIVE (camera exposure/cast; LCD-19 stop_gated
  not exercisable without `allow_heat`), the rest NOT_RUN (not_implemented or
  precondition absent); no FAIL. **Full rerun with heat opt-ins, 2026-10-01** (`20261001T183355Z_lcd_lcdsuite3`,
  `90fc6658`+`e52f256d`): LCD-08/09/14/16/19/21/22 PASS (LCD-16 rewind fix confirmed,
  7/7 pages; LCD-19 stop_gated and LCD-22 exercised), LCD-01 INCONCLUSIVE (camera cast), no FAIL.
- [x] Fix the SK-01/02 noise-tolerance/fw_commit-gate issue — done,
  2026-09-24 (`866003ea`; plan-note follow-up `9d905ab9`). **Still pending:
  rerun** the stack suite against this fix — the last attempt
  (`20260924T084524Z_stack`) hit a preflight refusal on a stale MCP server,
  unrelated to the fix.
- [ ] Open, 2026-10-01: after a double ESP reset the first reset left the board
  unreachable over UART and HTTP for minutes, then an S6b (reason 7) trip latched
  (cleared once with owner authorization, stayed clear). Likely a stale host
  serial session; not established. Reproduce with a live-log capture before
  changing firmware. Also open: `info_uart_bridge` lost 176 B free on
  `eb83c1ac` with no known cause.
- [x] Capture the `idle`-load stack-margin baseline at the currently running
  commit — done, 2026-09-24 (`5c44ae95`,
  `docs/stack_margin_baseline/stack_margin_idle_111b1b6f_20260924T175921Z.json`).
- [x] Capture the `mid_firing` and `web_ui_open` stack-margin baselines at the
  currently running commit (`111b1b6f`/`6bb41fe1`) — done, 2026-09-24
  (`e18c645d`; `web_ui_open` had never been committed before, so this is its
  first baseline, not a recapture). Only `kiln_io_owner` shrank more than
  256 B against the `75a5e459` mid_firing baseline (2708 to 2100 B free, still
  OK). Both files record `profile_executor` at CRITICAL (468 B of 4096 B
  worst-since-boot) during the firing; a stack raise is dispatched. **Raised
  2026-09-24 (`b1f6c127`):** `profile_executor`'s declared
  stack 4096 -> 6144 B (`profile_executor_start.c`, INTERNAL DRAM -- this task
  writes NVS on its tick path and cannot use a PSRAM stack). `info_uart_bridge`
  (976 B/3584 B, 27.2%) and `lvgl` (1968 B/8192 B, 24.0%) also read LOW in the
  same two captures but both sit above the 15% CRITICAL threshold, so neither
  was raised. `check_executor_task_stack_budget.py`'s own static-path model
  moved from 940 B/22.9% (LOW) to 3148 B/51.2% (OK) at the same measured
  deepest path (1776 B, unchanged); DRAM impact is +2048 B of internal-heap
  task-stack allocation at runtime, not a `.dram0.bss` change (the stack is
  allocated dynamically by `xTaskCreatePinnedToCore`, not a static array) --
  `check_kilnfw_dram_bss_budget.ps1` still passes at the same static `.bss`
  figure (97256 B of a 101000 B ceiling).
- [x] Raise the `info_uart_bridge`/`lvgl` LOW margins noted just above --
  done, 2026-10-01 (`eb83c1ac`, Opus-reviewed): `lvgl` 8192 -> 10240 B
  (internal DRAM; its UI pages write NVS), `info_uart_bridge` 3584 ->
  4096 B (PSRAM). `adaptive_tune_zones[]` (2700 B) moved to PSRAM via
  `EXT_RAM_BSS_ATTR` to fund the internal-DRAM half; net `.dram0.bss`
  99672 -> 99016 B against the 101000 B ceiling (1984 B headroom). Flashed
  and bench-verified same day: `get_stack_margin` now reads `lvgl`
  4640/10240 B free (45.3%) and `info_uart_bridge` 1624/4096 B (39.6%),
  both clear of the 15% CRITICAL threshold. Full detail:
  `docs/BENCH_TEST_LOG.md`'s "lvgl/info_uart_bridge stack raise" entry.
- [x] LCD-19 FAIL on run `20260924T180332Z_full` ("Start tap after the LCD
  timeout did not raise the PIN keypad", `keypad_raised=false`) root-caused
  2026-09-24 as a runner defect, not firmware: `_wait_for_overlay_names(present=True)`
  in `cases_lcd.py` exits on any non-empty tap-target set, and the home
  page's own buttons satisfy it before LVGL processes the click (the case ran
  0.92 s against a 2.0 s timeout). Firmware force-lock-on-enable
  (`ui_lcd_lock.c`, `security_backend_web_auth.c`) is correct. Fix (a
  baseline-then-changed wait) has landed: `_wait_for_overlay_names()` in
  `cases_lcd.py` takes a `baseline` and waits for the set to change (on main at
  59c9306a). **Bench rerun 2026-09-30** (`20260930T043143Z_lcd_lcd19_rerun_0929_59c9306a`):
  still INCONCLUSIVE -- "could not exercise: wrong_pin_refused, right_pin_started,
  stop_gated" even with `KILNCTL_LCD_PIN` set, so the original NOT_RUN/missing-PIN
  case is ruled out but the case still can't complete; under investigation, not
  chased further in that run. Same rerun also surfaced a new LCD-16 FAIL
  (`click_by_name('settings')` -> `not_found`), unrelated to any fix under test
  here; also under investigation. **Reran again 2026-09-30** against harness
  `7550fdf6` (LCD-19 follow-up fixes `1ae70883`/`fab4f456`/`7550fdf6`) on the
  bench board at ESP `50edd830` (`KilnCtrl-6c9152cebb9d`), Pico `405d3c54`:
  `20260930T073410Z_lcd_lcd_rerun_0930_7550fdf6` and a same-day
  `20260930T073503Z_lcd_lcd_rerun_0930_7550fdf6_r2` -- LCD-16 now PASS (7
  pages, no retries; the prior `settings` not-found FAIL did not recur) and
  LCD-14 PASS, but **LCD-19 FAILed both runs with a new failure shape**: every
  PIN digit tap returned `ok` for both the wrong PIN and the correct PIN,
  `wrong_pin_refused=true` (correctly refused), `start_click_result=ok`, but
  `right_pin_started=false` -- the firing never actually started after the
  correct PIN was accepted. No `page_before`/`navigate_home` evidence was
  recorded either run. This is a different failure shape from both the prior
  `keypad_raised=false` FAIL and the `59c9306a` INCONCLUSIVE -- root cause not
  yet found; board was left idle both runs, no trip, no crash. **Harness
  fixes landed 2026-09-30** (`46d9726d` waits for a stable, digit-bearing
  keypad read before typing a PIN; `b2cf2f89` adds `enter_pin_verified()` to
  `ui_test_client.py`, verifying each PIN digit was actually applied before
  typing the next, and only reports `wrong_pin_refused=True` when the
  OK+Cancel dialog is present and its dots are cleared) -- pushed to
  `origin/main`. **Bench reran against `b2cf2f89`** (firmware build "Sep 30
  2026 01:50:10"), three runs: `20260930T103536Z_lcd` FAILed with "keypad not
  raised" after the LCD's own idle timeout, suspected a harness race
  (`_wait_for_overlay_names` has no raised-then-closed detection), fix in
  progress; `20260930T103634Z_lcd` INCONCLUSIVE with only `stop_gated`
  missing -- wrong PIN refused, right PIN accepted, all 6 digits verified,
  but the case never presses the Confirm Start dialog, so the executor stayed
  idle throughout; `20260930T103752Z_lcd` INCONCLUSIVE on
  `keypad_closed_before_entry`. Board stayed healthy across all three (armed,
  no trip, no reboot, no crash). Three gaps remain: (1) `stop_gated` is
  structurally unreachable today -- `pin_cfg`'s `firing_active_with_lock`
  condition is never set because LCD-19 never presses Confirm Start, so no
  firing is ever active to test stopping; needs an owner decision on whether
  the case should press Confirm Start or on another way to arm that
  precondition. (2) The raise-detection race in `_wait_for_overlay_names`
  (harness fix in progress). (3) The keypad's own self-close cause is
  unknown -- candidates in `ui_lcd_lock.c`'s `tick_timer_cb` (inactivity
  expiry, policy disabled, an unlocked->locked edge, or
  `ui_lcd_lock_force_lock`) -- needs a serial log capture on COM14 during a
  rerun to narrow down. **Harness fix landed 2026-09-30** (`4754e91f`): the
  keypad-raise poll now uses a tail-based FAIL judgment (FAIL only when the
  last 2 reads after the last empty/truncated read are identical, real,
  non-truncated and not the keypad), fixing gap (2) above; a keypad that
  raised then closed now reads INCONCLUSIVE with `keypad_raised_then_closed`
  recorded instead of a false FAIL. Reruns on `4754e91f`, firmware `9b77e2b2`
  (built 2026-09-30 08:49:24Z), `allow_heat=False`: `20260930T185935Z_lcd`
  INCONCLUSIVE with only `stop_gated` unexercised (keypad raised, wrong PIN
  refused, right PIN started); `20260930T190017Z_lcd` INCONCLUSIVE with
  `keypad_raised_then_closed=true`, reproducing gap (3) on hardware -- the
  keypad raised ~1 s after the Start tap then the home screen returned, with
  no `ui_lcd_lock` line in the serial log during that window. Board healthy
  both runs. **Keypad self-close root-caused and fixed 2026-09-30**
  (`7a71229b`): `ui_lcd_lock.c`'s `tick_timer_cb` force-locked every tick and
  set `s_was_locked=false` while the lock policy was disabled, so the first
  tick after the policy was re-enabled saw a spurious unlocked->locked
  (relock) edge and closed the keypad the tap had just raised as its own PIN
  gate -- the relock edge now excludes a keypad it raised itself; prompts and
  confirm dialogs still close on relock (owner 2026-09-28 decision,
  unchanged), and every lock-driven close now logs its reason. Flashed to the
  bench from a clean worktree, verified, ELF archived
  `KilnCtrl-95f9e0e194e1.elf`, boot_guard persisted 0/0, no trip.
  `stop_gated`'s opt-in firing landed the same day (`aaae0a23`): a new
  `lcd_stop_heat` mode (runner/MCP param, `bench_test.ps1 -LcdStopHeat`) lets
  the case start a short API-started BENCH_HP firing so `firing_active_with_lock`
  is actually set; teardown verifies idle and relays off, and reasons are no
  longer clobbered. Harness commit `4ecadec6` also fixed
  `_wait_for_home_settled` to judge on target-present/Cancel-absent over
  consecutive reads (min 2.5 s) instead of full name identity, since the
  Elapsed label changes every second; it accepts a truncated read containing
  the target and logs every read. Three bench runs followed: `20260930T200715Z_lcd`
  (old firmware, harness `aaae0a23`) INCONCLUSIVE -- the post-heat home page
  never settled, cleanup verified; `20260930T203132Z_lcd` (firmware
  `7a71229b`, harness `4ecadec6`, `allow_heat=False`) INCONCLUSIVE by design
  (`stop_gated` needs heat) but confirmed the wrong PIN is refused, the right
  PIN starts, and the keypad no longer self-closes; `20260930T203243Z_lcd`
  (`allow_heat=True`, `lcd_stop_heat=True`) INCONCLUSIVE -- PIN flow correct,
  firing started, relock correctly closed only the open confirm dialog (device
  log: "LCD relock edge: closed open confirm dialog"), but all 20 post-relock
  home reads showed `Plan` rather than a `Stop` target, so Stop was never
  tapped; the ~13.7 s firing ended clean, teardown verified idle/relays off,
  no trip, no reboot. Full detail: `docs/BENCH_TEST_LOG.md`'s 2026-09-30
  section. Still open: why the post-heat home page shows `Plan` instead of
  `Stop`, diagnosis in progress. **Root-caused and fixed 2026-09-30**
  (`e388752c`, Opus-reviewed): `UI_TEST LIST_TAP_TARGETS`'s ~253-byte reply
  truncated all 20 `allow_heat_settle_reads` polls before reaching the home
  page's merged Start/Stop button -- the walk order put the WiFi label,
  temperature readings, and the chart legend (`Plan`, not a button) first.
  The confirm dialog the relock closed was a stale Confirm Start from the
  earlier right-PIN sub-check (intended relock behavior, not a bug). Fix:
  `log_all_tap_targets` (`kiln_ui.c`) now walks each group twice, actionable
  targets (`lv_button` and its subclasses, including list rows and msgbox
  footer/header buttons) before non-actionable ones, overlay-first group
  order unchanged; the harness also dismisses a stale Confirm Start via
  Cancel before the API-started firing (`allow_heat_pre_start_dismiss`).
  Flashed to the bench from a clean worktree at `e388752c` (an earlier
  worktree's `build/` lacked `build_info.h` and was correctly refused as
  stale; rebuilding and reflashing verified OK), ELF
  `KilnCtrl-5384de5815b4.elf`, boot_guard persisted 0/0, no trip. Bench runs:
  `20260930T212112Z_lcd` (`allow_heat=False`) INCONCLUSIVE by design
  (`stop_gated` needs heat), wrong PIN refused, right PIN started, settle
  reads OK; `20260930T212155Z_lcd` (`allow_heat=True`, `lcd_stop_heat=True`)
  **PASS, exit 0** -- `stop_gated=true`, settle reads now contain
  `Stop`/`Pause`/`Edit`, Stop click `ok`, `bench_cleanup` verified idle and
  relays de-energized, no reboot/crash/trip. Known remaining gap:
  `lv_keyboard` is still reported as one rectangle rather than per key
  (pre-existing). LCD-19 is now closed/passing. Full detail:
  `docs/BENCH_TEST_LOG.md`'s 2026-09-30 "LCD-19 root-caused and fixed"
  section.
- [x] LCD-09/LCD-16 regression from the `e388752c` LCD-19 fix, found in run
  `20260930T215921Z_lcd` -- resolved 2026-09-30/10-01. Root cause was not
  `e388752c`'s tap-target reordering: `kiln_ui_click_by_name()`/
  `list_tap_targets` dispatch the tap-target walk to `lvgl_port_task` with a
  300 ms wait (`UI_WALK_WAIT_TIMEOUT_MS`), and a timeout on a busy UI task
  returned `n=0`/truncated, which the PC side reported as `not_found` rather
  than "walk didn't run yet" -- the same symptom reproduced pre-`e388752c`
  (run `20260930T043143Z_lcd_lcd19_rerun_0929_59c9306a`, firmware `8ed37d8d`),
  ruling out that fix as the cause. Fixed in two steps: `82de0234` first made
  the PC harness retry a `not_found` (2x, 0.15 s pause, counted separately as
  `not_found_retries`) as a stopgap -- rerun
  `20260930T234916Z_lcd_harness_retry_verify` got LCD-16 to PASS but left
  LCD-09 INCONCLUSIVE (a busy `list_tap_targets` read against the topbar
  anchor, `profiles_count 29` vs `row_count 0`). `232e668f` + `c471101c` then
  fixed it at the source: firmware gained `KILN_UI_CLICK_WALK_BUSY`/
  `UI_TEST_CLICK_WALK_BUSY=0x08` (no UART protocol version bump needed, same
  precedent as OFFSCREEN `0ef18917`), and the PC side gained busy derivation
  for `list_tap_targets` (count==0 and truncated) via
  `_list_tap_targets_resolving_busy()`, used by LCD-09/14/16, plus a
  `walk_busy_retries` counter and host-test source-pattern pins. Flashed
  `c471101c` 2026-10-01 (ELF `KilnCtrl-39bdbebe1651.elf`, running `app`,
  boot_guard persisted 0/0, no trip). Verification: run
  `20261001T011155Z_lcd_walk_busy_verify` -- LCD-08/09/14/16/21 all PASS
  (LCD-01 INCONCLUSIVE on camera color, LCD-19 INCONCLUSIVE by design, no
  heat requested; LCD-14/LCD-16 each needed one `walk_busy_retries`); run
  `20261001T011311Z_lcd_walk_busy_lcd19` -- LCD-19 PASS. Full detail:
  `docs/BENCH_TEST_LOG.md`'s 2026-09-30/10-01 entries for this regression and
  its resolution. **2026-10-01 follow-up:** run `20261001T172420Z_lcd_lcd22run` --
  LCD-19 PASS with `lcd_stop_heat=True` (real firing, Stop PIN-gated); first
  LCD-22 board run FAILed (`click_by_name('Edit')` -> `not_found` while running);
  rerun after `e52f256d` PASSed (`20261001T183045Z_lcd_lcd22rerun`). **2026-10-02:**
  new LCD-23/LCD-24 first run `20261002T013957Z_lcd_lcdedit23_24` -- LCD-23 FAIL,
  LCD-24 INCONCLUSIVE (harness assumed the Edit page opens on segment 0 and the
  executor warm start skipped segment 0); rerun `20261002T021747Z_lcd_lcdedit23_24_r2`
  after `8200e7b1`/`010239ef`/`b7eb48c9` -- both PASS; see
  `docs/BENCH_TEST_LOG.md`.
- [x] Profile/autotune same-zone start race fix (atomic per-zone claim,
  `relay_authority_zone_claim_begin()`/`_end()` reusing `s_heat_claim_mux`) --
  landed 2026-09-24 (`540b2d72`, host tests in `test_profile_executor_prestart.c`,
  `test_autotune_engine_prestart.c`, `test_link_watchdog.c`). Confirmed flashed
  to the bench and reran clean 2026-09-30 (`20260930T043239Z_heat_heat_rerun_0929_773ec669_540b2d72`,
  HP-01..08 all PASS) -- see the top-of-file entry above.
- [x] Root-cause and fix the `profile_executor` dwell-fault panic hit by heat
  run `20260924T085059Z_heat` — done, 2026-09-24 (`3ce065ca` audit, `773ec669`
  fix, `adc7f65c` comment correction; see the top-of-file entry above).
  **Rerun done 2026-09-30, PASS:** heat suite `20260930T043239Z_heat_heat_rerun_0929_773ec669_540b2d72`
  ran HP-01 through HP-08 all PASS, no panic/reboot/trip during the run --
  first clean 8/8 heat-suite PASS recorded for this fix pair.
- [x] Add a bench board lock so concurrent heat/mutating runs cannot share one
  board — done, 2026-09-24 (`d5bfac19`, `759660c2`; see the top-of-file entry
  above).
- [x] Stop rebooting on an `exec_mode_state_check()` violation -- done,
  2026-09-24 (`002e71bd`, review fixes `118beb79`): target builds now latch
  FAULTED and log instead of asserting; host/debug builds keep the hard
  assert. See the top-of-file entry above.
- [x] Narrow `exec_enter_terminal_state()`'s zone-active clear to the IDLE
  transition only -- done, 2026-09-24 (`4012f8c7` then `fe938ef3`); FAULTED/DONE
  readers (`force_all_relays_off()`, firing-stats finalize, status JSON, fault
  clear) all depend on `active` staying set past those transitions. See the
  top-of-file entry above.
- [x] Harden the bench board lock -- done, 2026-09-24 (`8e696d78`): serialize
  reclaimers on an O_EXCL mutex file, publish-then-check ordering on both
  reader and mutating sides. See the top-of-file entry above.
- [x] Fix the LCD bench runner click-then-read race and per-frame capture
  naming -- done, 2026-09-24 (`93355ee9`). **Reran 2026-09-30**
  (`20260930T043143Z_lcd_lcd19_rerun_0929_59c9306a`): LCD-08/09/14/21 PASS,
  LCD-01 INCONCLUSIVE on camera exposure/cast (pre-existing limitation, not
  a firmware defect); no recurrence of the click-then-read FAIL shape this
  fix targeted.
- [x] Replace the email forgot-password design with TOTP -- done, 2026-09-24
  (`84fa2e7b`, owner change); WT-C and WT-D landed; **WT-B landed**
  (`62f8bd4e`, gesture follow-up `cafc80f3`); WT-A (firmware routes) landed
  `f5f06dee`..`9708015f`. See the top-of-file entry above.
- [x] `check_flash_worker_lint.ps1` red over `totp_config.c` -- green on main
  at `9708015f` (re-run 2026-09-24).
- [x] **S6a boot-clear one-shot fixed, 2026-09-28 (review find):**
  `safety_link_frames.c`'s stale-S6a boot-clear latched permanently on the
  first successful CLEAR_TRIP *send*, not on Pico *acceptance* -- CLEAR_TRIP
  has no on-wire ACK, and the Pico's `safety_guards_try_clear()` refuses
  while its own S6a release debounce (200 ms) hasn't yet elapsed, made more
  likely by `SAFETY_FAULT_MIN_HOLD_MS`'s >=300 ms fault-line hold. A refusal
  inside that window used to burn the one-shot with a releasable trip still
  latched. Replaced with a small bounded, spaced retry (3 attempts, 2 s
  apart, still inside the existing 30 s boot window, anchored to the ESP's
  own boot rather than the Pico's boot_id/link-up); "confirmed acceptance"
  is the next DIAG frame simply no longer reading TRIPPED/MAIN_FAULT --
  the existing gate condition, no new signal needed. All prior guards
  (S6a-only, `fault_sources==0`, the 30 s window) unchanged. Host-tested
  (refused-then-retried-succeeds, persistent-refusal-gives-up-after-bound,
  non-S6a-never-cleared) and negative-tested. Review fix: once a clear has
  gone out, a DIAG showing the trip gone or any new own-fault-source rising
  edge closes the window.
- [x] **Full-precision numeric printing, 2026-09-28 (`04fb2afe`):**
  `GET /api/zones` and backup export used to print PID gains, `model_k_dc`,
  `coupling_diag_k_dc` and `coupling_c%u` at `%.4f`, silently rounding a
  small `Ki` (or other small-magnitude value) to zero on read-back. Both now
  print at `%.9g`. The narrow MCP writers (`control_set_zone_limits`,
  `control_set_zone_type`, `control_set_zone_coupling`) no longer zero a
  small `Ki` on their GET-merge-POST round trip, and OTA-bench PID
  comparisons are now tolerance-based (`judgments.pid_gains_match`) instead
  of exact-string.
- [x] **`tuning_valid` invalidation tolerance, landed (`49b32a24`):** the
  compare in `zones_http_post_parse.c` that decides whether a re-posted
  value counts as "changed enough to invalidate `tuning_valid`" used an
  absolute `0.0001` tolerance, so a small-`Ki` edit (now printed and
  re-posted at full `%.9g` precision rather than being rounded to zero)
  could fail to clear `tuning_valid` even though the value genuinely
  changed. Now a relative tolerance; also carries a magnitude-sweep test and
  a comment fix from review.
- [x] **`backup_import` batched NVS save, landed (`c22ff081`, `5d0a2756`,
  `7b107411`).** The ~61 s import used to issue one NVS save per field as it
  restored each store; it now uses `_no_save` setter variants for the hot
  paths with one trailing save per store, a RAM rollback if a mid-batch
  setter fails partway through, and tracks the Pico's `abs_max_temp_c`
  ceiling down after import/rollback. **Bench-measured 2026-09-30:** a
  fresh-export/import round trip (`backup_export()` -> `backup_import()`)
  completed in 0.80 s (synchronous route), down from the ~61 s pre-fix
  measurement; readiness unchanged before/after.
- [x] **Web dashboard: drop the "Previous firing ended" card after a normal
  or deliberate stop, landed (`fd792793`).** The card now shows only for an
  *unexpected* end (fault/trip/crash) — a normal Stop or a profile reaching
  its own end clears it instead of leaving it displayed.
- [x] **Divergence-triggered stop recorded as FAULTED, landed (`c26df2ce`,
  `057ca631`).** Was recorded as HALTED with an empty reason; now records
  FAULTED with a reason. `057ca631`'s review fix keeps a DONE run DONE
  instead of overwriting it if a divergence appears after completion.
- [x] **Owner requests, 2026-09-28:**
  - (a) **Landed (`d484e51a`, `f017b285`):** an "Edit firing" button next to
    Start/Stop on the web UI opens `/live_profile`, prompting for login
    first if not already signed in (backend: `profiles_live_http.c`,
    `docs/LIVE_PROFILE_EDIT_PLAN.md`'s five routes, the `profile_live_*` MCP
    quartet). **LCD counterpart landed 2026-09-28** (`ebbbd34f`, review fixes
    `1a40133a`, `e35e1aff`): a live-edit-current-firing LCD page (guarded
    apply, refresh, heap state; `e35e1aff` clears a stale status line when
    the idle poll picks up a new firing after the page was opened).
  - **LCD relock on web credential change, landed 2026-09-28** (`33ecc6d5`,
    review fix `7783066c`): `ui_lcd_lock_force_lock()` made safe to call from
    the httpd task, firing a relock edge and gating a pending LCD request
    when the web password changes underneath an unlocked panel.
  - **Bootstrap-password gate redirect, landed 2026-09-28** (`da0accac`,
    review fix `8e3762e0`): fixed bootstrap_password being unreachable on a
    gated page (regression from `d25d5ccf`'s dashboards-only gate); review
    fix covers a first-load race and a lost `return=` param.
  - **`full_board_backup.py` now goes through web auth, landed 2026-09-28**
    (`91ed5818`): fails the run on any 401 instead of silently continuing,
    and reports a partial cfgfs restore rather than reporting success.
  - **Backup-restore false ceiling-divergence trip, fixed 2026-09-28**
    (`c35e2e8a`, review fix `8f777f1b`): a restore in flight now skips
    ceiling-divergence enforcement only for the latch, not for heat-off
    enforcement, which stays active throughout the restore.
  - (b) A confirm dialog before stopping a running profile, on both the web
    UI and the LCD — this already existed on both UIs before this round of
    requests (a prior status line here mistakenly listed it as still
    pending in `1037c4ff`; corrected). **Landed:** Stop itself now requires
    login on both UIs, per (c) below. The physical E-stop stays immediate,
    always available without login, and bypasses both the confirm dialog
    and the login requirement entirely.
  - (c) **Owner decision, supersedes the earlier "show login only when an
    action needs it" request. Landed on both UIs (`48962395`, `d25d5ccf`,
    `5901b09d`, `89fbe6d6`, `e4c5d7da`):** without login, the web and LCD
    show only the dashboards. Every other page and action — including
    Stop — requires login (`/api/profile_exec/stop` moved to USER tier,
    `/api/board_temps`/`/api/firing_history` to ADMIN; a `59e84b57`
    follow-up also moved `current_sweep/abort`, `autotune/abort` and
    `diagnostics/danger/stop` from SAFETY_REDUCE to ADMIN, so those three
    now need login too — the physical E-stop is the unauthenticated
    backstop for all of them). The LCD's PIN gate
    follows the same rules as the web password: the shared
    `login_backoff` module's lockout/backoff ladder (5/10/30/60/300 s) and
    the same idle-timeout semantics; the keypad's lockout text now states
    the actual wait ("Wrong PIN -- try again in Ns"). Setup/AP
    provisioning, the bootstrap-password flow, TOTP reset, and the LCD
    reset touch sequence stay reachable without login, since they exist to
    recover access in the first place. `/wifi`/`/networks`/`/scan` are a
    separate, narrower case: open only while the board is unprovisioned
    (`59e84b57`, `f3991c09`), ADMIN once provisioned, matching the
    captive-portal first-time-setup requirement.
  - **Wi-Fi AP fallback with auto-reconnect, landed 2026-09-28** (`8746e02d`,
    fixes `f4675a7e`/`ddebcf8c`, review fix `be7bcad4`): falls back to AP mode
    on home-network loss, reconnects when no web user is logged in. Owner
    decisions: keep the 30 s probe cadence while a user is logged in; with
    auth off, the AP stays up while any station is connected to it, not just
    while a user is logged in. `59e84b57`/`f3991c09`'s `/wifi` setup-tier gate
    (open only while unprovisioned) landed the same day and is unaffected.
  - **Fix, 2026-09-29:** `ap_teardown_should_defer()` deferred AP teardown for
    ANY active admin session, including a LAN-only one (e.g. the PC's own MCP
    tools reaching the board over the home LAN) -- so it stayed up with zero
    AP clients connected. Fixed by tagging each web session slot with
    `via_ap` (set at login; re-set on every successful touch, though in
    practice the login IP binding means a real session's tag never actually
    changes after login -- see `web_auth_session.h`'s field comment) and
    checking a new AP-scoped signal, `http_auth_any_ap_session_active()`,
    instead of "any session anywhere". A LAN/STA session now never defers
    teardown by itself. Owner-accepted judgment call: with auth on, a
    station physically associated to the AP but not yet authenticated
    (mid-login) still defers teardown, so a connecting operator is never
    stranded. **Review-fix round 2, same day:** `wifi_prov_request_arrived_on_ap()`
    always returned false on hardware (`CONFIG_LWIP_IPV6=y` means the httpd
    socket is PF_INET6, so `getsockname()` hands back an IPv4-mapped
    AF_INET6 address, not a plain AF_INET one) -- fixed to handle both
    shapes, which also fixes the `/status` `ap_password` field that gates on
    the same detector. Also: an AP-tagged session under a
    `WEB_AUTH_TIMEOUT_NEVER_S` policy now defers teardown only while an AP
    station is also actually associated, so one AP login under a never-expire
    policy can't pin the AP up forever.
  - **Live-edit feature bench-verified end to end, 2026-09-29 PASS** — see
    M18 item 3 above and `docs/BENCH_TEST_LOG.md` Test A.
  - **AP fallback/restore cycle bench-verified, 2026-09-29 PASS**
    (`docs/BENCH_TEST_LOG.md` Test B: home lost, board's own SoftAP came up,
    home restored, saved-network list matched baseline, no reboot). Still
    open: AP-fallback radio timing and the LCD's "[AP kept up]" render were
    not confirmed this run (LCD capture came back unreadable/black).
  - **Pending bench work (not yet done):** hardware-verify the LCD edit-firing
    page, the login gates, AP-fallback radio timing and the LCD's "[AP kept
    up]" render; measure `crash_report/clear` latency. (`wifi_prov_owner`
    stack margin under AP-fallback probing: measured 2026-10-01 on `eb83c1ac`,
    min 1344 B free of 4096 B (32.8%, OK) vs. 1536 B idle baseline -- done,
    see `docs/BENCH_TEST_LOG.md`.)
  - **`d3c4c826` AP-teardown fix, bench-verified PASS, 2026-09-30:** a
    decoy network was saved, the real one forgotten, mode cycled ap then
    home, and the board reached `state=reconnecting` with the LAN down.
    Restoring the real credentials rejoined within 26 s at `.156` with
    `ap_pending_teardown=False` on the first read after rejoin. A `GET
    /status` from the LAN admin session showed `ap_password` empty and
    `ap_password_known=false`, confirming the AP tears down after rejoin
    even with a LAN admin session active, and that a LAN client never sees
    the AP password. The companion AP-client case (a station associated to
    the SoftAP seeing `ap_password` in `/status`) was SKIPPED -- the bench
    PC has no free Wi-Fi adapter to join the SoftAP with.
  - **AP-password HMAC retired on the nine main-app admin routes, landed
    2026-09-30** (`a7b3e436`, review fixes `1822ab02`, build-failure fix
    `cae52ae9`, pytest fixes `429033f8`/`fd8fc308`, zero-caller allowlist
    `7cdb9539`): those nine routes (OTA/reset/boot_guard admin actions) now
    require only an admin web session and are reachable unauthenticated
    when web auth itself is off, matching every other admin route -- the
    AP-password HMAC challenge–response these routes used is gone. The
    recovery image's own HMAC (a separate, unrelated mechanism) is
    unchanged. `flash_firmware()`'s post-flash `boot_guard_reset` call now
    signs with the admin session instead of `KILNCTL_AP_PASSWORD`.
    **Bench-verified 2026-09-30:** an unauthenticated
    `POST /api/ota/esp/boot_guard_reset` returned 401; the same call under
    an admin session, issued right after a flash, succeeded.
  - **Static-IP reachability fix landed, through `50edd830`** (`03654117`
    fixes the shared `get_local_ipv4_string` helper's handling of an
    IPv4-mapped IPv6 address; `a829f71d`/`93a8716f` add the AP-subnet
    guard so the static-IP setter, the HTTP handler (a distinct 400) and
    the confirm-side check all refuse `192.168.4.0/24`; `dc1a8072`/
    `50edd830` render the provisioning page's errors via `textContent`
    with corrected no-response wording). Flashed to the bench; **bench-verified end to end, 2026-10-01 PASS** (`eb83c1ac`): static .156/24 gw .1 set,
    HTTP + reboot stayed at the static address, then DHCP revert, HTTP + reboot, same address.
    Remaining gap: the static-IP API has no DNS field and nothing reads the live DHCP
    netmask/gateway; see `docs/BENCH_TEST_LOG.md`.
  - **`persisted_count` fix landed on `origin/main`, 2026-09-30**
    (`9fc8b589`, `ee6a3809`, `9b77e2b2`, Opus-reviewed across three
    rounds): `GET /api/boot_guard` and `POST /api/ota/esp/boot_guard_reset`
    now report a separate `persisted_count`, the NVS-persisted counter,
    read by a strict reader that omits the field on any read or CRC
    failure rather than fabricating 0 -- `boot_count` stays this boot's
    fixed value. `flash_firmware()`'s result labels now distinguish
    "unknown (GET failed)" from "not reported (older firmware or read
    failure)". Flashed to the bench board at `9b77e2b2` (clean worktree,
    `flash_firmware()` verify OK: bootloader + partition table + app);
    afterward `boot_guard_get` showed `boot_count=1`, `persisted_count=0`,
    `recovery_mode=False`, and the reset line read "persisted before=0,
    after=0"; safety link up/armed/no trip, no unacknowledged crash.
    **Limitation:** the persisted count was already 0 before this reset,
    so the flash did not demonstrate a nonzero count dropping to 0 -- 0
    still merges "cleared" with "never written".
  - **Bench board flashed twice, 2026-09-30, both from clean worktrees via
    `flash_firmware()`:** first to `d5d6d64a` (`KilnCtrl-1ba4582e3e35`),
    then to `50edd830` (`KilnCtrl-6c9152cebb9d`); the Pico was untouched
    (`405d3c54`). Neither flash tripped or crashed the board. Readiness
    after: 17 ok / 1 not_done (`safety_commissioned`) / 3 other; task
    liveness OK.

---

## Future work — KilnFW PC-link command acknowledgement

**CLOSED**, moved out of this file 2026-08-24, closed in the owning doc
2026-08-24 (`5df2190`, `a458a8f`, `7b4c087` — all verified ancestors of
`origin/main`; `check_bridge_reject_reason.ps1` and
`test_bridge_reject_reply.py` both pass). This section had become a pure
forwarding address with no open row of its own — collapsed here 2026-09-16
so it can't be mistaken for live work. Two deliberate, still-visible gaps
live in the owning doc, not here: `display_bridge_task` awaiting an owner
decision (delete vs. restore), and `BLIT_DATA` staying fire-and-forget by
design. Full detail: [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md)
section 11.

---

## Decisions taken, so they are not re-litigated

| Decision | Date | Where the reasoning lives |
|---|---|---|
| **The web drop-down is three groups -- Firing, Kiln setup, System -- and every page is nested under exactly one of them.** A group carries no href and never navigates; it is a disclosure only. The former top-level "Safety" group is dissolved into Kiln setup at full depth, because the menu supports exactly one level of nesting. No destination was dropped, and the menu stays a static array -- it is not auth- or state-aware. | 2026-09-18 | `firmware/KilnFW/App/drivers/http/nav.js`, the NAV_LINKS header comment |
| **A zone's time-proportioning window and its minimum on-time are one constraint, not two settings.** `heater_window_ms >= 3 * max(heater_min_on_ms, 10 s)`, enforced at every config door. Below that ratio no fractional duty can be rendered and a PID zone silently becomes a bang-bang one — this bench ran a 2 s window against the 10 s floor and every commanded duty rendered as OFF. | 2026-08-29 | `firmware/KilnFW/docs/PID_CONTROL.md` "The window and the minimum on-time are not independent" |
| Pico bench path is the Debug Probe: SWD plus its UART bridge on GP16/GP17. **The Pico's own USB is not used.** | 2026-08-16 | `firmware/SaftyFW/docs/ARCHITECTURE.md` §1 |
| **On-board relays K1-K4 are for galvanic isolation and switch other relays only — never element current.** They are EE2-12NUH signal relays (2 A, 125 VA max), so element switching was never physically possible. This retires the duty-window contact-life worry; the downstream device's own life and coil inrush now set the limit. | 2026-08-24 | `firmware/KilnFW/TODO.md` §6A.0, `hardware/datasheets/mainBoard_Relay/EE2-12NUH.pdf` p7 |
| ~~**PSRAM stays disabled** on the ESP32-S3~~ — **reversed 2026-08-17: PSRAM is ENABLED** (octal, 8 MB) and used for LVGL draw buffers + heap + several task stacks | 2026-08-16, reversed 2026-08-17 | `firmware/KilnFW/TODO.md` §9.1a |
| Library paths use `${KIPRJMOD}/../lib`, not a KiCad path variable | 2026-08-16 | `docs/REPO_LAYOUT.md` B1 |
| OTA authentication is challenge–response on the AP password, never a form POST | 2026-08-16 | `firmware/CommonFW/docs/UPDATE_PROTOCOL.md` §2 |
| Update frames and the version handshake are a **frozen compatibility floor** | 2026-08-16 | `firmware/CommonFW/docs/LINK_PROTOCOL.md` |
| **A request and its reply may never share a command id.** Telling them apart by payload length structurally blocks a short refusal reply, which is why SAFETY/DISPLAY/TOUCH's driver-error paths stayed silent. `GET_CT_CAL`/`GET_PARAM`/`GET_CONFIG_PAGE` moved to `0x22`/`0x23`/`0x24`; `GET_FW_VERSION` keeps its shared `0x0B` as the documented exception, being inside the frozen floor where a refusal is never needed. | 2026-08-24 | `firmware/CommonFW/docs/LINK_PROTOCOL.md` |
| **The two links version independently.** `UART_PROTOCOL_VERSION` (PC↔ESP) was an alias of `KILNLINK_PROTOCOL_VERSION` (ESP↔Pico) behind a hard-equality gate, so an isolated-link bump refused every PC command until pc_tools moved. Bitten three times before being split. | 2026-08-24 | `firmware/KilnFW/App/drivers/common/uart_task_ids.h` |
| K4 → line-contactor interlock: J10 pin 1 = NO, pin 2 = COM, pin 3 = NC (read from the K4 symbol's rest position, not silkscreen) — still wants a continuity check against the physical part | 2026-08-16 | `firmware/SaftyFW/docs/HARDWARE.md` §3 |
| E-stop circuit is normally-closed by design. ~~No jumper fitted, so an as-built board reads permanent STOP~~ — **corrected 2026-08-24 by measurement**: `pico_gpio_read(9)` reads LOW on this bench, i.e. a contact IS fitted and S7 correctly stays quiet. Do not plan around needing to fit one; measure instead. The stale note also masked a real inversion — `discrete_task.c` had `!gpio_get()` on an active-HIGH pin, so this healthy reading decoded as *pressed*, hidden because S5 latched first and `safety_guards_tick()` early-returns while any trip is latched | 2026-08-16, corrected 2026-08-24 | `firmware/SaftyFW/docs/HARDWARE.md` §5, commit `642dd54` |
| ESP32-S3 boot-loop (repeating stack overflow in the main task, right after LVGL's boot banner) fixed by raising `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 3584→8192 | 2026-08-19 | `firmware/KilnFW/App/main.c`, `sdkconfig.defaults` |
| Internal SRAM exhaustion: `xTaskCreatePinnedToCore()` always takes TCB+stack from internal SRAM, and Wi-Fi/lwIP + LVGL had claimed nearly all of it by the time later tasks tried to start (caused the AUTOTUNE/WIFI UART-task registration failures). Fixed at the source — moved LVGL's allocator and the Wi-Fi/lwIP pools to PSRAM — not by shrinking the tasks that were failing | 2026-08-20 | `firmware/KilnFW/TODO.md` §1 |
| `uart_owner_transfer()` called `xSemaphoreCreateBinary()` (a heap alloc) on every single UART transfer; under real interactive load this exhausted internal SRAM (`ESP_ERR_NO_MEM` bursts every ~40s). Fixed with a static, stack-resident semaphore | 2026-08-18 | `firmware/hwAbstraction/esp/uart/uart_owner.c` |
| LVGL hit-testing cannot escape a parent that doesn't contain the touch point, and a non-`LV_OBJ_FLAG_FLOATING` child of a flex column silently joins the flow and eats the page's content budget | 2026-08-20 | `firmware/KilnFW/App/drivers/ui/ui_topbar.h` |
| benchproto returned a **stale reply from a different command** after every PC reconnect: the host's `msg_index` restarts at 0 per connection while the firmware's dedup ring and per-task ACK cache live for the MCU's boot lifetime. `FAULT_LIST` reported "0 faults" while 8 were armed — a clean-decoding wrong answer, diagnosed by reply *length* (3 bytes is `FAULT_SCHEDULE`'s shape, not an empty list's 2). Fixed with a `SYS_SESSION_RESET` handshake that must itself bypass dedup | 2026-08-23 | `firmware/CommonFW/src/benchproto_link.c` |

---

## How the work gets done — delegate to Sonnet subagents

Standing instruction from the repository owner, 2026-08-21. It applies to every
milestone below and to any new work filed against this roadmap.

**The coordinating session should not implement roadmap work itself.
Implementation is assigned to Sonnet subagents, and the coordinator oversees
them.** In practice that means:

- The coordinator reads enough of the code to write an accurate brief, splits
  the work into non-overlapping file scopes, dispatches Sonnet subagents, and
  then verifies what comes back. It does not sit down and write the feature.
- **Sonnet is the default worker model.** Opus and Haiku are available when a
  task genuinely calls for them; Fable coordinates and reviews only, and is
  never a worker.
- **Scopes must not overlap.** Two agents editing the same file collide
  silently and the loser's work is lost. Every brief names the files it owns
  and the files it must not touch.
- **Builds stay central.** Concurrent `idf.py` runs against one build
  directory clobber each other, so subagents write code and the coordinator
  compiles once. Briefs say "do not build" explicitly.
- **Verification is not delegated.** A subagent's report is a claim, not
  evidence. The coordinator builds, flashes, and exercises the change against
  the four levels in "What 'done' means" below before any item here is ticked.
- The narrow exception is shared scaffolding that encodes a hazard already paid
  for in a debugging session — the kind of thing a fresh agent reliably gets
  wrong. Writing that once, centrally, so every delegated task inherits the fix
  is cheaper than briefing the hazard into every agent.
  `firmware/KilnFW/App/drivers/ui/ui_topbar.h` is the worked example: it exists
  because LVGL hit-testing cannot escape a parent that does not contain the
  touch point, and because a non-`LV_OBJ_FLAG_FLOATING` child of a flex column
  silently joins the flow and eats the page's content budget. Both cost a
  session before they were understood.

---

## Working from the repository root

Claude and the editor are opened at `kilnCtl/` from 2026-08-16 onward. This is
now a load-bearing assumption rather than a preference:

- **Open `kilnCtl.code-workspace`, not the folder.** The ESP-IDF extension needs
  a `CMakeLists.txt` in the workspace folder it is pointed at, and the repository
  root does not have one. The multi-root workspace gives it `firmware/KilnFW`
  while keeping the root open alongside.
- `.mcp.json` uses root-relative paths, including `-C firmware/KilnFW` for the
  ESP-IDF server.
- `idf.py` needs `-C firmware/KilnFW`; `uv` needs `--project tools/PcTools`.

What it changes, and what still needs doing, is
[`docs/REPO_LAYOUT.md`](docs/REPO_LAYOUT.md) "Working from the repository root".

---

## Cross-processor invariants

Any change that touches these needs both plans read, not one:

- The safety processor **never blocks on the link**. A frozen ESP core must not
  be able to stall it.
- The safety processor is **RX-only while firing**; the telemetry relaxation
  applies outside firing and to unacknowledged broadcast frames.
- **The safety processor must be alive to start or continue any heater-on event.**
- Current measurement is **load-active detection and a power estimate only**.
- Guards clear two independent bars — a magnitude correct operation cannot reach,
  and a duration a transient cannot sustain.
- All board relays are **pilot relays**, driving external contactors and SSRs.
- **Neither processor is updatable while the kiln can heat**, and the Pico
  enforces that itself rather than trusting the ESP.
- **Each processor checks the other's protocol version**, in both directions, and
  neither trusts the other's data until both agree. The frames that establish
  that, and the ones that carry an update, are frozen so a mismatch can never
  lock out the fix.

## What "done" means

Same four levels everywhere, and they are not interchangeable:

**planned** → **built** (compiles, `-Wall -Wextra -Werror`) → **host-tested**
(synthetic inputs, negative paths) → **hardware-verified** (observed on the real
board). [`firmware/KilnFW/docs/PROJECT_STATUS.md`](firmware/KilnFW/docs/PROJECT_STATUS.md) keeps
built and verified distinct; every plan here is expected to do the same.

## Roadmap upkeep

Standing rules, not a to-do list — apply them every time this file changes,
they never get "checked off":

- Milestone ticks mirrored into the owning plan, not only here
- `Last reviewed` date bumped whenever a milestone changes state
- New work filed under a milestone, or a new milestone added with its owner
- **A finished item leaves this plan.** Either it moves to a
  reference/doc file (a decision, hazard, or reasoning someone will re-hit
  — a one-line pointer here is enough) or it is deleted. Ticked boxes and
  completion narratives do not accumulate here; a milestone that is fully
  done collapses to one line
