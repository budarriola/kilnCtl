# Bench test system — standardized, callable testing of the whole controller on the test kiln

> **Status:** plan, 2026-09-19, nothing built. Owner request the same day, verbatim: *"i would like there to be a system that we can call that performs standardized testing of the code including ota, checking the flash layout/validity, stack sizes, autotune. short heating profiles of about 5 min. really i would like this to cover all of the web page features and lcd features in all modes where possible on the test kiln. this should not be run as a whole often because it will probably take a long time but pieces of it may be used in routine testing. this should report to a log. tests may be interdependent but is discouraged unless the test is small in scope."*
>
> Every tool, route and file named in this plan was verified to exist in the tree at `cb8bec7d` unless it is explicitly marked **NEW**. The LCD section plans against the post-rework LCD of `firmware/KilnFW/docs/UI_PLAN.md` Section 6 (none of it built yet, see §7.2) and says where a case only applies before or after that rework.

## 1. Purpose and non-goals

**Purpose.** One callable entry point that runs a chosen subset, or the whole matrix, of standardized tests against the bench (ESP32-S3 main board at COM14, RP2040 safety processor, the ~4 W three-zone test fixture) and writes one structured, machine-checkable log per run. It sits *above* the existing static/host layer (`tools/run_all_checks.ps1`, the PcTools pytest suite, the SaftyFW host tests) and *only* touches the board through the existing `kilnctrl` MCP tools and the board's own HTTP API — never through a new side channel.

**Non-goals.**
- It is not a replacement for `run_all_checks.ps1`; it calls it (suite ST) and otherwise assumes it is green.
- It is not a safety-case evidence generator. Where the 4 W fixture cannot physically produce a verdict (S14/S15, CT attribution, real-kiln thermal behaviour) the case is defined to report `INCONCLUSIVE`, by design, and never tries harder (`docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md`, memory `project_s14_s15_cannot_arm_on_this_fixture`).
- It does not fix defects it finds. A red case names the observed value and the expected one; the fix is a separate task.
- It does not implement fault injection in firmware. The only sanctioned injection surfaces are the ones that already exist: the `/api/diagnostics/danger/*` family, `sw_reset_esp`, `debug_reset`, and `ota_update_*` with deliberately bad images. `CONFIG_KILNCTL_SIM_PLANT` is a separate build and is out of scope on the bench board (`firmware/KilnFW/App/drivers/sim/sim_backend.h:17-84`).
- It does not run on a shared tree with foreign WIP: a heating or flashing run reads `flash_firmware()`'s provenance and refuses to *claim* a commit it cannot prove (§3.3).

## 2. Architecture

### 2.1 One entry point, two skins

- **MCP tool (NEW):** `bench_test_run(suite, cases=None, dry_run=False, allow_heat=True, ap_password=None, tag=None)` in a new module `tools/PcTools/src/kilnctrl/mcp_server_bench_test.py`, registered with the existing `@_tool()` decorator (auto-registered per `docs/MCP_SERVERS.md` "Adding a tool"; add a keyword row to `kilnctrl/mcp_facade.py` so `kiln_find(query="run the bench tests")` ranks it first). Companions: `bench_test_list(suite=None)` (the catalogue, from the case registry), `bench_test_last(n=1)` (summaries of recent runs). `suite` is one of `smoke`, `nightly`, `full`, or a suite-group name from §4 (`static`, `flash`, `stack`, `ota`, `autotune`, `heat`, `web`, `lcd`, `safety`); `cases` narrows to explicit IDs.
- **PowerShell wrapper (NEW):** `tools/bench_test.ps1 -Suite smoke [-Cases HP-01,HP-02] [-DryRun] [-NoHeat] [-Tag text]`, a thin caller of the MCP tool over HTTP (8767), so a human or another script can run it without an MCP client. It exits 0 only on an all-`PASS` run, 3 if any case is `SKIP`/`INCONCLUSIVE` and nothing failed, 1 otherwise — the same three-way contract `run_all_checks.ps1` uses.

The runner itself lives in plain Python (`tools/PcTools/src/kilnctrl/bench_test/`: `runner.py`, `registry.py`, `report.py`, `cases_<area>.py`) so every case is unit-testable under pytest with the MCP tools mocked, the same way `tests/test_ota_http_client.py` mocks HTTP. Cases call the *Python functions* behind the MCP tools (`mcp_server_flash.flash_firmware`, `mcp_server_profiles.profiles_start`, …) in-process — the MCP facade is the human's door, not an extra hop.

### 2.2 Board access is only through existing tools

Every case's steps are written against names that exist today: `flash_firmware`, `debug_program(peer="pico")`, `debug_reset`, `debug_check_partition_table`, `get_heap_status`, `get_stack_margin` (`mcp_server_info.py:91`), `capability_preflight_check`, `load_config_preset`, `profiles_*`, `control_get_zones`, `autotune_*`, `ota_*`, `sw_reset_esp`, `safety_get_status/diag/link_stats/fw_version/commissioning`, `safety_clear_trip`, `thermo_read`, `io_read`, `touch_inject`, `ui_step`, `ui_run_script`, `wifi_get_status`, `build_kilnfw`, `build_saftyfw_host_tests`, `run_pctools_tests`, `run_repo_checks`, plus raw `GET`/`POST` to the routes in §4.7 through the existing `dashboard_http_client`/`ota_http_client` helpers. Anything not in that list is marked **NEW** in its case.

### 2.3 Run log

One directory per run under **`logs/bench_test/<UTC timestamp>_<suite>[_<tag>]/`** (a new sibling of `logs/coupling/`, never inside it), containing:

- `summary.json` — the machine record. Schema follows `stack_margin_baseline.py`'s convention (`to_json_dict`/`from_json_dict`, `json.dumps(indent=2, sort_keys=True)`): run id, suite, requested and executed case ids, host machine, PcTools commit and dirty flag, **commit + build timestamp + dirty flag running on each processor at run start and at run end** (ESP from `GET /api/ota/esp/status` `fw_build`/commit and `GET /api/status`; Pico from `safety_get_fw_version()`), `kilnctrl` MCP `/health` `fresh`/`stale` + `commit`, wall-clock start/end, and per case: `id`, `verdict` (one of `PASS`, `FAIL`, `SKIP`, `NOT_RUN`, `INCONCLUSIVE`), `reason` (one line, always present for anything but PASS), `started`, `duration_s`, `heated` (bool), `depends_on` (ids), `evidence` (paths of captures/transcripts relative to the run dir), `observed`/`expected` (small JSON, the mechanical judgment inputs).
- `transcript.md` — the human-readable log: one heading per case, every MCP call made with its arguments (passwords redacted to `***` before write; the harness never writes a credential anywhere), its returned text, and the verdict line.
- `captures/` — webcam frames from `capture_lcd.ps1`, HTTP bodies saved for diffs (zones config before/after, partition table), stack-margin snapshots.
- `board_before.json` / `board_after.json` — the same health snapshot `stability_soak.py` composes: `get_heap_status`, `safety_get_status`, `safety_get_link_stats`, `safety_get_diag`, `profiles_get_exec_status`, `GET /api/zones`, `GET /api/boot_guard`, `GET /api/crash_report`. The runner **fails the whole run** if `board_after` shows a new unacknowledged crash, a changed `boot_id` that no case explains, or a zones-config byte difference that no case declared it would make.

Verdict semantics: `PASS`/`FAIL` are mechanical judgments; `SKIP` means a precondition the harness checked was false (recorded with the precondition); `NOT_RUN` means the runner never reached it (an earlier fatal, `--cases` exclusion, or a dependency failed); `INCONCLUSIVE` is reserved for cases whose definition says the fixture cannot produce a verdict — it is never used to soften a FAIL.

### 2.4 Run lifecycle

1. **Preflight (every run, even `dry_run`):** MCP server `fresh` (abort on `stale`, cite `docs/MCP_SERVERS.md` "Stale-server self-announcing"); board reachable at its LAN address (`wifi_get_status()` station IP, AP fallback second); `profiles_get_exec_status` idle; `autotune_get_status` idle; `GET /api/ota/interlock` `ok:true`; `GET /api/crash_report` acknowledged (a run **never** acks a crash itself); `safety_get_status` link up, no trip latched (a latched S6a from an earlier dual reset is reported and the run stops — the operator clears it, §6); web-auth state read from `GET /api/auth/config` and recorded (when `web_enabled` is true the runner logs in as admin through `POST /api/auth/login` using a credential supplied via `ap_password`-style parameter, never stored); `abs_max_temp_c` equal on both sides (`safety_get_commissioning` vs the ESP's `GET /api/zones` limits) and non-zero. Write `board_before.json`.
2. **Ordering:** cases run in the fixed order of §5.2; heating cases never overlap; a case declared `heated` is followed by a cooldown gate (all zones within 2 °C of the run's ambient reference, timeout 25 min, else the next heated case `SKIP`s with reason `not_rested`).
3. **Teardown (always, including on exception):** `profiles_stop()` if anything is executing, `autotune_abort()` if active, `POST /api/diagnostics/danger/stop`, restore the zones config saved in step 1 if a case changed it (`load_config_preset` of the run's own snapshot), write `board_after.json` and `summary.json`. Teardown is `try/finally`, and the runner traps `KeyboardInterrupt` to run it — killing the host process is not how a firing ends (memory `project_stopping_host_does_not_stop_firing`).

## 3. Suite catalogue

Each case: **id** · preconditions · steps (existing tools/routes) · expected · mechanical judgment · depends-on · duration · heats?. Durations are estimates for the bench. "Admin" means the step needs an admin session when web auth is enabled.

### 3.1 Suite ST — static and host layer (reuses what exists)

| id | case | steps | judged by | dur | heat |
|---|---|---|---|---|---|
| ST-01 | Repo checks | `run_repo_checks()` (wraps `tools/run_all_checks.ps1`, no `-Fast`, no `-AllowSkips`) | exit 0; the pass/skip/fail counts parsed from stdout are recorded; count must be ≥ the last recorded count (drift guard, cf. CLAUDE.md "114 checks") | ~3 min | no |
| ST-02 | PcTools tests | `run_pctools_tests()` | exit 0 | ~2 min | no |
| ST-03 | SaftyFW host tests | `build_saftyfw_host_tests()` — must run from a short worktree path (`C:\wt\…`), the runner passes the repo root it was launched from and SKIPs with `path_too_long` otherwise | exit 0 | ~3 min | no |
| ST-04 | KilnFW target build | `build_kilnfw()` | exit 0; `build/KilnCtrl.bin` size < `app` partition size from `partitions.csv` (the same check `flash_firmware()` does, run here without flashing) | ~4 min | no |
| ST-05 | Tree provenance | `git status --porcelain` and HEAD of the tree that built ST-04; record; FAIL only if any dirty file matches `flash_provenance.py`'s `SENSITIVE_PATTERNS` (so a later FL/OT case would be refused anyway) | list empty of sensitive names | 5 s | no |

ST-01 already subsumes `check_stack_margin_registration.ps1`, `check_stack_margin_baseline.ps1`, `check_uri_handler_cap.ps1`, `check_flash_partition_offset_guard.ps1`, `check_recovery_image_size.ps1` and the lint/route-tier checks; the harness does not re-run them individually.

### 3.2 Suite FL — flash layout and image validity

| id | case | preconditions | steps | expected / judgment | dur | heat |
|---|---|---|---|---|---|---|
| FL-01 | Partition table matches source | board up | `debug_check_partition_table()` (compares `GET /api/partitions` to `firmware/KilnFW/partitions.csv`) | tool reports match; every row's offset/size identical | 5 s | no |
| FL-02 | Running partition is `app` | — | `GET /api/partitions` RUNNING marker | RUNNING == `app` (FAIL naming the partition if `recovery`; this is the `otadata` gap, CLAUDE.md flash section) | 2 s | no |
| FL-03 | app_desc build matches the archived flash | — | `GET /api/ota/esp/status` (`fw_build`, commit) vs `firmware/KilnFW/flash_provenance.json` and `elf_archive/KilnCtrl-<hash>.elf` via `find_crash_elf()` | an archived ELF matches the running build timestamp | 5 s | no |
| FL-04 | boot_guard counter | — | `GET /api/boot_guard` | `recovery_mode:false`, `boot_count` ≤ 1 (a higher count on a board that was just flashed by FL-10 is a FAIL — the footgun of `docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md`) | 2 s | no |
| FL-05 | Recovery image present and sized | — | `GET /api/partitions` has a `recovery` row; `firmware/KilnFW_recovery/build/recovery.bin` (if built) ≤ that row's size | bound holds; `NOT_RUN` for the size half when no recovery build exists | 2 s | no |
| FL-06 | Coredump partition readable | — | `GET /api/coredump/info` | route answers 200 with a well-formed body (a stored dump or none) | 2 s | no |
| FL-07 | cfg partition state | — | `GET /api/cfgfs`, `GET /api/cfgfs/format_pending` | record-only (plan §7 owner decision 6): `pending:false` is PASS, anything else is INCONCLUSIVE with the board's partition table and git's `partitions.csv` printed side by side, never FAIL; mounted flag recorded (today unmounted, `docs/CONFIG_FILESYSTEM.md`) | 3 s | no |
| FL-08 | Pico slot metadata | link up | `safety_get_fw_version()`, `safety_get_diag()` (boot reason, active slot per `firmware/SaftyFW/docs/BOOTLOADER.md`) | commit is a real hash; boot reason is `power_on`/`sw_reset`, **not** `watchdog`; active slot recorded | 3 s | no |
| FL-09 | Pico image vs archive | — | `find_safty_crash_elf(commit)` | an archived ELF exists for the running Pico commit | 3 s | no |
| FL-10 | ESP JTAG flash round trip (**optional, opt-in via `allow_flash`**) | idle, interlock ok, ST-04 fresh, no sensitive dirty files, `ap_password` supplied | `flash_firmware(verify=True, ap_password=…)` from the tree ST-04 built, or a `kiln_fw_root` worktree | tool returns success with verification PASSED, running partition `app`, `boot_guard` before/after reported and cleared; then FL-01..04 re-run | ~4 min | no |
| FL-11 | Pico JTAG flash round trip (**opt-in**) | idle, no trip pending, FL-10 not in flight | `debug_program(peer="pico")`, then the S6a procedure of §6 (link up, `trip_mask == 1 << (6-1)` only, `safety_clear_trip()`) | link up, `boot_id` changed, only `SAFETY_TRIP_MAIN_FAULT` was set and clears; `safety_get_fw_version()` commit == built commit | ~2 min | no |

### 3.3 Suite SK — stack sizes

| id | case | preconditions | steps | expected / judgment | dur | heat |
|---|---|---|---|---|---|---|
| SK-01 | ESP high-water marks, idle | board up ≥ 2 min | `get_stack_margin()` (`mcp_server_info.py:91`) | every registered task alive (none "not running" except tasks the build config omits, listed in `stack_margin_baseline.py`), each `level` ≥ the budget in the committed baseline records read by `stack_margin_baseline.load_records()` (`check_stack_margin_baseline.ps1`'s data); write a new `stack_margin_bench_idle_<commit>_<ts>.json` record into the run dir | 5 s | no |
| SK-02 | ESP high-water marks, exercised | HP-01 or HP-02 just completed in this run, plus one `GET /api/backup/export` and one `POST /api/zones` of the unchanged config to walk the httpd heavy paths | `get_stack_margin()` | as SK-01; additionally every task's margin ≥ 512 B absolute (the `httpd stack blob` class, memory `project_httpd_stack_blob_class`) | 5 s | no (reads after heat) |
| SK-03 | Pico task margins | link up | `GET /api/saftyfw_stack_margin` (`safety_stack_margin_http.c:182`) | every task's free ≥ 25 % of configured (memory `project_saftyfw_minimal_stack_overflows` — flat interim rule per plan §7 owner decision 4, pending a wave 1 Pico baseline) | 3 s | no |
| SK-04 | Heap and DRAM floor | — | `get_heap_status()` before and after the run's heaviest web case (WEB-DIAG-01) | internal DRAM largest free block never below 11.9 kB (memory `project_esp_internal_dram_exhaustion`); no `UNACKNOWLEDGED CRASH REPORT` banner | 5 s | no |

Judgment for SK-01/02 depends on **one dependency**: the baseline records must exist for the running commit's task set; if a task is registered but has no baseline, the case is `INCONCLUSIVE` for that task and the record is still written so the next commit of the baseline has data.

### 3.4 Suite OT — OTA, both processors (absorbs roadmap row `24f02f94`)

This section is the home of the roadmap row "Thorough OTA testing of both processors". The Pico half is **blocked** until the two open defects land (`docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md`, `docs/audits/pico_ota_staged_crc_mismatch_2026-09-18.md`); until then every OT-P case reports `SKIP` with reason `pico_ota_defect_open` and the runner never attempts a Pico OTA (constraint: an attempt watchdog-resets the safety processor). All OT cases need `ap_password` (or an admin session when web auth is on) and take the config fingerprint below before and after.

**Config fingerprint (shared helper, not a case):** `GET /api/zones`, `GET /api/profiles`, `GET /api/profiles/favorites`, `GET /api/kiln_configs`, `GET /api/settings/display_power`, `GET /api/auth/config` (no secrets in it), `wifi_get_networks()` (SSIDs only), `safety_get_commissioning()`; hashed and diffed. The Wi-Fi *password* is unrecoverable by design (memory `project_backup_round_trip_coverage`) and is judged only by "the board re-associated to the same SSID".

| id | case | preconditions | steps | expected / judgment | dur | heat |
|---|---|---|---|---|---|---|
| OT-E01 | Good image into `app` over Wi-Fi | idle, interlock ok, ST-04 image | `ota_update_esp(image_path, password)`; poll `GET /api/ota/esp/status` until reboot; wait for `/api/status` | `phase` reaches done; new `fw_build` == the `.bin`'s `esp_app_desc_t` build time; RUNNING == `app`; fingerprint identical; `boot_guard` `recovery_mode:false` | ~3 min | no |
| OT-E02 | Rollback | OT-E01 done and a previous image exists | `ota_rollback_esp(password)`; then `control_get_zones` | board boots the previous image (`fw_build` matches the pre-E01 value); **PID gains read back equal the pre-rollback values** (the `ZONES_CFG_VERSION` hazard — a `load_fault` in `/api/status` is a FAIL) | ~2 min | no |
| OT-E03 | Corrupt image: bad CRC | idle | push the ST-04 image with the last 4 KB flipped | refusal (4xx or `phase:failed`) before reboot; `fw_build` unchanged; RUNNING unchanged | 1 min | no |
| OT-E04 | Corrupt image: truncated | idle | push the first 60 % of the image | as OT-E03 | 1 min | no |
| OT-E05 | Wrong-build image | idle | push `recovery.bin` (or any non-KilnCtrl app_desc) to `/api/ota/esp` | refused naming the project mismatch, or rejected at verify without changing RUNNING | 1 min | no |
| OT-E06 | Power loss mid-write | idle; **operator or fixture relay on the ESP's supply** (`fixture_set_relay`, if the UnitTestFixture is wired to it — otherwise `SKIP: needs_operator`) | start OT-E01, cut power at 40 % progress, restore | board boots the old image (bootloader refuses the partial), `fw_build` unchanged, fingerprint identical | 3 min | no |
| OT-E07 | Update during a firing is refused | HP-01 running | `ota_update_esp(...)` while `profiles_get_exec_status` is RUNNING; also with PAUSED | HTTP 409/refusal quoting `ota_interlock_check()`'s reason (`ota_interlock.c:56,60`); firing continues unaffected | 30 s | uses HP-01's heat |
| OT-E08 | Update during autotune is refused | AT-01 running | as OT-E07 | refused (`ota_interlock.c:50`) | 30 s | uses AT-01's heat |
| OT-E09 | Auth: no credential | web auth off | `POST /api/ota/esp` with no `X-Ota-Mac` | 401/403; nothing written | 10 s | no |
| OT-E10 | Auth: web auth on, session | web auth on (WEB-SEC-03 turned it on in this run) | OT-E01 with an admin session cookie instead of the AP-password HMAC; and again with a `user`-tier session | admin succeeds, user refused (`route_tier_table.h` ADMIN) | 4 min | no |
| OT-E11 | Recovery image receives an app image | `ota_recovery_boot_esp` exists — **NEW** tool per `docs/OTA_SINGLE_SLOT_PLAN.md` §7; until then `SKIP: no_recovery_boot_tool` | boot recovery; confirm it serves `GET /api/ota/challenge`; push the app image; confirm return to `app` | RUNNING == `app`; while in recovery, Pico shows S6b link-dead with K4 open (`safety_get_status` read *after* return, plus the trip word) | 5 min | no |
| OT-E12 | otadata state after each case | after every OT-E | `GET /api/partitions` RUNNING + `GET /api/boot_guard` | recorded per case; FAIL if RUNNING == `recovery` after a case that expected `app` | — | no |
| OT-P01 | Relay update into inactive slot | Pico defects fixed; idle; no trip | `ota_update_pico(image_path, password)`; poll `ota_status()` through begin/erasing/sending/finishing | `phase` done; `safety_get_fw_version()` commit == image; `safety_get_diag()` boot reason not `watchdog`; commissioning read-back byte-identical | ~2 min | no |
| OT-P02 | Boot from new slot, then rollback | OT-P01 | `ota_rollback_pico()`; poll `GET /api/ota/pico/rollback/status` | previous commit running; commissioning identical | 1 min | no |
| OT-P03 | Bad image falls back | OT-P01 | push a CRC-corrupted Pico image | bootloader refuses/falls back; commit unchanged; boot reason not `watchdog` | 2 min | no |
| OT-P04 | Erase-time watchdog case | — | the OT-P01 flow, judged specifically on `safety_get_diag()` boot reason and `ota_status()` `last_error` | boot reason never `watchdog`, no `did not confirm RECEIVING` error (the 2026-09-18 defect's signature) | — | no |
| OT-P05 | Update with a trip pending is refused | a real trip latched (FL-11's S6a, *before* clearing) | `ota_update_pico(...)` | refused with the "trip is pending" reason (`UPDATE_PROTOCOL.md` hardware exercise); the harness then clears per §6 | 30 s | no |
| OT-B01 | Dual reset handshake trip | idle | `sw_reset_esp(password, confirm=True)` (reboots both) | within 30 s: link up, `boot_id` changed on both, `trip_reason == 6`, `trip_mask == 0x0020` and nothing else; `safety_clear_trip()` succeeds; then `GET /api/readiness` shows the trip item ok | 1 min | no |
| OT-B02 | Regression wrapper | — | this whole suite is `bench_test_run(suite="ota")` — the `run_pctools_tests`-style re-runnable form the roadmap row asked for | the row's PASS/FAIL/NOT RUN table is `summary.json` filtered to `OT-*` | — | — |

### 3.5 Suite AT — autotune on the 4 W fixture

| id | case | preconditions | steps | expected / judgment | dur | heat |
|---|---|---|---|---|---|---|
| AT-01 | Step test, zone 0, bounded | all zones rested (within 2 °C of ambient reference and of each other — not merely near their own cold junction, memory `project_autotune_needs_rested_baseline`); ramp assist off (`GET /api/ramp_assist`); `autotune_get_status` idle | `autotune_start(zone=0, method=step, …)` with the bench-appropriate step size; poll `autotune_get_status` up to 20 min; **never** `autotune_accept()`; `autotune_abort()` on timeout | run reaches a fitted result; `baseline_c` within 2 °C of the rested reading; fitted `K` within ±15 % of the last committed bench value (~38 °C/duty, memory `project_bench_is_a_4w_test_fixture`), `tau` within ±25 % of ~265 s; no trip; max temperature < 70 °C | 20 min | **yes** |
| AT-02 | Abort is immediate | AT-01 running (first 3 min) | `autotune_abort()` | status idle within 5 s; all zone duties 0 within one control tick; `io_read()` shows heater relays off | 30 s | uses AT-01 |
| AT-03 | Accept is guarded | an unsettled fit (AT-01 aborted early) | `autotune_accept()` without `ack_unsettled` | refused; `control_get_zones` gains unchanged | 5 s | no |
| AT-04 | Relay-feedback test (Åström–Hägglund) | rested, 25 min after AT-01 | `autotune_start(zone=0, method=relay)` | reaches a proposal; same bounds as AT-01; `INCONCLUSIVE` if the fixture cannot sustain the oscillation amplitude (record the amplitude) | 20 min | **yes** |
| AT-05 | Coupling matrix visible | AT-01 done | `GET /api/autotune/matrix` | well-formed 3×3 with zone 0's row populated | 2 s | no |

What "pass" means at 4 W: the algorithm converges and the fit lands in the band of previously measured fixture parameters; the *quality* of the gains for a real kiln is not judged here and never can be on this fixture (memory `project_bench_identification_limits`).

### 3.6 Suite HP — short heating profiles (~5 min)

A dedicated test profile per mode is saved into the hidden bench slot after the live-edit slot by the harness (`profiles_save(profile_id=<hidden bench slot>, name="BENCH_<mode>", zone_mask, segments_json)`) and deleted at teardown — never a user-visible slot (owner decision 2026-09-19, `docs/BENCH_TEST_SYSTEM_PLAN.md` §7); the concrete index is owned by `slots100`. All targets stay ≤ 45 °C, well under the 80 °C `abs_max_temp_c` on both processors and under the 70 °C historical bench targets; a profile is "target = ambient + 15 °C, ramp 600 °C/h, dwell 2 min", giving a 4-5 min run on this fixture.

| id | case | profile | steps | expected / judgment | dur | heat |
|---|---|---|---|---|---|---|
| HP-01 | Single zone | `zone_mask=0b001` | `profiles_start(7)`; poll `profiles_get_exec_status` every 2 s; read `thermo_read()`, `io_read()`, `safety_get_status()` | reaches RUNNING within 5 s, DONE within 8 min; zone 0 rises ≥ 5 °C above start while zones 1-2 rise less than zone 0 (coupling is 5-12 °C/duty, so they *will* rise — judged by ordering, not zero); K4 energized during RUNNING per `dashboard_status_t.safety_relay_energized`; no trip | 6 min | **yes** |
| HP-02 | All zones | `zone_mask=0b111` | as HP-01 | all three zones rise ≥ 5 °C; DONE; no trip | 6 min | **yes** |
| HP-03 | On/off device zone | a zone configured `zonetype` on/off with hysteresis (`docs/ON_OFF_ZONE_PLAN.md`) — set by `POST /api/zones` from the run's snapshot and restored after | HP-01 flow | relay for that zone toggles (count ≥ 2 transitions in `io_get_reports`) and never exceeds the band | 6 min | **yes** |
| HP-04 | Pause / resume | HP-02 profile | start; at 60 s `profiles_pause()`; 60 s later `profiles_resume()` | PAUSED reflected in `profiles_get_exec_status` within 2 s; all duties 0 while paused (`GET /api/status`); RUNNING again; profile completes with the pause time added | 7 min | **yes** |
| HP-05 | Stop | HP-02 profile | start; at 90 s `profiles_stop()` | state leaves RUNNING within 2 s; duties 0; relays off within one tick; `GET /api/profile_exec` shows the stopped run; `profiles_ack_last_run()` clears the last-run card | 3 min | **yes** |
| HP-06 | Stop is never gated | web auth **on** with no session | `POST /api/profile_exec/stop` unauthenticated while HP-05 runs | 200 (SAFETY_REDUCE tier, `route_tier_table.h`); firing stops | 10 s | uses HP-05 |
| HP-07 | Faulted run | HP-01 profile with a **safe** provoked fault: the profile's `max_temp_c` limit set 3 °C above ambient so the thermal guard trips it (a software limit, not a Pico trip) | start, wait | state FAULTED; fault words from `thermalGuardWords()` name the limit; sticky bar's Acknowledge (`POST /api/profile_exec/stop`) clears | 4 min | **yes** (brief) |
| HP-08 | Firing history | HP-01..05 done | `GET /api/firing_history`, `GET /api/history.csv`, `GET /api/logs/firing` | each completed run appears with the right profile name, start time and outcome | 5 s | no |

No case injects a Pico trip during heat: there is no sanctioned software surface for that on production firmware (§1), and pulling the E-stop or link cable is an operator action (`firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`), catalogued as SP-08/SP-09 below with `SKIP: needs_operator` when unattended.

### 3.7 Suite WEB — every web page feature

All cases use the `web` backend of `ui_step`/`ui_run_script` (`ui_test_runner.py` actions `goto`, `click`, `wait_for`, `assert_text`, `fill`, `sleep_ms`) where a DOM interaction is needed, and raw HTTP where judging the route is enough. Each page first gets a **render** case (page 200, its known element ids present, `nav.js` menu present, no JS error in the shell) — those are cheap and are the smoke set's web content.

**Dashboard `/` (`main_page.html`)**
WEB-DASH-01 render (`profileSelect`, `runBtn`, zone cards) · WEB-DASH-02 profile picker lists favorites first (`GET /api/profiles/favorites` order) · WEB-DASH-03 Start via `runBtn` starts the hidden bench slot's profile (uses HP-01) · WEB-DASH-04 feasibility popup: a profile whose ramp exceeds the zone's `max_ramp_c_per_hr` opens `feasPopupProceedBtn` "Start anyway"; proceeding starts · WEB-DASH-05 per-zone PID popup shows current gains and `pidPopupApplyBtn` writes `POST /api/zones/pid` (then restored) · WEB-DASH-06 sticky bar: Pause/Resume/Stop buttons appear in RUNNING (HP-04) and Acknowledge appears in DONE/FAULTED (HP-07) · WEB-DASH-07 last-run card + `ackLastRunBtn` (`POST /api/profile_exec/ack_last_run`) · WEB-DASH-08 trip banner + `clearTripBtn` visible only while `safety_get_status` shows a trip (OT-B01's S6a window) · WEB-DASH-09 history chart source `GET /api/history.csv` non-empty after HP-01 · WEB-DASH-10 recovery banner: `GET /api/ota/esp/status` `recovery_mode` false → banner absent (true branch only under OT-E11) · WEB-DASH-11 setup-offer banner appears iff `GET /api/readiness` has `not_done` items · WEB-DASH-12 disconnected banner after 2 failed `/api/profile_exec` polls (simulate by pointing the client at a closed port — client-side only) · WEB-DASH-13 unit toggle: `POST /api/unit_pref F` then `/api/status` `temp_unit` F, restore C.

**Profiles `/profiles`** WEB-PROF-01 render · 02 favorite star toggles `POST /api/profile/favorite` and reorders the dashboard picker · 03 new profile: `addSegBtn`, fill, `saveBtn` → `POST /api/profile` lands in a free slot · 04 edit and save existing · 05 delete with confirm (`POST /api/profile/delete`) · 06 export selected (`GET /api/profile/export`) round-trips through 07 import (`POST /api/profile/import`, both call sites) with identical segments · 08 hide a builtin (`POST /api/profile/builtin/hide`) and `restoreBtn` (`/restore`) · 09 bulk select/delete mode (`modeDeleteBtn`, `bulkActionBtn`, `bulkCancelBtn`) · 10 io-segment (relay) rows render targets from `GET /api/zones` · 11 a `user`-tier session can list but `POST /api/profile` is refused (ADMIN tier).

**Thermocouples & zones `/settings/zones`** WEB-ZONE-01 render (show-when-enabled sections absent when no zone enables them — the roadmap "Visually simplify" row's rule) · 02 `POST /api/zones` round trip of the unchanged config is byte-identical · 03 zone type / failsafe / relay type / TC type selects write and read back · 04 "Same as zone N" `groupsrc` selectors (post the roadmap zone-0 row: zone 0 also has them) · 05 PID fields → `POST /api/zones/pid` · 06 autotune Start step test / Start relay test / Abort / Accept buttons drive `/api/autotune/*` (AT-01/02/03 observed through the page) · 07 tuning recommendations (`GET /api/tuning_recommendations`) renders a method · 08 coupling matrix + RGA render after AT-05 · 09 Continuous Tuning enable/revert (`/api/adaptive_tune/*`) toggles and reverts, gains unchanged afterwards · 10 Measure Zone Normal Current: start refused or `INCONCLUSIVE` on this fixture (sweep floor 0.045 A, ROADMAP status) — the abort button (`/api/zones/current_sweep/abort`, SAFETY_REDUCE) always answers 200 · 11 Tuning quality / Firing quality sections populate after HP-01 · 12 `GET /api/zones_diag` well-formed · 13 `GET /api/zones/ct_channel_map` matches the Pico's commissioning.

**Safety processor `/safety`** WEB-SAF-01 render: link status, temperatures, build card matches `safety_get_fw_version()` · 02 Frame B diagnostics agree with `safety_get_diag()` · 03 Clear latched trip button disabled when no trip, enabled and effective during OT-B01 · 04 `GET /api/status?diag=1` fields present.

**Safety timings `/settings/safety`** WEB-STIM-01 render · 02 timing-profile assignment round trip through `POST /api/zones` (restored).

**Safety commissioning `/safety/commissioning`** WEB-COMM-01 render both guided and advanced views · 02 guided flow screens 1-4 reach "Stage & commit" with the board's current answers (no write) · 03 advanced "Stage & commit" of the **unchanged** parameter set → `POST /api/safety/commissioning` `commit=1`, then `kcCommissioningCommitAndVerify`'s re-GET matches · 04 busy check: with HP-01 running, the commit is refused client-side (`kcCommissioningCheckBusy`) · 05 bench preset button (`/bench_preset`) is present and **not pressed** (it would overwrite commissioning; owner question §7.7) · 06 CT cal / trim / auto-zero controls render for channel 2 only (summed topology) and their POSTs are **not** exercised (channel 2's calibration is correct and complete, ROADMAP status) · 07 relay_type commit round trip of the current value.

**Ready to fire `/readiness`** WEB-RDY-01 render, legend, every item has a status in the four-state set · 02 the four hard interlocks (trip, recovery, crash, E-stop) are all `ok` on the healthy board · 03 deep-link `#<key>` highlights the row · 04 during OT-B01's trip window the trip item is `not_done` and the dashboard Start is refused.

**Setup wizard `/setup`** WEB-WIZ-01 render, overview, `GET /api/setup/progress` · 02 Resume opens the first unfinished step · 03 step 1 save (`/api/settings/tz`, `/api/unit_pref`) round trip, restored · 04 step 2 live poll shows three channels · 05 step 3 TC types read back · 06 step 4/6 consequence-confirm dialogs appear when a value changes (cancel, no write) · 07 step 7 embedded commissioning = WEB-COMM-03 · 08 step 9 sweep: Start disabled until the presence checkbox; **not started** (INCONCLUSIVE on this fixture, same as WEB-ZONE-10) · 09 step 10 save gains for zone N = WEB-ZONE-05 · 10 step 11/13 mark-done/skip write progress and are reverted by re-posting the saved progress blob · 11 progress persists across `sw_reset_esp` (OT-B01).

**Diagnostics `/diagnostics`** WEB-DIAG-01 render all cards (this is the heaviest page; SK-04 samples DRAM around it) · 02 crash card "No crash recorded" on a healthy board; Acknowledge/Clear buttons exist and are **not** pressed · 03 partitions table equals FL-01 · 04 cfgfs card equals FL-07 · 05 thermocouple faults: three channels `ok`, safety-processor TC card `ok` · 06 relay wear rows incl. the "Safety (K4)" row; **Reset count is not pressed** (relay life is real data) · 07 watchdog-panic toggle: disable → `GET /api/watchdog_cfg` `disabled:1` → re-enable (restored, judged) · 08 ramp assist toggle round trip, restored to off · 09 Danger Mode: accept checkbox enables Enter; `POST /api/diagnostics/danger/start` → live view; toggle relay R0 on then off (`/danger/relay`, `io_read()` confirms) with **Firing mode tile untouched**; Exit (`/danger/stop`); 5-min auto-exit checked on a second entry left idle (`INCONCLUSIVE` if the run's time budget excludes it) · 10 `GET /api/debug/lwip_stats`, `/api/diagnostics/timing`, `/api/board_temps` well-formed · 11 stale banner appears when polling is blocked (client-side).

**Firmware update `/ota`** WEB-OTA-01 render: interlock box "Idle -- updates allowed", ESP info matches FL-03, Pico info matches FL-08 · 02 interlock box "Blocked" with the reason while HP-01 runs and pickers hidden · 03 ESP update via the page's file input (`pushImage`) = OT-E01 driven through the DOM (one of the two, not both, in a given run) · 04 rollback button = OT-E02 · 05 recovery-exit box hidden when not in recovery · 06 Pico picker present; update/rollback **not pressed** until OT-P is unblocked · 07 boot-button bypass banner absent · 08 protocol compatibility flag `yes`.

**Network `/wifi`** WEB-WIFI-01 render, `GET /status` rows for home mode: connected, SSID, IP, RSSI · 02 scan (`GET /networks`) returns the bench SSID as Saved · 03 QR canvas renders (AP section is hidden in home mode — judged by DOM presence only) · 04 IP-mode toggle shows static fields (no submit) · 05 `POST /provision`, `/forget`, `/ip_config` are **never called** by the harness (a bad write strands the board; owner question §7.8) · 06 mode toggle to AP and back is **operator-only** (`SKIP: needs_operator`) — the AP-fallback branch of every page is therefore recorded as `NOT_RUN` in unattended runs.

**Security `/settings/security`** WEB-SEC-01 render; `GET /api/auth/config` shows flags only, no secrets · 02 client-side rules: enabling web sign-in refused when a password is unset (DOM only) · 03 **enable web auth** (`cmd=set_policy web_enabled=1`) using the harness-supplied credentials, then verify: `/` still 200 (dashboard tier), `/settings/zones` redirects to `/login`, `POST /api/auth/login` succeeds and sets the cookie, `GET /api/auth/session` reports the role, `POST /api/auth/session/extend` 200, a `user` session gets 403 on `POST /api/zones`; then **disable** (`web_enabled=0`) and confirm PcTools' HTTP reads are un-blinded again (memory `project_web_auth_verified_and_blinds_pctools`) — this case *always* restores, in `finally` · 04 LCD PIN policy toggle round trip (`lcd_enabled`) with the LCD-01/02 lock cases · 05 lockout: 6 bad logins → 429 on `/login` (memory `project_login_lockout_saturation_accepted`: this denies every new address for a while — run it **last** in the suite and never in `smoke`) · 06 Clear login credentials is **not pressed**.

**Backup `/settings/backup`** WEB-BAK-01 render · 02 `GET /api/backup/export` downloads valid JSON containing every zone and profile · 03 import the just-exported file (`POST /api/backup/import`) → fingerprint identical (the round trip, memory `project_backup_round_trip_coverage`) · 04 import refused while HP-01 runs.

**Kiln configs `/settings/kiln_configs`** WEB-KCFG-01 render, `GET /api/kiln_configs` · 02 Save as new "BENCH_tmp" → clone → rename → download (`/export`) → import (creates a new slot) → delete both (every route in the family, all restored) · 03 apply the *active* config to itself: 202 then `apply_status` `done_ok` · 04 `kcAckHwDiffers` header path: apply a config saved with a different relay count → 428 without the header (uses the clone from 02 edited before import) · 05 divergence banner absent when `safety_ceiling_match` ok.

**Settings `/settings`** WEB-SET-01 render · 02 cfgfs format banner hidden (`format_pending:false`); **format is never pressed** · 03 Danger-zone reset buttons render; **none pressed** · 04 Reboot both processors (`/api/sw_reset`) = OT-B01, exercised through the page in one run of the two.

**Display `/settings/display`** WEB-DISP-01 render · 02 `GET/POST /api/settings/display_power` round trip: brightness 50 → LCD-05 samples the panel darker → restore; timeout "1 min" → LCD-06 confirms blanking → restore "Never" · 03 `brightness_inert` hint matches the build flag · 04 theme toggle is browser-local (DOM only).

**Login `/login`** WEB-LOG-01 render · 02 good/bad credential branches (with WEB-SEC-03 on) · 03 429 branch (= WEB-SEC-05).

**Cross-cutting (`app.js`, `nav.js`)** WEB-X-01 nav menu on every page has the 15 links and auto-expands the current group · 02 session-lock prompt appears in the last window of `web_timeout_min=1` (with WEB-SEC-03 on) and "Stay unlocked" extends · 03 every OPEN-tier route answers without a session, every ADMIN-tier route answers 401/redirect without one — generated from `route_tier_table.h` (150 rows) so the case stays complete as routes are added; this is the mechanical "auth tier" test of the whole API and reports per-route.

### 3.8 Suite LCD — every LCD page and mode, judged by numeric pixel sampling

**Mechanics.** Navigation and taps use the UI_TEST task (`ui_test_client.py`: `get_current_page()`, `list_tap_targets()`, `click_by_name()` via `ui_step(backend="lcd", …)`) and `touch_inject(x, y, pressed)`; every visual judgment is a `capture_lcd.ps1` frame plus `sample_lcd_region.ps1` mean-RGB of a ≤ 8×8 region, compared to the `ui_theme.h` constants (`BG 0x1a1f2b`, `CARD 0x242a3a`, `ACCENT_1 0xe8974e` orange, `ACCENT_4 0x5cc06e` green, `ACCENT_5 0xd6555f` red, text `0xf0f0f0`) with a per-channel tolerance calibrated once per run against the bezel reference (the camera's white balance is not the panel's) — the transform `frame_x = 102 + 1.890·lcd_x`, `frame_y = 12 + 1.903·lcd_y` (UI_PLAN.md:668) maps widget centres from `list_tap_targets()` to frame pixels, so regions are derived from the live widget tree, not hardcoded. A capture where the bezel reference itself is off by more than the tolerance (camera moved, ffmpeg exit `-5` busy device) makes every LCD case `INCONCLUSIVE: camera`, never FAIL. No framebuffer read-back exists on the device; the webcam is the only pixel path.

**Post-rework note.** UI_PLAN.md Section 6 deletes `profiles_mine`/`profiles_family`, turns `profiles` into a 4×64 px picker, adds the profile-name button left of Start (greyed while not IDLE), a right-hand rail, a Safety (K4) line on Temperature, and removes Relay-Life Reset. The cases below are written for **that** LCD; where today's LCD differs, the case names the pre-rework check in brackets so the suite is runnable before and after, and the case registry carries a `ui_rev` gate read from the page list `get_current_page()` can reach (`profile_picker` present ⇒ post-rework).

| id | page / mode | steps | judged by | dur |
|---|---|---|---|---|
| LCD-01 | home, idle | `get_current_page()` == `home`; capture | Start button region ≈ `ACCENT_4`; Pause button hidden (region ≈ `BG`); trip strip hidden; progress bar empty; profile-name button present and *tappable* [pre-rework: absent] | 20 s |
| LCD-02 | home, firing | HP-01 running | Start half now reads Stop (widget name from `list_tap_targets()`); Pause visible ≈ `ACCENT_1`; progress fill ≈ `ACCENT_1` grows between two captures 60 s apart; profile-name button greyed (`TEXT_SECONDARY`) and its tap does nothing (`get_current_page()` unchanged) | 2 min |
| LCD-03 | home, paused | HP-04 paused window | Pause widget label becomes Resume; duties 0 | 20 s |
| LCD-04 | home, tripped | OT-B01's S6a window | trip strip region ≈ `ACCENT_5` with white text; after `safety_clear_trip()` region ≈ `BG` | 1 min |
| LCD-05 | brightness | WEB-DISP-02 sets 50 % | mean luminance of a `TEXT_PRIMARY` region drops by ≥ 25 % vs 100 %; `INCONCLUSIVE` if `brightness_inert` | 30 s |
| LCD-06 | blanking | WEB-DISP-02 sets 1 min timeout; wait 70 s | whole-panel mean ≈ bezel (off); `touch_inject` wakes it (`touch_get_state` on) | 2 min |
| LCD-07 | home rail | post-rework only | rail region present (card colour) with 4 relay pills; the pill for zone 0's relay ≈ `ACCENT_4` during HP-01 and ≈ `NEUTRAL` idle | 30 s |
| LCD-08 | config hub | tap Menu | page == `config`; five tiles by name (Profiles, Temperature, Network / Wi-Fi, Touch Calibration if present, Diagnostics) | 15 s |
| LCD-09 | profiles picker | tap Profiles | page == `profiles` [pre: hub with 4 tiles]; rows ≤ 4, favorites first with a star glyph; paging indicator and New icon in the topbar; a row tap opens `profile_detail` | 30 s |
| LCD-10 | profile detail → start/stop confirm | from LCD-09 on the hidden bench slot | Start opens `ui_confirm`; cancel returns; confirm starts (= HP-01 via LCD); Stop from home requires confirm | 1 min (heat via HP-01) |
| LCD-11 | profile delete | post-rework | per-row Delete arms (first tap changes colour to `ACCENT_5`), second tap within 5 s deletes a harness-created copy; a single tap left 6 s disarms | 30 s |
| LCD-12 | profile builder | New → zones step → segment step → review → save | `ui_num_pad` accepts digits; "Segment 1 of 1" label; review page saves to a free slot (`GET /api/profiles` shows it); deleted afterwards | 2 min |
| LCD-13 | segments page | from detail | 4 rows/page paging works (a builtin with > 4 segments) | 20 s |
| LCD-14 | temperature page | from hub | three zone rows show values within 1 °C of `thermo_read()`; unowned-relay toggles present, zone relays not toggleable; Safety (K4) line ON during HP-01, off idle [pre: line absent] | 1 min |
| LCD-15 | network page | from hub | mode/state text equals `wifi_get_status()`; RSSI bar present; "Manage networks" opens `network_manage` whose saved list contains the bench SSID; **no connect/forget tap** | 30 s |
| LCD-16 | diagnostics, 5 sub-pages | from hub, page through | titles "Safety & Board Health", "Thermocouple Faults", "Trip Detail", "Relay Life", "Crash Report" reached in order; Relay Life has **no** Reset button [pre: has one, not pressed]; Crash Report shows none; board health heap within 10 % of `get_heap_status()` | 2 min |
| LCD-17 | touch test / touch cal | from hub | `touch_test` Clear/Done work, Done returns home; `touch_cal` entered and **backed out without completing** (a completed cal rewrites the transform) | 30 s |
| LCD-18 | topbar warning tier | during OT-B01 trip | topbar warning region ≈ `ACCENT_5` (error tier) vs `ACCENT_1` during a WARN-only condition if one is present, else that half `NOT_RUN` | 20 s |
| LCD-19 | PIN lock | WEB-SEC-04 enables `lcd_enabled` with a harness PIN | after `lcd_timeout_min`, Start tap raises the keypad; wrong PIN refused; right PIN starts; **Stop is never gated** (a tap on Stop during HP-01 with the lock engaged stops the firing) | 3 min |
| LCD-20 | recovery-mode idle policy | only under OT-E11 | `screen_idle`'s recovery flag: panel does not blank; otherwise `NOT_RUN` | — |
| LCD-21 | no-scroll budget | every page visited above | `list_tap_targets()` reports no target with `cy > 320` and `truncated:false` (memory `feedback_lcd_no_scrolling`) | in-line |

### 3.9 Suite SP — safety-processor surface

| id | case | steps | judged by | dur | heat |
|---|---|---|---|---|---|
| SP-01 | Commissioning read-back | `safety_get_commissioning()` vs `GET /api/safety/commissioning` | identical field set; `commissioned:true`, `stale:false`; `abs_max_temp_c` == ESP `max_temp_c` and > 0 (constraint 19 of §6) | 5 s | no |
| SP-02 | Status and diag consistency | `safety_get_status()`, `safety_get_diag()`, `GET /api/status` safety fields | link up, state `armed`/`idle` as appropriate, boot reason not `watchdog`, `trip_reason 0` | 5 s | no |
| SP-03 | Link stats over a firing | `safety_get_link_stats()` before/after HP-02 | `crc_errors`, `timeouts`, `broadcast_dropped` deltas are 0; GET_STATUS timeouts excluded by design (memory `project_get_status_has_no_reply`) | 5 s | uses HP-02 |
| SP-04 | Trip / clear | OT-B01's S6a | `trip_mask == 1 << (trip_reason-1)` exactly, `trip_reason == 6`; `safety_clear_trip()` refused while link is still down, accepted once up | 1 min | no |
| SP-05 | E-stop verify | `POST /api/estop/verify` current state | reports jumper fitted / not asserted (flags bit `0x04` clear, memory `project_estop_jumper_is_fitted`) | 5 s | no |
| SP-06 | Heat enable path | during HP-01 | `dashboard_status_t.safety_relay_energized` true only while RUNNING; false within one tick of HP-05's stop | in-line | uses HP |
| SP-07 | Rate guard read-back | `safety_get_rate_guard()`, `GET /api/safety/rate_guard/auto` | consistent; not written | 5 s | no |
| SP-08 | E-stop press | **operator**: press/release | trip S7 latches and clears per `GUARD_TEST_MATRIX.md`; `SKIP: needs_operator` unattended | 2 min | no |
| SP-09 | Link-loss | **operator**: pull the link cable during HP-01 | S6b (`trip_reason 7`, mask `0x0040`) within the liveness window, K4 open, firing FAULTED; `SKIP: needs_operator` unattended | 3 min | **yes** |
| SP-10 | CT / S9 / S14 / S15 | read `safety_get_commissioning` CT fields and guard states | recorded; `INCONCLUSIVE` by definition on this fixture (S14/S15 dormant, `i_normal_a` unset below the 0.045 A floor) | 5 s | no |
| SP-11 | Pico stack margins | = SK-03 | — | — | no |

## 4. Case counts

| suite | cases | of which heat | of which operator-only / blocked |
|---|---|---|---|
| ST static+host | 5 | 0 | 0 |
| FL flash layout | 11 | 0 | 2 opt-in flashes |
| SK stack | 4 | 0 | 0 |
| OT OTA | 19 (12 ESP, 5 Pico, 2 both) | 0 (E07/E08 ride on HP/AT heat) | 5 Pico blocked, E06 operator/fixture, E11 needs NEW tool |
| AT autotune | 5 | 2 | 0 |
| HP heating profiles | 8 | 6 | 0 |
| WEB | 119 (DASH 13, PROF 11, ZONE 13, SAF 4, STIM 2, COMM 7, RDY 4, WIZ 11, DIAG 11, OTA 8, WIFI 6, SEC 6, BAK 4, KCFG 5, SET 4, DISP 4, LOG 3, X 3) | 0 own heat (several observe HP/AT) | WIFI-06 operator |
| LCD | 21 | 0 own heat (observe HP) | LCD-20 only under OT-E11 |
| SP | 11 | 1 (SP-09) | SP-08/09 operator; SP-10 INCONCLUSIVE by design |
| **total** | **203** | **9 heat-originating** | |

## 5. Routine subsets, ordering, interdependence

### 5.1 Subsets

- **`smoke` (~10 min, no heat, no flash):** ST-05, FL-01..09, SK-01, SK-03, SK-04, SP-01, SP-02, SP-05, SP-07, every WEB `-01` render case (18) plus WEB-X-03 (the generated per-route tier sweep), LCD-01, LCD-08, LCD-21. Verdict: the board is the build it claims, boots the right partition, is healthy, and every page still renders behind the right auth tier.
- **`nightly` (~60 min):** `smoke` + ST-01..04 + HP-01, HP-02, HP-04, HP-05, HP-06, HP-08 + SK-02 + SP-03, SP-06 + WEB-DASH-03/06/07/09, WEB-PROF-02..09, WEB-ZONE-02/03/05/09/12, WEB-BAK-02/03, WEB-KCFG-02/03, WEB-DIAG-07/08, WEB-OTA-01/02, WEB-SEC-03, WEB-X-01/02 + LCD-02..04, 09, 14, 16 + OT-B01 + OT-E01/E02/E03/E12 (one ESP OTA round trip, ~6 min). Excludes autotune (25-min cooldowns), Pico OTA (blocked), lockout (WEB-SEC-05), operator cases.
- **`full` (several hours, not run often):** everything, in the §5.2 order, including AT-01..05 with their rest gates, all OT-E, HP-03/HP-07, WEB-SEC-05 last, and the operator cases prompting for the operator when `--attended` is passed (otherwise `SKIP: needs_operator`).

### 5.2 Ordering

Fixed, not alphabetical: **ST → FL (read-only) → SK-01/03/04 → SP read-only → WEB render cases → WEB read/write round trips (auth off) → HP-01 (with WEB-DASH/LCD/SP observers attached) → HP-02 → HP-04 → HP-05/06 → SK-02 → HP-03 → HP-07 → rest gate → AT-01/02/03 → rest gate → AT-04/05 → OT-B01 (with LCD-04/18, WEB-DASH-08, WEB-RDY-04, SP-04 observers) → OT-E01..E05, E07/E08 (re-using a short HP/AT), E12 → FL-10/11 opt-in → OT-E10 with WEB-SEC-03 → WEB-SEC-04 + LCD-19 → WEB-SEC-05 last → teardown**. Observers are cases that only *read* during another case's window; they are the one sanctioned form of interdependence beyond explicit `depends_on`.

### 5.3 Interdependence rules

1. A case may depend on at most **one** other case, and only when it is small in scope (a read during another's window, or "a previous image exists"). `depends_on` is declared in the registry, and the runner sets `NOT_RUN: dependency <id> was <verdict>` rather than attempting a case whose dependency failed.
2. Anything that changes board state restores it in the same case's `finally` (zones config, display power, watchdog cfg, ramp assist, auth policy, the hidden bench slot, kiln-config slots). A case that cannot restore marks the run `tainted` in `summary.json` and the teardown reloads the run's opening zones snapshot.
3. Heat-originating cases never overlap; their observers attach to the running case rather than starting their own heat.
4. Web-auth-on cases are grouped at the end so a failure to disable auth blinds as few later cases as possible, and WEB-SEC-03's restore runs even on exception.

## 6. What the harness must never do

1. Flash or OTA while `profiles_get_exec_status` is not idle, `autotune_get_status` is active, or `GET /api/ota/interlock` is not `ok` — checked immediately before the call, not at run start (`ota_interlock.c:28-97`).
2. Kill its own process or the MCP server to end a firing; the only stop is `profiles_stop()` / `POST /api/profile_exec/stop`.
3. Pass `allow_sensitive_dirty=True`, `verify=False`, or `ack_unsettled=True`, or bypass a refusal from any tool — a refusal is a FAIL with the tool's text, never a retry with a wider flag.
4. Acknowledge or clear a crash report, reset relay-life counts, format the cfg partition, factory-reset any scope, clear login credentials, press the bench-preset commissioning button, write CT calibration, write `abs_max_temp_c` on either processor, or change Wi-Fi provisioning — these are owner actions (several are listed in §3 as "present, not pressed").
5. Clear a safety trip without first confirming, from `safety_get_status()`, that the link is up and `trip_mask == 1 << (trip_reason - 1)` with `trip_reason == 6` and no other bit — any other trip stops the run for a human (`docs/MCP_SERVERS.md` dual-reflash procedure; `link_frame.c:237`).
6. Attempt a Pico OTA while `docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md` and `pico_ota_staged_crc_mismatch_2026-09-18.md` are open; and never loop-retry a Pico OTA.
7. Start a heated case unless all zones are rested (§2.4), both processors' ceilings agree and are > 0, and the E-stop jumper reads fitted/not asserted.
8. Target more than 45 °C in any HP profile or let AT exceed 70 °C (abort at 70 °C observed on any zone).
9. Write a credential anywhere: `ap_password` and web credentials arrive as call parameters, are redacted in `transcript.md`, and never appear in `summary.json`, `.env`, or a preset.
10. Run while the MCP server reports `stale`, or restart it mid-run.
11. Treat `INCONCLUSIVE` as a pass in the exit code, or a `SKIP` as failure of the board — both are reported distinctly and `bench_test.ps1` exits 3 for either.
12. Touch `.kicad_*`, `firmware/KilnFW/elf_archive/`, `logs/coupling/*`, or any tracked file — its only writes are under `logs/bench_test/` and the hidden bench slot (never a user-visible profile slot).

## 7. Open owner questions

### Owner decisions (2026-09-19)

1. **Log home (was Q1).** Decided: gitignored `logs/bench_test/<run>/` plus one human sentence per notable run in `docs/BENCH_TEST_LOG.md`. Matches the recommendation.
2. **LCD suite (was Q2).** Decided: write against the post-rework LCD (`docs/UI_PLAN.md` §6), gated on `ui_rev`. Matches the recommendation.
3. **Harness profile slot (was Q6).** Decided: **not** user slot 7. Owner's words: "i intended there to be 100 user profiles and 1 running profile for live edits. please place this after that with no visibility to the user." The harness slot is a hidden slot after the 100 user slots and the 1 live-edit slot, never listed, exported, or shown on any page or the LCD. `slots100` (worktree, not yet on `main`) owns the concrete slot layout; this plan and any harness code only reference "the hidden bench slot after the live-edit slot" — the index itself is `slots100`'s to assign.
4. **Pico stack threshold (was Q5).** Decided: capture a Pico baseline in wave 1; flat 25% interim rule for SK-03 meanwhile. Matches the recommendation.
5. **Recovery-boot tool (was Q3, OT-E11).** Decided: stays owned by `docs/OTA_SINGLE_SLOT_PLAN.md`; bench cases that need `ota_recovery_boot_esp()` SKIP until it exists there. Matches the recommendation.
6. **cfg partition FL-07 (was Q4).** Decided: record-only — INCONCLUSIVE with both the board's partition table and git's `partitions.csv` printed side by side, never FAIL. Firmer than the recommendation (which left the eventual verdict shape open); the verdict shape itself (INCONCLUSIVE, not PASS/FAIL) is now fixed.
7. **Bench preset and commissioning writes (was Q7).** Decided: **override of the recommendation.** Commissioning checks MAY WRITE test values — the harness may provision Wi-Fi and web credentials from `KILNCTL_WEB_USERNAME`/`KILNCTL_WEB_PASSWORD` and the Wi-Fi env vars. Never write a credential into any repo path, doc, fixture, log, or report — redact in logs (same discipline as `report.py`'s existing `_redact`). Restore the pre-run state afterward wherever the API allows it. Suite design for WEB-COMM changes accordingly: these cases are no longer permanently read-only, and need credential-sourced values threaded from env vars rather than hardcoded in `cases_*.py`, plus restore-in-`finally` for anything they change.
8. **Wi-Fi writes / AP-fallback (was Q8).** Decided: **override of the recommendation.** Wi-Fi reconnect / AP-fallback cases run in the unattended `nightly` suite too, not attended-only. Design requirement: leave the board on the LAN at the end — reconnect is the last step, with bounded retries, and a board that does not come back is recorded INCONCLUSIVE rather than causing the run to hang.
9. **New HTTP routes (was Q9).** Decided: none. Matches the recommendation — the harness uses the existing HTTP API plus benchproto/MCP only; the `httpd_uri_t` cap (`check_uri_handler_cap.ps1`) stays untouched.
10. **Opt-in JTAG flashes in `nightly` (was Q10, FL-10/11).** Decided: **override of the recommendation.** The unattended nightly MAY perform JTAG flashes (ESP via `flash_firmware()`, Pico via `debug_program(peer="pico")`). Design requirement: flash only after a passing pre-flight (board reachable, executor IDLE, no unacknowledged crash report); `verify=True` always; if post-flash verification fails, the run records FAIL and STOPS without attempting any further flashes, so a human recovers the board the next morning rather than the harness compounding the failure.

All ten owner questions from this section are now decided; none remain open. Wave 0 (this commit) implements none of the writing/flashing behavior above — it stays read-only per its own scope; these decisions govern wave 1+ suite design (`web`, `nightly`, `full`).

## 8. Staged implementation

Waves that can run in parallel are marked; each is one PR-sized change and each ships with pytest coverage under `tools/PcTools/tests/test_bench_test_*.py` (mocked MCP functions, one negative test per judgment function — every check here must be negative-tested, memory `feedback_negative_test_every_check`).

**Wave 0 — skeleton (serial, first).** `tools/PcTools/src/kilnctrl/bench_test/{__init__,registry,runner,report}.py`; `mcp_server_bench_test.py` with `bench_test_run/list/last`; facade keywords in `mcp_facade.py`; `tools/bench_test.ps1`; `logs/bench_test/` `.gitignore` entry; `summary.json` schema + `transcript.md` writer; preflight/teardown of §2.4; verdict enum; the `smoke` suite wired to ST-05/FL-01..09/SK-01/03/04/SP-01/02/05/07 (all read-only). Files touched: the new package, `mcp_server.py` (one import line, `mcp_server.py:459-489`), `mcp_facade.py`, `.gitignore`, `docs/MCP_SERVERS.md` (tool table), `tools/check_mcp_tool_count_doc.ps1` expectations.

**Wave 1a — WEB render + tier sweep (parallel).** `cases_web.py`: the 18 `-01` render cases via `ui_step(backend="web")`, WEB-X-01, WEB-X-03 generated from `route_tier_table.h` (parse the 150 `ROUTE_TIER(...)` rows at run time so the sweep cannot go stale). Touches only the new package and tests. **DONE (2026-09-19).** 19 case ids wired (17 page renders + WEB-X-01 + WEB-X-03), registered into `SUITES["smoke"]` via `_WEB_SMOKE_IDS`; wired via the one-line `from . import cases_web` in `bench_test/__init__.py` per the low-conflict pattern. `parse_route_tier_table()` regex-parses the real header (asserted at 150 rows in a dedicated test); WEB-X-03 exercises only `HTTP_GET` rows live, records non-GET rows without invoking them (read-only this wave). 145 PcTools unit tests pass (`pytest tools/PcTools/tests/test_bench_test_*.py`), including a negative test proving `judge_web_render`'s landmark check can fail. Verified at the unit level only — the bench board is currently blocked by an unacknowledged crash report (`touch_log_tap_targets`), so no hardware run was attempted.

**Wave 1b — HP + SP observers (parallel). DONE (unit-tested only, never run against the bench board).**
`cases_heat.py`, `cases_safety.py` implement the bench profile builder
(`BENCH_PROFILE_SLOT_ID`, defaulting to 7 today with a one-line TODO to
switch to 101 once `PROFILES_MAX_COUNT` is 100 on main -- `slots100` owns
the concrete index), the rest gate (`_rest_gate`, 2C band, 25-min timeout),
HP-01/02/04/05/06/08, and SP-03/SP-06 (implemented as observers reading
`ctx["_hp01"]`/`ctx["_hp02"]`, stashed there by `cases_heat._hp_run`, rather
than driving their own firing -- `registry.py` now marks both `depends_on`
the HP case they observe). Every heat case refuses to start if
`capability_preflight` is not ok (unacknowledged crash report, a latched
trip, an unreachable board) and restores board state in `finally`
(`_cleanup_bench_profile`: stop + delete the hidden slot). Calls the same
in-process client objects the MCP tools already wrap (`srv._profiles`,
`srv._safety`, `srv._thermo`) -- no new HTTP routes, no firmware changes.
Tests: `test_bench_test_judgments_heat.py` (pure verdict derivation,
including the "zone 0 rises more than zones 1-2" ordering rule),
`test_bench_test_cases_heat.py` (profile builder, rest gate, capability
preflight gate, cleanup, all board access faked), `test_bench_test_cases_safety.py`
(SP-03/SP-06 observer logic and NOT_RUN-when-absent behavior). **Not run
against the bench board** -- it is blocked by a standing unacknowledged
crash report (`touch_log_tap_targets` panic); this wave is unit-level only.

**Wave 1c — LCD capture pipeline (parallel). DONE 2026-09-19.** `cases_lcd.py` + `lcd_sampler.py`: wrap `capture_lcd.ps1`/`sample_lcd_region.ps1`, the widget-centre→frame transform, the bezel-calibrated tolerance, LCD-01/08/21 first. Touches the new package; added a `-Json` output switch to `sample_lcd_region.ps1` (one file, backwards compatible — plain-text output unchanged when omitted). The transform is a full least-squares affine fit (not scale-only) from the four CLAUDE.md 2026-09-19 corner measurements, since the panel sits rotated a few degrees; residual on each corner is ~6.3 px on the 1280x720 frame (the 4 real-world corners are not perfectly affine-consistent, so a true least-squares fit leaves this small, uniform residual rather than interpolating one corner exactly at another's expense). LCD-01/08 degrade their color half to `INCONCLUSIVE` (never a fabricated PASS) when no webcam frame can be captured (camera busy, ffmpeg exit -5); LCD-21 checks the no-scroll budget over whatever pages the run actually visited. The webcam was free this session: one real `capture_lcd.ps1 -Full` frame was taken and sampled at the panel's top-left corner (296,58) and the bezel reference (100,100) via the new `-Json` switch, both reading pure black -- consistent with the board's screen off/idle in ambient light at capture time, not diagnostic of the pipeline (no live board session was attempted, so page state and LVGL calls were not exercised). Coverage is 45 new pytest cases against mocked subprocess/board calls, all passing.

**Wave 1d — FL/SK baselines (parallel). LANDED.** `cases_fl.py` implements SK-01/02 against `stack_margin_baseline.load_records()`/`worst_case_across_conditions()` (per-task INCONCLUSIVE when no committed baseline exists, FAIL on regression below it; SK-02 adds a 512 B absolute floor and `depends_on="HP-01"` so it runs under real httpd traffic), the §7.5 Pico baseline capture (new `PicoStackMarginEntry`/`PicoStackMarginBaselineRecord` types in `stack_margin_baseline.py`, additive and backwards compatible — an old ESP-only record with no `load`/`processor` field still loads unchanged, and a Pico record is invisible to the ESP loader via its existing per-file exception-catching), and an FL-09 override that downgrades an otherwise-PASS `find_safty_crash_elf()` match to INCONCLUSIVE (never FAIL) when the archived ELF has no sibling `project_description.json` — permanently absent for SaftyFW/`elf_archive/` per the 2026-09-19 revert. 13 new unit tests (`test_bench_test_wave1d.py`) plus the full pre-existing 115-test bench_test suite pass with no regressions. One negative test performed (sabotaged `_case_sp05`'s asserted-bit check in `cases_smoke.py`, confirmed a wrongly-PASSing E-stop-asserted case, hand-restored via `git cat-file blob HEAD:<path>`, `git hash-object` confirmed byte-identical to HEAD). Not run against hardware — the bench board has an unacknowledged crash report blocking `capability_preflight` (`project_touch_log_tap_targets_panics_board`); all judge functions are unit-tested against synthetic data only. `SK-02`'s `depends_on="HP-01"` means it only actually exercises hardware once a wave implementing HP-01 runs it.

**Wave 2 — nightly (after 1a-1d).** WEB read/write round trips with restore (`finally` discipline), WEB-SEC-03 auth on/off, LCD-02/03/04/09/14/16/19, OT-B01 + SP-04, OT-E01/E02/E03/E12 using `ota_http_client`, the `nightly` suite definition, `bench_test.ps1` exit contract. Touches the new package; `docs/BENCH_TEST_LOG.md` (NEW, one-line-per-run human log, per §7.1).

**Wave 3 — full (after 2).** AT-01..05 with rest gates, HP-03/HP-07, OT-E04..E10, FL-10/11 opt-in, WEB-SEC-05 last, `--attended` operator prompts for SP-08/09, WEB-WIFI-06, OT-E06. Touches the new package only.

**Wave 4 — Pico OTA and recovery (blocked).** OT-P01..05 once the two Pico defects close; OT-E11 and LCD-20 once `ota_recovery_boot_esp()` exists (single-slot plan step 5). Touches the new package; updates the M8 rows and the roadmap OTA row's status from this suite's `summary.json`.

**Guards to add alongside:** `tools/check_bench_test_registry.ps1` (every case id in this document exists in `registry.py` and vice versa, so the plan and the code cannot drift — negative-tested by removing one id), picked up automatically by `run_all_checks.ps1`'s glob; and a pytest that asserts no case's steps reference a tool name that `kiln_help()`'s registry does not export.

## 9. Sources verified for this plan

`tools/PcTools/src/kilnctrl/mcp_server_{flash,debug,info,profiles,autotune,ota,safety,control,config_presets,capability_preflight,ui_test,touch,wifi,io,fixture}.py`; `tools/PcTools/src/mcpkit/workbench.py` (`build_kilnfw`, `build_saftyfw_host_tests`, `run_pctools_tests`, `run_repo_checks`); `tools/PcTools/src/kilnctrl/{ui_test_client,ui_test_runner,stack_margin_baseline,capability_preflight,flash_provenance}.py`; `tools/PcTools/ui_scripts/*.json`; `tools/PcTools/scripts/{capture_lcd,sample_lcd_region}.ps1`, `stability_soak.py`; `tools/run_all_checks.ps1`; `firmware/KilnFW/App/drivers/http/route_tier_table.h` and the 17 `*_page.html` files plus `app.js`/`nav.js`/`commissioning_shared.js`; `firmware/KilnFW/App/drivers/net/{ota_http,ota_interlock,wifi_provision_http}.c`; `firmware/KilnFW/App/drivers/ui/{kiln_ui,ui_theme.h,ui_page_*}.c`; `firmware/KilnFW/docs/UI_PLAN.md` §6; `firmware/SaftyFW/src/tasks/link_frame.c:237`; `docs/{MCP_SERVERS,OTA_SINGLE_SLOT_PLAN,SETUP_WIZARD,WEB_AUTH_PLAN,CONFIG_FILESYSTEM}.md`; `firmware/CommonFW/docs/UPDATE_PROTOCOL.md`; `docs/audits/{s6a_startup_grace_revert_2026-09-07,boot_guard_recovery_loop_2026-09-08,boot_guard_post_flash_recovery_footgun_2026-09-08,pico_ota_erase_watchdog_reset_2026-09-18,pico_ota_staged_crc_mismatch_2026-09-18}.md`; ROADMAP.md rows `24f02f94` (OTA) and `f8692c55`/`9dae015b` (LCD rework).
