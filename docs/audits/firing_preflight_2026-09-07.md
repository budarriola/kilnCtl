# Firing Preflight — 2026-09-07

Live board reads only, taken 2026-09-07 against ESP host `192.168.1.156`,
ESP commit `e584067f` / Pico commit `bdb1c504`. All calls via `kilnctrl` MCP,
read-only.

## 1. `capability_preflight_check`

There is no bare "run the gate" call — `capability_preflight_check(name, ...)`
diffs a *named config preset* (`tools/PcTools/config_presets/<name>.json`)
against the board's live capabilities before applying it. There is no preset
named `firing`; calling it with an invented name fails immediately
(`no preset named 'firing'`). Existing presets are tuning/test snapshots
(`tuned_baseline_20260831.json`, `bench_fixture.json`, etc.), not a
firing-readiness gate. **This item cannot be answered as "pass/fail" until
the owner names (or creates) the preset a firing should be checked against.**
Per CLAUDE.md, `capability_preflight` (the related crash-aware gate)
separately refuses to start a run on a board with an unacknowledged crash —
see §4, no unacknowledged crash currently.

## 2. Zone commissioning (`control_get_zones` / raw `/api/zones`)

| Zone | max_temp_c | max_ramp_c_per_hr | tc_type verified | Kp / Ki / Kd | progress_band_c | coupling row (c0,c1,c2) |
|---|---|---|---|---|---|---|
| 0 | 80.0 | 900 | type 3 (`tc_type=3`) | 0.0318 / 0.00010 / 0.8401 | **0.000** | 0.00, 27.32, 21.72 |
| 1 | 80.0 | 900 | type 3 | 0.0485 / 0.00020 / 1.0548 | **0.000** | 14.30, 0.00, 22.15 |
| 2 | 80.0 | 900 | type 3 | 0.0631 / 0.00020 / 1.0690 | **0.000** | 8.33, 12.42, 0.00 |

All three zones are `control_mode=3` (fuzzy, per
`project_fuzzy_ab_inert_control_mode.md` — 91c5d6d set mode 3 live).
`tuning_valid=false` / `tuning_settled=false` on all three raw zone records —
the PID gains above are the currently-active values but the firmware does not
consider a fresh autotune "settled" for any zone right now.
`progress_band_c=0.000` on all three — recorded only, per instruction not
diagnosed (an opus agent is separately investigating this; see
`project_*progress_band*` if one exists).
Coupling matrix values read back from `/api/zones` (raw JSON, since
`control_get_zones`'s summary render reported "coupling matrix: unavailable" —
a formatting-tool gap, not a missing value) match
`project_coupling_matrix_resolved.md`'s three-run fit.

## 3. Safety processor (RP2040 / SaftyFW)

- **State: TRIPPED.** `safety_get_diag()` → `state tripped | trip_reason 5 |
  warn_mask 0x0010 | trip_mask 0x0010`, `context frames ok 8596, bad 0`.
  `safety_get_status()` corroborates: `safety TC invalid`, currents read
  0.00/0.00/0.00 A, `ct zone: -`. This must be understood and cleared before
  any firing — see GO/NO-GO below.
- **Commissioned:** `safety_get_commissioning()` → `commissioned=True`,
  config CRC 9343 matches live (not stale). `borrowed=False`.
- **Guards armed on this bench right now:**
  - S1 abs_max_temp_c = 80 C — ARMED.
  - S8 rate-of-rise = 33.3 C/min, rate_window_s=60 — ARMED
    (`safety_get_rate_guard`).
- **Guards dormant / structurally unreachable on this bench** (from memory
  entries, unchanged as of this preflight):
  - S14 over-current per-channel — DORMANT, `ct_installed=0`, no CTs fitted
    per individual relay channel (`project_no_cts_fitted_guard_coverage.md`).
  - S15 under-current (WARN-only) — DORMANT, `ct_topology=per_zone`.
  - S1/S13 — dormant by commissioning state (per
    `project_saftyfw_guard_reachability.md`).
  - S3/S4/S9/S11/S14 — per `project_no_cts_fitted_guard_coverage.md`, cannot
    fire: no independent per-channel current sensor: welded-contactor
    detection (S9) specifically needs a CT-fitted, current-driving fixture
    that does not exist on this bench (ROADMAP.md line ~101).
  - The one CT that *is* fitted (GPIO28, summed-heater, 1A:1V + 59 mV offset,
    `project_ct_sensor_on_gpio28.md`, fitted 2026-09-05) only "partially
    supersedes" the no-CTs note — it is a summed/aggregate signal, not
    per-channel, so it does not itself arm S14 (see `ct_installed=0` above —
    the summed CT is apparently not what that commissioning flag tracks).

## 4. Live hazards to check before starting

- **Relays:** `io_read()` → `R1=0 R2=0 R3=0 R4=0` — all four relays off. Good.
- **Unacknowledged crash:** `get_heap_status()` did not print the
  `UNACKNOWLEDGED CRASH REPORT` banner CLAUDE.md documents for a live
  unreviewed panic — none pending. `reset_reason='software (esp_restart)'`,
  `uptime_s=1150` (recent clean restart, not a crash reboot).
- **Partition table:** `debug_check_partition_table()` → MATCH, on-chip table
  matches `partitions.csv`.
- **Both processors, matching builds:**
  - ESP: `get_fw_version()` → commit `e584067f`, tree clean, built
    2026-09-07 11:09:35Z, protocol v11, compatible=yes. **Flagged by the tool
    itself:** `board is 2 commit(s) behind HEAD: board=e584067f,
    HEAD=f7b6040c` — two commits landed on `main` since this board was
    flashed.
  - Pico: `safety_get_fw_version()` → commit `bdb1c504`, built
    2026-09-07 04:55:16Z, boot_id=243, config_version=102, config CRC 0x247F
    (commissioned), protocol v12 (min compatible v7).
  - **Open item, explicitly flagged per instruction:** a newer SaftyFW fix,
    `4f1b9a4f`, exists for a masked flash-program failure and is **not yet
    flashed** to the Pico (still running `bdb1c504`). This is a Pico-side
    programming-failure fix, not yet verified live.
- **Link health:** `safety_get_link_stats()` → 0 CRC/framing errors, 0
  resync events, 97 timeouts out of thousands of frames (poll period 500 ms),
  `routed nowhere 0`, `length/crc mismatch 0` — link itself looks healthy
  aside from the active trip in §3.

## 5. Roadmap items needing a live firing (batch these into one run)

- **30 s firing-abort stopwatch** — ROADMAP.md line 100 / line 256: the 1.5 s
  link-staleness ceiling was bench-verified 2026-09-06 with no firing needed,
  but the 30 s full-abort-of-a-running-firing timing is still unverified and
  explicitly "needs a real running firing to observe."
- **S9 welded-contactor escalation** — ROADMAP.md line 101: needs a CT-driving
  fixture that does not exist yet; not closeable by this firing alone, but
  worth re-checking whether the GPIO28 CT changes the picture.
- **Ramp-assist default (OFF→ON) validation** — ROADMAP.md line 396: needs a
  real firing at cone temperatures; everything measured to date is
  bench-range (0-80 C per the zone table above), below where the cone-table
  heat-work weighting activates.
- **AP-fallback end-to-end** — ROADMAP.md line 102: needs a router with both
  correct and deliberately-wrong static config; not itself a "run a firing"
  item but listed alongside these as another bench-procedure gap worth
  clearing in the same session if convenient.

## GO / NO-GO

- [ ] **NO-GO as of this read:** safety processor is currently in `state
      tripped` (`trip_reason 5`, `warn_mask/trip_mask 0x0010`, "safety TC
      invalid") — must be understood and cleared/reset before heating.
- [ ] Confirm what preset `capability_preflight_check` should be run against
      for a firing gate, or accept that no automated preflight gate exists
      yet and proceed on this manual report instead.
- [ ] Decide whether to flash Pico `4f1b9a4f` (masked flash-program failure
      fix) before firing, or explicitly accept running `bdb1c504`.
- [ ] Decide whether to update ESP to `HEAD` (`f7b6040c`, 2 commits ahead of
      the running `e584067f`) or explicitly accept running the current build.
- [x] Relays confirmed off (`R1..R4=0`).
- [x] No unacknowledged crash report.
- [x] Partition table matches `partitions.csv`.
- [x] Zone commissioning present for all 3 zones (limits, PID, coupling);
      `progress_band_c=0.000` on all zones noted, not blocking, being
      diagnosed separately.
- [x] S1 (80 C ceiling) and S8 (33.3 C/min rate) guards armed and commissioned.
- [ ] Owner aware: S3/S4/S9/S11/S14 cannot fire on this bench (no per-channel
      CT) — a welded contactor or per-channel overcurrent would go undetected.

## Re-checked 2026-09-07 (live, `kilnctrl` MCP, read-only)

- **ESP now at HEAD.** `get_fw_version()` → commit `20c2a5d5`, tree clean,
  built `2026-09-07 11:43:30Z`, protocol v11, compatible=yes, "board is
  running HEAD (20c2a5d5)". The `C:/wt/espflash` reflash completed; no
  firmware-affecting commits pending.
- **Pico:** `safety_get_fw_version()` → commit `b25663e2`, built
  `2026-09-07 11:40:31Z`, boot_id=145, config_version=103, config CRC 0x0B8D
  (commissioned), protocol v12 (min v7). `4f1b9a4f` is in — this is the
  post-fix build, superseding `bdb1c504`.
- **Safety state: still TRIPPED, but this is live, not a stale latch.**
  `safety_get_diag()` → `state tripped | trip_reason 5 | warn_mask 0x0010 |
  trip_mask 0x0010`, uptime 208 s, context frames ok 340/bad 0.
  `safety_get_status()` → "safety TC invalid", currents 0.00 A all three,
  `ct zone: -`. Same S5 sensor-invalid condition as before, now re-armed:
  expected with **no bench thermocouple attached**, not a firmware defect —
  the original NO-GO's concern (a latch surviving reflash) is resolved
  (boot_id changed 145, fresh trip on the current, real condition), but the
  board is *still* not clear to fire until a TC is connected.
- **Link:** `safety_get_link_stats()` → 0 CRC/framing errors, 4 timeouts,
  19 frames deframed/dequeued, 0 routed-nowhere, 0 length/CRC mismatch — clean.
- **Commissioning:** `safety_get_commissioning()` → `commissioned=True`,
  config CRC 2957 matches live, S1 (80 C) and S8 (33.3 C/min) ARMED, S14/S15
  DORMANT as before (no per-channel CT).
- **Zones:** `control_get_zones()` unchanged in shape — 3 zones, mode 3,
  same PID/coupling values as §2. `progress_band_c=0.000` on all zones is
  **not a defect**: it is the "use firmware default (3.0 C)" sentinel;
  guard 1 runs at 3.0 C as intended.
- **Protocol fields, corrected:** `get_fw_version()`'s `protocol_version 11`
  is the PC-link UART version; `/api/status`'s `self_protocol_version 12`
  (seen via `safety_get_fw_version`'s protocol v12) is the separate kilnlink
  version. Both read correctly for their respective links — not a mismatch.
- **Relays:** `io_read()` (via `get_board_state`) → `relays=0`, all off.
- **Crash:** `get_heap_status(host=192.168.1.156)` → no unacknowledged-crash
  banner, `reset_reason='software (esp_restart)'`, `uptime_s=28` (fresh clean
  reboot from the flash, not a panic).
- **Partitions:** `debug_check_partition_table()` → MATCH.
- **`capability_preflight_check`:** still no preset named for a firing gate;
  tried `default` and `tuned_baseline_20260831`, both fail (no such preset /
  preset missing a required `name` field). Item unchanged from §1 — still
  needs an owner decision, not itself a blocker for the mechanical checks
  above.

### Revised GO / NO-GO (2026-09-07, re-checked)

**NO-GO** — one live blocker remains, everything else clear:

- [ ] **Blocking:** safety processor is genuinely tripped right now
      (`trip_reason 5`, S5 TC invalid) because no bench thermocouple is
      connected. Connect a TC (or otherwise satisfy S5) and re-check
      `safety_get_diag()` before heating.
- [x] ESP running HEAD (`20c2a5d5`), clean tree, verified build timestamp.
- [x] Pico running post-`4f1b9a4f` build (`b25663e2`, boot_id 145),
      commissioned, config CRC matches live.
- [x] Trip-latch-survives-reflash concern from the original NO-GO is
      resolved — boot_id advanced, current trip is a fresh, real condition.
- [x] Relays off, no unacknowledged crash, partition table matches.
- [x] Link healthy (0 CRC/framing errors, 0 mismatches).
- [x] Zone commissioning intact; `progress_band_c=0.000` and protocol
      version fields confirmed correct, not defects.
- [ ] `capability_preflight_check` still has no named firing-gate preset —
      owner decision still open, same as original report.
- [ ] Owner aware: S3/S4/S9/S11/S14 still cannot fire on this bench (no
      per-channel CT) — unchanged.
