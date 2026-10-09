# One-firing bench plan — 2026-09-07

One firing session, bundled to close as many `GUARD_TEST_MATRIX` §3 hardware
rows and `CT_COMMISSIONING_PLAN.md` step-6 items as possible. Checklist form —
follow top to bottom, tick as you go. Sources: `firmware/SaftyFW/docs/
GUARD_TEST_MATRIX.md` §3/§6c, `firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md`.

**Live board state at time of writing (2026-09-07, read-only queries):**
- `get_fw_version`: ESP `fc6d30f8`, **1 commit behind HEAD (`29ee94ec`)** — reflash before firing.
- `safety_get_fw_version`: Pico `b25663e2` built 2026-09-07 11:40:31Z, commissioned, config CRC `0xBD82`.
- `safety_get_status`: link up, **safety TC invalid** (NaN) — blocks all firing per S5.
- `safety_get_commissioning`: `S1 abs_max_temp_c=80C ARMED`; `S8 max_rate_c_per_min=33.3C/min ARMED, window 60s`; `S14 DORMANT (ct_installed=0)`; `S15 DORMANT (ct_topology=per_zone)`; `tc_source=0` (OWN_J7, S13 not commissioned).
- `get_heap_status`: no unacknowledged-crash banner, uptime 58s (recent clean reboot).
- **Discrepancy to fix before the CT steps**: memory record says a summed CT was fitted on GPIO28 2026-09-05, but live commissioning still reads `ct_installed=0`, `ct_topology=per_zone`. Set both (`ct_installed=yes`, `ct_topology=summed`) before any sweep/S14/S15 work below, or the sweep runs the wrong (per-zone) mapping check against a single shared channel and produces nonsense.

---

## 0. Prerequisites — verify before anything else

- [ ] Safety thermocouple physically attached and reading valid (`safety_get_status` — currently **NOT met**, must be fixed first; S5 blocks all firing until it is)
- [ ] Both processors on the same HEAD build (currently **NOT met** — ESP is 1 commit behind; reflash + re-verify `get_fw_version`)
- [ ] Relays confirmed off at start (`safety_get_status` — currently true: currents 0.00/0.00/0.00 A)
- [ ] No unacknowledged crash (`get_heap_status` — currently clean)
- [ ] Config CRC recorded before starting (Pico `0xBD82`, live-matching per `safety_get_commissioning`) — re-check after every commissioning write below
- [ ] Owner sets `ct_installed=yes`, `ct_topology=summed` via the commissioning page (`safety_set_ct_cal` / `POST .../ct_cal`) — required for CT step 6 and S14/S15
- [ ] If S13 is in scope this session: owner commissions `tc_source=BORROWED_ZONE`, `borrowed_zone_index=<0..2>` now (reversible, restore after)

## 1. Items that need a live firing — what to provoke, what counts as pass

| Item | Needs heating? | Provoke | Pass evidence |
|---|---|---|---|
| S1 overtemp | Yes (near ceiling) | Lower `abs_max_temp_c` below current reading | Trip on 3rd consecutive over-ceiling reading (~300 ms), K4 drops, S9 confirms current stopped |
| S3 stuck-on | Yes (relay energized) | Command/jumper a relay on while KilnFW reports it off | Trip after `stuck_on_time_s` (20 s default), clearable once current stops |
| S6a mainFault | No (bench-only, can precede heat) | Short GPIO10 to ground momentarily | Trip within ~200 ms, K4 drops; only guard SimFW/host tests structurally cannot reach — bench is the only evidence path |
| S6b link dead | Yes (current must be present for soft tier) | Unplug ESP mid-firing | Soft trip at `link_timeout_s` (10 s) with current present; hard backstop at 120 s regardless |
| S7 E-stop | No (can precede heat) | Press/release E-stop | Immediate trip, no debounce beyond 50 ms |
| S8 rate-of-rise (33.3 °C/min, 60 s window, ARMED) | Yes (ramp) | Command a setpoint step that forces >33.3 °C/min sustained over the 60 s window | Trip once the two-window average exceeds threshold; must NOT trip during the profile's normal ramp (nuisance check) |
| S9 TRIP_INEFFECTIVE | Yes, plus a live-mains bypass jig | Bypass the contactor so current continues after K4 opens (trigger via another guard tripping first, e.g. S3) | `SAFETY_TRIP_INEFFECTIVE` after `trip_verify_s` (10 s) + 3-tick current-present debounce; **never clearable** — schedule last, before cooldown |
| S10 stratification | Yes | Decouple safety TC from chamber, heat it independently >200 °C disagreement | WARN only after `tc_disagreement_c` sustained 300 s; no relay effect |
| S11 frozen sensor | Yes, ≥10 min continuous heat | Isolate safety TC in an unheated thermal mass so reading is genuinely constant while current flows | Trip after `frozen_window_s` (600 s) with heat commanded throughout; do NOT stall SPI/DRDY to fake this — that trips S5 instead |
| S13 borrowed-zone stale (if commissioned in §0) | Yes | Stall/unplug the borrowed zone's own thermocouple | Graduated: WARN at 10 s, trip at 60 s once that zone's `sample_counter` stops advancing |
| CT commissioning step 6 | Yes | Run zones through a normal heat/dwell cycle with `ct_topology=summed`, capture the zone-sweep's derived normals | Three zone `i_normal_a` values recorded, `ct_counts` baseline sane, and the owner's ~70 mA figure checked against the summed reading (±~10 mA is the honest resolution of the current probe) |
| Dwell/ramp thermal behaviour | Yes | Just run the profile — ramp lock band, guard-progress windows, coupling-matrix tracking | Record temperature/time traces via `get_board_state` at each stage transition; compare against the coupling matrix / model params already on the board |

Trips above (all except S9 and the abort stopwatch) are recoverable via
`CLEAR_TRIP` once the provoking condition is removed, so a trip does not have
to end the firing — clear and resume.

## 2. Single-firing timeline

**Stage 0 — bench, cold, before ignition**
1. Prerequisite checklist (§0) — includes reflashing ESP to HEAD and fixing the safety-TC-invalid condition
2. S6a: short GPIO10 momentarily, confirm trip, remove short, `CLEAR_TRIP`
3. S7: press/release E-stop, confirm trip, `CLEAR_TRIP`
4. CT step 0 — noise floor: `safety_capture_ct_counts(seconds=60)`, relays off, record mean/stddev to `CURRENT_SENSE.md` §4
5. CT step 2 — auto-zero: run the auto-zero begin/poll/confirm sequence (relays off ≥5 s precondition), commit
6. Set `ct_installed=yes`, `ct_topology=summed` (if not already done in §0)

**Stage 1 — heat-up**
7. S3: jumper/force one relay on while KilnFW reports it off, confirm trip (~20 s), de-energize the forced relay, `CLEAR_TRIP`
8. S1: temporarily lower `abs_max_temp_c` (safe while still cold, e.g. to just above ambient), confirm trip + S9 "current stopped" observation, `CLEAR_TRIP`, restore `abs_max_temp_c=80`, confirm via config CRC
9. Start the normal profile; let the zone-sweep record per-zone summed normals (CT step 6's live-run half)
10. S6b: unplug ESP mid-ramp, confirm soft trip (10 s, current present), reconnect, `CLEAR_TRIP`

**Stage 2 — ramp**
11. S8: command a setpoint step to exceed 33.3 °C/min over the 60 s window, confirm trip, `CLEAR_TRIP`, resume normal ramp
12. Record ramp-lock-band / guard-progress traces via `get_board_state` at intervals (dwell/ramp thermal behaviour row)

**Stage 3 — dwell**
13. S10: decouple safety TC, heat independently to >200 °C disagreement, hold 300 s+, confirm WARN, restore TC, confirm WARN clears (non-latching)
14. S13 (only if commissioned in §0): stall the borrowed zone's TC, confirm WARN at 10 s / trip at 60 s, reconnect, `CLEAR_TRIP`, restore `tc_source`/`borrowed_zone_index`
15. S11: isolate safety TC in an unheated mass, hold ≥10 min with heat commanded, confirm trip at 600 s, restore TC position, `CLEAR_TRIP` (refused until reading unfreezes — expected)

**Stage 4 — end-of-session, irreversible, do last**
16. S9: with the bypass jig ready, trip another guard (e.g. S3 again) to open K4, then bypass the contactor so current persists; confirm `SAFETY_TRIP_INEFFECTIVE` after ~10 s; de-energize and remove the bypass immediately after the observable is recorded — **never clearable, this ends software control of that relay for the session**
17. Cool down; confirm no further guard activity
18. **Last of all**: hand off to the separately-scripted, destructive abort-stopwatch work (`tools/PcTools/scripts/bench_firing_abort_stopwatch.py`) — out of scope for this plan/session, owned by another workstream; do not run it before every item above is recorded

**After every temporarily-changed field (steps 8, 11 if changed permanently, 14)**: restore and re-read the config CRC from telemetry.

## 3. Tooling per step

| Step | Tool/script | Record to |
|---|---|---|
| Prereqs | `get_fw_version`, `safety_get_fw_version`, `safety_get_status`, `get_heap_status`, `safety_get_commissioning` (all read-only, already run) | this file's top block, then `GUARD_TEST_MATRIX.md` §4 |
| S6a/S7 | Manual wiring action + `safety_get_status`/`safety_get_diag` to confirm trip reason | `GUARD_TEST_MATRIX.md` §3.4 row |
| CT step 0 | `safety_capture_ct_counts(seconds=60)` (MCP, read-only) | `CURRENT_SENSE.md` §4 |
| CT step 2 | Auto-zero wire commands via the commissioning page's "Auto-zero" button (or the `POST /api/safety/commissioning/ct_auto_zero` handler directly) | `safetyctcal` NVS record; note in `CT_COMMISSIONING_PLAN.md` step 2 |
| S1/S3/S8/S6b/S10/S11/S13 | `safety_set_rate_guard`/manual commissioning writes for setup, `safety_get_status`/`safety_get_diag`/`safety_get_link_stats` to confirm trip/warn and timing | `GUARD_TEST_MATRIX.md` §3.4 rows, each with date/commit/CRC/pass-fail |
| CT step 6 heating run | Zone current sweep tool (`kiln_find(query="zone current sweep")`) + `control_get_zones`/`get_board_state` | `CT_COMMISSIONING_PLAN.md` step 6, `CURRENT_SENSE.md` |
| Dwell/ramp traces | `get_board_state` at each stage transition | new bench log, cite in `PROJECT_STATUS.md` if notable |
| S9 | Bypass jig + ammeter/scope on K4 coil + `safety_get_status` | `GUARD_TEST_MATRIX.md` §3.4 S9 row — mark **hardware-verified** for the first time if it passes |

**Manual measurements that would benefit from a small script** (read-only,
safe to write): a one-shot poller that logs `safety_get_status` +
`get_board_state` at 1 Hz to a CSV for the duration of stages 1-3, so the
ramp-lock-band/S8/S10/S11 timing windows can be reconstructed after the fact
without babysitting a terminal. Not written yet — flag for a follow-up
session; it touches no relay/firing control, only polls existing read-only
endpoints.

## 4. Cannot be done this session

- **S4 per-channel overcurrent-correlation on channels 0/1** — no per-zone CT
  is fitted; only the summed channel (index 2, GPIO28) has a real signal, and
  `amps_valid[0]`/`[1]` are always false in summed mode. S4's per-channel
  design has no per-relay signal to test against; only S14/S15's
  summed-topology substitutes are testable.
- **True per-zone S14 (per-channel overcurrent) as originally specified** —
  same reason; only the summed-mode branch (channel 2 vs. sum of commanded
  zones' normals) is reachable, which is a coarser check ("one of these
  zones," never "this specific zone").
- **S5's fault-injection cases** (a reading actually outside the
  commissioned type's band, a deliberate `tc_type` mismatch, CR1
  MISMATCH/DEAD_BUS) — only a healthy in-band reading has ever been observed
  live; provoking the rejection paths needs deliberately damaging/miswiring
  the safety MAX31856, out of scope for a routine firing.
- **S1/S13 as "coverage gaps"** — not blocked by missing wiring, only by
  commissioning. S1 is already armed (do it). S13 requires the owner's §0
  commissioning step; if skipped, S13 stays untested this session, by choice
  not by defect.
- **Abort stopwatch** — explicitly out of scope, another workstream's
  destructive script; sequenced last if/when that session runs.

## 5. Estimated bench time

Roughly 3-4 hours: prerequisites and stage 0 (~45 min including a reflash),
heat-up through dwell with S1/S3/S6b/S8/S10/S13 interleaved (~90-120 min,
dominated by S11's mandatory 10+ minute frozen hold and S10's 300 s
disagreement hold), S9's bypass-jig test and cooldown (~30-45 min). CT step 0
and step 2 can happen in parallel with the reflash/prereq window since they
need no heat.
