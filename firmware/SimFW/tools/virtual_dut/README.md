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
                                       │   - builds a PUSH_CONTEXT-     │
                                       │     equivalent from telemetry  │
                                       │   - reads CT amps (CT_GET_STATE)│
                                       │   - ticks dut_core.exe ONCE per│
                                       │     observed sample (dt = real │
                                       │     elapsed sim time)          │
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
                                       │   snapshots.h (context_reduce_ │
                                       │     zones/current_any_present) │
                                       └───────────────────────────────┘
                TCP (benchproto, multi-client -- up to 4)
 virtual_dut ───────────────────────────────────────────► virtual_simfw.exe
                                                            (built in this
                                                             repo, RELAY
                                                             group gained a
                                                             virtual-only
                                                             SET_SENSE cmd)
```

Three processes: `virtual_simfw.exe` (the fixture; its RELAY command group
gained one virtual-only extension, `SIMFW_VIRTUAL_CMD_RELAY_SET_SENSE`, see
its own README), `dut_core.exe` (the real guard code behind a tiny stdio
protocol, built in `dut_core/`), and `run_dut_scenarios.py` (the
orchestrator, which drives K4's sensed state back into `virtual_simfw.exe`
every poll -- see "The K4 physical loop IS now closed" below).
`virtual_simfw.exe` now accepts multiple concurrent TCP clients, so
`kilnsim`'s own CLI/MCP surface could in principle watch the same run live;
`run_dut_scenarios.py` still launches and owns a private instance per
scenario for reproducibility (see "Known limitation: single-client
protocol -- RESOLVED" below).

## Exactly which SaftyFW source files are compiled, and which are not

### Compiled verbatim (real, unmodified, safety-relevant logic)

| File | Why it is safe to host-compile |
|---|---|
| `firmware/SaftyFW/src/safety_guards.c` | Pure function of `(config, input, state) -> verdict`. No `#include` of FreeRTOS/pico-sdk/link headers by design (its own header comment: "no FreeRTOS, no pico-sdk, no logging, no I/O, no time source of its own"). Already host-tested by `firmware/SaftyFW/test/build_host_tests.ps1` (`test_safety_guards.c`, part of that script's 549 checks). |
| `firmware/SaftyFW/src/tasks/relay_grace.c` | The two pure state-transition functions (`relay_grace_tick`, `relay_trip_transition`) factored out of `relay_owner_task()`'s FreeRTOS loop specifically so they could be host-tested (`relay_grace.h`'s own header comment: "Deliberately free of FreeRTOS/pico-sdk... buildable and testable on the host"). Already covered by `build_host_tests.ps1`'s `test_relay_grace.c`. |
| `firmware/SaftyFW/src/snapshots.h` | Header-only, `static inline`, no SDK dependency by design (its own doc comment explains why the two reduction helpers live here rather than in `link_frame.c`). `context_reduce_zones()` and `current_any_present()` are the exact functions `safety_core_build_input()` calls, so `dut_core/main.c` calls them too instead of reimplementing them. Already host-tested by `build_host_tests.ps1`'s `test_snapshots.c` (24 checks). |

### Explicitly NOT compiled, and why (out of scope, not stubbed)

| File | Why it cannot be host-compiled | What this means for coverage |
|---|---|---|
| `firmware/SaftyFW/src/tasks/safety_core.c` | `#include "FreeRTOS.h"`, `"pico/time.h"`, `thermo_task.h`, `discrete_task.h`, `relay_owner.h`'s task, `reboot_announce.h`, `watchdog_task.h` -- the real FreeRTOS task that owns the safety-core loop and cannot run unmodified on a PC. | `dut_core/main.c` is a from-scratch, host-only replacement for this file's **outer loop only** (build the input struct, call the two library functions, apply the same two relay-state calls `relay_owner_task()` would). It is written to reproduce `safety_core_build_input()`'s exact current field-by-field behavior -- see "Faithfulness to `safety_core_build_input()`" below, including several fields that are honestly always false today because the real function never sets them either. **It calls the real `context_reduce_zones()`/`current_any_present()` (see `snapshots.h` above) rather than re-deriving them; only the FreeRTOS-shaped glue around them is hand-written here.** |
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

**Derived from real fixture data by the REAL, unmodified SaftyFW helpers**
(rewritten 2026-08-20, after SaftyFW commit `f304392` gave
`safety_core_build_input()` real producers for all of these):
- `context_valid`, `zone_count`, `max_zone_setpoint_c`,
  `nearest_zone_measured_c` -- `run_dut_scenarios.py`'s `_FixtureContext`
  builds a `context_snapshot_t` per poll from `virtual_simfw` telemetry (the
  same physical facts a real ESP's `SAFETY_CMD_PUSH_CONTEXT` carries), and
  `dut_core/main.c` runs the **real** `context_reduce_zones()` from
  `firmware/SaftyFW/src/snapshots.h` over it, behind the same
  `CONTEXT_MAX_AGE_MS` staleness test `safety_core.c` applies. The helper is
  `#include`d and compiled, not copied.
- `any_current_present` -- the real `current_any_present()` from the same
  header, over the fixture's per-zone CT amps, with the same
  `i_present_a` = 2.0 A default substitution `safety_core.c` does.
- `relay_commanded_recently` / `_continuously` -- from the context frame's
  `relay_recent_mask` and a continuous-on-duration measured the way
  `link_task_get_relay_on_continuous_ms()` measures it, compared against
  `correlation_window_s` (150 s) exactly as `safety_core.c` compares it.
- `link_up` -- the telemetry stream's own liveness, standing in for
  `link_task_link_up()`'s "CRC-valid frame within `LINK_UP_RECENCY_MS`".

**Set to a fixed value, matching `safety_core.c`'s own current code exactly
(not a harness simplification):**
- `heat_commanded = false` -- `safety_core.c`'s own comment: *"no current
  sense yet, Phase 6"*. See "Finding: S11 cannot trip" below.
- `sample_counter_advancing = false` -- `safety_core.c` leaves it false on
  purpose: S13 needs a commissioned `borrowed_zone_index` that exists
  nowhere in the codebase. Guessing zone 0 here would invent a commissioning
  decision and hide the gap.
- `reboot_grace_active = false` -- no `SAFETY_CMD_ANNOUNCE_REBOOT` source
  exists in this fixture (there is no ESP in the loop at all).

**Still left at zero because `safety_core_build_input()`'s own struct literal
still does not name them:** `main_fault_asserted`, `relay_deenergized`.

**The one field with no fixture producer at all:** `setpoint_c`. Nothing in
`kilnsim` or `virtual_simfw` carries a zone setpoint -- a scenario's
`dut: {profile: cone6_fast}` names a KilnFW profile that nothing here
executes. It is sent as **NaN**, never a guessed number, so S2's
`tc_c > max_zone_setpoint_c + margin` is false rather than trippable against
an invented ceiling. S2 is reachable on the real target; it is simply not
provokable here.

## Findings

These are the genuine disagreements/gaps this tool turned up, each judged
against the real source, not guessed.

> **Findings 1, 2 and 4 below are HISTORY as of SaftyFW commit `f304392`.**
> They are kept, struck through where wrong, because they are what this
> tool was built to find and they are what the `blocked_on:` annotations in
> `firmware/SimFW/scenarios/*.yaml` were originally written against. Finding
> 0 is the current state.

### 0. What `f304392` changed, and what this fixture can now see

`safety_core_build_input()` gained real producers for `context_valid`,
`zone_count`/`max_zone_setpoint_c`/`nearest_zone_measured_c`,
`any_current_present`, `relay_commanded_recently`/`_continuously` and
`link_up`; `relay_owner_command_energize()` gained its first caller
(`SAFETY_CMD_REQUEST_ENABLE` 0x02 → `safety_core_request_enable()`).

Measured, by re-running all 19 scenarios before and after this directory was
brought up to date with it:

| | Before | After |
|---|---|---|
| Scenarios where S6b's unconditional hard-backstop trip fired | **16 of 19** | **0 of 19** |
| Polls presenting `context_valid = true` to the guards | 0 | **every poll of every scenario** |
| Guards force-reset every tick by `!context_valid` (S2/S3/S4/S10/S13) | all 5 | **none** |
| Expectation verdicts changed | — | **none** (see below) |

**Not one expectation verdict flipped**, and that is itself the finding:
every K4-based clause is still dominated by K4 sitting open from sim-time 0,
and the newly-unblocked guards split three ways —

- **S10 is now genuinely exercised.** `main_safety_skew`'s `s10_stays_quiet`
  (+80 °C skew must stay under `tc_disagreement_c` = 200 °C) was a vacuous
  PASS before and is a real anti-nuisance PASS now.
- **S2 is reachable but not provokable here**: no setpoint producer exists
  in this fixture (see "Faithfulness" above).
- **S3/S4 were reachable but not provokable here** — measured
  `any_current_present` true on 0 of 1 229 polls across the whole suite.
  **This is now fixed; see Finding 7.**
- **S13 stays dormant deliberately** (uncommissioned `borrowed_zone_index`,
  `tc_source` defaulting to `OWN_J7`) — a commissioning gap, same category
  as S1's `abs_max_temp_c`.

> **The `tc_flaky` paragraph immediately below is HISTORY as of Finding 11.**
> The batching it describes ("one batch replays a single TC sample across
> ~2.5 s of sim time ... synthesizes ~25 consecutive bad reads") was removed
> in this pass; its instruction — "either fix the batching or record that this
> scenario needs a finer poll" — was carried out by fixing the batching.

One event-stream change worth reading carefully: `tc_flaky` previously
showed an S6b trip and now shows an **S5 trip** at t≈247 s. That is not a
newly-discovered S5 defect and not a wrong scenario expectation — it is this
tool's own documented batch-ticking approximation. At the default
`--poll-interval 0.25` and `timescale: 10`, one batch replays a single TC
sample across ~2.5 s of sim time, which cannot resolve `tc_flaky`'s 900 ms
bad/900 ms good alternation at all, so a batch that lands in a bad phase
synthesizes ~25 consecutive bad reads and clears S5's 10-read AND 5 s bars.
Re-running the same scenario at `--poll-interval 0.02` makes the trip
disappear (the S5 warns remain — a separate, pre-existing question this pass
did not change). S6b's trip was simply latching first before, masking it.
**Do not lower `--poll-interval` only for `tc_flaky` and call it green;
either fix the batching or record that this scenario needs a finer poll.**

### 1. ~~`link_up` is never set to `true` anywhere in current SaftyFW -- S6b
   (LINK_DEAD) trips unconditionally, in every scenario, around t≈120s~~
   **FIXED by `f304392`** -- kept below as the original finding

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

### 2. K4 is never energized in a `virtual_dut` run -- every "held closed
   then trips" scenario expectation fails from t=0
   **(cause changed by `f304392`; the observable is unchanged)**

`relay_owner_command_energize()` now HAS a caller —
`safety_core_request_enable()`, reached from `link_task.c`'s
`SAFETY_CMD_REQUEST_ENABLE` (0x02) decoder. But that path is driven only by
an explicit operator/PC command (KilnFW's `uart_bridge.c` →
`safety_link_request_enable()`, i.e. PcTools' `safety_request_enable`);
**KilnFW does not request enable automatically when a profile runs**, and no
scenario in `firmware/SimFW/scenarios/` models that operator step. So K4
still sits open for every run. `dut_core.exe` accepts an `ENABLE` command
(mirroring `relay_owner`'s TRIPPED-refuses / GRACE-defers / ARMED-honours
behavior) for whoever writes the first scenario that needs it;
`run_dut_scenarios.py` deliberately never sends one, because deciding *when*
a scenario would issue it is a scenario-library design decision, not
something a harness should invent.

The original finding, now superseded in its cause:

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

### 4. ~~S2, S3, S4, S9, S10, S13 are also structurally unreachable today~~
   **Superseded by Finding 0**: S2/S3/S4/S10 became reachable in `f304392`,
   S13's block turned out to be a commissioning gap rather than a wiring
   gap, and only S9 (`relay_deenergized`) is still unreachable for the
   reason stated here. Original text:

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

### 7. S3 and S4 are provokable now — and two of the things that "blocked"
   them were wrong

The gap Finding 0 recorded ("S3/S4 reachable but not provokable") was real,
but its stated cause was only half right, and one of the reasons given in
the "K4 physical loop" section below was simply **false**. Both are corrected
here, and both were corrected by writing scenarios rather than by changing
any DUT code.

**What was actually missing:** nobody had ever told the fixture to do the two
things a real system does — issue `SAFETY_CMD_REQUEST_ENABLE`, and command a
zone relay on. `run_dut_scenarios.py` now replays a scenario's own
`dut.operator_actions:` list (see that file's module docstring for the
per-action rationale) and three new scenarios use it:

| Scenario | What it proves | Measured |
|---|---|---|
| `enabled_firing_healthy` | S3 and S4 both stay quiet through a healthy, enabled, commanded firing | K4 closes 53.2 s after K1 is commanded (the real 60 s `SAFTYFW_STARTUP_GRACE_MS`), 125 of 564 polls carry real CT current, no `guard_trip`/`guard_warn` at all |
| `stuck_load_no_command_s3` | **S3 genuinely TRIPS** | weld at t=70.2 s → 16 A on CT0 at t=70.8 s with `relay_recent_mask == 0` → `GUARD_TRIP {S3}` at **t=89.0 s** (18.2 s of current, against a 20 s `stuck_on_time_s`) → K4 opens at 89.6 s → current gone at 91.0 s |
| `commanded_no_current_s4` | **S4 genuinely WARNS**, and does not touch K4 | K1 commanded at t≈5 s into a dead element, K4 closed for 323 of 373 polls, `GUARD_WARN {S4}` **152.8 s after K1 closes** (150 s `correlation_window_s`), K4 still closed at end of run |

**The false blocker.** The section below used to claim, as its reason 2, that
`virtual_simfw`'s `device_tick()` "never consults K4 when computing
`duty[]`/`current_a[]`". That is not true, and neither is it true of real
`sim_engine.c`: **both gate every zone's duty on K4**, after the relay-derived
base and after any fault duty override (`sim_engine.c`'s own `k4_closed`
block, ported verbatim into `virtual_simfw.c`). So the loop closure described
below was never a no-op waiting on an unimplemented model — it was the load-
bearing mechanism, and the moment K4 actually closed, current appeared on the
very next telemetry frame (measured: 0.6 s of sim time). Reason 1 of that
section (K4 is never energized) was correct at the time and is what these
scenarios fix.

**The real remaining blocker was a chicken-and-egg, not a missing model:**
current needs K4 closed; K4 closes only on an operator enable that survives
the 60 s startup grace; nothing issued one. `dut_core.exe` had had the
`ENABLE` command ready for exactly this.

### 8. ~~This harness's guard clock is only accurate when each poll covers
   ≥ 100 ms of sim time~~ **FIXED in this pass — see Finding 11.** The
   `max(1, ...)` floor described below no longer exists: one tick per observed
   sample carries the true elapsed `dt_s`, so the guard clock is exact at any
   poll interval and the measurements in the table below are history. Kept
   unedited because it is the measurement that pinned the bias.

Not a DUT finding — a harness one, discovered while validating Finding 7's
trip times, and it inverts the obvious intuition that a finer
`--poll-interval` is always more faithful.

`run_dut_scenarios.py` converts elapsed *sim* time into
`SAFTYFW_PERIOD_SAFETY_CORE_MS` ticks with
`n_steps = max(1, sim_delta_us // 100_000)`. That `max(1, ...)` floor means a
poll covering **less** than 100 ms of sim time still costs the DUT a full
100 ms tick, so the guard clock runs *fast* — every timer fires early, in
proportion to the over-ticking. Measured on `stuck_load_no_command_s3`, whose
two real bars are a 60 s startup grace and a 20 s `stuck_on_time_s`:

| Run | Sim time per poll | K4 armed at | S3 tripped after |
|---|---|---|---|
| `timescale: 2`, `--poll-interval 0.25` (the scenario's own settings) | ~1.0 s | **58.6 s** (true: 60) | **18.2 s** (true: 20) |
| `timescale: 2`, `--poll-interval 0.1` | ~0.4 s | 54.4 s | 16.2 s |
| `--timescale 1 --poll-interval 0.02` | ~0.02 s → floored | **21.7 s** | **7.4 s** |

The last row is not a more careful measurement of the guard; it is the
harness ticking ~5× too fast. **Do not "verify" a suspicious trip by lowering
`--poll-interval` alone** — check that `sim_delta_per_poll` stays at or above
100 ms first (`--trace` prints `steps=` per poll; `steps=1` repeatedly is the
warning sign), and lower `timescale` rather than the poll interval when you
need finer resolution. The residual ~3 % fast bias in the top row comes from
polls where the fixture republished the same sim time and still cost one
tick.

### 9. `virtual_simfw` used to advance its sim clock by timescale² per wall
   second — **FIXED**, and fixing it moved three verdicts

The paragraph that used to sit here reported the defect and left it alone
("whether the squaring is intentional is a `virtual_simfw` question, not one
this directory owns"). It was not intentional. `virtual_simfw.c`'s main loop
scaled its tick *accumulator* by `timescale` and then let each 100 ms tick
advance sim by `100 ms × timescale`, applying the factor twice. Real
`sim_engine_task()` applies it exactly once — it is a
`vTaskDelayUntil(SIMFW_PERIOD_SIM_ENGINE_MS)` loop, i.e. a **real-time**
cadence, whose body advances the sim clock by `period × timescale` — and
`device_tick()` already ported that body verbatim, so the accumulator scaling
was pure duplication. `timescale` now means what
`kilnsim.runner.run_scenario()`'s own docstring always claimed ("real time
actually elapsed is `duration_s / timescale`"): **N sim-seconds per wall
second**. Guarded by
`tools/PcTools/tests/test_kilnsim_virtual_simfw.py::test_timescale_advances_sim_linearly_not_quadratically`.

Two consequences worth knowing before reading any older result:

- **Every run's sim-time span shrank by its own `timescale`.** The wall-clock
  budget (`run_duration / timescale + 5`) did not change, so a `timescale: 10`
  scenario that used to reach ~2 450 sim-seconds now reaches ~245 — which is
  what its `run_duration` estimate always intended. Runs take the same wall
  time; they simply no longer overshoot by 10×.
- **The telemetry broadcast is now `0.5 / timescale` sim-seconds per frame**
  (2 Hz of wall time), not `0.5 / timescale²`. At `timescale: 10` that is one
  frame per 5 sim-seconds — still only ~4 samples across S3's 20 s
  `stuck_on_time_s`, which is why the K4/guard-timing scenarios keep
  `timescale: 2` (one frame per sim-second). The advice in Finding 8 is
  unchanged: lower `timescale`, never `--poll-interval`.

Exactly two pre-existing expectation verdicts moved, and both moved from a
vacuous PASS to an honest non-PASS:

| Scenario | Clause | Before | After | Why |
|---|---|---|---|---|
| `mainfault_tc_disconnect` | `no_early_trip` | PASS | BLOCKED | Vacuous PASS. At timescale 10 the first telemetry frame did not land until sim-time ~45 s — *after* the fault — so K4's from-boot "open" edge was recorded after the clause's boundary. It now lands at ~1 s and the clause sees what was always true (Finding 2's corollary). Annotated `blocked_on:` in the scenario file. |
| `safety_tc_frozen` | `no_early_trip` | PASS | BLOCKED | Identical cause. |

Everything else held its verdict. `tc_flaky` is worth a specific note because
an intermediate build made it *look* like it had moved: its run no longer
overshoots to ~953 sim-seconds (it ends at ~96 s) and its per-poll tick batch
dropped from ~500 ticks to ~50, but `no_warn_storm` still FAILs on S5 warns —
now at t≈51 s and t≈92 s instead of t≈146 s and t≈196 s. Finding 0's warning
therefore stands unchanged: 2 Hz telemetry at `timescale: 10` still cannot
resolve this scenario's 900 ms bad / 900 ms good alternation, so the batch
still replays one TC sample across ~50 guard ticks and manufactures streaks.
This scenario needs `timescale: 1` or below (or a batching fix) before its
verdict means anything. It was deliberately **not** re-tuned here, so a clock
fix and a scenario change could not be conflated.

> **Superseded by Finding 11**: the batching fix is the option that was taken.
> `no_warn_storm` now PASSes, and the same file re-run at `--timescale 0.2`
> (0.1 sim-seconds per frame — the real MAX31856 cadence) stays quiet across
> 453 guard ticks, so the anti-nuisance claim itself is measured, not assumed.

### 10. `welded_ssr_midfire` and `welded_contactor_s9` were dead tests — now
   live, and what is left blocking them is a different, smaller thing

Both files carried a second, independent blocker beside "no operator
actions": an `at_zone_temp: {temp_c: 400}` trigger the run could never reach.
Measured against `virtual_simfw` with these files' own
`zones[0].R_element: 12.0` override, zone 0 needs ~300 sim-seconds of *full
duty* to cross 400 °C (100 °C at ~35 s of heating, 150 °C at ~60 s, 200 °C at
~92 s), on top of the 60 s startup grace before K4 can even permit heat. The
fault never fired, so every clause downstream of it reported "triggering event
never occurred".

Fixed differently in each file, per what each is trying to prove:

- **`welded_ssr_midfire`** keeps a temperature trigger — "the SSR welds
  part-way up the ramp" is its physical story — at a temperature the run
  reaches (150 °C). It now performs the operator enable, commands K1 on at
  t=65, welds at t≈126, and commands K1 **off** at t=135. That last step is
  what makes it distinct from `stuck_load_no_command_s3`: S3's second half is
  a 150 s `correlation_window_s`, and only this scenario exercises it as a
  timing property. Measured: `GUARD_TRIP {S3}` at sim-time **303.8 s**, i.e.
  168 s after the last K1 command (150 s window + ~18 s of the 20 s
  `stuck_on_time_s`, the ~2 s shortfall being Finding 8's batching bias), K4
  opens at 304.4 s, current gone at 304.8 s. Four of its seven clauses now
  genuinely PASS.
- **`welded_contactor_s9`** converts to `at_sim_time: 70` and reuses
  `stuck_load_no_command_s3`'s uncommanded-weld recipe. Nothing about S9
  depends on kiln temperature, and S3 is only the *initiating* trip here, so
  the shortest real trip is the right one. Measured: `GUARD_TRIP {S3}` 18.6 s
  after the weld, K4 opens, and the `welded_contactor` fault genuinely fires
  on that K4-open edge.

**The new finding this turned up:** `contactor_weld_engages_on_k4_open` still
cannot pass, and the reason is neither the scenario nor a guard.
`WELDED_K4_CURRENT_PERSIST` deliberately bypasses the `duty[]`/`current_a[]`
path and drives the CT channel's wave synth directly — real `sim_engine.c`'s
own comment at that call site says so, and `virtual_simfw.c` ports it verbatim
— while telemetry's per-zone `i_amps` field is `snap.zones[].current_a`, the
MODEL value, which K4 gates to zero the moment it opens. On real hardware the
DUT reads the persisted current through its own CT ADC off the synthesized
waveform; this fixture has no CT waveform and no DUT ADC, so **the persisted
half of S9's signature has no observable at all here**. Confirmed by reading
`sim_engine.c`/`telemetry.c`/`virtual_simfw.c`, not inferred from the FAIL.
Annotated `blocked_on:` in the scenario. Note that S9's *other* historical
blocker is gone: `relay_deenergized` is now genuinely produced
(`safety_core.c`'s `.relay_deenergized = !relay_owner_is_energized()`, and
`dut_core/main.c` mirrors it). So S9 is no longer blocked on SaftyFW at all —
what stops it here is only that the fixture has nowhere to put the persisted
current. Finding 4's table row for S9, and the "still dormant" table in
`results/SCENARIO_RESULTS.md`, are stale on that point.

### 11. The batching was manufacturing data, and S9's persisted current had
   no observable — both fixed here

Two fixture defects, both closed in this pass, both regression-covered by
`test_virtual_dut_harness.py` (run it directly: it needs no pytest).

**(a) A tick batch could synthesize consecutive-read streaks the fixture never
observed.** The old rule was `n_steps = max(1, sim_delta_us // 100_000)`, with
the single TC sample fetched at the start of the poll replayed for every tick
in the batch. `safety_guards.c` counts *reads* as well as seconds (S5: 10
consecutive bad reads **and** 5 s; S1: 3 consecutive readings), so at
`timescale: 10` — one telemetry frame per 5 sim-seconds — one bad sample became
~50 consecutive bad reads and cleared a count bar nothing had actually cleared.
That is what `tc_flaky`'s `no_warn_storm` had been failing on since Finding 0.

The `timescale` workaround was rejected on purpose: it leaves the trap armed
for every future scenario. There is also no honest filler for the other N-1
ticks — repeating the sample invents reads, and dropping `tc_valid` invents
*bad* reads — so the batch must simply not tick more than once per observed
sample. The rule now is **one tick per observed sample, `dt_s` = the sim time
that actually elapsed since the previous sample, and no tick at all when the
fixture published nothing new** (`plan_tick_dt_ms()` in
`run_dut_scenarios.py`; `dut_core.exe`'s `TICK` line gained an optional
trailing `<dt_ms>`, defaulting to the real 100 ms period). Every `dt_s` use in
`safety_guards.c` is `+= in->dt_s`, so time-based bars are unaffected to the
microsecond; only the invented reads are gone. Finding 8's opposite-sign bias
(the `max(1, ...)` floor over-ticking short polls and running every guard timer
fast) disappears with the same change — `relay_grace`'s 60 s startup timer is
now driven by accumulated `dt_ms` instead of a call count, so K4 arms at a true
60 s rather than 58.6 s.

**What it costs, said plainly:** a count-based bar can now only be cleared by
real observations, so a scenario whose telemetry is too coarse to resolve its
own stimulus shows a **quiet guard** instead of a manufactured trip. `tc_flaky`
is exactly that case and its `no_warn_storm` verdict must be read with the
caveat below.

**(b) S9's persisted current now has an observable.**
`FT_WELDED_K4_CURRENT_PERSIST` forces the CT channel to MANUAL amps and
bypasses `duty[]`/`current_a[]` (real `sim_engine.c` does the same, by design),
while this harness read per-zone `i_amps` — the MODEL current, which K4 gates
to zero. Current is now read through the fixture's own **real** `CT_GET_STATE`
command (`kilnsim.protocol.CtCmd.GET_STATE`, PROTOCOL.md sec 5.3 — not a new
virtual-only extension), by `read_ct_amps()`, and feeds both consumers: the
DUT's `any_current_present` (through the real `current_any_present()`) and the
`dut: current_present` edges (`telemetry_with_ct_amps()` substitutes the CT
value into the sample handed to `kilnsim.runner`'s edge tracker, which is
read-only library code). In MODEL mode the two numbers are identical, so
nothing else in the suite changes meaning.

One subtlety worth keeping: the substitution applies to **MANUAL channels
only**. A telemetry frame is a snapshot stamped with the sim time it was
published at, while `CT_GET_STATE` is answered live, up to one broadcast
period later — so feeding a live reading into an older frame moves an edge
*earlier* in the recorded stream than the EVT-stream relay edge that caused
it. Measured while building this: substituting unconditionally put
`enabled_firing_healthy`'s current-present edge at 60.8 s against a K4-close
edge at 61.0 s, failing two clauses that had been correctly passing. Taking
the frame's own number whenever it is meaningful, and the CT's only when the
CT carries something the model path cannot express, keeps both properties.

**Measured, whole suite before vs after (22 scenarios, same seeds):
5 PASS / 10 BLOCKED / 7 FAIL → 5 PASS / 11 BLOCKED / 6 FAIL.** Exactly four
expectation verdicts moved, all of them upward, none downward:

| Scenario | Clause | Before | After | Why |
|---|---|---|---|---|
| `tc_flaky` | `no_warn_storm` | **FAIL** | **PASS** | The manufactured streak is gone: with one tick per sample, no S5 warn is produced at all. **Read the caveat below before treating this as proof of the scenario's own claim.** (Scenario verdict FAIL → BLOCKED; its other clause, `no_trip_from_flapping`, is still the pre-existing K4-never-closes BLOCKED.) |
| `welded_contactor_s9` | `s9_escalates` | BLOCKED | **PASS** | S9 genuinely escalates for the first time: S3 trips at t=90.2 s, K4 opens, the welded contactor keeps 20 A on CT0, and `TRIP_INEFFECTIVE_LATCHED` lands **9.2 s after K4 opened** (9.8 s on a re-run — the committed report), against S9's 10 s `trip_verify_s` and one sim-second of telemetry quantization. Its `blocked_on:` is now flagged STALE by `report.py`. |
| `welded_contactor_s9` | `escalation_latched_at_end` | BLOCKED | **PASS** | Follows the clause above; also flagged STALE. |
| `enabled_firing_healthy` | (none) | PASS | PASS | Listed because it moved *during* development and moved back: see the MANUAL-only note above. Its K4 now closes at a true **60.8 s** rather than 58.6 s, which is the `max(1, ...)` bias leaving. |

**`tc_flaky`'s PASS at its own `timescale: 10` is honest but under-resolved —
so it was separately checked at full resolution, and it holds there too.** The
suite run took **18 samples** across ~96 sim-seconds (one frame per 5
sim-seconds), so the guards saw isolated single bad reads, never the *9*
consecutive the scenario's own comment derives from a 900 ms bad phase at the
MAX31856's ~100 ms cadence. That run only proves a coarse sampler no longer
fabricates a streak. Re-running the same file with `--timescale 0.2`
(0.5 / timescale = **0.1 sim-seconds per frame**, i.e. the real conversion
cadence, ~8 min of wall time) presents the scenario's actual stimulus:

    453 guard ticks over 45.2 sim-seconds — **zero** S5 warns, zero trips.

That is the claim the scenario was written to make, measured: a 900 ms bad run
is ~9 consecutive bad reads, one short of `BAD_READ_COUNT_DEFAULT` = 10, and
the good phase resets the streak (`safety_guards.c`: `s5_bad_streak = 0`).
**The clause was not re-tuned in either direction** — the FAIL was removed by
removing the fabrication, not by changing what is asserted. A scenario-library
follow-up worth considering (not made here): drop `tc_flaky`'s `timescale` to
0.2 so its own default run is the meaningful one.

**What did NOT move**, and is worth stating: `welded_contactor_s9`'s
`contactor_weld_engages_on_k4_open` is still BLOCKED, and the persisted
current is now genuinely observable — so its `blocked_on:` reason ("no
observable at all") is stale and has been rewritten in the scenario file. The
real reason is Finding 2's corollary applied to current: `report.py` matches
`then: {dut: current_present}` against an **edge**, and a correctly persisting
current produces none — it was already present before K4 opened (the weld) and
simply never stops. `virtual_simfw` fires the `welded_contactor` fault in the
same `device_tick()` that opens K4, so the CT never even dips. Expressing
"current did not stop" needs a `forbid:`-shaped clause, not a `then:` — a
scenario-grammar question, not a fixture gap.

## Known, documented limitations

### The K4 physical loop IS now closed -- and it changed nothing *at the time*, for one correct reason and one wrong one

> **Superseded by Finding 7.** Reason 1 below was correct and has since been
> fixed by scenarios that issue the operator enable. **Reason 2 below is
> factually wrong** -- `virtual_simfw`'s `device_tick()` *does* gate
> `duty[]`/`current_a[]` on K4, exactly as real `sim_engine.c` does. The text
> is kept unedited beneath so the correction is legible; do not cite reason 2.

A later pass than the one that wrote the finding below closed this gap.
`firmware/SimFW/tools/virtual_simfw/` (owned by that pass, read-only for
*this* one) gained a virtual-only command,
`SIMFW_VIRTUAL_CMD_RELAY_SET_SENSE` (RELAY group, wire id `0xF0` --
deliberately absent from `PROTOCOL.md`/`cmd_ids.h`/`kilnsim.protocol`, see
that directory's README "Known, virtual-only extensions" section), and
`run_dut_scenarios.py` in *this* directory now calls it every poll,
feeding `dut_core.exe`'s real, unmodified `energized` output back as K4's
sensed state (see `_send_relay_set_sense()` and its call site in
`run_dut_scenarios.py`).

**Re-running the full scenario suite through this now-closed loop produced
a byte-for-byte identical set of expectation verdicts** (only harness-timing
fields like `start_time` differ -- see `results/SCENARIO_RESULTS.md`'s "What
changed in this pass" section for the full comparison). Two independent,
separately-verified facts explain why, neither of them a wiring failure:

1. **Finding 2 above still holds, unaffected:** K4 is never energized
   anywhere in current SaftyFW, so the real value this loop now genuinely
   transmits is always the same `false`/open value `virtual_simfw` already
   defaulted K4's sense to. No new edge is ever produced.
2. **A second, previously-unverified finding, now confirmed by reading the
   code:** `virtual_simfw`'s `device_tick()` (a near-verbatim port of
   **real, unmodified** `firmware/SimFW/src/tasks/sim_engine.c`) never
   consults K4 when computing `duty[]`/`current_a[]` -- only K1/K2/K3 gate
   heater duty and CT current, in both the harness and real firmware alike.
   So even on a day K4 *did* close, that alone still would not gate
   simulated heat/current in this fixture's model today (PLAN.md sec 2 loop
   2's "K4 permits" clause is not implemented anywhere in the codebase this
   fixture is built from). Confirming or fixing that is out of scope for
   both this pass and the one that added the RELAY command
   (`firmware/SimFW/src/**` is read-only in both).

Zone heating duty is, as before, driven by relay sense (now genuinely
settable) plus `FAULT_SCHEDULE` overrides (`WELDED_RELAY`/
`STUCK_OPEN_RELAY`) -- `d->k1`/`d->k2`/`d->k3` are simply never set to
`true` by anything in *this* pass's scenario runs (no virtual KilnFW/PID
exists yet to decide when to close them), so zone heat still never turns on
in these particular runs; that absence is unrelated to K4.

### Known limitation: single-client protocol -- RESOLVED

`virtual_simfw.exe` now accepts up to 4 concurrent TCP clients (each with
its own dedup/registration state and its own EVT-ring read cursor -- see
that directory's README). `kilnsim`'s own CLI/MCP surface and `virtual_dut`
CAN now both connect directly to the same running `virtual_simfw.exe`
process. `run_dut_scenarios.py` in this pass still launches and owns its
own private `virtual_simfw.exe` per scenario run (the simplest,
most-reproducible setup for an automated suite, and it still uses
`kilnsim`'s library modules -- `kilnsim.link.TcpSimLink`, `kilnsim.scenario`,
`kilnsim.report`, several private helpers from `kilnsim.runner` -- rather
than a second, divergent implementation of that logic); the multi-client
capability is available for anyone who wants to point `kilnsim`'s CLI/GUI/
MCP surface at the same live process a `virtual_dut` run is using, e.g. to
watch a run interactively while it executes. Nothing under `tools/PcTools/`
was modified to add multi-client support.

### Sampling rule: one guard tick per observed sample — REPLACES batch ticking

> The section that used to sit here described the **batch-ticking
> approximation** (`n_steps = max(1, sim_delta_us // 100ms)`, the same TC
> sample replayed for every tick in the batch). That approximation was
> removed in this pass — it was not a neutral coarsening, it manufactured
> data. See Finding 11 below.

`safety_core.c` ticks every 100 ms of real (or, on real hardware, RTOS)
time and reads a fresh TC sample on each of those ticks. This harness has no
independent 100 ms clock and, more to the point, **no independent 100 ms
sample source**: `run_dut_scenarios.py` observes the fixture at the fixture's
own telemetry rate (~2 Hz of wall time, i.e. `0.5 / timescale` sim-seconds per
frame). The rule that follows from that, and the one implemented now, is:

> **One `TICK` per observed sample, carrying `dt_s` equal to the sim time
> that actually elapsed since the previous observed sample. No tick at all
> when the fixture published nothing new.**

Two invariants come out of it, and they are what make a verdict readable:

- **Tick count == sample count.** No guard ever sees a read the fixture did
  not produce, so a consecutive-read bar (S5's 10, S1's 3) can only be cleared
  by real consecutive observations.
- **Integrated `dt_s` == elapsed sim time, exactly.** Every use of `dt_s` in
  `safety_guards.c` is `state->..._elapsed_s += in->dt_s`, so one tick of 5.0 s
  and fifty ticks of 0.1 s advance every time-based bar identically. The
  `max(1, ...)` floor's "guard clock runs fast" bias (Finding 8) is gone with
  it — it was the same arithmetic seen from the other end.

The cost, stated plainly: a scenario whose telemetry is too coarse to resolve
its own stimulus now shows a **quiet guard** rather than a manufactured trip.
That is a resolution limit made visible instead of hidden, but it is still a
resolution limit — read a quiet count-based guard together with `--trace`'s
`dt=`/`n=` fields (how much sim time one tick carried, and how many samples the
run has taken at all), and lower `timescale` — never `--poll-interval` — when a
scenario needs to see finer structure. Lowering `--poll-interval` below the
fixture's own broadcast rate now buys nothing at all: polls that observe no new
sample simply do not tick.

### Context and current: wired, but the fixture cannot drive all of it

The context/current/link path is now real end to end (Finding 0). What this
fixture still cannot generate on its own:

- **A setpoint.** No producer exists anywhere in `kilnsim`/`virtual_simfw`.
  Sent as NaN. S2 cannot be exercised here.
- ~~**CT current.**~~ and ~~**A commanded zone relay.**~~ **Both resolved --
  see Finding 7.** A scenario's `dut.operator_actions:` list can now issue
  the operator enable (closing K4 after the real 60 s startup grace) and
  command K1..K3 on/off, so heater duty, CT current and
  `relay_commanded_recently`/`_continuously` are all genuinely presentable.
  S3 trips and S4 warns for real in the three scenarios named in Finding 7.
  There is still no PID deciding *when* a zone should be on -- a scenario
  says so explicitly instead -- which is a modelling choice, not a gap.

Read a quiet **S2** in a `virtual_dut` run as "no stimulus", never as
evidence about the guard. A quiet S3/S4 is only meaningful in a scenario that
actually presents their inputs; the `inputs:` line's `any_current_present`
and `K4 energized` counts are there to tell the two cases apart at a glance.

### Keeping this harness honest when `safety_core.c` changes

`dut_core/main.c` is an independent stand-in for
`safety_core_build_input()`, because the real function is FreeRTOS/pico-sdk
shaped. `f304392` is the cautionary case: it changed the real function but
not this file, and the re-run's byte-identical verdicts were briefly read as
"the fix changed nothing" when in fact the fixture was still mirroring the
pre-fix code. Two rules follow:

1. **Compile the real thing wherever it is compilable.** `dut_core/main.c`
   `#include`s `firmware/SaftyFW/src/snapshots.h` and calls the real
   `context_reduce_zones()`/`current_any_present()`. Any new pure helper
   `safety_core_build_input()` gains should go in an SDK-free header so this
   harness can compile it too, rather than being hand-mirrored a second time.
2. **Mirror the FreeRTOS-shaped glue in the same commit** that changes it,
   and say so in that commit's message.

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
# diagnosing a suspicious trip time (see Finding 8 -- check `steps=` first):
python firmware/SimFW/tools/virtual_dut/run_dut_scenarios.py firmware/SimFW/scenarios/stuck_load_no_command_s3.yaml --trace
```

Per-scenario JSON reports (same shape `kilnsim.report.Report.to_dict()`
produces) are written to `results/<scenario>.json`.

## Per-scenario results

See `results/SCENARIO_RESULTS.md` for the full run's output (all 22
scenarios), generated by the run in this pass. Headline pattern, per the
findings above: `guard_trip`/`K4_open`-based expectations now genuinely
evaluate (PASS or FAIL, not SKIP) against every scenario; most currently
FAIL for the reasons in Findings 1-2 above (S6b's unconditional ~120s trip
and K4 never energizing), which are real gaps in current SaftyFW, not
regressions introduced by this harness. Fault-trigger-side expectations
(the ones `virtual_simfw` alone already satisfied, e.g. `fault_fired`
events, `tc:*` faults) are unaffected and continue to pass exactly as
`virtual_simfw`'s own README documents.
