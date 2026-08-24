# virtual_simfw

A host-side program that runs SimFW's **real** simulation logic and speaks
the **real** `benchproto` wire protocol over a plain TCP socket, so `kilnsim`
(`tools/PcTools/src/kilnsim/`) can drive complete scenarios end-to-end on a
PC with no bench hardware attached.

Before this tool existed, `kilnsim run --mock` only ever exercised
`MockSimLink`'s canned replies -- no scenario had ever actually run: the
scenario runner, the fault-trigger logic, the expectation evaluator, and the
report generator had no real device to run against. `virtual_simfw` closes
that gap for everything **firmware/SimFW/src/sim/** owns (the thermal model,
the MAX31856 register machine, the fault engine, the sine synth, the TC
fault-state contract) while staying honest about the one thing it cannot
simulate: **there is no DUT**. See "What this does NOT simulate" below.

## What it is

* `src/virtual_simfw.c` -- a single-threaded Win32 console program that:
  1. Compiles and links `firmware/SimFW/src/sim/*.c` **unmodified** --
     `thermal_model.c`, `max31856_regs.c`, `fault_engine.c`,
     `tc_fault_state.c`, `sine_synth.c`. This is not a reimplementation of
     the simulation; it is the same code the firmware runs, compiled for
     the host, the same way `firmware/SimFW/test/build_host_tests.ps1`
     already proves that code is pure and portable (~4955 host-test checks).
  2. Compiles and links `firmware/CommonFW/src/benchproto_*.c` **unmodified**
     -- the real wire codec (SLIP-style framing, CRC-16/CCITT-FALSE) and
     reliability layer (sequence numbers, retry/dedup, task registration).
     `kilnsim`'s own `benchproto_codec.py` is proven byte-identical against
     this same C library's shared test vectors, so this device and
     `kilnsim`'s encoder are provably speaking the same protocol.
  3. Owns a sim clock with a settable timescale and ticks the thermal
     model + fault engine at 10 Hz of sim time (`device_tick()`, a
     near-verbatim single-threaded port of `sim_engine.c`'s tick order and
     `fault_sched.c`'s "recompute overrides from the active slot set every
     tick" strategy -- FreeRTOS queues/mutexes are simply unnecessary here,
     since there is only one thread of execution).
  4. Implements `sim_snapshot.h`'s reader contract (`sim_snapshot_read`/
     `sim_event_ring_drain`) as the same struct-and-ring shape, single-
     threaded (no seqlock needed for the same reason as above).
  5. Speaks SimFW's own command groups (SYS/MODEL/TC/CT/RELAY/IO/FAULT/EVT,
     `firmware/SimFW/docs/PROTOCOL.md`) over the TCP socket, `cmd_ids.h`
     included directly as the numeric source of truth so this file's
     dispatch can never silently drift from that header.
* `CMakeLists.txt` / `build_host.ps1` -- a standalone build, deliberately
  separate from SimFW's own `../../CMakeLists.txt` (which builds RP2040
  firmware against pico-sdk/FreeRTOS), the same way `../spi_test_master/` is
  kept apart. No pico-sdk, no FreeRTOS -- a plain Win32 console program.

## What it does NOT simulate

* **There is no real DUT.** No KilnFW, no SaftyFW. Relay sense
  (K1/K2/K3/K5/K4) and the ESP-driven `Fault` line default to open/not-
  closed and stay there unless something explicitly reports otherwise --
  because on real hardware nothing but a real relay's physical contact
  could close them, and there is no real relay here. Power-path faults
  (`welded_ssr`, `broken_heater_coil`, ...) still work correctly -- they act
  at the duty-override level exactly as real `fault_sched.c` does, so
  simulated heat/current still flows -- and separately, a **virtual** DUT
  (one with no physical relay coil to close at all, e.g.
  `firmware/SimFW/tools/virtual_dut/`) can now report a relay's sensed state
  through a virtual-only command (see "Known, virtual-only extensions"
  below) -- but the fixture itself never closes a relay on its own, and a
  real DUT still has no wire to do so either: this command exists only to
  stand in for the missing physical wire a real relay coil would use.
* No real SPI bytes ever flow (no DUT to drive CS/SCLK against the emulated
  MAX31856 register machine), so `TC_GET_REGS`/`TC_GET_MASTER_CONFIG`'s
  transaction/error/underrun counters are always 0, and `configured` is
  always `false`. That is the correct, honest answer for "no master has
  ever touched this channel" -- not a bug.
* No SaftyFW guards exist, so `guard_warn`/`guard_trip` events (referenced
  by most of `firmware/SimFW/scenarios/*.yaml`'s `expect` clauses) can never
  appear. See "Which scenarios are DUT-gated" below.
* No PWM/DMA/PIO hardware -- CT channel state (amps/phase/distortion) is
  tracked as plain numbers. **Correction:** real firmware no longer carries a
  bare `TODO(M-D calibration)` identity placeholder for this -- it now calls
  through `src/sim/ct_calibration.{c,h}`'s real per-channel `gain`/`offset`
  fit (`ct_cal_apply()`). The compiled-in default table is still deliberately
  all-uncalibrated, so the *observed* amps == a 0..1 PWM-scale fraction
  behavior this bullet describes is still accurate today, but the mechanism
  producing it is a real (currently-neutral) calibration table, not a
  hardcoded placeholder -- worth knowing if this harness is ever extended to
  exercise a real calibration table.
* No I2C expander hardware -- `IO_SET_DIR`/`IO_WRITE`/`IO_READ` ack but do
  nothing (no scenario in the standard library needs J20/spare-pin coverage
  today).

## Known, documented deviations from real SimFW firmware

* **SYS `RESET_SIM`/`SET_TIMESCALE`/`SET_SEED`/`GET_SIM_STATE` now have real
  handlers on both sides.** `virtual_simfw` implemented all three original
  ids (`RESET_SIM`/`SET_TIMESCALE`/`SET_SEED`) before real firmware did --
  `kilnsim`'s scenario runner needed a way to set the seed/timescale before
  a run and no other path existed yet. A later gap-closure pass gave real
  firmware's `cmd_task.c` its own handlers for all four ids (`PROTOCOL.md`
  sec 4 now lists all four as **implemented**, including the new
  `GET_SIM_STATE` (`0x07`), a read-back getter for whatever `SET_TIMESCALE`/
  `SET_SEED` last set) -- so these are no longer a virtual-device-only
  extension, and `virtual_simfw` gained a matching `GET_SIM_STATE` handler
  in the same pass to close that drift. The one real remaining difference
  is *how* they apply -- see the very next bullet: real firmware's are
  queued to a tick boundary, `virtual_simfw`'s apply synchronously. (History
  note: `tools/PcTools/src/kilnsim/payloads.py`'s encoder for the original
  three was fixed in the pass that first added them to `virtual_simfw` -- it
  used to silently drop `payload["value"]`/`payload["keep_params"]`
  entirely, harmless against real firmware's then-stub, a real bug against
  anything that actually reads them.)
* **Commands apply synchronously on arrival, not queued to a tick
  boundary.** Real firmware's owner tasks all follow a "queue-then-apply-
  next-tick" doctrine (DESIGN_NOTES.md sec 4.5) so a command's effect always lands
  on a clean tick boundary. This harness applies every command directly
  the instant its TCP frame is decoded (there is no second thread to queue
  toward). The practical consequence: the exact sim-time at which a
  scheduled fault first becomes `ARMED` can vary slightly run-to-run with
  ordinary process-scheduling/TCP round-trip jitter in the setup handshake
  (`SET_SEED`/`SET_TIMESCALE`/`LOAD_PRESET`/`FAULT_SCHEDULE`), since the
  device's sim clock keeps free-running the whole time. This is not unique
  to the simplification -- any live, asynchronously-commanded system (real
  hardware over USB CDC included) has the same setup-latency variance. It
  does NOT affect the determinism contract the fixture actually promises
  (DESIGN_NOTES.md 4.2/7.2): once a fault's own evaluation starts, every subsequent
  `EVERY`+jitter re-arm interval is a pure function of the seeded PRNG
  stream, byte-for-byte identical run to run -- see
  `tools/PcTools/tests/test_kilnsim_virtual_simfw.py`'s determinism test
  and its own extensive comment on exactly this distinction (it compares
  the *sequence of intervals between fires*, not their absolute sim-time,
  for precisely this reason). A `RANDOM_IN` *trigger*, by contrast, picks
  its fire time relative to "now" at first evaluation and IS measurably
  affected by this wrinkle -- documented, not swept under the rug.
* **DUT-power relays boot ON here, OFF on real firmware.** `device_init()`
  sets both `dut_power_on` and `dut_power_safety_on` true, so no scenario
  starts against a dead board; real `i2c_owner.c` boots
  `s_dut_power_main_on`/`s_dut_power_safety_on` both false. **This is the
  deviation most likely to matter**, because it changes the *starting
  electrical state* every scenario runs from -- a real-firmware regression
  where DUT power fails to come up at boot could never be caught by a
  `--virtual` run. Both domains deviate identically, so J18/J19 independence
  (PROTOCOL.md sec 5.5) is preserved. Added to this list 2026-08-24: it had
  been explained only in an inline C comment, which is not where someone
  deciding whether to trust a virtual run will look.
* **CJ (cold-junction) temperature is a fixed 25 C.** *Not currently a
  deviation from real firmware* -- `spi_emu_a.c`/`spi_emu_b.c` use the same
  fixed 25 C stand-in for the same reason, so both sides do the identical
  thing today. What it deviates from is the "slow ambient drift"
  DESIGN_NOTES.md sec 3.2 describes for the *eventual* real firmware. Kept
  here rather than deleted because it will become a real divergence the
  moment that drift is implemented on one side only -- but it is listed
  under the wrong heading until then, and a list that mixes "differs from
  shipping code" with "differs from a future plan" is a list people stop
  reading carefully. Nothing in the required scenario set needs CJ drift;
  `cj_fault.yaml` injects an explicit CJ *offset*, which this simplification
  does not affect.
* **Four real commands are unimplemented here and answer `ERR_NOT_IMPL`:**
  SYS `REBOOT_BOOTLOADER` (0x08), SYS `SESSION_RESET` (0x09), SYS
  `GET_TASK_STATS` (0x0A), and IO `BUS_SCAN` (0x0B) -- all four are
  implemented on real firmware (PROTOCOL.md sec 4). They fail loudly
  (`SimLinkError`), never with a canned reply, so nothing silently passes;
  and `GET_TASK_STATS` is already handled gracefully upstream, since
  `selftest._check_task_stack_margins()` detects the `fw_git_hash ==
  "virtual"` sentinel and reports NOT_RUNNABLE without sending it. No
  current scenario or `testmgr` path exercises the other three -- but the
  next person writing a `kilnsim io scan` regression test against
  `--virtual` will hit an undocumented wall, which is why they are listed.
* **No `RELAY_SET_CONTACT_FAULT`** -- matches real firmware/PROTOCOL.md
  exactly (deliberately not allocated; `FAULT_SCHEDULE`'s `welded_relay`/
  `stuck_open_relay` types are the real path either way).
* **`SET_TIMESCALE`'s wire shape was a real protocol mismatch, fixed this
  pass.** This handler used to decode a raw `f32` argument; `PROTOCOL.md`
  sec 4 and real firmware's `cmd_task.c` (`handle_sys_set_timescale()`) both
  document/decode `u32 timescale_x100 LE` (DESIGN_NOTES.md 4.2/5.2's x100 fixed
  point). `tools/PcTools/src/kilnsim/payloads.py`'s encoder had the matching
  bug (also encoding a bare `f32`) -- the two happened to agree with each
  other, so the virtual harness stayed green throughout, but both disagreed
  with real hardware. Fixed on both sides in the same pass; a request now
  reads correctly against either device.
* **`FAULT_FIRE_NOW`/`TC_INJECT_FAULT` now emit a `FAULT_FIRED` ring event on
  both real firmware and this harness -- the gap below is closed, not a
  live divergence.** This section previously documented a real gap: real
  firmware's `fault_sched_fire_now()`/`cmd_task.c` never pushed a ring event
  for an immediately-fired fault, because `fault_engine_fire_now()` used to
  transition a slot straight to `FAULT_STATE_ACTIVE`, bypassing the
  ARMED->ACTIVE transition `fault_engine_tick()` watches for to emit a FIRED
  event -- so this harness had grown its own `push_fault_events_to_ring()`
  workaround to stay observable for `kilnsim`'s scenario suite, a deliberate
  divergence from real firmware's (buggy) behavior at the time. A separate
  pass fixed the underlying gap in `firmware/SimFW/src/sim/fault_engine.c`
  itself: `fault_engine_fire_now()` now only sets `manual_fire_pending`
  (see that function's doc comment), and the very next tick's ordinary
  ARMED-slot handling in `fault_engine_tick()` fires the slot through the
  exact same code path -- and therefore the same FIRED-event emission -- any
  triggered fire already goes through. Real firmware's `cmd_task.c` handlers
  were updated to match (`handle_fault_fire_now()`/`handle_tc_inject_fault()`
  now just call `fault_sched_fire_now()`/schedule, no local event handling).
  This harness's `SIMFW_CMD_FAULT_FIRE_NOW`/`SIMFW_CMD_TC_INJECT_FAULT`
  handlers were updated in the same pass to match: they now call the new
  2-argument `fault_engine_fire_now(eng, slot_id)` and let `device_tick()`'s
  regular `fault_engine_tick()` call pick up the pending fire on the next
  tick, same as real firmware -- `push_fault_events_to_ring()` is gone,
  no longer needed. The one remaining, harmless difference: this harness's
  "next tick" can be reached one poll-interval sooner than real firmware's
  since ticks aren't gated behind a FreeRTOS queue here (see "Commands apply
  synchronously on arrival" above) -- not a parity bug, the same known
  timing-only wrinkle that bullet already documents elsewhere.

## Known, virtual-only extensions

Commands in this section exist **only** on this host harness. They are not
in `PROTOCOL.md`, not in `firmware/SimFW/src/tasks/cmd_ids.h` (read-only for
this project regardless of this pass), and must never be added there --
adding a real command like these to real firmware would misrepresent what
the physical fixture can actually do.

* **`SIMFW_VIRTUAL_CMD_RELAY_SET_SENSE` (RELAY group, wire id `0xF0`).** On
  real hardware the fixture only ever *senses* a relay's physical contact
  (DESIGN_NOTES.md sec 3.4) -- there is no GPIO, no wire, no mechanism by which a
  DUT could ever tell the fixture "I closed this contact"; `cmd_ids.h`'s own
  comment on the RELAY group already makes this explicit ("relay sense is
  read-only from this task's perspective by design"). This command was
  added for exactly one reason: a **virtual** DUT
  (`firmware/SimFW/tools/virtual_dut/`) has no physical relay coil to close
  in the first place, so there is no contact for a real fixture-style sense
  wire to ever pick up. Without some substitute for that missing wire,
  nothing could ever report K4's (or K1/K2/K3/K5's) sensed state on behalf
  of a virtual DUT. `SIMFW_VIRTUAL_CMD_RELAY_SET_SENSE` is that narrowly-
  scoped substitute: `{u8 signal, u8 level}` (`signal` 0..4 = K1/K2/K3/K5/K4,
  same numbering as `RELAY_GET_STATES`'s reply order; 5/FAULT_LINE refused,
  ERR_BAD_ARGS -- that line has no relay coil to represent), reply
  `[status]`. Once told, `device_tick()` treats the value exactly like any
  other sensed contact: same `relay_mask` bit, same `duty[]`/edge-log/
  telemetry path K1/K2/K3/K5 already had. `firmware/SimFW/tools/virtual_dut/
  run_dut_scenarios.py` is this command's one caller today, feeding
  `dut_core.exe`'s real, unmodified `relay_owner_task()`-equivalent
  `energized` decision back as K4's sensed state every poll -- see that
  directory's README for what closing this loop did (and, just as
  importantly, did not) change about scenario results.
  * **Verified, not assumed: K4 does not gate simulated heater current
    today.** `device_tick()`'s `duty[]`/`current_a[]` computation (this
    file, ported near-verbatim from `sim_engine.c`) only reads the K1/K2/K3
    bits of `relay_mask` -- K4 is tracked (folded into `relay_mask`, exposed
    via `RELAY_GET_STATES`, now settable by this command) but never
    consulted when deciding duty or current. Cross-checked against **real,
    unmodified** `firmware/SimFW/src/tasks/sim_engine.c`: its own `duty[]`
    loop uses the identical three-relay `zone_relay_bit[]` array with no K4
    anywhere in the file. So this is a real, pre-existing property of
    SimFW's fixture thermal/current model (real firmware included), not
    something this harness introduced and not something this pass could fix
    (`firmware/SimFW/src/**` is read-only here) -- DESIGN_NOTES.md sec 2 loop 2's
    "heater current appears ... only when the right relays are closed *and*
    K4 permits" is not yet implemented anywhere in the codebase this fixture
    is built from. `firmware/SimFW/tools/virtual_dut/results/
    SCENARIO_RESULTS.md` reports the resulting (zero) scenario-level delta
    from wiring this command up.

## FAULT group: `UNTIL_TRIGGER` two-frame handshake

`FAULT_SCHEDULE`'s `duration_kind == 2` (`UNTIL_TRIGGER`) needs a second,
full nested release trigger that does not fit alongside everything else in
one 128-byte frame (`PROTOCOL.md` sec 5.6). `virtual_simfw` implements the
same two-frame design real firmware's `cmd_task.c` does, mirrored faithfully
(same status codes, same discard rules):

1. `FAULT_SCHEDULE` with `duration_kind == 2` parks `fault_type`/`target`/
   the ARM trigger/`repeat`/`params` into a per-slot pending entry
   (`s_pending_until[slot_id]`, indexed `0..FAULT_ENGINE_MAX_SLOTS-1`) but
   does **not** arm the slot yet. `duration_for_s` is ignored. Reply:
   `[status, u16 slot_id echo]`.
2. `FAULT_SET_UNTIL_TRIGGER` (`0x05`) supplies the release trigger and
   performs the actual `fault_engine_schedule()` call, combining it with the
   fields step 1 parked. `ERR_BAD_ARGS` if nothing is pending for that slot.
   Reply: `[status, u16 slot_id echo]`.

A direct `PERMANENT`/`FOR` `FAULT_SCHEDULE`, or a `FAULT_CANCEL`, on a slot
discards any stale pending entry for it -- same as real firmware. The
underlying `fault_engine.c` (compiled unmodified here) already supported
`UNTIL_TRIGGER` durations natively; this closed the wire-level gap that kept
`virtual_simfw` from ever reaching that code path.

## Build

Same environment `firmware/SimFW/test/build_host_tests.ps1` uses (MSVC Build
Tools, no cmake/ninja required on PATH):

```powershell
cd firmware/SimFW/tools/virtual_simfw
powershell -File build_host.ps1
# -> build\virtual_simfw.exe
```

A `CMakeLists.txt` is also provided (mirrors `../spi_test_master`'s pattern)
for anyone with `cmake`+a generator on PATH:

```powershell
cmake -G Ninja -B build
cmake --build build
```

## Run it standalone

```powershell
.\build\virtual_simfw.exe --port 0 --seed 42
```

Prints `VIRTUAL_SIMFW_LISTENING port=<N> seed=42` on stdout once the
listening socket is up (`--port 0` asks the OS for an ephemeral port --
exactly what the pytest integration test does). Binds to `127.0.0.1` only,
accepts up to `SIMFW_MAX_CLIENTS` (4) concurrent TCP clients -- e.g.
`kilnsim`'s own CLI/MCP surface and `firmware/SimFW/tools/virtual_dut/`
connected directly to the same running process at once. Each client gets
its own `benchproto_link_t` (dedup/task-registration state), its own RX
reassembly buffer, and its own cursor into the shared, globally-sequenced
EVT ring, so `report.py`'s per-client sequence-gap check stays honest no
matter which clients are connected when an event is written (see
`src/virtual_simfw.c`'s block comment above `client_t`'s definition for the
full design rationale, including why a single shared read cursor was
considered and rejected). A connection beyond the 4th is accepted then
immediately closed rather than left to sit in the listen backlog.

## Point `kilnsim` at it

```powershell
kilnsim --virtual 127.0.0.1:<port> state
kilnsim --virtual 127.0.0.1:<port> run ../../scenarios/baseline_firing.yaml
```

`--virtual` (no address) defaults to `127.0.0.1:8765`. Under the hood this
is `tools/PcTools/src/kilnsim/link.py`'s `TcpSimLink` -- a new `SimLink`
implementation alongside `SerialSimLink`/`MockSimLink`, sharing all of the
real request/reply/BROADCAST-demultiplexing logic with `SerialSimLink`
through a new shared base class (`_FramedSimLink`); only the byte-pipe
primitives (open/close/read/write) differ. Every module above `SimLink`
(CLI, MCP server, GUI, scenario runner, report generator) is unchanged --
that was the whole point of the existing `SimLink` abstraction.

Real scenario execution -- arming the fault schedule, waiting for the
estimated run duration while draining/translating the EVT stream, polling
TELEMETRY for the observations the EVT stream can't carry
(`fault_line_asserted`, `estop_open`, current presence), and evaluating
`expect` clauses -- is `tools/PcTools/src/kilnsim/runner.py`'s job, a new
module: neither `mcp_server.run_test_scenario` nor `cli.cmd_run` had a real
wait loop before this pass (both called `read_events(timeout=0.0)`
immediately after arming, correct only against a `MockSimLink`). `cli.py`'s
`run` subcommand now calls into `runner.run_scenario()` for any non-mock
link; `--duration`/`--timescale` let a caller override the estimate.

`tools/PcTools/src/kilnsim/fault_catalog.py` is also new: the scenario-
vocabulary (`type: welded_ssr`, `target: relay:K1`) <-> wire-numeric
(`fault_sched_fault_type_t`, a zone/TC/CT channel index) translation table
that `payloads.py`'s `FAULT_SCHEDULE`/`TC_INJECT_FAULT` encoders now use --
before this pass, `payloads._encode_fault_schedule` fed a bare catalog
*string* straight into `struct.pack("<B", ...)`, which raises; this
translation had never been written.

## Which scenarios are meaningfully DUT-independent vs. DUT-gated

Every scenario's `expect` clauses were run against `virtual_simfw` (see the
task's own verification output). Two honestly different outcomes showed up,
both correct:

* **Fixture-side machinery genuinely exercised, no DUT needed for the
  cause:** any fault whose trigger is `at_sim_time` (or `manual`) fires
  correctly with no DUT at all -- `tc_stuck`, `tc_disconnect_ramp`,
  `cj_fault`, `broken_element` all produce a real `fault_fired` EVT frame
  at the right sim time. This proves the fault-trigger engine, the TC
  register machine, and the thermal model actually work end-to-end.
* **DUT-gated, correctly SKIPPED/FAILED, never a spurious PASS:** every
  `expect` clause that needs a SaftyFW guard (`guard_warn`/`guard_trip`) or
  a real relay actually closing (`K4_open`, `K1_closed`,
  `safety_temp_valid`) cannot be satisfied by the fixture alone --
  `evaluate_expectations` correctly reports these as `SKIPPED` (the
  triggering condition never arose) or `FAIL` (the triggering condition
  fired, but the DUT-side reaction never showed up in time), never `PASS`.
  This is `report.py`'s validity honesty working exactly as designed.
* **A notable sub-case:** `welded_ssr_midfire`'s fault trigger is itself
  `at_zone_temp` (zone 0 reaching 400 C) -- which *also* never happens
  without a DUT, because heat only flows when a relay is sensed closed, and
  nothing closes it. So this scenario's fault never even fires against the
  fixture alone; every one of its `expect` clauses correctly `SKIP`s. This
  is a good illustration of why some scenarios are DUT-gated at a deeper
  level than their `expect` clauses alone suggest.

## Tests

`tools/PcTools/tests/test_kilnsim_virtual_simfw.py` is the standing
regression gate: it builds nothing itself (skips cleanly if
`build\virtual_simfw.exe` doesn't exist yet -- run `build_host.ps1` first),
but spins up the compiled executable on an ephemeral port, drives it with
`TcpSimLink`, and asserts on real scenario reports, including the
determinism check.
