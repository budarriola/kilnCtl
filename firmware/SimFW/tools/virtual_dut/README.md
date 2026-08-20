# virtual_dut

**A software-level cross-check of SaftyFW's real guard code against a
simulated kiln. This is NOT hardware verification, and it does not replace
bench testing with real SaftyFW/KilnFW boards.** Every finding in this
document describes how the code that will eventually run on the RP2040
behaves in isolation, against synthetic fixture data, in a build that omits
large parts of the real system (no FreeRTOS scheduling jitter, no real SPI
bus, no real relay coils, no ESP link, no watchdog, no flash). Treat a PASS
here as "the guard's decision logic did the right thing given this input,"
never as "the board is safe." Treat a FAIL as a genuine disagreement worth
investigating, per this document's "Findings" section below -- some of
those disagreements are firmware gaps, not fixture or harness bugs.

## What problem this solves

`firmware/SimFW/tools/virtual_simfw/` runs SimFW's real simulation logic
(thermal model, MAX31856 register emulator, fault engine) on a PC and
speaks the real `benchproto` wire protocol over TCP. Its own README is
explicit about the one thing it cannot do: *"No SaftyFW guards exist, so
`guard_warn`/`guard_trip` events ... can never appear."* Roughly half of
every scenario's `expect` clauses in `firmware/SimFW/scenarios/*.yaml`
assert on exactly that DUT-side behavior, and correctly report SKIPPED or
FAIL against `virtual_simfw` alone.

`virtual_dut` closes that gap for the guard-evaluation half: it compiles
SaftyFW's real, pure `safety_guards.c` (and the real GRACE/TRIP relay state
machine, `relay_grace.c`) for the host, ticks them against fixture-derived
data at the real 100ms cadence, and turns their real output into
`kilnsim`-shaped events so `report.py`'s expectation evaluator can genuinely
PASS/FAIL `guard_warn`/`guard_trip`/`K4_open`-style clauses instead of
SKIPPING them.

## The one rule that governs everything here

**Nothing under `firmware/SaftyFW/` is modified.** `safety_guards.c` and
`relay_grace.c` are compiled byte-for-byte as they exist in the tree. No
threshold, timing constant, or trip condition is re-derived, approximated,
or "improved" anywhere in this directory. If a piece of SaftyFW's real
behavior cannot be reproduced here (because the surrounding code is
FreeRTOS/hardware-shaped and does not host-compile), that piece is left out
of scope and documented below -- never stubbed in with a plausible-looking
substitute.

## Architecture

```
 kilnsim (library, read-only)        virtual_dut (this directory)
 ┌─────────────────────────┐         ┌───────────────────────────────┐
 │ TcpSimLink (benchproto)  │◄───────►│ run_dut_scenarios.py           │
 │ scenario.py / report.py  │  (sole  │   - arms each scenario         │
 │ runner.py (helpers only) │  TCP    │     (reuses kilnsim.runner's   │
 └─────────────────────────┘  client) │      private arm/translate     │
                                       │      helpers as a library)     │
                                       │   - polls telemetry + TC_GET_  │
                                       │     REGS(channel=safety)       │
                                       │   - batch-ticks dut_core.exe   │
                                       │   - synthesizes guard_warn/    │
                                       │     guard_trip/K4 Events       │
                                       │   - calls evaluate_expectations│
                                       └───────────────┬─────────────────┘
                                                        │ stdio (text lines)
                                                        ▼
                                       ┌───────────────────────────────┐
                                       │ dut_core.exe                   │
                                       │   main.c (this dir's harness)  │
                                       │   max31856_decode.c (this dir) │
                                       │ ── real, unmodified: ──        │
                                       │   safety_guards.c               │
                                       │   tasks/relay_grace.c          │
                                       └───────────────────────────────┘
                     TCP (benchproto, single client)
 virtual_dut ───────────────────────────────────────────► virtual_simfw.exe
                                                            (unmodified,
                                                             another agent's
                                                             build)
```

Three processes: `virtual_simfw.exe` (the fixture, unmodified, built
elsewhere), `dut_core.exe` (the real guard code behind a tiny stdio
protocol, built in `dut_core/`), and `run_dut_scenarios.py` (the
orchestrator, which is the *only* TCP client of `virtual_simfw.exe` for the
duration of a run -- see "Known limitation: single-client protocol" below
for why kilnsim's own CLI cannot also be connected at the same time).

## Exactly which SaftyFW source files are compiled, and which are not

### Compiled verbatim (real, unmodified, safety-relevant logic)

| File | Why it is safe to host-compile |
|---|---|
| `firmware/SaftyFW/src/safety_guards.c` | Pure function of `(config, input, state) -> verdict`. No `#include` of FreeRTOS/pico-sdk/link headers by design (its own header comment: "no FreeRTOS, no pico-sdk, no logging, no I/O, no time source of its own"). Already host-tested by `firmware/SaftyFW/test/build_host_tests.ps1` (`test_safety_guards.c`, part of that script's ~525 checks). |
| `firmware/SaftyFW/src/tasks/relay_grace.c` | The two pure state-transition functions (`relay_grace_tick`, `relay_trip_transition`) factored out of `relay_owner_task()`'s FreeRTOS loop specifically so they could be host-tested (`relay_grace.h`'s own header comment: "Deliberately free of FreeRTOS/pico-sdk... buildable and testable on the host"). Already covered by `build_host_tests.ps1`'s `test_relay_grace.c`. |

### Explicitly NOT compiled, and why (out of scope, not stubbed)

| File | Why it cannot be host-compiled | What this means for coverage |
|---|---|---|
| `firmware/SaftyFW/src/tasks/safety_core.c` | `#include "FreeRTOS.h"`, `"pico/time.h"`, `thermo_task.h`, `discrete_task.h`, `relay_owner.h`'s task, `reboot_announce.h`, `watchdog_task.h` -- the real FreeRTOS task that owns the safety-core loop and cannot run unmodified on a PC. | `dut_core/main.c` is a from-scratch, host-only replacement for this file's **outer loop only** (build the input struct, call the two library functions, apply the same two relay-state calls `relay_owner_task()` would). It is written to reproduce `safety_core_build_input()`'s exact current field-by-field behavior -- see "Faithfulness to `safety_core_build_input()`" below, including several fields that are honestly always false today because the real function never sets them either. |
| `firmware/SaftyFW/src/tasks/relay_owner.c` | `#include "FreeRTOS.h"`, `"queue.h"`, `"hardware/gpio.h"` -- the GPIO6-owning task itself. | Not needed: its only non-FreeRTOS logic (`relay_grace_tick`/`relay_trip_transition`) is already factored into `relay_grace.c` above, which IS compiled. `relay_owner.c`'s queue/command dispatch is reproduced structurally (not logically -- there is no logic there beyond "call relay_grace.c and set a GPIO") by `dut_core/main.c`. |
| `firmware/SaftyFW/src/max31856.c` | `#include "spi_owner.h"`, `"hardware/gpio.h"` -- real SPI transactions against real hardware. There is no real SPI bus in this fixture at all (`virtual_simfw`'s own README: "no real SPI bytes ever flow"). | `dut_core/max31856_decode.c` reproduces only this file's two **pure, static** fixed-point register-decode functions (`max31856_decode_cj`/`max31856_decode_tc`) and its `TC_INVALIDATING_FAULTS` rule, byte-for-byte, with a header comment explaining exactly why and citing the source. This is register arithmetic (a datasheet fact), not a safety decision -- no threshold, timing rule, or trip condition lives in this file. |
| Everything else under `firmware/SaftyFW/src/` (`thermo_task.c`, `discrete_task.c`, `current_task.c`, `link_task.c`, `config_store*.c`, `update/*`, `bootloader/*`, ...) | Not needed by `safety_guards.c`/`relay_grace.c`'s dependency graph, and/or FreeRTOS/hardware-shaped. | No guard behavior from these is exercised or claimed. |

## Faithfulness to `safety_core_build_input()`

`safety_core.c`'s `safety_core_build_input()` builds one tick's
`safety_guard_input_t` with a C99 designated-initializer struct literal.
**Any field that literal does not name is zero-initialized** -- this is not
a simplification this harness introduces, it is what the real, current
function does. `dut_core/main.c` reproduces the same set, field for field:

**Set from real fixture data every tick:**
- `tc_valid` -- always `true` (this fixture's TC_GET_REGS transaction never
  electrically fails; see `virtual_simfw`'s own "no real SPI bytes ever
  flow" note -- there is no failure mode to report here).
- `tc_c`, `cj_c`, `fault_bits` -- decoded from the safety channel's raw
  16-byte MAX31856 register image (`TC_GET_REGS` channel 3,
  `TC_FAULT_CHANNEL_SAFETY`) via `dut_core/max31856_decode.c`.
- `spi_failed` -- always `false`, same reasoning as `tc_valid`.
- `estop_pressed` -- from the fixture's `estop_open` telemetry field.
- `dt_s` -- fixed `0.1f`, matching `safety_core.c`'s own
  `SAFTYFW_PERIOD_SAFETY_CORE_MS` (100).

**Set to a fixed value, matching `safety_core.c`'s own current code exactly
(not a harness simplification):**
- `heat_commanded = false` -- `safety_core.c`'s own comment: *"no current
  sense yet, Phase 6"*. See "Finding: S11 cannot trip" below.
- `reboot_grace_active = false` -- no `SAFETY_CMD_ANNOUNCE_REBOOT` source
  exists in this fixture (there is no ESP in the loop at all).

**Left at zero because `safety_core_build_input()`'s own struct literal
never names them either** (this is the important one -- see "Finding:
several guards are structurally unreachable in current SaftyFW" below):
`context_valid`, `zone_count`, `max_zone_setpoint_c`,
`nearest_zone_measured_c`, `any_current_present`,
`relay_commanded_recently`, `relay_commanded_continuously`,
`sample_counter_advancing`, `main_fault_asserted`, `link_up`,
`relay_deenergized`.

## Findings

These are the genuine disagreements/gaps this pass turned up, each judged
against the real source, not guessed:

### 1. `link_up` is never set to `true` anywhere in current SaftyFW -- S6b
   (LINK_DEAD) trips unconditionally, in every scenario, around t≈120s

`safety_core_build_input()`'s struct literal never names `link_up`, so it
is `false` from the first tick of every boot (real hardware included, not
just this harness). `safety_guards.c`'s S6b logic accumulates
`s6b_link_down_elapsed_s` every tick `link_up` is false and trips
unconditionally once it reaches `link_dead_hard_s` (default 120.0s),
**regardless of any current presence, regardless of any injected fault**.
This harness reproduces it exactly: every scenario run against
`virtual_dut` shows a `guard_trip {"guard": "S6b"}` at sim-time ≈120-150s
(observed at 147s in `baseline_firing`, the extra ~27s being this harness's
own telemetry-poll granularity, not a guard delay -- see "Known
approximation: batch ticking" below).

**Verdict: this is a real, currently-shipping gap in SaftyFW, not a fixture
or harness artifact.** `link_task.c` (Phase 7, per `safety_core.c`'s own
TODO) does not exist yet, so nothing has ever set `link_up = true`. On real
hardware today, K4 would trip on this unconditional timer ~2 minutes after
every boot even with a perfectly healthy ESP link, because nothing tells
`safety_core` the link exists. This is squarely "exactly the kind of
discovery this whole fixture exists to produce" -- reported here, not
silently worked around by inventing a `link_up = true` in this harness
(which would hide the gap, not verify it).

### 2. K4 is never energized in current SaftyFW -- every "held closed then
   trips" scenario expectation fails from t=0

The only caller of `relay_owner_command_energize()` would be Phase 7's
link_task/GUI, which does not exist yet -- nothing in the current SaftyFW
source tree calls it. `relay_owner_task()` starts in GRACE (K4 never
driven), and even after the GRACE timer expires to ARMED, nothing ever asks
for an energize. **K4 is permanently open from the first tick of every
boot.** This harness reproduces that too (`dut_core/main.c`'s `s_energized`
is set `false` at RESET and never set `true` anywhere in the file, matching
the real absence of a caller).

**Verdict: a real, currently-shipping gap, not a bug in this harness or a
scenario-writing mistake.** Every scenario in `firmware/SimFW/scenarios/`
that asserts `forbid: {dut: K4_open, before: ...}` (i.e., "K4 must be
closed/energized until the fault provokes a trip") genuinely FAILs against
today's SaftyFW for this reason, independent of whatever guard the scenario
is actually trying to exercise. This is not a disagreement about the
*guard's* logic (S1-S13 are all evaluated correctly); it is that the
system this scenario models (a firing in progress, K4 energized, heat
flowing) cannot exist yet in current SaftyFW because the piece that would
energize K4 (Phase 7) has not been built. Scenarios written against
SAFETY_MODEL.md's target behavior are correct to expect K4 closed during a
normal firing; today's code just cannot get there yet.

### 3. S11 (frozen safety reading) cannot trip -- already documented, now
   independently confirmed

`safety_tc_frozen.yaml`'s own `manual_checks` already flags this
(`heat_commanded` hardcoded `false` in `safety_core.c`). This harness
reproduces the same hardcoded `false` and confirms it end-to-end: S11's
`in->tc_valid && in->heat_commanded` gate is never satisfied, so
`safety_guards_tick()` never enters S11's branch regardless of how long a
frozen reading is held. **Confirmed: S11 correctly does not trip** in this
harness, for the same documented reason it cannot trip on real current
firmware.

### 4. S2, S3, S4, S9, S10, S13 are also structurally unreachable today --
   a broader version of the S11 finding

Not previously called out in any scenario's `manual_checks`, but the same
root cause: `context_valid` is never set `true` by
`safety_core_build_input()` (link_task/Phase 7 again), so every
context-gated guard (S2, S3, S4, S10, S13) resets its own accumulator every
tick and never trips or warns, no matter what the fixture does. S9 is
additionally gated on `in->relay_deenergized`, also never set `true`
(nothing computes it; `relay_owner_is_energized()`'s inverse is never
plumbed into `safety_core_build_input()`), so S9's post-trip verification
window never even starts. **This harness reproduces all of it faithfully**
(these fields are left at their real zero/false value, per "Faithfulness to
`safety_core_build_input()`" above) -- scenarios exercising S2/S3/S4/S9/
S10/S13 (`welded_ssr_midfire`, `welded_contactor_s9`, `runaway_zone`,
`main_safety_skew`, `tc_stuck` for S13, ...) correctly SKIP or FAIL their
guard-specific `expect` clauses here, for the same reason S11 does: nothing
in current SaftyFW has wired the context path yet.

### 5. Guards that DO work end-to-end against this harness

S1 (abs_max_temp_c, though `abs_max_temp_c` itself defaults to
"uncommissioned" / never trips, matching `safety_core.c`'s own zero-init
cfg -- see the code comment there), S5 (bad TC read, graduated warn->trip),
S6a (main_fault, though always false here too, no fixture source), S6b
(confirmed above, if anything too eagerly), S7 (E-stop, immediate), S12
(cold-junction over-temperature, graduated warn->trip) are all genuinely
reachable and were observed transitioning correctly during the scenario
run below (S7/S12 need a scenario that actually asserts E-stop or drives CJ
hot; see per-scenario results).

### Corollary of Finding 2: `then: {dut: K4_open, within_s: N}` clauses are
   structurally unobservable once K4 starts open

`report.py`'s `_eval_event_then` matches **edges** (entity/state Events) in
a time window, not a sampled level. Because K4 is already reported "open"
from this harness's very first observation (Finding 2 -- nothing ever
energizes it), there is no *new* K4 edge inside a later
`not_before_s`/`within_s` window even when K4 genuinely is (and remains)
open at that time, having "already" tripped for real. This shows up as
`dut:K4_open not observed within ... after fault_fired` FAILs on several
scenarios (`tc_disconnect_soak`, `tc_stuck`, `spi_flaky_tc_ic`, ...) even
where the underlying guard **did** genuinely fire correctly -- confirmed by
looking at the `guard_trip`/`guard_warn` events directly in each report's
`events` list rather than at the `K4_open` clause's own verdict. For
example, `tc_disconnect_soak`'s `trips_after_blind_grace` clause reports
FAIL, but that run's own event list shows a completely real
`{"guard": "S5"} GUARD_WARN` at t=97s and `GUARD_TRIP` at t=147s --
S5's graduated warn-then-trip behavior worked exactly as designed; the
`K4_open` clause just has nothing new to point to as evidence once K4 was
already open before the run started. **Read each scenario's raw
`guard_trip`/`guard_warn` events, not just its expectation verdicts, to see
what the guard actually did** -- `results/SCENARIO_RESULTS.md` calls this
out per-scenario where it applies.

### 6. `cj_fault.yaml` never actually moves the CJ temperature -- a scenario
   file gap, not a guard or fixture bug

`cj_fault.yaml`'s fault entry has no `params:` key. `kilnsim.scenario`'s
`compile_faults` defaults an omitted `params` to `(0.0, 0.0, 0.0, 0.0)`, so
`virtual_simfw`'s `FT_TC_CJ_FAULT` handler applies a `cj_fault_offset_c` of
**0.0** -- the fault fires (a real `fault_fired` event is emitted, and this
harness correctly confirms S5 never trips on it) but never actually pushes
`cj_c` past `cj_warn_c`/`cj_max_c`, so S12 (the guard the scenario exists to
exercise) never has anything to react to. **Verdict: neither a guard bug
nor a fixture bug -- the scenario file itself needs a `params: [40.0]`
(or similar, enough to clear `cj_warn_c=60C`/`cj_max_c=85C` from the fixed
25C simulated cold junction) to actually drive S12.** Reported here since
`firmware/SimFW/scenarios/` is out of scope for this task to edit.

## Known, documented limitations

### The K4 physical loop is not closed

The task's aspiration was for the DUT's real relay decision to feed back
into the fixture's thermal model (heat responds to the real guard's real
K4 decision). **This is not implemented, and cannot be, without a change to
`virtual_simfw` (explicitly out of scope for this task -- read-only,
another agent may be editing it):**

- `virtual_simfw`'s `RELAY` command group is read-only:
  `RELAY_GET_STATES`/`RELAY_GET_EDGES` only (`firmware/SimFW/src/tasks/
  cmd_ids.h`'s own comment on `SIMFW_CMD_RELAY_*`: no `SET` id is even
  allocated -- "relay sense is read-only from this task's perspective by
  design"). There is no wire command a DUT (real or virtual) could ever
  send to set K4's sensed state.
- Zone heating duty in `virtual_simfw` (`device_tick()`'s `duty[]` array) is
  driven entirely by `FAULT_SCHEDULE` overrides (`WELDED_RELAY`/
  `STUCK_OPEN_RELAY`), never by `d->k1`/`d->k2`/`d->k3`, which are
  themselves never set by any external command either. **Zone heat in this
  fixture, with no DUT and no fault forcing duty, never turns on at all** --
  this is a pre-existing property of `virtual_simfw`, not something this
  pass introduces or could work around.

**Precisely what would be needed to close this loop** (reported per the
task brief, not implemented, since it requires editing `virtual_simfw`):
a new `RELAY` command (e.g. `RELAY_SET_K4_SENSE`) that lets a connected DUT
report K4's actual state, feeding `d->k4` (and ideally `d->k1..d->k3` for
the main-side relays too, once KilnFW/PID has an equivalent virtual DUT) so
`device_tick()`'s existing `relay_mask`-derived `duty[]` logic starts
reflecting commanded reality instead of only fault overrides.

### Known limitation: single-client protocol

`virtual_simfw.exe` accepts exactly one TCP client at a time (its own
`main()`: `listen(listener, 1)`, and its accept loop only polls for a new
connection while `g_client == INVALID_SOCKET` -- a second connection
attempt while one client is active simply never gets accepted). This means
`kilnsim`'s own CLI/MCP surface and `virtual_dut` cannot both be connected
to the same running `virtual_simfw.exe` process at once.

`run_dut_scenarios.py` resolves this by being the *only* client itself: it
launches its own `virtual_simfw.exe` per scenario (same pattern the
existing `tools/PcTools/tests/test_kilnsim_virtual_simfw.py` uses) and
drives the whole conversation -- arming, fault scheduling, telemetry
polling, event translation -- using `kilnsim`'s own library modules
(`kilnsim.link.TcpSimLink`, `kilnsim.scenario`, `kilnsim.report`,
several private helpers from `kilnsim.runner`) rather than a second,
divergent implementation of that logic. Nothing under `tools/PcTools/` is
modified to make this work.

### Known approximation: batch ticking

`safety_core.c` ticks every 100ms of real (or, on real hardware, RTOS)
time. This harness has no independent 100ms clock of its own --
`run_dut_scenarios.py` polls `virtual_simfw`'s telemetry (and the safety
channel's `TC_GET_REGS`) on a wall-clock cadence (`--poll-interval`,
default 0.25s), then sends `dut_core.exe` however many 100ms `TICK`s are
needed to cover the sim-time that elapsed since the last poll, **replaying
the single TC reading fetched at the start of that interval for every tick
in the batch**. This is a documented approximation of this harness's own
polling loop, not a change to `safety_guards.c`'s logic (which still
receives a real dt_s=0.1 every call) -- its practical effect is coarser
time resolution on a *changing* TC reading (e.g. S1's 3-consecutive-reading
streak could, in principle, see the same batched value 3+ times in a row
where real 100ms sampling might have seen it change), and a few seconds of
extra delay before a long timer (S6b's 120s, S11's 600s, S5's 60s) is
observed to complete, purely from `--poll-interval` granularity -- visible
in the S6b finding above (tripped at "147s" sim-time against a 120s
threshold). Lowering `--poll-interval` tightens this at the cost of wall
time per scenario.

### No context path, no current sense -- see Finding 4

Not a limitation of this harness specifically; a faithful reproduction of
current SaftyFW's own incompleteness (Phase 6/7 TODOs). Re-run this suite
once `current_task.c`/`link_task.c` land and `safety_core_build_input()`
starts setting these fields for real.

## Building

```powershell
cd firmware/SimFW/tools/virtual_dut/dut_core
powershell -ExecutionPolicy Bypass -File build_host.ps1   # -> build\dut_core.exe

cd ../../virtual_simfw
powershell -ExecutionPolicy Bypass -File build_host.ps1   # -> build\virtual_simfw.exe (if not already built)
```

## Running

```powershell
python firmware/SimFW/tools/virtual_dut/run_dut_scenarios.py
# or a subset:
python firmware/SimFW/tools/virtual_dut/run_dut_scenarios.py firmware/SimFW/scenarios/estop_midfire.yaml
```

Per-scenario JSON reports (same shape `kilnsim.report.Report.to_dict()`
produces) are written to `results/<scenario>.json`.

## Per-scenario results

See `results/SCENARIO_RESULTS.md` for the full run's output (all 18
scenarios), generated by the run in this pass. Headline pattern, per the
findings above: `guard_trip`/`K4_open`-based expectations now genuinely
evaluate (PASS or FAIL, not SKIP) against every scenario; most currently
FAIL for the reasons in Findings 1-2 above (S6b's unconditional ~120s trip
and K4 never energizing), which are real gaps in current SaftyFW, not
regressions introduced by this harness. Fault-trigger-side expectations
(the ones `virtual_simfw` alone already satisfied, e.g. `fault_fired`
events, `tc:*` faults) are unaffected and continue to pass exactly as
`virtual_simfw`'s own README documents.
