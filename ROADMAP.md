# kilnCtl Roadmap — both processors

> **Status:** planning · **Last reviewed:** 2026-10-05. The dated status log
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
> copy) plus the recovery-image MCP tools (`40c04dfd`). Landed 2026-10-02 afterwards, listed in `docs/COMPLETED_2026-10.md`: A3, S7, recovery entry, Pico bootloader hardening, recovery hardening and the unauthenticated LCD-passphrase recovery AP. Bench-verified 2026-10-03: the S7 409, recovery entry/exit and the LCD-passphrase AP; the rest is still bench-unverified. Landed 2026-10-05: GitHub-update WP9 (the repo setting, `d0bb22e4`, review fixes `5e6a26f4`); WP8, the board's TLS fetch, landed 2026-10-06 (`40f5635b`..`bc3bfde7`, host-tested and target-built, bench-untested), spare-relay WP-3..WP-6 and the MCP half of WP-7 (also: aux convert and the monitor-only freed zone, zones-page aux rows for every relay, a partial bench of the aux section 12, all in the On/off devices row), the backup-export float round-trip fix (`74056f6c`, `c0fefe16`), the `debug_reset(esp)` retry (`31da9bf6`), and a fresh bench soak that passed (`c39ab79c`), which cleared the cfg/NVS dual-write close; the close itself landed 2026-10-06 (row 65, bench soak owed). **Pending right now:**
> W5, the remaining recovery bench verification (threshold switch; the Pico upload waits on the debug probe), and
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
| — | **LittleFS `cfg` partition** (owner-directed migration, 2026-09-07): mounted on the bench board, NVS still authoritative (it was reformatted to 0 files 2026-09-30, `docs/BENCH_TEST_LOG.md`; that format did cover the profile-slot reformat: `GET /api/cfgfs` read the full 2,424,832 B geometry with 8 files on 2026-10-03); the RP2040 `config_store` A/B sectors and their seqlock fix are flashed. Dual-write window closed in code 2026-10-06 (owner decision 2026-10-05; evidence in `docs/CONFIG_FILESYSTEM.md`: 20 clean boots, one file-backed firing, one backup round trip): config saves now go to the cfg file only and NVS stays a read-only legacy source migrated lazily; with cfg unmounted saves are refused (503) and readiness/LCD prompt the format (owner decision 2026-10-06). Opus-reviewed and landed on origin/main 2026-10-06 (`af6e12eb`, `677af06a`, `87488d77`, `2c04a8eb`, plus the review-fix commits `9cd3cbd8` and `d5e8d3bd`: a profile delete no longer copies the in-RAM rev array and bitmap into NVS, which reverted other post-close edits and deleted file-only slots at the next boot; recovery mode no longer advises a format and `POST /api/cfgfs/format_confirm` answers 409 there), host-tested and target-built, not flashed; a post-close bench soak (boots, a firing, a backup round trip, a profile delete across a reboot) is the remaining open item (`docs/CONFIG_FILESYSTEM.md`, "NVS dual-write close"); relay names, TZ and the hidden-builtin mask are dual-written too (`288dc91c`, `2749be53`, zone normals `208de3d4`); a kiln-scope factory reset now also deletes its 11 kiln_nvs mirror files (`22027692`), and a profiles-scope reset sweeps the `profiles/prof<N>.json` slot mirrors and `stats/fs<N>.dat` firing-history mirrors (`2a94ae43`, `b1e3f3e6`), and `GET /api/cfgfs` now carries `zone_normals` and `relay_names` rows, 13 in all, with an honest 13-row worst-case sizing test and a 4096 B heap scratch (`d74e67cd`, `b1b5b2bf`, `e3e16ffc`); all of this is on the bench board since the 2026-10-04 reflash to `e3e16ffc`, and the earlier items since the 2026-10-04 reflash to `419e426b`, where `zone_normals.dat` appeared in `/api/cfgfs` on first boot (the reset paths and the banner are not bench-verified); follow-on commits of 2026-10-04, all of today's items are on the bench board since the 2026-10-04 reflash to `2eb0e431`: `ca44e060` (quiet read-only status accessors, `pref_cfg_fs_load_raw_quiet()` and `relay_names_decode_any_impl(quiet)`, so `GET /api/cfgfs` polling no longer logs wrong-size/REJECTED warnings), `f28b5aff`/`2eb0e431` (relay_cycles false `diverged:true` at equal revs traced to uninitialised struct padding in `relay_cycles_init()`'s NVS candidate; candidates are now zeroed and the relay_cycles and adaptive_tune ki-baseline status accessors compare data fields, never padding; a file written by the old init carries garbage padding, so the first boot of the fixed firmware logs one more "DIVERGED, adopting NVS" and rewrites it, after which it stays in sync; bench-verified on the 2026-10-04 reflash to `2eb0e431`: all 13 rows `diverged:false` on the first read, `docs/BENCH_TEST_LOG.md`), `4bcc4b0e` (`cfg_fs.c` `sweep_tmp()` path buffers moved to heap scratch, fixing a 16 B `check_system_uart_bridge_stack_budget.ps1` overshoot on origin/main; deepest path now 1648 B of the 1936 B ceiling) and `7570ac19` (`flash_worker_lint.py` `CFG_FS_ALLOWLIST` entries for the three factory-reset scope-sweep `cfg_fs_delete` sites, which had been failing the standing suite since 22027692/2a94ae43); `9310367b` (the `GET /api/cfgfs` status path's transient scratch moved to PSRAM via `persist_scratch_alloc()`; one GET no longer moves the internal heap low-water mark, 16519 B before and after on the bench, which now runs `9310367b` since the 2026-10-04 reflash); the ask-first refusal banner is wired into the LCD home strip, `/api/status` and the web dashboard (informational; confirm stays on Settings; host-tested only, not bench-verified). Open nits: the diagnostics page polls `/api/cfgfs` every 10 s (decision pending whether to slow it). `check_persist_scratch_malloc_caps.ps1` now covers the status-path files (`79a069bb`) and `kiln_cfg_store_cfg_fs.c`/`zones_config_cfg_fs.c` (their file buffers moved to `persist_scratch_alloc()`). History: `docs/COMPLETED_2026-10.md`, `docs/CONFIG_FILESYSTEM.md`. | `docs/CONFIG_FILESYSTEM.md` |
| — | **On/off device zones — owner request 2026-09-07, decisions settled 2026-09-14.** A zone may drive a non-heater on/off device (vent, damper, fan, water feed) instead of a heating element, switched by per-segment rules on ramp phase / direction / temperature / time, with a stalled ramp counting as a dwell. **Not "design only" — steps 1-8 of the 9-step plan are shipped and host-tested** (`d58492c9`, `3d740f78`, `dd1d6ada`, `172e3081`/`b46c120c`, `bf1db47f`, `83c8b28b`/`e8e32c7a`, `be27d461`); this row previously understated remaining work by ~8 steps. Safety core: guards 1/2/3/4/9 disabled for such a zone, per zone (`docs/ON_OFF_ZONE_PLAN.md` sec 1's guard table) — guard 1 (HEATING_FAILED) would otherwise false-trip on a *correctly working* vent, since "duty high, temperature flat" is both its trip condition and the device's normal signature. **2026-09-14: owner asked to decouple an on/off device from the 3-slot heating-zone array so it binds a spare relay instead — investigated and found genuinely large** (the zone array is hard-sized at `MAX31856_CHANNEL_COUNT` = 3 everywhere: guards, coupling matrix, firing records, persisted `zone_cfg_t`, HTTP surface; widening it needs a `ZONES_CFG_VERSION` schema bump, a frozen prior struct, a converter and a CRC check — `docs/audits/on_off_spare_relay_binding_2026-09-14.md`); **not implemented at that time, per D1 of `docs/audits/on_off_zone_decisions_2026-09-14.md` -- D1 is OVERTURNED by the owner requirement below (2026-10-04); see `docs/SPARE_RELAY_ONOFF_PLAN.md`**. Two engineering gaps from that decisions doc closed 2026-09-14: `adaptive_tune`/`firing_score`'s firing-stats snapshot now skip on/off zones as training data (`adaptive_tune.c`, `profile_executor_firing_stats.c`), and `docs/SAFETY_CASE.md` now carries the guard-3 coverage gap and the `max_temp_c == 0` relaxation. **Remaining open item: step 9, a supervised bench session with dry contacts — no on/off zone has ever actuated a physical relay.** **2026-09-20: step 5b added** — `profiles_page.html` previously had no editor for a profile's on/off rules at all (it only echoed `on_off_rules` back unchanged on save); an "On/off devices" section now lets an operator add/edit/remove per-segment rules from the browser, wired into save/load/preview, plus a real fixed gap (`rule%u_temp_source` was never sent, so any saved temperature condition was silently inert). **Follow-up 2026-09-20:** fixed a silent rule-destruction defect in that same editor -- a stale/absent zone used to serialize as an empty value and get silently dropped on save; it now round-trips via a flagged orphan option and save is refused client-side while any row is stale, plus a widened JSON byte budget and a new `check_page_js_tests.ps1` standing check; `4fe0a38d` then closed the Opus review nits on that pass (bounded temp_c import, stderr-safe check wrapper). **Owner requirement 2026-10-04 (overturns the 2026-09-14 D1 rejection above):** "we have 4 relays, the 4th was always intended for this and the ones not assigned to a zone where always intended to be used for things like this". Spare (unassigned) relays must be bindable to on/off devices without consuming a heating-zone slot. The plan has landed (`docs/SPARE_RELAY_ONOFF_PLAN.md`) and the owner's open questions are decided (2026-10-04, its section 14, including a v1 manual aux toggle); spare-relay binding is not yet usable end to end. WP-0 (verify premises) done (findings in plan sec 15, bfc4a455); WP-1 (`aux_outputs_cfg` store) landed 2026-10-04 (`1f70c419`); WP-2 landed (`622f0539`: ADMIN `/api/aux_outputs` config and `/api/aux_outputs/manual` routes, manual idle-only 409, aux-conflict 409 in zones POST and backup import, `max_uri_handlers` 170 to 175); WP-9 landed (`bc21b218`: ESP strips aux-bound relay bits from the Pico relay mask). WP-4 landed (`5dcf89f3`: profile rule targets for aux, validated on every entry point); WP-5 landed 2026-10-05 (zones aux editor, aux rule targets, dashboard manual toggle; browser/bench check still owed); WP-3 (executor) landed (host-tested only, no board run; follow-ups FOLLOWUPSHAS: shared input builder, dead-sensor fail-safe, on/off zones off the run drive, min_off not applied before the first ON, min_off_s seeded from a RAM-only last-OFF time per relay); WP-6 landed (2026-10-05: aux state in /api/status, LCD home rail captions); WP-7 landed (MCP half `control_get_aux_outputs`/`control_set_aux_output`/`control_set_aux_manual` in `d6559fac`; backup half 2026-10-06: `aux_outputs` in backup export/import, an absent key preserves); WP-8 docs half landed 2026-10-06 (bench session still pending). Zones page aux rows now cover every relay the firmware accepts (1-4), including spares past relay_count (host-checked only; firmware and LCD were already relay_count-independent). Convert of an existing ON_OFF zone to an aux output landed 2026-10-06 (`move_zone_to_aux` on POST /api/zones, MCP `control_convert_onoff_zone_to_aux`; host-tested only). The freed zone is monitor-only per the owner decision of 2026-10-05, now in code through `zone_is_monitor_only()`: no PID, ramp lock, guard-9 start check or lag/heat-rise guards; guards 5/6/7 stay; its coupling row and column are masked (`docs/SPARE_RELAY_ONOFF_PLAN.md` sec 10). Its per-tick executor wiring is host-tested through the real executor tick (`test_monitor_only_zone_tick_wiring()`, negative-tested). The run-start temperature baseline and the warm-start coolest-zone pick skip monitor-only zones (`profile_executor_baseline_zone()`, `profile_executor_run.c`; a mask with only monitor-only zones is refused at start with a message naming monitor-only; host-tested, negative-tested). Reviewed and left as is on purpose: the per-zone approach-rate cap and resume bumpless-seed loops still visit a monitor-only zone (their values feed only PID and guards the zone is exempt from), and the start-time "segment target above zone max_temp_c" and missing-ceiling refusals still check it (guard 5 stays active for it, so refusing a target above its ceiling fails closed). S3 blind spot decided 2026-10-04: aux outside the CT, ESP strips aux bits from the Pico masks (WP-9). **Bench 2026-10-06 (partial, `docs/BENCH_TEST_LOG.md`):** aux config and manual on/off on relay 4 verified by relay shadow/io_read with the safety link up; the rule-driven firing steps (sec 12 steps 3, 4, 6, 7) and the convert route are still unrun (no profile with an aux rule was authored; firmware 9662c3fe has no `move_zone_to_aux`). | `docs/ON_OFF_ZONE_PLAN.md`, `docs/SPARE_RELAY_ONOFF_PLAN.md` |
| S | S8 sanity rate — tool added `c49bb0e9`: `safety_set_rate_guard()`/`safety_get_rate_guard()` now expose config_store 0x0204/0x0205 over `POST`/`GET /api/safety/commissioning` (mirrors `safety_set_ct_cal`'s confirm-gated, read-back-verified pattern; refuses off `confirm`, a running firing/autotune, or an ARMED relay). **Corrected 2026-09-14 roadmap truth-up: this row said the guard "remains DORMANT (0)" — live `safety_get_rate_guard()` then read `max_rate_c_per_min=20 (ARMED)`, `rate_window_s=60`, a hand-set bench value.** **Corrected 2026-10-04: that 20 C/min is stale.** A live `safety_get_rate_guard()` read on 2026-10-04 (firmware `9310367b`, Pico `405d3c54`) returned "S8 max_rate_c_per_min=33.3C/min (ARMED) | rate_window_s=60", matching `docs/BENCH_TEST_LOG.md:47` and the compiled default (`firmware/SaftyFW/src/config_store.h:216`). **Decided 2026-10-04:** the owner keeps 33.3 C/min for now and will commission a tighter measured value after the first real-kiln firings (input: `docs/audits/s8_rate_guard_nuisance_trip_2026-10-04.md`). | M3 |
| **S** | **Display items needing the owner's own hands/eyes, 2026-09-04.** Three separate (touch corner accuracy CLOSED `f028e2f` — see M1): (1) a residual blue tint on the ST7796 panel with every firmware cause eliminated by measurement — needs the owner's eye, or a colorimeter, or a second unit; (2) wake-on-touch, first-touch-swallow and error-dismissal behaviour on display power, which need a finger on the actual glass; (3) the STOP-block 5V I2C hazard measurement at meter-module pins 10/12, still not taken. | `DISPLAY_ST7796_PLAN.md` §4 |
| — | **Guard evidence is mostly host-tested, not hardware-verified.** Of ~20 tracked guard-level claims, 19 are host-tested and only **3** are hardware-verified (**corrected 2026-09-15 roadmap claim audit** — the three rows are S5's *hardware fit*, S5's *masking-before-fit* finding, and KilnFW thermal_guard guard 6. The E-stop polarity fix was named here as the third and is **not** one: `SAFETY_CASE.md` §4 classes S7 as host-tested and negative-tested. The count was right, the attribution was wrong, and it credited the E-stop path with evidence it does not have) — everything else, including all of S1–S4/S6–S14's trip logic and KilnFW guards 1/2/3/4/5/7/9, has never been provoked on real silicon | `docs/SAFETY_CASE.md` §4 rollup; `GUARD_TEST_MATRIX.md` §3 |

**Current owner-dependent items, 2026-09-09 sweep** (nothing in the code can close these; listed together so they don't have to be re-derived per session):

| Item | Blocks | Where |
|---|---|---|
| Bench webcam re-aim + LCD colour verification (numeric pixel sampling, not eyeball) | Display power / colour items above | `CLAUDE.md` "Camera aim (2026-09-06)"; `DISPLAY_ST7796_PLAN.md` §4 |
| `abs_max_temp_c` must be raised **Pico-first, then ESP**, before a real (non-bench) firing — and the Pico's ceiling must never end up tighter than the ESP's | Real-kiln firing readiness | `docs/SETUP_WIZARD.md`; `docs/ON_OFF_ZONE_PLAN.md` |
| S8 is ARMED at 33.3 °C/min on the bench (read back 2026-10-04; the earlier hand-set 20 is gone, see the M3 row above) — whether 33.3 is right for a real kiln, or a tighter measured value should be commissioned, is **decided 2026-10-04**: keep 33.3 for now, commission a tighter measured value after the first real-kiln firings | S8 (rate-of-rise) real-world accuracy | M3 row above; `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md` |
| Main-tree fast-forward (2026-10-03): `docs/BENCH_TEST_LOG.md` carries foreign staged and unstaged edits (`MM`) in the shared tree, so the tree sits at `410c346c`. The kilnctrl MCP server imports from that tree, so no PcTools change landed since then is live on the bench | Every bench run through the runner that needs newer PcTools code; the server restart after the `mcp_server_core` split | `CLAUDE.md` "Stale-server self-announcing" |
| Landed 2026-10-05 (cherry-picked from `C:\wt\migcheck_f3idn3` onto origin/main): `tools/check_config_migration_steps.ps1` chain-integrity rules for kiln slots, profiles and the RP2040 store, 40/40 tests, PASS on real sources, negative-tested by hand. Nothing left to push; the old worktree can be removed by its owner | None | `docs/CONFIG_MIGRATION_CHAIN_PLAN.md` |
| `iter_tune` replay inputs live in gitignored logs the session cannot read, so the A8 failure and the section 6.5 credibility gate cannot be replayed without the owner | `iter_tune` redesign row below | `docs/ITER_TUNE_REDESIGN_PLAN.md` |
| T20 and T26 are untouched; T26 needs `firmware/KilnFW/TODO.md`, which the session is not permitted to read | Those two tasks | M1 |
| The SaftyFW debug probe (CMSIS-DAP, serial E66540F0A36C6E21) has not enumerated since 2026-10-03; every Pico flash and OT-P* case waits on reconnecting it | Pico flashing, OT-P01..05, the Pico relay through recovery | M8 |
| `C:\wt\recboot_csdr53`: the recovery image's own boot-partition read-back patch left `recovery_http.c` broken and the repair was classifier-denied. **Decided 2026-10-04: discard it. DONE 2026-10-04: the read-back was re-implemented fresh from origin/main (`11161e85`, `recovery_boot_verify.c` plus host test and check), so this item is closed** | Recovery-image copy of the `75098657` read-back | `docs/RECOVERY_IMAGE_PLAN.md` |

### Software, doable now — no hardware, no decisions

| Size | Item | Where |
|---|---|---|
| L | **Every fault says what was detected and what to do** — a standing rule, not a closing milestone, so it never fully closes: applies to every fault surface added from here on. All of S6a's own checklist items landed 2026-08-28 | M13 |
| L | **CT clamp attribution — built and verified under both topologies (individual per-zone CTs and shared/summed), re-confirmed 2026-09-19** (host suite 2499/2499, `run_all_checks.ps1` 113/0/0). Nothing software-only remains. Pending, and hardware-gated only: any real PASS or FAIL verdict, and the true envelope settling behaviour at real current — both require a real kiln; this ~4 W bench can only ever produce INCONCLUSIVE, by design. See `docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md`'s status line. | `docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md`; `docs/CT_CHANNEL_MASK.md`; `firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md` |
| **L** | **`iter_tune` redesign -- the one open question.** Owner decision 2026-09-08: keep and redesign rather than wire `control/iter_tune.c` as-is. Steps 1-2, 4-5, 7 landed; 3 built, section 6.5 credibility gate fails; 6 run, A8 A1-half FAIL (45/660 = 6.82%), plan stopped there; 8 wired, awaiting firings (per-step status in `docs/ITER_TUNE_REDESIGN_PLAN.md`). **Still open: the credibility gate (plan section 6.5) FAILS against a real recorded firing for a currently-unknown reason** -- two explanations were investigated and retired (see `docs/ROADMAP_STATUS_LOG_2026-09.md`; do not re-propose either); next is an offline replay of the recorded firing through the gate. Also open: A8 (A1-half failure above), the noise-floor artifact and step 9's first hardware trial (owner present). | `docs/ITER_TUNE_REDESIGN_PLAN.md` sections 8/9; `PID_EXPANSION_PLAN.md` |
| L | **ESP32-S3 OTA: single `app` slot plus a ~1.9 MB non-firing `recovery` image.** The `app` slot was 8 MiB; since the WP2 split (`b78e8701` + `12d193aa`, row for the GitHub release update) the layout is `app` 0x210000/0x400000 (4 MiB), `stage` 0x610000/0x400000 (4 MiB data), `recovery` 0xA10000 unchanged, with the one-time JTAG flash of the new table done (bench-verified 2026-10-04, `12d193aa`). Steps 1-3 landed (recovery project builds, `partitions.csv` carries `app` and `recovery`, `flash_firmware()` targets `app`, `flash_recovery` writes `recovery`); the bench board already boots from this table. The plan records no result for the migration-flash verification (step 4). Pending: step 5 (exercise recovery deliberately; this is recovery W5), step 6 (the esptool last-resort path, never run on this board), step 7 (delete in-app RECOVERY MODE and retarget the `boot_guard` counter to boot recovery, which must not precede 5 and 6), step 8 (documentation cutover). | M8; `docs/OTA_SINGLE_SLOT_PLAN.md` |
| M | **Recovery image rework -- owner decisions 2026-10-02.** W1-W4 landed (build trims and optional PSRAM, relays held low and LCD status page, validated streaming ESP upload with status/exit/wifi_reset routes, Pico relay from a PSRAM copy, all host-tested and target-built only), and the recovery-image MCP tools landed in `40c04dfd` (plus `flash_recovery`, `e7e8724f`). Also landed 2026-10-02 (detail in `docs/COMPLETED_2026-10.md`): the deliberate entry route `POST /api/ota/esp/recovery_boot`, the boot_guard threshold switch and the `recovery_enter` tool (`e25d8a30`..`24043ba9`); recovery hardening A-G plus the 1 s relay/LCD hold watchdog (`7a3331a8`..`d4eac0f6`); the recovery image made unauthenticated with a random per-boot LCD-only SoftAP passphrase, owner decision 2026-10-02 (`00e99237`, `581278ba`); `recovery_status` rendering of the new keys (`c7bd87d9`). **W5 bench verification (no owner needed, test fixture): the real write, recovery boot, LCD page, AP join and ESP push are done 2026-10-03 (below); still pending: the Pico relay DATA pace and target-slot guard (Pico upload through recovery is blocked, see the last paragraph).** **Bench-verified 2026-10-03 (`docs/BENCH_TEST_LOG.md`):** `flash_recovery` real write with read-back, `recovery_enter` into the image, the LCD page (fits 480x320, SSID, 12-character passphrase, IP), the WPA2 SoftAP joined with the LCD passphrase, `GET /api/recovery/status` (passphrase absent from the JSON, relays verified off, `relay_hold_stack_free` 1952 B of 3072 B), `recovery_push_esp_image` of a clean `4a4b7594` build over the recovery AP (board rebooted into `app`, boot_guard cleared and verified), and `recovery_exit` back to `app` with boot_guard cleared. **Recovery reliability audit 2026-10-04** (read-only review of the image at `e77e8895`) found that an LCD init failure hid the passphrase with no retry and no `lcd_ready` in status, a wrong-length boot_guard blob showed as "none", and draw failures were dropped silently; fixed in `39816057`/`a78fe73c` (init retried with at most 3 capped SX1509 re-resets per boot and only after an expander I2C failure, `lcd_ready`, `lcd_init_attempts`, `lcd_draw_failures`, `lcd_task_stack_free_bytes` and `boot_guard_record` in `/api/recovery/status`, a passphrase log scan over every recovery source file). The wall-clock cap on the Pico VERIFYING state (30 s, expiry reported as `outcome_unknown` with `power_cycle` on `/api/recovery/pico/status`) and the I2C bring-up retry (3 attempts, `relay_io_init_error` in status) landed in `cdfdf944`. The httpd/Wi-Fi come-up stream landed in `9a17648a` (`recovery_http_start()` returns an error and the image retries 3 times, then shows the failure on the LCD; the SoftAP is re-raised after an unexpected AP_STOP; both restart paths are capped at 3 per power cycle by RTC_NOINIT counters in `recovery_health.c`, after which the image stays up with the error shown so JTAG and the LCD stay stable; `http_start_attempts`/`http_last_error` in `/api/recovery/status`; `check_recovery_health.ps1`, 110 assertions, 27 mutants). The bench board's `recovery` partition was refreshed to the `f3a25207` image (`recovery.bin` 771,040 B, built in a clean worktree) via `flash_recovery` on 2026-10-04 with JTAG read-back verification; the board came back running `app` with the safety link up and no trip. The audit's remaining item (boot-partition read-back after every `esp_ota_set_boot_partition`, stalled: the implementer's own patch broke `recovery_http.c` in `C:\\wt\\recboot_csdr53` and the repair was classifier-denied, owner item) landed for the application firmware in `75098657` (2026-10-03): `boot_partition_set_and_verify()` (`firmware/KilnFW/App/drivers/persist/boot_partition_verify.{c,h}`) now wraps every `esp_ota_set_boot_partition` call in KilnFW outside the recovery image, reads the boot partition back and fails loud on mismatch (`ota_http_esp.c` answers 500 naming it; `recovery_switch.c` returns SET_FAILED/false), host-tested. Flashed to the bench board 2026-10-04 from the clean worktree `C:\wt\benchflash_fo2x1x` at `73debf63` via `flash_firmware(kiln_fw_root=...)`: running `app`, build verified, boot_guard cleared and verified, no trip, `startup` readiness item ok, `heap_internal` min_free 17863 B, PID gains unchanged (`docs/BENCH_TEST_LOG.md`). The recovery image's own copy of the fix is still open (the `C:\\wt\\recboot_csdr53` repair is owner-only). **Still pending on the bench:** the boot_guard threshold switch (JTAG reset attempts 2026-10-03 did not provoke it; the healthy mark clears the counter within ~6 s of boot) and the Pico upload through recovery (blocked, the SaftyFW debug probe is disconnected as of 2026-10-03, owner action), proof that `wifi_reset` erased the home credentials (the route was verified 2026-10-03 and the board was exited and re-provisioned over UART the same day, but `wifi_get_networks` was not read between exit and re-provisioning, so the erase is consistent with what was seen, not proven), the "wifi_storage_fail" path, and the no-PSRAM boot. `KILNCTL_RECOVERY_AP_PASSPHRASE` is a documented convention for a human or joiner automation to supply the LCD passphrase; no code reads or prints it, by design (never a call parameter, never echoed). | `docs/RECOVERY_IMAGE_PLAN.md`; `docs/OTA_SINGLE_SLOT_PLAN.md` |
| **M** | **ESP application auto-updates the Pico's firmware on boot -- owner requirement 2026-09-16.** In source and flashed: both `SaftyFW` slot images embedded in the ESP build, boot-time identity comparison, persisted 3-attempt budget (exhaustion is a `/readiness` warning, only NO_IMAGE and CHAIN_GAP refuse firing), the Pico's reported active slot wins slot selection (2026-10-02), `check_embedded_pico_image_fresh.ps1`, identity scoped to the SaftyFW source paths. The erase-watchdog and CRC defects that blocked the relay are fixed. Pending: a real ESP-driven Pico OTA on hardware end to end (the bench Pico runs a flat image; installing the two-slot bootloader there is NO-GO until the three items in plan section 11 resolve), so the auto-update path is inert on this bench unit and the wire-driven slot selection is bench-unverified. | `docs/PICO_AUTO_UPDATE_PLAN.md`; M8 |
| **L** | **One-step-at-a-time config migration — owner requirement 2026-09-16:** "Each new fw should support migration of the nearest configuration forward allowing a one way one step at a time config update path". From the next schema bump onward, a release that bumps a persisted config version ships exactly one new step (N-1 -> N) and carries **only** that step, so a board more than one version behind cannot read its own config and must be upgraded one release at a time (settled by the owner 2026-09-16 as "one way one step at a time"). **Settled by the owner the same day: forward-only, NOT retroactive** — `convert_versioned_blob_to_current()` (`firmware/KilnFW/App/drivers/persist/zones_config_migrate.c`) stays as the pre-v26 tail, unchanged, and the chain's input floor is v26, so the step table is empty until `ZONES_CFG_VERSION` moves to 27. `ZONES_CFG_VERSION` is **not** bumped by this work. Governs the zones config, the kiln-config slots (`KILN_CFG_STORE_VERSION`, already a real two-step chain and the shape to copy), fire profiles, and the RP2040's `CONFIG_STORE_FORMAT_VERSION`; not the boot-critical NVS items nor the `cfg` partition bridges, which re-use the same versioned blob. Carries three dependent pieces: a per-step `calibration_missing`/`fields_set` policy on the Pico — the actual mechanism that lets a `CONFIG_STORE_FORMAT_VERSION` bump carry the CT normals `i_normal_a` forward instead of forcing recalibration; a firing-blocking quarantine for a newer-than-known blob, closing the "runs on firmware-default PID gains after a rollback, unannounced" hazard; and `tools/check_config_migration_steps.ps1` (landed 2026-09-17 for the zones store; **extended 2026-09-19** to also require a current-version step/macro for kiln-config slots, fire profiles, and the RP2040 store — narrower than the zones rule set for those three, see `docs/CONFIG_MIGRATION_CHAIN_PLAN.md` §5.1 for exactly what's still deferred per store; **extended again 2026-09-19** to teach the fire-profiles rule the frozen-input `_Static_assert`(sizeof)/`crc32`-last-field discipline that store's code already had but the check did not yet inspect — `test_check_config_migration_steps.ps1` now 22 assertions, up from 18), failing a build that bumps `ZONES_CFG_VERSION` without its step, its frozen-struct asserts and its captured-blob test. Testing is deliberately asymmetric: the existing tail keeps the coverage it has, every new step owes a real captured blob at its input version from the day it lands. All four owner decisions are now settled (one step only; steps expire past a fixed age; quarantine firing; mandatory pre-bump blob capture). **The blocking prerequisite (a migrated blob was never written back on the ordinary `nvs_load()` load path) is CLOSED, corrected 2026-09-17** — `d3f74d67` persists a migrated blob immediately, read-back verified, and `6985c89b` surfaces a write-back verify failure to the operator (`zones_cfg_migration_persist_fault_t`, wired through `/api/status` and the LCD trip strip). See `docs/CONFIG_MIGRATION_CHAIN_PLAN.md` §1.6, itself corrected the same day. The step table itself is still empty pending the first schema bump past v26 | `docs/CONFIG_MIGRATION_CHAIN_PLAN.md`; `docs/PICO_AUTO_UPDATE_PLAN.md`; `docs/OTA_SINGLE_SLOT_PLAN.md` |
| **S** | **PC-side arbitrary-jump config converter — owner request 2026-09-17.** Firmware stays one-step-only (row above); this is the separate PC-side tool that jumps any version to any other, best-effort, file-only, never touches a board. Landed: `tools/PcTools/src/kilnctrl/config_convert.py` (CLI `tools/PcTools/scripts/config_convert.py`, MCP `convert_config`), covering the `kilnctl_backup` document, the `kilnctl_profile_blob` raw-NVS wrapper (v1-v4), and (2026-09-23) the `kilnctl_safety_config_blob` wrapper for SaftyFW's raw `config_store_record_t` (v1-v3 decode, v3-only encode — firmware has no pack path for v1/v2), plus a mirror-drift check (`tools/check_config_convert_mirror.py`, negative-tested, now also covering `CONFIG_STORE_FORMAT_VERSION`). Review before landing caught the CRC being computed over the body only, where firmware's `compute_profile_crc()` covers the whole struct with `crc32` zeroed; fixed, decode now verifies the CRC too. **2026-09-24:** added the ESP's raw `zones_cfg_t` blob (`kilnctl_zones_blob`) and `kiln_cfg_store`'s `kilnpkg.json` package format (`kilnctl_kiln_package`) in three stages. Stage 1 decodes/encodes byte-exactly the CURRENT `ZONES_CFG_VERSION` (26, 896-byte blob) only, its layout hand-derived from `zone_cfg_t`/`zone_timing_profile_t` (natural C alignment, no `#pragma pack`) and cross-checked against firmware's own frozen-struct `_Static_assert(sizeof/offsetof)` lines for `zone_cfg_v23_t`/`v24_t`/`v25_t` plus an independent match against `ZONES_CONFIG_BLOB_MAX_SIZE`; `tools/check_config_convert_mirror.py` now also mirrors `ZONES_CFG_VERSION`/`ZONE_NAME_MAX_LEN`/`TIMING_PROFILE_NAME_MAX_LEN`/`SRC_GROUP_COUNT`/`ZONES_CONFIG_BLOB_MAX_SIZE`, negative-tested. Stage 2 (porting `zones_config_migrate.c`'s older-version chain, 25 historical versions) was assessed and explicitly NOT attempted this pass — firmware converts an arbitrary historical version straight to current in one function rather than a chained N-1→N step, so each version needs its own firmware-derived test vector; `decode_zones_blob`/`convert_zones_blob` refuse any non-current version by name rather than guessing. Stage 3 (`kiln_package`/`kilnpkg.json`, `convert_kiln_package`) landed on top of Stage 1, mirroring `kiln_package_compute_hash()`'s byte-buffer/CRC32 algorithm in Python; also fixed a pre-existing doc bug (the module's old comment named the kind `kilnctl_kiln_cfg_package`, firmware's real `KILN_PKG_KIND` is `kilnctl_kiln_package`) (61 tests in `test_config_convert.py` as of 2026-09-24). See `docs/CONFIG_MIGRATION_CHAIN_PLAN.md` §7. | `docs/CONFIG_MIGRATION_CHAIN_PLAN.md` |
| **L** | **LCD dashboard and profiles rework -- owner request 2026-09-19.** All seven items are in source and flashed. Bench-verified by webcam 2026-09-20: items 5, 6, 7; 2026-10-04 on `a1232077`: items 1 (home name button opens the picker) and 4 (relay-life reset gone), and item 2's list and New-button presence. Item 2's New tap verified 2026-10-04 on `7e31cafd`: the Add glyph (rightmost header target on the manage page Settings -> Configuration -> Profiles; the home-page `profile_picker` has no New button by design, `add_cb` is set only with `manage`) opens `profile_builder_zones`, page name read from firmware and the webcam frame showing "New Profile -- Name & Zones"; the glyph has no text label so `click_by_name("New")` cannot reach it, only `touch_inject` can. Items 1 and 3 closed 2026-10-04 on `7e31cafd` with a 1-minute `M18C_TEST` heat run (40 C target): while firing the home name button is absent from the tap-target list (`click_by_name("M18C_TEST")` returns `not_found`, firmware exposes no separate disabled flag) and the webcam frame shows it greyed; the four relay pills read green (R above 30, G above 130) for every zone the executor reported on and blue (R below 20) for every zone off and for all four after stop, sampled numerically 8x8 at the pill centres against a bezel reference. Nothing remains open in this row. Detail: `firmware/KilnFW/docs/UI_PLAN.md` section 6. | `firmware/KilnFW/docs/UI_PLAN.md` |
| **L** | **Thorough OTA testing of both processors -- owner request 2026-09-19.** Case bodies for suite OT (OT-E01..12, OT-P01..05) and the `ota_matrix_run` MCP tool (hard `confirm is True` gate, `dry_run`, fail-closed run-level gate) are implemented; the matrix is unit-tested with a fake board. Real hardware so far: only OT-B01, PASS on 2026-10-01 (run `20261001T072647Z_ota`, `outcome=no_trip`) and again 2026-10-03 (run `20261003T155642Z_ota_ota_b01_4a4b7594`, Pico boot_id 42 to 87, no S6a), so `sw_reset` of both processors does not latch S6a. OT-P* cases are blocked: the SaftyFW debug probe (serial E66540F0A36C6E21) is not enumerated as of 2026-10-03 (owner action: reconnect it) and `pico_update` is deliberately off. Every image-dependent case SKIPs without an `ota_*` image parameter. **OT-E01 found a firmware defect 2026-10-03:** pushing an ESP image to the running `app` partition (single-slot design) returned HTTP 500 and the board reset with a TASK_WDT panic (`logs/bench_test/20261003T195937Z_ota_ot_4a4b7594/`, `docs/BENCH_TEST_LOG.md`). Fixed in two commits in `C:\wt\otafix_r2i1yr` (`eefbbd6c` 409 refusal when the target is the running partition; `4c64bfc8` refusal body drain plus the 401/403 close rule and `tools/check_ota_esp_refuses_running_target.ps1`), reviewed SHIP, flashed to the bench board and re-verified 2026-10-03 (OT-E01 409 with body and continuous uptime, OT-E09 401, OT-E10, OT-E12 all PASS; OT-E02 NOT_RUN under the single-slot design). Both firmware commits are on origin/main as `9faed0e5` and `652eb695`. **Matrix run with images 2026-10-03** (`20261003T223938Z_ota_ot_410c346c_rerun`, server at `410c346c`): OT-E01, E03, E04, E05, E09, E12 all PASS on the fixed firmware; three harness defects found by the first run (readback inside the ~6.6 s refusal drain window, a vacuous E05 on a missing image file, unsessioned pushes closed by the 4096 B rule) are fixed in `f08e554a`/`cf9dd550`/`410c346c`. Still pending: OT-E10 (needs a user-tier account in `KILNCTL_WEB_USER_USERNAME`/`KILNCTL_WEB_USER_PASSWORD`, owner item), OT-E07/E08 (`allow_heat=True`), OT-E11 (recovery W5). OT-E11 has no recorded run in `docs/BENCH_TEST_LOG.md`; its recovery prerequisites (`recovery_enter`, `recovery_push_esp_image`) were bench-verified 2026-10-03 (row M). **Landed 2026-10-02, bench verification pending (venv repair):** the Pico bootloader slot-linkage check before PENDING_VERIFY, the slot trailer in `UPDATE_STATUS` and the 8 s pre-jump watchdog (`79264f5f`..`d7d6e9fc`); S7 single-flight guard for every Pico safety-config writer (`beaab290`..`b8c3e4a8`; the synchronous HTTP writers now claim it as `HTTP_SYNC` and boot recovery retries, see `docs/COMPLETED_2026-10.md`; the 409 against a running sweep was verified on hardware 2026-10-03; two racing HTTP_SYNC callers cannot be observed because httpd serialises them). A3's after-change coredump-erase stall measured 2026-10-04 on `7e31cafd`: `crash_report_clear(confirm=True)` on a present-and-acknowledged record with an 872,352 B coredump image completed in under 5.4 s wall clock including tool overhead (one sample), read-back confirmed `present=False`, board and link stayed up, uptime continuous; the coredump was saved first (`firmware/KilnFW/coredump_archive/coredump-f074e62a3a69.bin`, the 2026-10-03 OT-E01 TASK_WDT panic). | M8; `docs/BENCH_TEST_SYSTEM_PLAN.md`; `firmware/CommonFW/docs/UPDATE_PROTOCOL.md`; `docs/OTA_SINGLE_SLOT_PLAN.md` |
| **L** | **Standardized bench test system -- owner request 2026-09-19.** Waves 0-4 are implemented (`tools/PcTools/src/kilnctrl/bench_test/`, `bench_test_run`/`bench_test_list`/`bench_test_last`, `tools/bench_test.ps1`, case bodies for suites ST/FL/SK/OT/AT/HP/WEB/LCD/SP). Real-hardware record so far: heat suite HP-01..08 fixed and passing after harness fixes, LCD-08/09/14/16/19/21 and LCD-22..25 PASS, stack suite SK-01..04 PASS on `eb83c1ac` with idle, `web_ui_open` and `mid_firing` baselines committed, web suite render-only rows clean. No firmware defect found by any FAIL so far. Recovery entry/exit ran on hardware 2026-10-03 (`docs/BENCH_TEST_LOG.md`); the HP-07 trip-clear window fix `2570f751` was exercised live 2026-10-03 (PASS, clear in 0.92 s); one earlier HP-07 run that day saw relay on but no heater current, tracked as a possible fixture connection issue. Pending: OT-E11 and LCD-20 (no run recorded in `docs/BENCH_TEST_LOG.md`; recovery entry and push were bench-verified 2026-10-03), suite OT against real hardware; AT ran again 2026-10-03 on the running firmware (`20261003T224803Z_autotune_at_410c346c`): AT-01/02/03 PASS, AT-04 INCONCLUSIVE as a fixture limit (relay guard saw a 1.37 C peak-to-peak swing, needs 2.00 C; since 2026-10-04 the reason also states the fixture's peak temperature and elapsed time against the required setpoint, thresholds unchanged; the case now records `swing_pp_c` and `abort_reason` on every path, including the early returns, `a33fafdf` and `e77e8895`, both on origin/main), AT-05 a harness false FAIL on the `/api/autotune/matrix` wire shape, fixed in `b7decf35`/`cdf0b7cc`; a full re-run through the runner waits on the main-tree fast-forward so the server carries that code. OT-E07/OT-E08 harness fix landed 2026-10-04 (`f2aafa2f`..`0ae62a1a`, four review rounds): with `allow_heat=True` the cases start and stop their own firing/autotune, run verified teardown on every start failure, SKIP on a tainted run, a busy executor or active autotune, a refused start, or a missing image path, all before any ramp-assist write; the hardware run is still pending. Stack suite re-run 2026-10-04 on `a1232077`: SK-03/04 PASS, SK-01/02 INCONCLUSIVE until the owner captures baselines; the full web suite was not run (launch blocked by the permission classifier, suite is classified mutating; owner to run or allow). Per-run detail lives in `docs/BENCH_TEST_LOG.md`. The PcTools pytest runners (`run_pctools_tests`, the `pctools_pytest` regression gate) no longer report green when an xdist worker dies or tests go missing: `f17acdbd`/`e4cfbe2b` (2026-10-04) fail a run on a "node down" line or a collected-versus-reported shortfall even when pytest exits 0, and pass a 300 s per-test timeout. HP cases now record heater current from the UART safety-status cache during the heat and the preflight confirms the firmware version against the ELF (`e958e7d9`..`48bb8eaf`, landed 2026-10-04 after four review rounds; the CT topology-fallback marker is one constant in `devices_safety.py` shared by the status renderer, the parser and the tests); a hardware run of these HP changes waits on the main-tree fast-forward, since the running MCP server imports the main tree. **Addressed in software 2026-10-03, bench-unverified (board was in the recovery image):** the B3 stall after taking `logs/bench_test/.board_lock` is now diagnosable and bounded (`7090b0c5`: run directory and a redacted `runner.log` are created right after the lock, preflight and teardown run on bounded threads, 120 s and 60 s, a stalled preflight fails the run naming its last step and skips teardown); AT-01/02/04 now disable ramp assist for the case and restore it afterward, a failed restore marks the run TAINTED (`21195261`); B7 `ZONE_GRAPHIC` now has the read-only case WEB-ZONE-14 (`cceabd17`, full suite only; its judge PASSED by hand against the live `GET /api/zones` on 2026-10-03, the full-suite run is still pending). The other two still need a run on hardware. **Landed 2026-10-03:** `65108980` makes `log_tap_targets()` report hidden lv_buttonmatrix keys with `hidden=true` so `click_by_name` answers "hidden" consistently, and reports disabled keys normally (the pre-existing gap where the generic path skipped HIDDEN whole widgets closed in `f2e58657`, 2026-10-04: a final hidden pass in `log_tap_targets`, run only while `kiln_ui_click_by_name()` collects, reports whole hidden widgets so a tap on one answers HIDDEN rather than NOT_FOUND; LIST_TAP_TARGETS output unchanged, frame size unchanged at 144 B; the log-only follow-up landed in `fec20423`: the hidden pass runs only for a click collect, never for a log dump, so the collect flag cannot leak into a concurrent `LOG_TAP_TARGETS` dump); `4a13f140` adds a pytest `slow` marker so the 12-minute `test_ramp_assist_cone_scale.py` classes run only under `KILNCTL_SLOW_TESTS=1` or `-m slow`, with `run_pctools_tests` and `tools/regression_suite.py` exporting it and passing `-rfEs`, and `pytest_verdict` failing loud on a `KILNCTL_SLOW_TESTS` skip in a run that should have set it, which closes the test-pollution/"node down" investigation (no crash existed, only that one file's runtime); `43d79863`/`88a799c1`/`fded5558` move `mcp`/`_tool` into `mcp_server_core.py` so the 39 `mcp_server_*` submodules no longer import the aggregate at import time (202 decorator sites now `@_core._tool()`, tool count 207 unchanged; the `-m` launcher fallback now imports `main` instead of running the module twice). None of the PcTools changes is live on the bench until the main-tree fast-forward and a kilnctrl server restart. | `docs/BENCH_TEST_SYSTEM_PLAN.md`; `docs/BENCH_TEST_LOG.md` |
| **M** | **Edit the running profile mid-firing, from the web UI -- delivered 2026-09-19** (detail in `docs/COMPLETED_2026-10.md`; bench PASS 2026-09-29 and 2026-10-01). No bench leftover (the stray "LiveEditTest" profile was already gone on 2026-10-03; slot 0 holds `M18C_TEST`). On 2026-10-04 the bench env-var credentials (`KILNCTL_WEB_USERNAME`/`KILNCTL_WEB_PASSWORD`) logged in as administrator (web auth on, unauthenticated `GET /api/zones` 401; ADMIN-tier `totp_enroll_status` and `POST /api/ota/esp/boot_guard_reset` both succeeded through `http_auth.urlopen()`), so the stored `kiln_auth` record and the env vars agree; the 2026-09-21 401 predates the 2026-09-21 `web_auth_setup` bootstrap. | `docs/LIVE_PROFILE_EDIT_PLAN.md` |
| **L** | **100 user profile slots plus a live-edit slot -- owner request 2026-09-19.** Done: all 12 plan tasks, including the 2026-09-20 bench migration (new partition table, `cfg` grown to 0x250000, `profiles_http.c` fallback moved out of internal DRAM, guarded by `check_kilnfw_dram_bss_budget.ps1`). The cfg reformat is done: `GET /api/cfgfs` on 2026-10-03 reported the volume mounted at 2,424,832 B total (the full 0x250000 partition), 61,440 B used, 8 files, so nothing is pending here. | `docs/PROFILE_SLOTS_100_PLAN.md` |
| **L** | **Update from GitHub release (web GUI) -- owner request 2026-10-04. Plan written; M1 in progress: WP1 and WP3 landed 2026-10-04, WP2 (partition split) landed 2026-10-04 (`b78e8701` + `12d193aa`), one-time JTAG flash done (bench-verified 2026-10-04, `OTA_SINGLE_SLOT_PLAN` section 10); WP4 stager landed (`e4ebc0aa`, `d2cdeb69`: ADMIN `POST /api/update/stage`, `/stage/clear`, `GET /api/update/stage`, header-last power-cut-safe stage write with sha256 verify; host-tested only); WP5 recovery apply landed 2026-10-05 (`5dc6d34e`, advisories `dbb625db`; `recovery.bin` 780,560 B; host-tested, bench-untested); WP6 (UI/MCP) landed 2026-10-05 for the stage (page section, `update_status`/`update_stage_upload`/`update_stage_clear`; Install button and `update_apply` still stubbed); OT-G06 (app clears a stale stage matching the running image) landed app-side 2026-10-05, host-tested and target-built, bench-untested (the power-cut bench case is still owed). The MCP tools `recovery_apply_staged`/`recovery_apply_status` landed (`238b44e3`). WP10 (2026-10-06): the page's GitHub card and Install button (posts `recovery_boot`; Apply on the recovery page) plus MCP `update_check`/`update_stage_release`/`update_fetch_status`/`update_fetch_cancel`/`update_get_settings`/`update_set_settings` implemented, host/page-tested and target-built, bench-untested; `update_check` now reports `http_status` on a failed check (`e35f2108`). OT-G01..G06 (the GitHub-release update cases of the `ota` suite, `docs/BENCH_TEST_SYSTEM_PLAN.md` section 3.4) landed 2026-10-06 (`55178463`, `ac258429`; review fixes `c8548771`: clear every leftover stage, cancel unfinished fetch jobs, strict G02/G05/G06), unit-tested with a fake board only, never run on hardware. Pending: a bench run of that flow, and a bench test of the apply path including a power cut during the first pending-verify boot.** Owner decisions 2026-10-04: the board downloads the release itself; phone/PC upload over the normal Wi-Fi into a stage area is kept (recovery-AP upload stays as fallback); semver tags, first release `v1.0.0` once stable; downgrade refused by default with an ADMIN typed-confirm override. The repo to update from is a changeable setting (default `budarriola/kilnCtl`, M2, included in backup). Design: the application cannot write its own `app` slot, so a new `stage` partition (app shrinks to 4 MB, one-time JTAG table flash) is filled by the application and installed by the recovery image (`apply_staged`, boot partition set only after `esp_image_verify`). M1 = stage partition + ADMIN stage upload over normal Wi-Fi + recovery apply, no TLS; M2 = board HTTPS download + repo setting (TLS memory gated on the 8192 B floor and login KDF latency); M3 = signing. `tools/make_release.ps1` (REST upload, `gh` absent) landed 2026-10-04 (`c2cd5dac`, with `docs/RELEASING.md`); the live GitHub hop is still unverified; `-NotesFile`, a generated git-log release body and the gates record `docs/release_gates.json` (`tools/release_gates.py`; a stable `-Publish` is refused while any gate is open unless `-AllowOpenGates`) landed 2026-10-06, 15 of 16 gates open. M2 TLS spike (`2d7bfb0d`, `CONFIG_KILNCTL_TLS_SPIKE` default n; bench `f2402dda`/`95ffd91a`): min free internal 20639 B (PASS vs the 8192 B floor), handshake about 2.7-3.9 s with verified cert, asset/redirect hop untested (hop 0 returned 404); **gate (b) FAILED**: a web login during the fetch caused TASK_WDT (IDLE0 starved by the spike task, priority 3 pinned to core 0, in ECDH). Production WP8 fetch task must run on core 1 or unpinned (host allowlist, at most 3 hops, PSRAM buffers) and gate (b) must be re-run before WP8 is done. WP9 settings/backup landed 2026-10-05 (`update_repo` setting, ADMIN `/api/update/settings`, backup export/import, `/api/cfgfs` row; host-tested and target-built, bench-untested). WP9 review fixes landed in `5e6a26f4` (dots rule, %00/backslash refusal, canonical default, cfgfs row, mode-gate action, locking). WP8 TLS fetch landed 2026-10-06 (`40f5635b`..`bc3bfde7`: ADMIN `/api/update/check`, `/download`, `GET /api/update/fetch`, `POST /api/update/fetch/cancel`; TLS option D enforced by `#error`; heat refused while a fetch job is busy; host-tested and target-built, `.dram0.bss` 100296 of 101000 B). Remaining M2: **WP8 bench 2026-10-06 (firmware 9662c3fe): gate (a) PASS for the API hop (check reaches api.github.com over TLS, 404 on budarriola/kilnCtl = no release, 200 plus parse on a public repo), gate (b) PASS (web login during the handshake took 0.56 s, no TASK_WDT, no reboot, heap_internal min_free 17859 B); the asset/redirect hop is STILL UNVERIFIED (needs a repo whose latest release carries `KilnCtrl-<tag>.bin` and `release.json`, so a real release must be cut first).** See `docs/BENCH_TEST_LOG.md` 2026-10-06 WP8. `v1.0.0` is cut when the project is stable. WP3 pure update logic landed (`d7f39070`); WP2 partition split landed (`b78e8701` + `12d193aa`): `app` 0x210000/0x400000, `stage` 0x610000/0x400000, `recovery` 0xA10000 unchanged; `KilnCtrl.bin` 2,582,608 of 4,194,304 B; new gate `tools/check_app_image_size.py`; one-time JTAG flash done (bench-verified 2026-10-04, `OTA_SINGLE_SLOT_PLAN` section 10). Pre-release discovery landed 2026-10-06 (check with `allow_prerelease=1` reads `releases?per_page=5`, highest semver non-draft; GitHub's `/releases/latest` hides pre-releases; bench-untested). | `docs/GITHUB_RELEASE_UPDATE_PLAN.md` |
| S | **Backup export does not round-trip through import -- fix landed 2026-10-04 (`82ac2ad0`), bench-verified 2026-10-04 on `14d23a1d`: zones, profiles, timing byte-identical after re-export, PID/coupling unchanged. Follow-up on the `kiln_configs` change across the import (Pico param flags 0 to 1, active config ESP blob bytes 440 and 688, package hash) diagnosed: `zones_config_set_tuning_quality_no_save()` bumps `tuning_seq` unconditionally on import (`zones_config_accessors.c:2200`, called from `backup_import.c:2490`); `tuning_seq` is in `pkg_hash`, so each import duplicates the active slot. Fix landed 2026-10-04 (`aea29836`): the backup import now compares tuning floats through the shared "%.3f" format, so a re-import should no longer duplicate `kiln_configs`; **Reopened 2026-10-05 (bench still showed a `kiln_configs` zone `tuning_seq` bump (13 to 14) on import with `14a112a5`/`aea29836` in the running build); fixed 2026-10-05: `tuning_seq` no longer bumps on an identity import (`4d4b2f68`), the `kiln_cfg_store` Pico half is not captured from an unfetched cache (`4e651d3b`), duplicate zone indices are refused (`493ad099`), the recapture poll is gated on `has_data` and save-as-new is refused while unfetched (`00ddbe56`), and `GET /api/kiln_configs` reports `pico_half_recapture_pending` and `has_data`. The export-side round-trip drift is fixed (`74056f6c`: plant model floats; `c0fefe16`: every blob-visible float printed exact at %.9g) and the bench backup round trip passed in the 2026-10-05 re-soak on `c0fefe16` (`6704f4d3`). In progress: cleanup of the old "(2)"/"(3)" duplicates. Known limitation: a slot captured from an uncommissioned Pico applies "successfully" while the Pico keeps its old values.** Re-importing a board's own export fails with HTTP 400 `kiln_configs[0]: ... no abs_max_temp_c ceiling set`. | `fee835fa`/`b4bad2c7` (earlier import fix) |
| M | **Startup-fault firing gate -- decided 2026-10-04, implemented; bench check done 2026-10-04 on `14d23a1d` (`startup_guard9` readiness item ok).** A guard-9 startup failure BLOCKS a firing; a PC-link-watchdog startup failure is advisory (readiness warning only). Follows the M13 third sweep, which latches both but gates neither (the `READINESS_GATE_KEY_*` set is unchanged until this lands). | M13 |
| L | **CLOSED 2026-10-04: SAFETY_CASE H5 and H9 mitigations.** H5 (heat-rise check) is done as the existing thermal_guard guard 1 (`THERMAL_GUARD_TRIP_HEATING_FAILED`), no new code; residual gaps (a)-(d) are in SAFETY_CASE H5. H9 CT alarm bench-checked on `14d23a1d` (`ct_leak_alarm` ok, `/api/status` `ct_leak` false at idle; only the no-fault state observed). | `docs/SAFETY_CASE.md` H5, H9 |
| M | **Web GUI navigation reorganisation and diagnostics flash totals -- owner request 2026-10-04, landed `c44133d5` (bench/browser check not yet recorded).** Move Diagnostics under System and Ready to fire under Kiln setup; the diagnostics memory view shows total allocated flash, and the running image as used of its partition. | web GUI |

### Blocked on hardware that does not exist yet

| Size | Item | Where |
|---|---|---|
| S | Time the firing abort (30 s) with a stopwatch during a real running firing — the 1.5 s staleness ceiling was bench-verified 2026-09-06 (`LINK_PROTOCOL.md` §8) with no firing needed | M6 |
| M | S9's welded-contactor escalation — by definition needs a welded contactor. **Checked 2026-09-03: SimFW cannot do this — SimFW itself no longer exists** (removed `8553244`, 2026-08-28; `firmware/UnitTestFw` took its place and is unrelated ESP32-S3 bench-instrument firmware — DAC/AD9833/OLED/PCF8575 — with no path to the safety processor's current-sense input at all). Even when SimFW existed, its own removal commit records that `ct_calibration` "needs the fixture to physically drive current into the CT" — S9 (`firmware/SaftyFW/src/safety_guards.c:363-389`) latches only on real `any_current_present`, gated by `in->context_valid`, `in->current_sensing_commissioned` and NOT `in->current_sensing_disabled`; that flag comes from the CT's analog current-transformer signal through `current_sense.c`, not a GPIO a simulator MCU could assert. What would actually be required: a fixture that injects genuine AC current through the CT sense loop while the K4 drive line is confirmed de-energized — i.e. a hardware jig, not firmware simulation — plus a CT actually fitted and commissioned (`ct_installed=yes`; this was `ct_installed=no` on the bare bench as of the checked date above). **Corrected 2026-09-18, then superseded the same day by the CT-summed-topology fix:** the board reads `ct_installed=1` (channel 2's summed CT fitted and calibrated, per `docs/COMPLETED_2026-10.md` "CT commissioning steps 0-6 closed"); `s_current_sensing_commissioned` used to require all three `k_ct_v_per_a` entries greater than zero regardless of topology — a deliberate decision at the time, but one that permanently blocked any SUMMED-topology board (only one CT, wired to channel 2) from ever reporting commissioned. It now instead requires `k_ct_v_per_a > 0` only on channels that are actually fitted for the board's topology (`config_store_current_sensing_commissioned()`, `firmware/SaftyFW/src/config_store.h`), landed together with masking `any_current_present` to fitted channels only (channels 0/1's idle ADC noise must not count) so the unclearable S9 latch cannot arm off noise. **S9's `TRIP_INEFFECTIVE` is now armable on this board for the first time** — this is a live change to the bench's safety posture, not only to source, once flashed: a welded-contactor exercise here can now actually latch S9, independent of the fixture-availability question above. | M4 |
| M | AP-fallback: verified end to end 2026-09-29 (`docs/BENCH_TEST_LOG.md` Test B). Remaining scenario: a router with deliberately-wrong static config (`docs/BENCH_HOTSPOT.md`'s bench hotspot provides the second AP); not run yet | M6 |
| M | **HW changes:** relay status LEDs for K1–K4/S9, distinct connector types for the thermocouple daughterboards, I2C broken out on an expansion connector. (LCD backlight control's flying wire is fitted and confirmed — see M1, closed 2026-09-04.) | M1 |
| S | **Blocking, before the MSP4031 touches J2 at all**: meter module pins 10/12 (CTP_SCL/CTP_SDA) at 5V — confirms or clears a hazard that can back-feed the SX1509/ESP32-S3 through the shared I2C bus. `DISPLAY_ST7796_PLAN.md` §4 | M1 |
| M | DEBUG header and GP16/GP17 access before A1 is soldered down | M0 |
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
| [`docs/HTTP_POST_OWNER_MIGRATION.md`](docs/HTTP_POST_OWNER_MIGRATION.md) | KilnFW TODO.md 10.14 "Web side": which action-taking POST handlers move off `httpd_worker`, and how (the owner replies before the slow part, or an async-request handoff), with the response contract unchanged. It also lists what is already deferred and what is not worth moving |
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

**Software backlog state, refreshed 2026-10-04 (first written 2026-09-23, when every remaining box was hardware-gated):** the open software-adjacent items are now the `debug_reset` live verification after the kilnctrl MCP server restart (the "double ESP reset" bullet, `ROADMAP.md:1871`), the static-IP `dns`/`dns2` bench check (`ROADMAP.md:1892`), `persisted_count` going nonzero to 0 never demonstrated (`ROADMAP.md:1897`), the post-close `cfg` bench soak (row at `ROADMAP.md:65`: the NVS writers were removed and landed 2026-10-06; the pre-close soak passed 2026-10-05, `c39ab79c`), and two owner decisions (the guard-9/PC-link-watchdog startup gate, `ROADMAP.md:85` and `:1250`; the S8 rate guard: whether the armed 33.3 C/min stays acceptable on a real kiln, `ROADMAP.md:67`). Everything else is hardware- or owner-gated. Older state: every box in `firmware/SaftyFW/TODO.md` (26) and `tools/PcTools/TODO.md` (4, two duplicates) was hardware-gated. `firmware/KilnFW/TODO.md`'s remaining open items are the two "POST handlers should post commands to owner tasks" items (~lines 1950/1958, architectural, scoped not urgent per `docs/HTTP_HANDLER_OWNERSHIP.md`). **6A.3's guard-disable ack gate closed 2026-09-28 by owner decision: thermal protection gets no disable switch, ever, so the item needs no design.**

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

- [x] **Known-good config presets, so a test always starts from the same
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
- [x] **Live-bench regression suite built on that preset** (2026-08-28):
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
- [x] **Second LCD panel (ST7796/MSP4031), auto-detection, display SPI
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
      **Moved to the owner table (display items, 2026-09-04 row):** a residual blue bias, with every firmware cause
      eliminated by measurement (camera response refuted by a neutral
      off-screen bezel sample; RGB565 field boundaries unit-tested against
      known values; LVGL double-swap ruled out; blit paths compiled out; SPI
      clock swept 20/15/10 MHz with the blue *fraction* flat at 0.62/0.61/0.58
      — `07cad60`, table in `DISPLAY_ST7796_PLAN.md` §4). Treat as a module
      characteristic needing the owner's eye, a colorimeter or a second unit,
      not more firmware. Touch corner accuracy CLOSED 2026-09-04 (`f028e2f`
      — Y was mirrored, `KILNCTL_TOUCH_CAP_INVERT_Y` now defaults on).
      The 5V I2C hazard measurement is likewise in the owner table. Rendering can now be
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
- [x] MAX31856 driver + config plumbing (tc_type via flash-backed
      `config_store`, commissioned over `SAFETY_CMD_SET_CONFIG`) built and
      wired end-to-end in code (2026-08-19). ~~The part itself is not
      physically populated~~ — **fitted 2026-08-24 and reading correctly.**
      The LCD/web commissioning surface landed in M12 (CLOSED 2026-08-28). The
      four no-default section-1 fields are per-board commissioning values, not code; until set they keep
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
      per-channel CT-to-jack commissioning check on real hardware. The ADC noise
      floor is measured (2026-09-06 and 2026-09-18 captures,
      `firmware/SaftyFW/docs/CURRENT_SENSE.md` section 4: 3 sigma about 13 counts
      on the fitted channel, std 3.8-4.7 counts), below the uncommissioned
      presence fallback margin of 25 counts
- [x] ESP → Pico context frames (`SAFETY_CMD_PUSH_CONTEXT`, incl.
      `relay_recent_mask`) built from live board state — 2026-08-18.
      **Hardware-verified 2026-08-24**: the Pico is on the bench, the link is
      up, and `context_valid` is computed from frames that actually arrive.
      The "no Pico on this bench" caveat this bullet used to carry is retired
- [x] Pico → ESP telemetry (status, diagnostics, firmware version, trip events,
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
- [x] GUI (web + LCD) surfaces safety temperature, enclosure temperature, and
      power — built and wired to the same status cache the wire frames land
      in. Frames arrive every 500 ms. The safety MAX31856 was fitted
      2026-08-24 and reads live (M3), so safety temperature is a real value; the
      current channels report the CT hardware actually fitted (one summed CT
      on channel 2 since 2026-09-05) and read 0.00 A where none is, which is
      honest reporting of absent hardware, not a code gap
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
      -- ESP side now host-tested (2026-10-04): `ota_esp_image_header_check()`,
      `test_ota_esp_image_header.c`
- [x] Challenge–response on the AP password, never crosses the wire, 3-failure
      lockout (2026-08-17)
- [x] Both update paths refused unless idle and cool, with the specific
      blocker named (2026-08-17)
- [ ] Link-loss heating block **not** bypassed during a Pico update —
      pinned in CI on both sides (2026-09-04), still OPEN as a
      hardware-exercise item (a test suite is not a substitute for running a
      real update on a real board; hardware-gated):
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
      - **2026-09-21 SKIP note:** hardware-exercise attempt checked the premise first -- the bench Pico runs a flat image (two-slot bootloader install is owner-gated NO-GO per `docs/PICO_AUTO_UPDATE_PLAN.md` section 1), and the board's own `ota_status()` history shows the last Pico relay attempt already refused structurally (`REFUSED_RUNNING_IMAGE_OVERLAP`) before reaching the data phase, so no update -- and therefore no link-loss window -- can be pushed on this fixture today. No board state changed. Full record: `docs/COMMISSIONING_TEST_MATRIX.md` "Link-loss heating block during a Pico update -- 2026-09-21 hardware exercise (SKIP)".
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

**Third sweep, landed 2026-10-03 (`db2bd5d9`, `73debf63`) and flashed to the bench
2026-10-04:** a per-boot startup-fault latch,
`firmware/KilnFW/App/drivers/common/startup_faults.{h,c}`, with
`startup_fault_note()` placed on 13 boot-path failure branches that previously
only logged (`main_boot_early.c`, `main_bridges_bringup.c`,
`main_control_bringup.c`, `main_network_http.c`, `profile_executor_start.c`),
surfaced as a new advisory `startup` item in `GET /api/readiness` naming each
latched fault and its impact. RAM-only, no NVS, no new task or route. The bench
board reads `ok startup: every required task and subsystem started this boot`.

**Fourth sweep, landed 2026-10-04 (`8ebed650`, `a548dfc6`) and flashed to the bench the same day (`7e31cafd` content, `startup` item ok, 25 ids armed):**
twelve more ids (13 to 25): boot_guard mutex/NVS/persist failures, the
danger_mode task, the nine PC-link bridge tasks and the flash worker,
dns_hijack socket/bind/task, the 23 `*_http_start` failures under one
`HTTP_ROUTES` id (the per-group log line still names the group), the web-auth
route groups, the six settings stores (`unit_pref`, `ramp_assist_cfg`,
`profiles_builtin`, `profiles_favorites`, `kiln_cfg_store`,
`safety_cfg_store`), `pico_auto_update` (inside the non-recovery branch),
the heartbeat monitor, time sync, touch (noted only when the controller
answered the probe and then failed, never when absent) and the screen-idle
and backlight tasks. The review found three notes that could never fire
because the start functions always returned `ESP_OK`; `a548dfc6` makes
`time_sync_start()`, `unit_pref_start()` and `ramp_assist_cfg_start()` return
the real error on partition-init or unrecovered open/read failure (callers
only log and note, stored defaults unchanged); `9e5b8005` adds host tests for
those returns (partition-init failure, scripted open error, corrupted read,
and the cfg_fs file fallback still returning `ESP_OK` with the file value;
sntp init failure propagated, TZ still applied), negative-tested at
`test_unit_pref.c:328`. `time_sync_start()` still swallows a partition-init
failure, acceptable because `unit_pref_start()` opens the same partition
later in the same boot and notes it. The impact strings are held to
100 characters by the host test so the readiness `detail[192]` budget holds
(17 + 63 + 4 + 99 + NUL = 184). Skipped as debug-only or cosmetic: the log,
touch, ui_test and gpio_probe bridges, `board_temps_start`, `mdns_init`;
skipped as already surfaced: i2c/SX1509/kiln_io/spi/thermo bring-up,
`safety_link_start`, the UART owner/protocol init. Row 19 audited 2026-10-04
(`aaed0689`): every boot-path persist write outside the settings stores is
either self-retrying with no data loss (legacy partition migrations, kiln_cfg
v1/v2 to v3 rewrites, the iter_tune NVS catch-up), a cfg_fs file sync where
NVS stays authoritative and only the file goes stale, or already surfaced
(zones migration write-back, kiln_cfg_swap boot recover, boot_guard, cfg_fs
mount/format); no new note. `eb4090f5` makes `FT6336U_start()` return
`ESP_ERR_INVALID_RESPONSE` for an identity mismatch or a failed ID read, so
that case now notes STARTUP_FAULT_TOUCH; an absent panel stays
`ESP_ERR_NOT_FOUND` and un-noted (the bench panel passes the identity check,
so no standing fault). Found by that audit's review, open: `relay_cycles.c`
`migrate_from_default_partition()` runs on every boot (`relay_cycles_init()`
never checks whether the kiln partition already holds `NVS_KEY_CYCLES`,
contrary to its own comment), so a board that predates the partition split
and still carries the old default-partition copy would have its live wear
counts and relay types overwritten by the stale v1 copy each boot; fixed in
`0c39f72f` (migrate only when the kiln partition has no `relay_cyc` blob,
fail closed on an unreadable one, old copy kept as `zones_config_store.c`
does; the bench board's default partition holds no legacy copy, so it was
never exposed). Owner decision 2026-10-04, implemented: a guard-9 startup failure now BLOCKS a firing (new `startup_guard9` readiness item and `READINESS_GATE_KEY_STARTUP_GUARD9`, refusal text names the failure and says reboot, reflash if it repeats; refused in `readiness_gate_evaluate()` so every start path, including autotune, is covered); a PC-link-watchdog startup failure stays ADVISORY, shown only in the `startup` item.

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
dual reflash. ~~Not yet flashed to the bench board.~~ Flashed since (ancestor of `eb83c1ac`, the build the bench ran 2026-10-01).

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

## M17 — The zone graphic: configuration you can look at · *opened 2026-09-18; all five stages landed; WEB-ZONE-14 passes on the bench through the runner (2026-10-03)*

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
Initially not hardware-verified (another task owned flashing; only the render function against real captured board JSON). Since then web suite
`20261003T224226Z_web_web_410c346c` ran 26 PASS, 0 FAIL, and WEB-ZONE-14 now passes through the runner, not only by hand (`docs/BENCH_TEST_LOG.md`).

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
  Unit-tested only at this point; no live re-run of W30 is recorded in this file (check `docs/BENCH_TEST_LOG.md` before relying on the write landing). No board defect
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
  30 of 51 runbook rows. W8/W9/W10/W50 are wired; W8 and W50 later PASSED live 2026-09-22 (entries below), W9/W10 are still waiting on the owner's go-ahead after a classifier refusal.
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
  covered by tests. Retrying through the safety-ack wrapper is not a
  defect: the retry already carries the ack (`app.js` `retryOnce()` and the
  comment at the 403/401 handlers), pinned by `test_fetch_auth_ack.js`, which
  `check_page_js_tests.ps1` runs. (2) Session/LCD timeouts -- already implemented
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

**M18 items still open** (the rest of the 2026-09-21..2026-10-04 narrative, all landed, is in
`docs/COMPLETED_2026-10.md`, "M18 commissioning narrative"):

- [ ] Open, 2026-10-01: after a double ESP reset the first reset left the board
  unreachable over UART and HTTP for minutes, then an S6b (reason 7) trip latched
  (cleared once with owner authorization, stayed clear). Reproduced 2026-10-04 on a SINGLE
  `debug_reset(peer="esp")` (board dark about 69 min, no UART boot banner, black LCD;
  recovered by one `debug_reset(..., allow_dark_rereset=True)`; see
  `docs/BENCH_TEST_LOG.md`, 2026-10-04 dark-board section). The stale-serial-session
  theory is ruled out; the cause is on the OpenOCD/`debug_reset` side (core left
  halted after a software core reset), not firmware, and the S6b that results is
  expected after more than 120 s of silence. Open steps: (a) the owner clears the
  S6b now latched on the bench (reason 7, mask 0x0040; relays off, link up); (b) fix or harden `debug_reset` so a run-mode reset verifies the target actually resumed (PC-side fix landed 2026-10-04 (`8c2beee5`, `a136a54a`, `1c9e28a4`): `debug_reset` prints `KCTL_RESET_ISSUED` after `reset run`, polls each target's `curstate`, resumes a still-halted core once and fails loud (`still_halted` in history.jsonl, honoured by the dark-rereset guard) if any target is not `running`; `debug_read_registers`/`read_memory`/`read_symbol` wrap their dump in `catch` so the resume tail always runs and a failed resume is surfaced; the ESP register list is Xtensa (`pc ps a0..a15`). Unit-tested against mocked OpenOCD only; live verification on the bench waits for the kilnctrl MCP server restart (blocked on the main-tree fast-forward, owner item); the S6b clear in (a) was done 2026-10-05 under owner authorization, `c39ab79c`). Bench 2026-10-05 (`docs/BENCH_TEST_LOG.md`, 2026-10-05 section): one `debug_reset(peer="esp")` on a healthy board ran clean on the new server (both cores `running`, HTTP and UART up at 20.9 s, no trip), so the healthy path is live-verified; the dark-board failure did not recur, so this stays open for that case, and the `KCTL_RESET_ISSUED` marker is not printed in the tool's text output); **fixed 2026-10-05 (`31da9bf6`)**: root cause is OpenOCD's `esp32s3_soc_reset` stub intermittently not completing the SoC reset (cores left halted at 0x403C8908 = `call_start_cpu0` with a stale IBREAKA0, or in ROM), which a `resume` can never recover; `debug_probe.reset` now re-issues halt-all + `reset run` in the same session (bounded, 2 retries) before the fallback resume. Bench: 25 resets, 0 dark, 8 needed one retry (`docs/BENCH_TEST_LOG.md`, 2026-10-05 debug_reset section). `info_uart_bridge`'s 176 B free-margin drop on `eb83c1ac` is not a code change (static review 2026-10-03):
  `111b1b6f..eb83c1ac` touched `uart_bridge_info.c` only for the 3584 -> 4096 stack literal and comments, and no callee on the task's path
  (`uart_protocol_*`, `uart_log_bridge.c`, `stack_margin.c`) changed. Same-build high-water spread (1624 fresh vs 1496 B later; used 2480 vs 2544 B idle,
  2608 vs 2608 B mid_firing across the two baselines) is runtime path depth (ESP_LOGW under log-queue pressure), already absorbed by SK-01's 384 B tolerance;
  the static ceiling is `check_all_task_stack_budgets.py`'s `info_uart_bridge` row (2208 B).
- **SK-01/SK-02 baselines captured 2026-10-06 (bench agent, delegated owner step), on `c0fefe16`:** `idle` and `web_ui_open` records committed under `docs/stack_margin_baseline/` (`web_ui_open` is synthetic HTTP polling, not two browser tabs; no firing was run, so no new `mid_firing` record -- the existing `eb83c1ac` one is the only one). Stack suite re-run `20261006T151338Z_stack`: SK-01/02/03 PASS; SK-04 FAIL, DRAM largest free block 8192 B under the 8704 B floor (same 8192 B in `get_heap_status` before the suite; not investigated). See `docs/BENCH_TEST_LOG.md` 2026-10-06.
- **SK-04 sampling 2026-10-06 (bench agent):** 29 samples over 85-7293 s uptime, internal largest free block 9728 B in every row (floor 8704 B), no step at web-page fetches or a login; the 57000 s 8192 B failure is not reproduced and 2 h is too short to refute it. A >16 h run is still needed. See `docs/BENCH_TEST_LOG.md` 2026-10-06.
- **SK-04 instrumented 2026-10-06 (not yet flashed):** `dram_watch` (`App/drivers/common/dram_watch.c`) samples the internal largest free block every 2 s from an esp_timer and keeps the lowest value plus the uptime it was first seen; `/api/status` reports it as `heap_internal_largest_low` (null before the first sample) and `get_heap_status` prints it. The first sample under 8704 B logs a WARN plus a once-per-boot per-region internal heap summary, and the on-demand task creates log largest/free before and after. All logging runs from `telemetry_log_task`, never in the timer callback. No Part B code fix was made: the 2 h sampling stayed flat at 9728 B with no step at page fetches or a login, so no allocation is identified to fix, and a speculative move would only guess. Owed: flash this build, then a >16 h bench run that reads `heap_internal_largest_low` and the `dram_watch` log lines to name what takes the block to 8192 B.
- **M17 relay_type round trip 2026-10-06 (bench agent):** `control_set_relay_type` relay 4 0->4->0 read back, persisted across `sw_reset_esp`, `control_get_zones` identical to the snapshot after restore. Web page rendering was not checked; b45d225b then made the zones page list spare relays (past relay_count) with type selectors (host-checked only, not yet viewed on the board). See `docs/BENCH_TEST_LOG.md` 2026-10-06.
- **Pending bench work:** hardware-verify the login gates, AP-fallback radio timing and the
  LCD's "[AP kept up]" render. `crash_report/clear` latency measured 2026-10-04: under 5.4 s
  round trip on a present-and-acknowledged record with a 872 KB coredump (row L/OTA A3 note).
- [x] Bench-verified 2026-10-06 on `c0fefe16` (static 192.168.1.156, mask 255.255.255.0, gw/dns 192.168.1.1, dns2 8.8.8.8: `/api/status` `time_last_sync_epoch` moved from 1791296637 to 1791299702 after the switch, uptime continuous, reverted to DHCP and reachable at the same address): the static-IP API takes optional `dns`/`dns2` (firmware, NVS, web page, `network_*_ip_config`); PC-side unit tests landed 2026-10-04 (`tools/PcTools/tests/test_network_ip_config.py`, `DnsTest`/`DnsToolTest`: field names `dns`/`dns2` per `wifi_provision_http.c`, omitted never sent, dns2-alone refused, read-back compares dns/dns2; negative-tested by dropping dns2 from the body); the bench DNS check is partly done: 2026-10-05 on `5bbdb714`, static 192.168.1.156 with `dns` = gateway and `dns2` = 8.8.8.8 read back matching, `time_synced:true`, board stayed reachable, reverted to DHCP; NOT shown is a resolver actually in use (the last SNTP sync may predate the static switch), so tick only after a sync is observed after the switch. Attempted 2026-10-04: blocked, nothing changed on the board. The running kilnctrl MCP server is at 410c346c, which predates ff1851ca/a3c7d454, so its `network_set_ip_config` has no `dns`/`dns2` parameters (the firmware at 9310367b has the feature). Fast-forwarding the shared main tree was refused (foreign uncommitted `docs/BENCH_TEST_LOG.md` lines). Owner step: fast-forward the main tree, restart the server, then re-run: static 192.168.1.156 with the DHCP mask and gateway, `dns` = gateway, `dns2` = 8.8.8.8, poll, confirm `sntp_synced` (true under DHCP on 2026-10-04), revert to DHCP.
- [ ] Open (owner, manual): the AP-client `ap_password` check (a SoftAP-associated station seeing
  `ap_password` in `/status`) was SKIPPED for lack of a free Wi-Fi adapter on the bench PC. No
  coded bench case exists and none joins a SoftAP from PcTools today, so this stays a manual
  step until the owner adds an adapter or decides how a case should join.
- [ ] Open: `persisted_count` going nonzero to 0 has never been demonstrated.

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
