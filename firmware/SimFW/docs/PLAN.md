# SimFW — Kiln Simulator / Unit-Test Fixture Plan

> **Status:** software complete, hardware-gated · **Last reviewed:** 2026-08-21
>
> **See [section 0](#0-whats-left--index) for the short checklist of what
> remains.** Sections 10–15 carry per-item checkboxes.
>
> **This is the live working plan — only what is still undecided or
> unfinished.** Settled architecture, finished work, and superseded-decision
> reasoning worth keeping now live in `docs/DESIGN_NOTES.md`. If you're
> looking for *why* something is built the way it is, or the history behind
> a decision, that's the file — this one should only ever describe what's
> left to do.

**What is actually true right now.** Every task and every `src/sim/` module
in this plan's architecture is implemented — no stubs remain anywhere in
`firmware/SimFW/src/` — and the standard 27-scenario test library (§8 of
`DESIGN_NOTES.md`) exists as real YAML. That is **build-verified and
host-test-verified**, not hardware-verified, and the two are not the same
thing:

- **Build-verified:** `firmware/SimFW` builds clean under the real
  arm-none-eabi-gcc/pico-sdk/FreeRTOS-SMP toolchain, `-Wall -Wextra -Werror`.
- **Host-test-verified:** the pure `src/sim/` modules pass their MSVC/CMake
  host-test suite (4873/4873 checks at the landing commit `c891b72`); the
  `CommonFW` `benchproto` protocol library passes its own host tests (18/18)
  alongside the pre-existing `kilnlink` suite; `kilnsim` has its own pytest
  suite (59 passed + 17 subtests) and `kilnctrl`'s suite (104 passed) is
  confirmed untouched; all 27 scenario YAML files load cleanly through
  `kilnsim`'s loader.
- **Genuinely hardware-gated — nothing below has ever touched real
  silicon:** the PIO SPI slave timing proof at 4 MHz (M-A's exit criterion —
  no Saleae capture exists, no fixture hardware has ever been built or
  connected to a bench ESP32/Pico); the `UnitTestFw` decommission (§12 —
  gated on SimFW's replacement link being *proven on real hardware*, so
  `firmware/UnitTestFw/` still exists in the tree, untouched); the CT
  calibration procedure (§10 M-D — `wave_owner.c`'s amplitude mapping is
  currently an IDENTITY placeholder, explicitly marked
  `TODO(M-D calibration)`, not the real sweep-and-fit table); every
  relay-sense, E-stop, DUT-power, and ground-isolation claim in
  `DESIGN_NOTES.md` §3; and every one of the 27 scenarios actually *running*
  against a real `KilnFW`+`SaftyFW` pair (a scenario existing and loading is
  not the same as it having ever executed against hardware — see §10's
  milestone table).

**A fourth, software-only verification layer exists** —
`firmware/SimFW/tools/virtual_simfw/` + `virtual_dut/` — that runs real
`SimFW` simulation code and real `SaftyFW` guard code against each other on a
PC, with no RP2040 attached. It is not a substitute for §10's hardware-gated
milestones and doesn't change any milestone's status. It did establish a real
finding about `SaftyFW` guard reachability, since acted on — see
`DESIGN_NOTES.md` §§10–11 for the tool description and the full provenance,
and `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md` §6 for the live,
current reachability table. **The one fact that still matters for future
work:** S1 and S13 are the only guards that don't yet trip, and both are
deliberate *commissioning* gaps (uncommissioned `abs_max_temp_c`; no
`borrowed_zone_index` field exists), not wiring gaps — every other guard's
input is produced.

---

## 0. What's left — index

Checkbox legend used throughout this document: `[x]` done and verified,
`[~]` partially done (the text says which half), `[ ]` not started or not
met. **Done means verified at the level the item itself demands** — for a
milestone whose exit criterion names hardware, host tests passing is `[~]`,
never `[x]`.

### 0.1 Doable now, in software (no hardware required)

- [ ] **Bridge ACK still precedes dispatch — narrower than it was.** Every
      unimplemented subcommand's silent-ACK case is now fixed (`c91ed50`: all
      11 `default:` branches reply `ok=0` with the echoed subcmd). **Still
      open:** the transport ACKs on inbox delivery, before dispatch, so other
      failure paths (truncated args, range refusals, ownership refusals) stay
      silent by documented design; `tools/PcTools/src/kilnctrl` still only
      checks the transport ACK and would need updating to benefit.

### 0.2 Hardware-gated (nothing here can progress without the fixture)

- [ ] **M-A SPI slave timing proof** — Saleae capture, ≥10k transactions,
      zero underruns. *The single biggest unretired risk in the plan.*
- [ ] **CT calibration against `SaftyFW`'s real ADC readback** (M-D). The
      ~3:1 transformer ratio behind it is still an unmeasured estimate, but
      `docs/BENCH_RUNBOOK.md` step 4 now flags it as such explicitly
      (`9c90d7b`) rather than leaving it only as an open question.
- [ ] **J7 pin 1 continuity check** — the two main-board docs contradict each
      other; getting it wrong back-feeds a rail or leaves the isolator side
      unpowered. (§11 item 9)
- [ ] **DUT-power inrush measurement** — the ~60 A / ~190 µs figure rests on
      an *assumed* source resistance; still unmeasured, but now gated by an
      explicit step in `docs/BENCH_RUNBOOK.md` rather than only a to-do
      here (`9c90d7b`). (§11 item 5)
- [ ] **Verify every provisional GPIO assignment** in `HARDWARE.md`
- [ ] **`UnitTestFw` decommission** — gated on proving the replacement link on
      real hardware. (§12)
- [ ] **Fixture hardware form factor** — breadboard vs a real
      `hardware/SimFixture/` board. Not answerable until M-E. (§11 item 6)

### 0.3 Done

All finished work has been moved into `DESIGN_NOTES.md` (see its table of
contents) rather than kept here — this plan only tracks what's left.

- [x] S6a's last fixture-emulation gap closed — real KilnFW guard-6 now
      drives `fault_line_asserted` in `virtual_dut`, non-vacuously
      (`94f2fc3`); every fixture-emulation and harness gap in the 27-scenario
      suite is closed — `DESIGN_NOTES.md` §10
- [x] **Fixture sysclk decided: stays at stock 125 MHz.** No overclock to
      200 MHz — `DESIGN_NOTES.md` §3.2.1 and §13.
- [x] **`kilnsim` will never gain a SAFETY command group; `REQUEST_ENABLE`
      stays an operator action.** Resolved by the SimFW scope boundary
      (physical-only: thermocouples, board I/O, relays, E-stop — never the
      UI, never a safety command) recorded in `DESIGN_NOTES.md` §1/§13.

---

## 10. Milestones

Ordered by dependency and risk; each states its exit criterion — the thing
that must be *demonstrated*, not just built. **Software for every milestone
M-A through M-H has been written** (skeleton, protocol, thermal model, TC
emulation, CT synthesis, relay/IO, fault engine, MCP/CLI/GUI, and the
27-scenario library all exist in the tree). What follows is honest about
which exit criteria that satisfies and which it does not — build-verified
and host-tested is not hardware-verified, and for this fixture almost every
exit criterion as originally written specifically demands hardware evidence.
None of the milestone statuses below change because of `virtual_simfw`/
`virtual_dut` (`DESIGN_NOTES.md` §10) — those tools run real code against a
simulated fixture on a PC, which is neither "built" nor "hardware-verified"
in this table's sense. Where it matters (M-G/M-H, which talk about scenarios
"running"), that distinction is called out explicitly.

- [ ] **M-A — SPI slave proof of concept.** PIO MAX31856 emulation, one
  channel, against a real master (bench ESP32 running unmodified `KilnFW`
  driver code, or `tools/spi_test_master/`'s scripted test master first).
  *This is the risk item; it goes first, before any framework code.*
  **Exit:** Saleae capture showing correct mode-1 multi-byte reads at the
  master's real clock with zero TX underruns over ≥10k transactions; the
  driver access-pattern audit (open question 11.4) is answered in writing —
  **it now is** (`DESIGN_NOTES.md` §3.2.1) — but the Saleae half is not.
  **Status: NOT MET.** The PIO engine is written and its mode-1 clocking bug
  is fixed (`DESIGN_NOTES.md` §14) — but no fixture hardware has ever been
  built, so no real master has ever clocked it and no Saleae capture exists.
  This remains the single biggest unproven risk in the whole plan;
  software completeness elsewhere does not retire it.
- [~] **M-B — Protocol lift + skeleton + USB.** Protocol core extracted from
  `UnitTestFw` into `CommonFW` (pure, host-tested, spec doc moved); FreeRTOS
  task skeleton; CDC link speaking the extracted protocol; PING/VERSION/
  GET_CAPS; fresh `kilnsim` CLI talking to it.
  **Exit:** CLI round-trips against real hardware; CommonFW host tests green
  under MSVC; **`UnitTestFw` decommission executed (§12)**.
  **Status: PARTIALLY MET.** Extraction done (`benchproto` in `CommonFW`,
  spec in `BENCHPROTO.md`), CommonFW host tests green (18/18), task skeleton
  has real bodies throughout. **Not met:** no fixture exists to round-trip
  against — only host-side wire-format simulation has been exercised.
  **Not met:** the `UnitTestFw` decommission — §12 step 2 ("prove the
  replacement on real hardware") is exactly the hardware gate above, so
  `firmware/UnitTestFw/` and `hardware/UnitTestFixture/` are both still in
  the tree, untouched.
- [~] **M-C — Thermal model + TC emulation wired.** 3+1 channels, model-driven
  temps, presets, time-scale, MODEL/MANUAL modes.
  **Exit:** host-test suite green incl. golden traces; on the bench, real
  `KilnFW` displays a plausible warming curve driven entirely by the model;
  `TC_GET_MASTER_CONFIG` shows the DUT's real register writes.
  **Status: HOST-TEST MET, HARDWARE NOT MET.** `thermal_model.c` and
  `max31856_regs.c` are implemented and host-tested with golden traces. No
  board has ever been connected to this fixture.
- [~] **M-D — CT synthesis.** 3x 60 Hz with amplitude tracking, transformer
  coupling network built.
  **Exit:** calibration table fitted against `SaftyFW`'s own ADC readback;
  commanded 0→N A sweep reads back within ±5% over the usable range;
  one-relay/one-channel commissioning check passes.
  **Status: NOT MET, further from met than the others.** The waveform
  generates in software and passes host tests. Both halves of the
  calibration *machinery* exist and are tested (PC-side sweep/fit/crosstalk
  runner in `tools/ct_calibration/`; firmware apply-path in
  `src/sim/ct_calibration.{c,h}`) — see `DESIGN_NOTES.md` §3.3. **What
  remains is entirely hardware-gated:** a real bench run against real
  CT/transformer/ADC hardware to produce the first JSON table, then
  regenerating the header from it. The shipped table is an explicit "no
  data" marker, not placeholder constants. No transformer coupling network
  has been built. "Store the table in fixture flash keyed by channel" also
  waits on a SimFW `config_store`, which does not exist.
- [ ] **M-E — Relay sense + discrete I/O.** Expanders, E-stop, fault line, DUT
  power switch.
  **Exit:** heat loop closes end-to-end — DUT PID actually regulates a
  simulated zone through relay cycling with no fixture intervention;
  `kilnsim power cycle` reboots the DUT and telemetry shows it.
  **Status: NOT MET.** Drivers are host- and build-verified, and
  `docs/HARDWARE.md` has reconciled their pin claims against the other
  driver files with zero collisions found. The closed-loop,
  DUT-power-cycle, and E-stop exit behaviors are all bench-only
  demonstrations that have not been attempted — no fixture hardware exists.
- [~] **M-F — Fault engine + scheduler.** Full catalog, trigger spec, slots,
  composition rules.
  **Exit:** same scenario + seed twice ⇒ byte-identical event logs;
  every fault type demonstrated at least once with an event trace.
  **Status: HOST-TEST MET (software determinism), HARDWARE NOT MET.**
  The full catalog is implemented against a simulated snapshot and
  host-tested for deterministic replay from a seed. Nothing has produced an
  event trace against a real DUT.
- [~] **M-G — MCP server + GUI + scenario runner.** Scenario YAML schema
  frozen; reports with assertions and validity flags.
  **Exit:** `baseline_firing`, `welded_ssr_midfire`, `tc_disconnect_ramp`
  green against real `KilnFW`+`SaftyFW` with reports archived; GUI drives
  every MANUAL mode.
  **Status: SOFTWARE MET, HARDWARE NOT MET.** `kilnsim`'s MCP server, CLI,
  GUI, scenario loader, and report generator all exist; the YAML schema is
  frozen and all 27 scenario files parse cleanly through the loader. None of
  the named scenarios — or any other — has ever run against a real
  `KilnFW`+`SaftyFW` pair, so no scenario report has ever been archived from
  a live run, and the GUI's MANUAL-mode controls have never driven real
  fixture hardware. All 27 scenarios *have* run against
  `virtual_simfw`+`virtual_dut` (`DESIGN_NOTES.md` §10) — real `SimFW`
  simulation code and real `SaftyFW` guard code, on a PC, with no RP2040 at
  all — but "real `KilnFW`+`SaftyFW`" in this exit criterion means silicon,
  and none has run. Latest `virtual_dut` run: 25 PASS / 2 BLOCKED / 0 FAIL
  across all 27 (`53eb463`) — every fixture-emulation and harness gap in the
  suite is now closed; the 2 BLOCKED are `main_safety_skew` (S1) and
  `tc_stuck` (S13), both uncommissioned-field gaps (`abs_max_temp_c`,
  `borrowed_zone_index`), not code — see `firmware/SimFW/tools/
  virtual_dut/results/SCENARIO_RESULTS.md` for the per-scenario pattern.
- [~] **M-H — Standard library complete.** All 16 scenarios written and run.
  **Exit:** each maps to its `GUARD_TEST_MATRIX.md` rows and that file is
  updated in the same change; `kilnsim run --all` is a one-command
  regression gate.
  **Status: LIBRARY MET (over-delivered: 27, not 16), "AND RUN" NOT MET
  AGAINST REAL HARDWARE.** `GUARD_TEST_MATRIX.md` §5 cross-references guards
  to scenarios; its reachability subsection (§6) records, per guard, whether
  it can currently fire at all. **None of the 27 scenarios has ever run
  against real hardware** — "written" and "run" are different verbs in this
  milestone's own exit criterion, and only the first is true against
  silicon today. All 27 *have* run against `virtual_simfw`+`virtual_dut`,
  which is real code but not real hardware — see `DESIGN_NOTES.md` §§10–11
  for what that run found (mostly: guards whose inputs weren't populated in
  `SaftyFW` at the time, since fixed, not scenario or fixture bugs).

---

## 11. Open questions (resolve before the matching milestone)

Resolved questions have moved to `DESIGN_NOTES.md` §13. What's still open:

1. [~] **Exact J6/J7 mating pinout and voltage levels** — substantially
   resolved by `docs/HARDWARE.md` §3.1/3.2 (full mating table for both
   connectors, including J6's reverse-pin-order trap). **Still open, and
   newly discovered while writing that document:** J6/J7's own voltage/
   supply pins carry a real, unresolved contradiction between the two
   main-board docs — see item 9 below. Nothing here has been
   continuity-checked against physical silicon; `HARDWARE.md` itself is
   explicit that it reconciles source *documents*, not hardware. (M-A)
2. [~] **CT input stage transfer function** — target gain/full-scale figures
   and the transformer ratio (~3:1) are decided; see `DESIGN_NOTES.md` §3.3
   for the full arithmetic. **Still open:** the ~1.5 Vpk usable-Pico-drive
   figure behind that ratio is a medium-confidence estimate, not measured or
   firmware-confirmed; the candidate part's (Triad TY-300P) exact turns
   ratio is unconfirmed against its datasheet; and `wave_owner.c`'s
   amplitude mapping is still an IDENTITY placeholder pending the real
   calibration procedure — see M-D's status in §10. (M-D)
6. [ ] **Fixture hardware form** — how long does the breadboard harness
   survive before a real `hardware/SimFixture/` KiCad board is worth it?
   Revisit after M-E. Unchanged — no fixture hardware, breadboard or
   otherwise, has been built yet, so this has not become answerable.
9. [ ] **J7 pin 1 contradiction between the two main-board hardware docs.**
   `firmware/KilnFW/docs/HARDWARE.md` ("Safety thermocouple board (J7 ->
   J1)") says J7 pin 1 is "(no connect)". `firmware/SaftyFW/docs/HARDWARE.md`
   §8 says J7 pin 1 is `3.3v_Safty` (via R51, 0 Ω). These cannot both be true
   of the same physical connector. `docs/HARDWARE.md` §0 item 5 follows the
   `SaftyFW` doc as the more recently reviewed, more narrowly scoped
   safety-domain source — but flags this explicitly as **unverified,
   requiring a continuity check before the fixture's isolated-side power
   feed is wired**, since getting it wrong means either back-feeding an
   unintended 3.3 V rail or leaving the isolator side unpowered. Resolve at
   bring-up step 5–6 (§14), alongside the ground-domain check that already
   lives there. (M-A/pre-M-A bring-up)

(Item numbers 3, 4, 5, 7, 8, 10, 11 are resolved — see `DESIGN_NOTES.md`
§13. Numbers kept stable here so cross-references elsewhere in the repo
don't break.)

---

## 12. `UnitTestFw` decommission plan

`UnitTestFw` (the ESP32-S3 instrument bench) was a first attempt and is being
thrown away (decided). Order matters — the protocol lives only there today.
Extraction history and rationale: `DESIGN_NOTES.md` §12.

1. [x] **Extract first (M-B)** — done; `benchproto` lives in `CommonFW`.
2. [ ] **Prove the replacement:** SimFW's CDC link and the fresh `kilnsim` PC
   link layer both speak the `CommonFW`-hosted protocol, PING/VERSION green
   on real hardware. The independent Python implementation doubles as the
   extraction's cross-check.
3. [ ] **Delete, one commit, no stragglers:**
   - `firmware/UnitTestFw/` entirely — App, pc_tools, docs, build trees, and
     the embedded `UnitTestFixture.kicad_*` files plus
     `UnitTestFixture-backups/`.
   - `hardware/UnitTestFixture/` entirely.
4. [ ] **Sweep the references in the same commit** (grep hit list as of
   2026-08-20): `CLAUDE.md`, `README.md`, `ROADMAP.md`, `docs/SETUP.md`,
   `docs/REPO_LAYOUT.md`, `kilnCtl.code-workspace` (folder/tasks entries),
   `tools/setup.ps1`, `.gitignore`, and
   `tools/check_no_duplicate_crc.ps1` — its allowlist entry for
   `UnitTestFw`'s `uart_protocol.c` dies with the file (per that script's own
   rule: deleting an allowlist entry is part of finishing a migration).
   Re-grep for `UnitTestFw|UnitTestFixture` before committing.
5. [x] **No tag needed** — git history is the archive.

---

## 13. Testing strategy

Originally three layers, cheapest first; a fourth, unplanned layer now sits
between 1 and 2 — see `DESIGN_NOTES.md` §10 for its full description.

1. [x] **Host tests** (MSVC/CMake, `SaftyFW/test` pattern, `test/`) — done.
   See `DESIGN_NOTES.md` §9/§13 for what's covered.
2. [x] **Loopback tests** (fixture alone, no DUT) — `kilnsim selftest`, done
   and verified against source. Two of its three checks
   (`spi_master_loopback`, `ct_adc_loopback`) are *permanently*
   `NOT_RUNNABLE` from `kilnsim` itself — they need hardware `kilnsim` has no
   way to drive from software alone (`tools/spi_test_master/` is the
   intended separate tool for the SPI half). See `DESIGN_NOTES.md` §13.
3. [ ] **DUT integration** (the point of the project): the scenario library
   against real `KilnFW`+`SaftyFW`. `kilnsim run` exit codes make it a
   scriptable gate; reports are the archived evidence. **Status: unchanged,
   fully hardware-gated** — see §10's milestone table. Nothing about the
   fourth, software-only layer substitutes for this; it is real hardware or
   nothing.

---

## 14. Bring-up order (bench checklist)

The wiring order is chosen so each step is verifiable before the next adds
risk, and the DUT is not connected until the fixture alone is proven:

1. [ ] Pico alone: USB CDC + protocol + heartbeat; `kilnsim state` works.
2. [ ] Expanders on I2C: read/write, interrupt lines if used.
3. [ ] SPI A loopback (scripted master on spare pins): register machine correct.
4. [ ] CT synthesis into a scope/DMM through the transformer: waveform + levels.
5. [ ] **Ground-domain check before first DUT contact:** with the bring-up
   jumper OUT, verify no continuity fixture-GND ↔ GND_Safty; verify isolator
   and transformer orientation.
6. [ ] DUT thermocouple path: J6 unplugged from the real daughterboard, fixture
   in its place, `KilnFW` booted — temperatures appear.
7. [ ] Safety path: J7 via isolator, `SaftyFW`'s single channel reads.
8. [ ] Relay sense: command relays via existing kilnctrl tools, fixture sees
   edges.
9. [ ] E-stop + fault line + DUT power relay, one at a time.
10. [ ] First closed-loop firing on `fast_test` preset.

Each step gets a row in `docs/HARDWARE.md`'s checklist when that doc is
written, same keep-it-current rule as `SaftyFW/docs/HARDWARE.md`.

---

## 15. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| First-byte latency not met at the capped 4 MHz | Core feature (TC emulation) degraded — and the failure mode is a whole burst shifted, returning *plausible* wrong temperatures, not an obvious fault | 4 MHz cap enforced on both masters; DMA-fed path meets the hard deadline at stock sysclk (`DESIGN_NOTES.md` §3.2.1); M-A's Saleae capture is the real gate |
| Master driver access patterns surprise the responder | Emulation subtly wrong, flaky DUT reads | Audit done (`DESIGN_NOTES.md` §3.2.1); Saleae capture of real traffic still needed before final sign-off |
| J6 pinout traced wrong (reverse-order trap) | Possible damage on first plug-in | §11 item 1 resolved on paper *and* continuity-checked at bring-up step 5–6; series resistors on fixture bus-A lines for the first plug-in |
| Ground strap through the fixture defeats isolation | Isolation-dependent behavior untestable; masks real design errors | §3.5 discipline (`DESIGN_NOTES.md`); bring-up step 5 explicit continuity check; standard library runs jumper-out |
| CT amplitude calibration drifts / transformer nonlinearity | Current-based guards tested against wrong magnitudes | Calibration stored per channel with date; re-cal procedure in `kilnsim` (M-D); validity flag in reports if cal older than N days |
| Protocol extraction stalls (stale-fork cleanup balloons) | M-B late, UnitTestFw lingers | Scope extraction to exactly what SimFW needs; the deletion deadline is the forcing function |
| Pin budget overruns during layout | Redesign churn | Documented 3-pin fallback (`DESIGN_NOTES.md` §3.6) reserved before it is needed |
| Fixture bugs masquerade as DUT bugs | Wasted debugging, false confidence | Shadow-truth in every reply, validity flags, selftest mode, instrumentation counters never silent |
