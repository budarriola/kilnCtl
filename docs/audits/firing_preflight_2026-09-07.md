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
