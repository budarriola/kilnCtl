# virtual_kiln

**A software-level cross-check of KilnFW's real per-zone control/guard code
(one zone) against a simulated kiln. This is NOT hardware verification and
does not run a firing profile.** Read this whole document, especially "What
is explicitly OUT of scope" and "Findings", before trusting a PASS or FAIL
from this harness for anything beyond exactly what it claims.

## What problem this solves

`firmware/SimFW/tools/virtual_simfw/` runs SimFW's real simulation logic and
speaks the real `benchproto` wire protocol over TCP, but has no DUT at all.
`firmware/SimFW/tools/virtual_dut/` closed the *safety*-processor half of
that gap: it compiles SaftyFW's real `safety_guards.c`/`relay_grace.c` for
the host and feeds K4's sensed state back into the fixture.

That still left the *main controller* half open: many `expect` clauses in
`firmware/SimFW/scenarios/*.yaml` assert on KilnFW behavior -- a zone's PID
regulating toward a setpoint, its heater relay (K1/K2/K3) actually closing,
a thermal-protection guard raising a fault. Before this pass, K1/K2/K3 never
closed (`virtual_simfw`'s own README, "no real DUT ... Relay sense
... default to open/not-closed and stay there"), so no scenario whose
trigger or expectation depends on a real relay closing and real heat flowing
could ever pass, and KilnFW's own `thermal_guard.c` never saw a single tick.

`virtual_kiln` closes that gap for **one zone**: it compiles KilnFW's real,
unmodified `pid.c`/`thermal_guard.c`/`heater_output.c`, ticks them against
the fixture's real simulated zone-0 thermocouple at the real 1Hz cadence,
and feeds the resulting relay decision back into `virtual_simfw` as K1's
sensed contact state -- closing the loop the same way `virtual_dut` closes
it for K4.

## The one rule that governs everything here

**Nothing under `firmware/KilnFW/` is modified.** `pid.c`, `thermal_guard.c`,
and `heater_output.c` are compiled byte-for-byte as they exist in the tree.
No gain, threshold, or control-law term is re-derived, approximated, or
"improved" anywhere in this directory. Anything KilnFW does that cannot be
reproduced here because the surrounding code is FreeRTOS/ESP-IDF-shaped and
does not host-compile is left out of scope and documented below -- never
stubbed in with a plausible-looking substitute.

## Phase 1 finding: what in KilnFW/App is genuinely host-portable

Surveyed `firmware/KilnFW/App/drivers/` for the profile executor, the
PID/control code, thermal-protection guards, and the relay-decision path.

| File | `#include`s | FreeRTOS/ESP-IDF/LVGL calls? | Host-portable? |
|---|---|---|---|
| `pid.c`/`pid.h` | `math.h`, `string.h`, `stdbool.h` | None | **Yes** -- already host-tested (`App/test/test_pid.c`) |
| `thermal_guard.c`/`.h` | `math.h`, `stdarg.h`, `stdio.h`, `string.h` | None | **Yes** -- already host-tested (`test_thermal_guard.c`) |
| `heater_output.c`/`.h` | (pure, no ESP includes -- verified via `App/test/build_host_tests.ps1`'s own source list) | None | **Yes** -- already host-tested (`test_heater_output.c`) |
| `thermo_combine.c`/`.h` | `math.h` | None | **Yes** -- already host-tested (`test_thermo_combine.c`) |
| `profile_executor.c`/`.h` | `esp_log.h`, `freertos/FreeRTOS.h`, `freertos/semphr.h`, `freertos/task.h`, plus `kiln_io_owner.h`, `zones_http.h` (HTTP/NVS-backed config), `run_state.h` (NVS), `sim_backend.h`, `safety_link.h` | Yes -- owns the control task, a second watchdog task, a `SemaphoreHandle_t` lock, direct `kiln_io_owner_command_set_relay_mask_authorized()` calls, `ESP_LOGx` throughout | **No** |

This is not a guess: `profile_executor.c`'s own header comment says it
outright ("Module layout (TODO.md 6A.7): this is the only module in the set
that talks to FreeRTOS, kiln_io, relay_authority, or the HTTP layer. The
control math (pid.h), the safety guards (thermal_guard.h), and the
duty-to-relay rendering (heater_output.h) are all pure and live in their own
files specifically so they can be tested without this one.") -- and
`firmware/KilnFW/App/test/build_host_tests.ps1` (a pre-existing harness this
pass did not create) already proves it: it host-compiles `pid.c`,
`thermal_guard.c`, `heater_output.c`, `thermo_combine.c`, `pid_autotune.c`,
`ota_auth.c`, `ota_interlock.c` for MSVC today (288 checks, all green), and
even has a `test_closed_loop.c` that wires `pid.c` + `thermal_guard.c` +
`heater_output.c` + a local thermal model (`sim_plant.c`) together for a
4-hour closed-loop regression -- "the same way `profile_executor.c` wires
the real hardware (minus `relay_authority`/FreeRTOS)", per that test file's
own header comment.

**`profile_executor.c` is not host-portable, and is not made portable by
this pass** (`firmware/KilnFW/**` is read-only for this task regardless).
Its job splits into two genuinely different things:

1. **Per-tick zone regulation** (read a combined thermocouple reading, call
   `pid_update_terms()`, call `heater_output_duty()`, call
   `thermal_guard_tick()`, decide the relay) -- this is *entirely* built
   from the four pure modules above. `virtual_kiln` reproduces this part.
2. **Profile orchestration** (ramp/dwell segment stepping across a firing's
   multiple segments, multi-zone ramp-lock -- "the slowest zone sets the
   pace", model feedforward from an autotune-identified plant, the
   TODO.md 6A.5 load-staggering cap, `relay_authority_zone_blocked()`
   gating, config-reload-while-running, the NVS reboot breadcrumb) -- this
   is `profile_executor.c`-only logic, not extracted into any pure,
   header-exposed function, and not covered by any existing host test.
   **`virtual_kiln` deliberately does NOT reproduce this part.**

Reimplementing (2) by hand in this harness -- guessing at ramp-lock
semantics, feedforward, load-cap tie-breaking -- would be exactly the
"second implementation" the parent task's brief forbids: it would pass its
own tests while telling nobody anything about the real firmware, and would
silently rot out of sync with `profile_executor.c` the next time that file
changes. So this pass takes the `virtual_dut` precedent seriously: compile
the real thing, or document the exclusion. (2) is the documented exclusion.

## What this means for "the realistic prize"

The parent brief floated "the profile executor plus the zone control law
consuming simulated temperatures and producing relay commands" as the
realistic goal. Given the finding above, **only the zone control law half of
that is available** without either modifying KilnFW (forbidden) or
reimplementing profile orchestration by hand (also forbidden, for good
reason). `virtual_kiln` delivers exactly that half, for one zone, against a
setpoint supplied on the command line rather than a ramping/dwelling
profile schedule -- see `--setpoint-c` in `run_kiln_scenarios.py`.

## Architecture

```
 kilnsim (library, read-only)         virtual_kiln (this directory)
 ┌─────────────────────────┐          ┌───────────────────────────────┐
 │ TcpSimLink (benchproto)  │◄────────►│ run_kiln_scenarios.py          │
 │ scenario.py / report.py  │  (sole   │   - arms each scenario         │
 │ runner.py (helpers only) │  TCP     │     (reuses kilnsim.runner's   │
 └─────────────────────────┘  client)  │      private arm/translate     │
                                        │      helpers as a library)     │
                                        │   - polls telemetry + TC_GET_  │
                                        │     REGS(channel=0)            │
                                        │   - decodes MAX31856 tc_c/     │
                                        │     sensor_ok (datasheet math, │
                                        │     not a decision -- see      │
                                        │     _decode_tc_c())            │
                                        │   - batch-ticks kiln_core.exe  │
                                        │   - RELAY_SET_SENSE(K1) back   │
                                        │   - calls evaluate_expectations│
                                        └───────────────┬─────────────────┘
                                                         │ stdio (text lines)
                                                         ▼
                                        ┌───────────────────────────────┐
                                        │ kiln_core.exe                  │
                                        │   main.c (this dir's harness)  │
                                        │ ── real, unmodified: ──        │
                                        │   pid.c                        │
                                        │   thermal_guard.c               │
                                        │   heater_output.c               │
                                        └───────────────────────────────┘
                TCP (benchproto)
 virtual_kiln ──────────────────────────────────────────► virtual_simfw.exe
                                                            (built elsewhere
                                                             in this repo,
                                                             read-only here)
```

This mirrors `virtual_dut`'s architecture deliberately: same 3-process
split, same stdio line protocol for the compiled-core child process, same
`kilnsim`-as-a-library reuse, same `SIMFW_VIRTUAL_CMD_RELAY_SET_SENSE`
mechanism (generalized from K4 to K1). A NEW directory rather than an
extension of `virtual_dut/` because the two now drive genuinely independent
halves of the system (SaftyFW guards vs. KilnFW zone control) against
independent subsets of the fixture's channels/relays, and can eventually run
**concurrently** against the same `virtual_simfw.exe` process (it accepts up
to 4 TCP clients) to close both halves of a scenario at once -- a future
pass's job, not this one's; see "What's out of scope" below.

## Exactly which KilnFW source files are compiled, and which are not

### Compiled verbatim (real, unmodified, control/guard logic)

| File | Why it is safe to host-compile |
|---|---|
| `firmware/KilnFW/App/drivers/pid.c` | Pure function of `(cfg, state, setpoint, measurement, dt, ff) -> duty`. Its own header comment: "Pure C, no FreeRTOS, no ESP-IDF, no logging, no I/O, no globals." Already host-tested (`test_pid.c`, part of `build_host_tests.ps1`'s 288 checks). |
| `firmware/KilnFW/App/drivers/thermal_guard.c` | Pure function of `(cfg, state, input) -> verdict`. Its own header comment: "no FreeRTOS, no ESP-IDF, no logging, no I/O, no relay access." Already host-tested (`test_thermal_guard.c`). |
| `firmware/KilnFW/App/drivers/heater_output.c` | Pure state-in/decision-out, same discipline as the two above (its own header: "never touches kiln_io itself"). Already host-tested (`test_heater_output.c`). |

`thermo_combine.c` is NOT compiled here: with exactly one TC channel per
zone in this single-zone harness, `thermo_combine()`'s averaging logic has
nothing to combine -- see `_decode_tc_c()`'s own comment. A multi-channel
follow-on would need it (see "What's out of scope").

### Explicitly NOT compiled, and why (out of scope, not stubbed)

| File | Why it cannot be host-compiled | What this means for coverage |
|---|---|---|
| `firmware/KilnFW/App/drivers/profile_executor.c` | `#include "esp_log.h"`, `"freertos/FreeRTOS.h"`, `"freertos/semphr.h"`, `"freertos/task.h"`, plus `kiln_io_owner.h`/`zones_http.h`/`run_state.h`/`sim_backend.h`/`safety_link.h` -- the FreeRTOS task that owns the whole control loop, a second watchdog task, and every side-effecting call (relay writes, NVS breadcrumbs, HTTP-backed config reads). Cannot run unmodified on a PC. | `kiln_core/main.c` is a from-scratch, host-only replacement for a NARROW slice of this file's *outer loop* only: the per-tick zone-regulation sequence for ONE zone in PID mode with no feedforward (see `kiln_core/main.c`'s own header comment, "Faithfulness to `profile_executor.c`'s tick loop"). Ramp/dwell segment stepping, multi-zone ramp-lock, feedforward, the load-staggering cap, `relay_authority` gating, and config-reload-while-running are ALL absent -- see "What's out of scope" below. |
| `firmware/KilnFW/App/drivers/relay_authority.c` | Not FreeRTOS-coupled itself (it's pure global/static state), but its `relay_authority_zone_blocked()` gate depends on `safety_link_get_fault_sources()` (an ESP-side link to SaftyFW) and per-run `relay_authority_set_zone_blocked()` calls only `profile_executor.c` makes. Nothing in this single-zone, no-safety-link harness has a fault source to gate on. | K1 in this harness can close even when a real board's `relay_authority` would have refused it (e.g. a SaftyFW-side fault asserted over the link). Documented, not hidden -- see `run_kiln_scenarios.py`'s own comment at the `_send_relay_set_sense()` call site. |
| Everything else under `firmware/KilnFW/App/drivers/` (`kiln_io*.c`, `wifi_prov.c`, `uart_bridge*.c`, `kiln_ui.c`, `ui_page_*.c`, `zones_http.c`, `profiles_http.c`, `run_state.c`, ...) | Not needed by `pid.c`/`thermal_guard.c`/`heater_output.c`'s dependency graph, and/or ESP-IDF/LVGL/FreeRTOS-shaped. | No behavior from these is exercised or claimed. |

## Faithfulness to `profile_executor.c`'s tick loop

`profile_executor.c`'s control task, for a single PID-mode zone with no
identified feedforward model, does (see that file's "Control mode, per
active zone" / "Apply relays + guards, per active zone" sections):

```
duty = sensor_ok ? pid_update_terms(pid_state, pid_cfg, target_c, actual_c, dt_s, /*ff_u=*/0.0f, &terms) : 0.0f
want_relay_on = heater_output_duty(heater_state, heater_cfg, duty, dt_ms)
apply_relay(zi, want_relay_on)   // relay_authority-gated in real fw -- see table above
commanded_duty = relay_commanded_on ? (duty > 0 ? duty : 1.0f) : 0.0f
guard_input = { sensor_ok, measurement_c: raw_c (UNCALIBRATED), setpoint_c: target_c, commanded_duty, dt_s }
thermal_guard_tick(guard_state, guard_cfg, &guard_input)
```

`kiln_core/main.c`'s `do_tick()` reproduces this sequence field-for-field,
including the exact `commanded_duty` expression (`relay_on ? (duty > 0.0f ?
duty : 1.0f) : 0.0f`, copied verbatim from `apply_relay()`'s caller site)
and the "a faulted zone is never ticked again" short-circuit
(`profile_executor.c`'s `if (!active || faulted) continue`). ff_u is
hardcoded to `0.0f` -- not a simplification, but byte-for-byte the real
behavior of any zone that has never been autotuned (`zone_feedforward()`
returns exactly `0.0f` when `!ff_enabled`, and `ff_enabled` starts `false`
for every zone).

**PID gains and guard thresholds are NOT read from any real zone config**
(that is runtime NVS data set by an operator via the dashboard, invisible to
a static host build). `kiln_core`'s defaults are the SAME gains
`test_closed_loop.c` already uses to validate this exact wiring
(`kp=0.01, ki=0.0005, kd=0.05, d_filter_tau_s=30, b=1.0, pid_range_c=50`),
traceable to a real, already-host-tested source rather than invented here.
`kiln_core`'s `CONFIG` command lets a caller override every one of these
before `RESET` if a specific run needs different tuning.

## What's out of scope (honestly, not swept under the rug)

- **No firing profile.** `run_kiln_scenarios.py --setpoint-c N` drives a
  FIXED setpoint for the whole run. Segment ramp rates, dwell times,
  multi-segment schedules (`cone6_fast` or any other named profile) are
  data KilnFW would load from its own profile store -- not present anywhere
  in this repo outside KilnFW's own (read-only, ESP-IDF-shaped) code, and
  not reproduced here.
- **Single zone only** (zone 0 / TC channel 0 / relay K1). Multi-zone
  ramp-lock, cross-zone guard 8, and the load-staggering cap all require
  more than one zone and are `profile_executor.c`-only logic (see above).
- **No feedforward.** No autotune-identified plant model exists in this
  harness; `ff_u` is always `0.0f` (see "Faithfulness" above -- this is
  real current behavior for an un-autotuned zone, not a gap).
- **No `relay_authority` gating.** K1 can close in this harness even when a
  real board's safety-link fault state would have refused it. See the table
  above.
- **Batch-tick timing, same documented approximation `virtual_dut` makes**:
  each telemetry poll batch-ticks `kiln_core.exe` in real 1Hz
  (`PROFILE_EXECUTOR_TICK_MS`) steps to cover the elapsed sim time, replaying
  the same TC reading for every step in one batch, rather than a live
  per-second poll.
- **Two concurrent DUT-halves not yet composed.** `virtual_dut` (SaftyFW) and
  `virtual_kiln` (KilnFW zone 0) each run their own private `virtual_simfw.exe`
  instance today. Running both against ONE shared instance (it supports up
  to 4 clients) so a single scenario run could evaluate both S-guard and
  KilnFW-guard/relay clauses together is a natural follow-on, not attempted
  in this pass (timeboxed).

## Findings

Ran every scenario in `firmware/SimFW/scenarios/` through
`run_kiln_scenarios.py --setpoint-c 1200` (1200 C: comfortably above
`fast_test` preset's ~505 C duty=1.0 steady-state asymptote, documented in
`safety_tc_frozen.yaml`'s own comment, so the zone drives at maximum duty
for the whole run rather than settling into a duty-cycled soak).

* **`baseline_firing`'s `heat_actually_cycles` clause genuinely PASSES for
  the first time.** Before this pass, `virtual_simfw` alone left K1 open for
  the whole run (`virtual_simfw`'s own README: "Relay sense ... default to
  open/not-closed and stay there"), so this clause could only ever SKIP.
  With `virtual_kiln` in the loop, `pid_update_terms()` demands full duty
  (280C below setpoint is outside `pid_range_c`), `heater_output_duty()`
  renders that as relay-on, and the resulting `RELAY_SET_SENSE(K1, closed)`
  produces a REAL `relay_edge{K1, close}` EVT frame from `virtual_simfw`
  itself (not synthesized by this harness -- see `_KilnEdgeTracker`'s own
  comment on why `RELAY_EDGE` is deliberately not fabricated here) within
  the same tick. Verified output:
  ```
  === baseline_firing ===
    verdict: PASS
      [PASS] never_faults: dut:fault_line_asserted never observed before the boundary
      [PASS] never_estops: dut:estop_open never observed before the boundary
      [PASS] never_trips: dut:K4_open never observed before the boundary
      [PASS] heat_actually_cycles: dut:K1_closed observed 0.000s after relay_edge
  ```
* **`welded_ssr_midfire`'s fault trigger (`at_zone_temp: zone 0, 400C
  rising`) still does not fire against this harness, for a different,
  narrower reason than before.** Before this pass it could never fire at
  all (no relay ever closed, so no heat ever flowed). Now heat genuinely
  flows (same mechanism as `heat_actually_cycles` above), but this
  scenario's `estimate_run_duration_s()` budget is 195 simulated seconds --
  calibrated around the SaftyFW-side fault/guard timing windows the
  scenario's `expect` clauses actually check, not around how long an
  open-loop zone takes to climb from ambient (~20C) to 400C against
  `fast_test`'s thermal time constant. This is an honest scenario-duration
  mismatch for a fixed-setpoint, no-profile harness, not a defect in the
  compiled control code: a longer `--poll-interval`/wall budget, or (out of
  scope here) driving the real profile's ramp rate, would let this trigger
  actually fire. Every SaftyFW-side clause in this scenario stays BLOCKED
  for the SAME reason `virtual_dut/README.md` already documents
  (`context_valid` never set true) -- unaffected by, and unrelated to,
  `virtual_kiln`.
* **Every other scenario's KilnFW-independent clauses are unaffected** --
  fault-trigger timing, SaftyFW guard evaluation (correctly not evaluated at
  all here, since this harness runs no `dut_core.exe`), and telemetry-only
  checks behave exactly as `virtual_simfw` alone already produces them.

## Build

Same environment `firmware/KilnFW/App/test/build_host_tests.ps1` and
`firmware/SimFW/tools/virtual_dut/dut_core/build_host.ps1` use (MSVC Build
Tools):

```powershell
cd firmware/SimFW/tools/virtual_kiln/kiln_core
powershell -ExecutionPolicy Bypass -File build_host.ps1
# -> build\kiln_core.exe
```

## Run it

Requires `virtual_simfw.exe` already built
(`firmware/SimFW/tools/virtual_simfw/build_host.ps1`, owned by another pass,
read-only here) and `kiln_core.exe` built as above:

```powershell
python firmware/SimFW/tools/virtual_kiln/run_kiln_scenarios.py `
    firmware/SimFW/scenarios/baseline_firing.yaml --setpoint-c 1200
```

Omit scenario paths to run every scenario in `firmware/SimFW/scenarios/`.
Reports are written to `results/<scenario_name>.json`, same shape
`kilnsim.report.evaluate_expectations()` always produces.

## Regression checks (unaffected by this pass -- verified, not assumed)

This pass added only new files under `firmware/SimFW/tools/virtual_kiln/`
and touched nothing under `firmware/KilnFW/`, `firmware/SaftyFW/`,
`firmware/SimFW/src/`, `firmware/SimFW/tools/virtual_simfw/`, or
`tools/PcTools/src/`. Confirmed green after this pass:

* `firmware/SaftyFW/test/build_host_tests.ps1`: 525/525
* `firmware/SimFW/test/build_host_tests.ps1`: 5016/5016
* `firmware/KilnFW/App/test/build_host_tests.ps1`: 288/288
* `tools/PcTools` pytest: 291 passed, 23 subtests passed
