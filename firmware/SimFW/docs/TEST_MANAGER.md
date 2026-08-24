# `kilnsim testmgr` — the bench regression suite

> **Status:** built, unit-tested against fakes, never yet run against real
> hardware · **Last reviewed:** 2026-08-24
> **Keep this file current.** If a command below stops matching
> `tools/PcTools/src/kilnsim/testmgr.py`/`cli.py`, fix this file in the same
> session — `BENCH_RUNBOOK.md`'s own rule.

One command that answers "did this firmware/hardware update break
anything I can check from the bench": hardware presence detection, the
fixture's own health check (`kilnsim selftest`), every SimFW scenario the
attached hardware can actually run, and a guard-coverage report against
`firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`'s S1..S13 list.

This document explains what it does, how to run it, how to read its report,
how to add a scenario the suite will pick up automatically, and — because
this matters as much as the feature itself — the honest limits of what it
can check today.

---

## 1. Run it

```powershell
kilnsim testmgr                 # full suite, real hardware, text report
kilnsim testmgr --quick         # fast subset -- "I just reflashed, is it still sane"
kilnsim testmgr --json          # machine-readable, for CI
kilnsim testmgr --mock          # against MockSimLink, no hardware at all (smoke test)
kilnsim testmgr --port COM11    # explicit fixture port, same convention as every other kilnsim subcommand
```

Exit code, same convention as `kilnsim run`:

| Code | Meaning |
|---|---|
| 0 | Everything that ran, passed. Nothing needed hardware that wasn't there. |
| 1 | A real failure: `kilnsim selftest` failed, a scenario genuinely FAILed, or a scenario ERRORed (couldn't even complete a run). |
| 2 | No real failure, but at least one scenario is BLOCKED on documented, tracked DUT incompleteness (same meaning as `kilnsim run`'s exit 2). |

`--quick` is the primary use case the task this suite was built for asked
for by name: reflash, run `kilnsim testmgr --quick`, get a fast answer.
It runs `kilnsim selftest` plus the scenarios with the smallest *estimated*
run duration (`kilnsim.runner.estimate_run_duration_s`), not a hand-picked
list — so it stays representative as scenarios are added without anyone
having to remember to update a curated subset.

---

## 2. How to read the report

### Hardware presence

```
hardware presence:
  fixture: PRESENT -- SimFW fixture responded to PING on COM11
  SaftyFW: absent -- ESP has no live status from the safety processor (age_ms=4294967295, link_up=False)
  ESP:     absent -- ESP not reachable: ...
  max tier available: 0 (fixture alone)
```

Three tiers, gated in a strict staircase (`HardwarePresence.max_tier`):

| Tier | Needs | What runs |
|---|---|---|
| 0 | SimFW fixture only | `kilnsim selftest`, plus any scenario whose `expect:` clauses only ever reference fixture-native observations (the fixture drives its own E-stop loop and DUT-power relays directly — DESIGN_NOTES.md §3.4/3.5's "the fixture *becomes* the jumper") |
| 1 | fixture + SaftyFW | scenarios that assert on relay sense (K1..K5) or current, since those are SaftyFW-driven |
| 2 | fixture + SaftyFW + ESP (KilnFW) | scenarios that assert on the isolated Fault line, which only the ESP can drive |

**SaftyFW presence is ESP-mediated.** In the shipped protocol there is no
PC-visible link to the safety processor except through the ESP's UART
bridge (`tools/PcTools/src/kilnctrl/mcp_server.py`'s own module docstring:
"the opto-isolated link to the RP2040 safety processor"), so this suite
reports SaftyFW present only when a connected ESP link's own
`SAFETY_CMD_GET_STATUS` shows `link_up=True` with a real age. A standalone
SaftyFW-without-ESP check would need the separate OpenOCD/SWD `debug_*`
tool family instead — this pass does not wire that up. **This is a real,
current limitation, not a placeholder for something the manager pretends to
do**: with the ESP link down, SaftyFW is always reported absent here even
if the Pico itself is healthy and reachable over SWD.

`kilnctrl` (the package that talks to the real boards) is read, never
modified, by this suite — it is owned by a concurrent workstream. If it
isn't importable in a given environment, both the SaftyFW and ESP tiers
report absent with that reason, honestly, rather than raising.

### Scenario outcomes

```
scenarios (full suite):
  [   NOT_RUNNABLE  ] mainfault_esp_asserted (0.0s) -- needs tier 2 (fixture + SaftyFW + ESP (KilnFW)); only tier 0 is available (fixture alone) -- reported NOT_RUNNABLE, not FAIL, ...
  [     PASS        ] baseline_firing (34.2s)
  [     PASS        ] estop_at_boot (12.1s) -- this scenario's guard-evidence clause(s) target event type(s) [...] that kilnsim.runner cannot produce ...
```

Five verdicts:

- **PASS / FAIL / BLOCKED / SKIPPED** — exactly `kilnsim.report`'s own
  verdicts, unchanged; a scenario carrying one of these actually ran.
- **NOT_RUNNABLE** — the scenario needs a hardware tier that isn't
  attached. This is the fix for a real problem found while building this
  suite: every scenario in the library declares a `dut:` block, and running
  one with no SaftyFW/ESP attached makes `kilnsim.report` correctly report a
  genuine **FAIL** ("K4 state never observed; cannot be dut:K4_open at
  end") — a positive assertion failing purely because there is no DUT to
  observe. That FAIL is not wrong on its own terms, but a *suite* that
  reports it as FAIL is lying about what was actually tested, and that is
  exactly how a suite loses trust. `testmgr` classifies each scenario's
  minimum tier from its own `expect:` clauses (see §4) and reports it
  NOT_RUNNABLE instead whenever the attached hardware can't produce that
  observation at all.
- **ERROR** — the run itself couldn't complete (a link error, a scenario
  YAML that failed to load, `SYS/RESET_SIM` refused). Always a real problem
  worth looking at, always counts toward exit code 1.
- **SKIPPED_QUICK** — only in `--quick` mode: this scenario wasn't in the
  fast subset this run. Never counts as a failure.

### The runner-gap caveat — read this before trusting a clean PASS

`kilnsim.protocol.EventType` defines four values — `GUARD_TRIP`,
`GUARD_WARN`, `LINK_UP`, `TRIP_INEFFECTIVE_LATCHED` — as, in its own words,
"kilnsim-local/synthetic only ... nothing in SimFW's own EVT wire stream can
produce them today". `kilnsim.runner` (the module that actually assembles a
scenario's event list against real hardware) only ever synthesizes
`fault_line`/`estop`/`current` edges from TELEMETRY — never these four.
**25 of the 27 shipped scenarios reference one of these types in an
`expect:` clause.** Run one of those scenarios for real via `kilnsim run`
today and that clause's cause event never occurs, so
`evaluate_expectations` reports it **SKIPPED** ("triggering event never
occurred") — which does not fail the run. A report reader sees a clean
overall PASS with no obvious sign that the guard-trip evidence the scenario
exists to produce was never actually gathered.

`testmgr` closes the *reporting* half of this gap (not the underlying one —
see §5): every scenario outcome carries a `has_runner_gap` flag, and any
outcome whose overall verdict is PASS or SKIPPED *and* carries that flag
gets an explicit note in its `detail`. The guard-coverage table (§3) refuses
to credit a guard with "hardware evidence obtained" on the strength of a
runner-gap-flagged scenario alone, however clean its overall verdict looks.

The single-scenario path (`kilnsim run <scenario.yaml>`, the CI-gate entry
point, `kilnsim.cli.cmd_run`) used to have no equivalent notice — a scenario
run that way, outside this suite, printed only the raw JSON report, and a
clean PASS on a runner-gap scenario was indistinguishable from a real one.
It now calls the same `kilnsim.testmgr.classify_scenario`/
`describe_runner_gap` this module uses and prints a `WARNING: ...` line to
stderr under the identical condition (PASS or SKIPPED overall verdict, a
runner-gap clause present) — one function, one wording, shared by both entry
points rather than a second concept that could drift out of sync with this
one.

---

## 3. Guard coverage

```
guard coverage (S1..S13):
  S1: no scenario declares exercises: [S1]
  S2: declared and ran, but every scenario's guard clause is a kilnsim.runner gap -- no usable evidence
       declared in: tc_noise_storm, s2_setpoint_overshoot
  S3: hardware evidence obtained this session (a scenario ran clean, no runner gap)
       declared in: runaway_zone, welded_contactor_s9, welded_ssr_midfire, stuck_load_no_command_s3, enabled_firing_healthy
  ...
```

This is the report the task cares about more than any raw pass count: **not
"how many scenarios passed" but "which guards have hardware evidence"** —
`firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`'s own central claim is that a
guard that has only ever passed a host test is not commissioned; the
hardware tests prove the wiring. Five possible statuses per guard, most to
least informative:

1. **"hardware evidence obtained this session"** — a scenario that
   declares this guard in its `exercises:` ran with an overall PASS and no
   runner-gap flag. The only status that means real, fresh evidence.
2. **"declared and ran, but did not produce a clean PASS this session"** —
   ran, but FAILed/BLOCKED/wasn't the clean case above. Look at the
   scenario's own report.
3. **"declared and ran, but every scenario's guard clause is a kilnsim.runner
   gap — no usable evidence"** — see §2's runner-gap caveat. The scenario(s)
   ran and reported PASS, but that PASS is vacuous for this guard.
4. **"declared, but hardware tier unavailable this session"** — the
   scenario(s) exist and would exercise this guard, but nothing attached
   could run them. Not this suite's fault; attach more hardware.
5. **"no scenario declares exercises: [<guard>]"** — a real library gap.
   `GUARD_TEST_MATRIX.md` §5 already tracks this for S11's history; check
   there before assuming it's new.

---

## 4. How scenario-tier classification works

`kilnsim.testmgr.classify_scenario(scenario)` walks every `expect:` clause
generically (any `dut:`/`event:` key found anywhere in the clause's raw
dict, not a hand-maintained parse of each clause shape) and looks up the
minimum hardware tier each reference implies:

| Reference | Tier | Why |
|---|---|---|
| `dut: estop_*` / `dut: dut_power_*` | 0 | The fixture drives these itself |
| `dut: K1_*` .. `dut: K5_*` / `dut: current_*` | 1 | SaftyFW-driven (relay_owner + the wetting circuit; current only flows because a SaftyFW-driven relay closed) |
| `dut: fault_line_*` | 2 | Only the ESP can assert the isolated Fault output |
| `event: {type: guard_trip / guard_warn / link_up / trip_ineffective_latched, ...}` | 1, **and flagged as a runner gap** | Conceptually a SaftyFW-only observation, but see §2 — `kilnsim.runner` cannot produce it at all today |
| any other real wire `event:` type (relay_edge, fault_fired, ...) | 0 | Fixture-native |
| an unrecognized `dut:` entity | 1 (conservative default) | Safer to over-require hardware than under-require it |
| `dut.operator_actions` containing `request_enable` | 1 | See §5 |

A scenario's overall `min_tier` is the max across every reference found.

---

## 5. Known-state reset between scenarios

`kilnsim.runner.run_scenario` already sets seed and timescale fresh on
every call (its own first two commands) — a later scenario cannot silently
inherit a prior one's `timescale`/`seed` *through this suite*, because every
scenario is run through that function with its own values, never a partial
hand-rolled sequence. What that function does **not** do is clear a fault
schedule a *previous* scenario left armed (it only ever adds fault slots).
`testmgr` closes that the rest of the way: before every scenario, it sends
`SYS/RESET_SIM` — the same call `kilnsim.selftest`'s own determinism probe
already uses for exactly this purpose. A `RESET_SIM` refusal is reported as
an ERROR outcome for that scenario, not silently swallowed.

---

## 6. Acting as the operator

The task explicitly allows the test manager to act as the operator
(starting firings, requesting enable) — the fixture itself must not, since
its scope is the kiln's physical stimulus only. `testmgr` does exactly one
thing along these lines: for a scenario whose `dut.operator_actions:` list
contains a `request_enable` action, it sends one
`SAFETY_CMD_REQUEST_ENABLE(True)` over a fresh `kilnctrl.serial_link.UartLink`
immediately before the run starts.

**What it deliberately does NOT do**: replay *timed* `operator_actions`
(e.g. `command_relay` at a specific `at_sim_time`). Doing that correctly
means scheduling real commands against the fixture's own sim-time/wall-time
relationship *while* `kilnsim.runner.run_scenario`'s wait loop is running —
a real extension to that module, not something to bolt on here. A scenario
whose `operator_actions` include a timed relay command gets an explicit note
in its outcome's `detail` saying so, so this gap is visible per-run rather
than silently producing a scenario that never reaches the state its
`expect:` clauses assume.

---

## 7. Adding a scenario

Nothing to register. `kilnsim testmgr` discovers every `*.yaml` in
`firmware/SimFW/scenarios/` (`kilnsim.testmgr.discover_scenarios`) each run.
Write the scenario per `DESIGN_NOTES.md` §8.1's schema as usual; it is
picked up automatically, classified into a tier from its own `expect:`
clauses (§4), and its `exercises:` tags feed the guard-coverage table (§3).
A scenario file that fails to load becomes its own `ERROR` outcome in the
report (never silently dropped) so a YAML typo is visible in the same
report as everything else.

---

## 8. What this suite has actually been run against

**Never real hardware, as of this writing.** It is built and unit-tested
(`tools/PcTools/tests/test_kilnsim_testmgr.py`) entirely against
`MockSimLink` and injected fake presence probes — no real
`SerialSimLink`/`kilnctrl.serial_link.UartLink` connection has been opened
by this code. The presence-detection functions
(`default_fixture_probe`, `default_esp_and_saftyfw_probe`,
`make_request_enable_fn`) are real, not stubs, and are designed to be run
for real on the bench — but "designed to" is not "verified against a real
board", and this file will say so honestly until that changes. Whoever
first runs `kilnsim testmgr` against real hardware should update this
section with what actually happened, the same discipline
`BENCH_RUNBOOK.md` §6 already asks for every bench session.

---

## 9. Known limitations, summarized

- **The runner gap is closed in `kilnsim.runner`, but `testmgr` does not
  attach an observer yet** (updated 2026-08-24, `6f1cbbf`).
  `kilnsim/guard_observer.py` now polls a `kilnctrl` SafetyClient over a
  second link and edge-detects GUARD_TRIP/GUARD_WARN/LINK_UP/
  TRIP_INEFFECTIVE_LATCHED, and `run_scenario(..., guard_observer=...)`
  merges them into the event stream. When **no** observer is passed — which
  is still every call this suite makes — guard-typed expectations are
  reported `BLOCKED` with an explicit reason rather than passing vacuously,
  so the honesty property below is unchanged and is now enforced in the
  runner itself rather than only surfaced by this suite. **What remains:**
  wiring the observer into `testmgr`'s own runs, and confirming the
  SafetyClient polling shape against a real board — it has only been
  exercised against fakes. Note also that `GUARD_WARN` carries
  `guard: None` on purpose: `link_task.c` says `warn_mask` has no per-guard
  identity in this build, so there is nothing honest to map it to.
  For contrast, `virtual_dut`'s separate approach covers the
  host-simulated case (`firmware/SimFW/tools/virtual_dut/dut_core` runs the
  real, unmodified `safety_guards.c`/`relay_grace.c` against a simulated
  fixture on the host — a genuinely different, non-hardware kind of
  evidence from what a real-hardware `kilnsim` run would produce; see
  `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`'s "host-software evidence,
  not hardware evidence" note above its per-guard provocation table). That
  module still cannot simply be imported here — it drives a
  separately-compiled `dut_core.exe` and virtual-only `virtual_simfw.exe`
  wire extensions with no equivalent against a real SaftyFW board — which is
  why the observer takes the other route, reading a real board's guard state
  through `kilnctrl` rather than recompiling the guards.
- **SaftyFW-without-ESP presence detection is not implemented** — would
  need the OpenOCD/SWD `debug_*` path. Reported as absent whenever the ESP
  link is down, even if the Pico itself is fine.
- **Timed `operator_actions` are not replayed** — see §6.
- **`--virtual` (TCP link to `virtual_simfw`) is not wired into `testmgr`**
  in this pass, only into the per-command `kilnsim` subcommands. Adding it
  is straightforward (the same `SimLink` interface) if a CI environment
  without real hardware wants to run this suite against the virtual
  fixture instead of `--mock`.
