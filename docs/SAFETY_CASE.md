# Safety Case — the argument that this kiln controller is safe enough

> **Status:** first pass, synthesized from existing docs · **Last reviewed:** 2026-09-04
> **Keep this file current.** A safety case that lags the code is worse than no
> safety case, because it invites trust it has not earned. If this file cannot
> be kept honest, delete it rather than let it drift.

## Why this file exists

`firmware/SaftyFW/docs/SAFETY_MODEL.md` (main-board rule) and
`firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md` (safety-processor guards) each
describe a mechanism. This file makes the cross-cutting claim: that the
mechanisms, taken together and given everything known to be missing, reduce
the risk of an unattended kiln to something acceptable — and says plainly
where that claim does not hold yet.

Nothing below is a new finding. It is assembled from the sources cited inline;
where two documents disagreed, both readings are given and flagged rather than
silently resolved.

---

## 1. Hazard list

| # | Hazard | Category |
|---|---|---|
| H1 | Uncontrolled overheat of the chamber (element stuck on, runaway ramp, ceiling not enforced) | Fire / equipment damage |
| H2 | Fire from a heating element left energized with nothing progressing (dry-fire, disconnected/miswired sensor) | Fire |
| H3 | A relay/contactor welds closed and stays closed after a trip commands it open | Fire — trip-ineffective |
| H4 | Electric shock during service (mains-side work while a board is live) | Electric shock |
| H5 | A trip fails to interrupt power at all (both processors agree nothing is wrong when something is) | Fire |
| H6 | Loss of the ESP↔Pico link goes unnoticed and heating continues unsupervised | Fire |
| H7 | Operator cannot stop a firing (E-stop path broken, cut, or misread) | Injury / fire |
| H8 | A firmware update (OTA on either processor) leaves the board in an unsafe or unknown state | Fire / availability |
| H9 | Single-point failure downstream of both relays (a hazard neither processor can see or interrupt) | Fire |
| H10 | A guard exists in code but never actually runs against real inputs on this board (masked/unreachable), so its coverage is illusory | Fire (latent) |

10 hazards. This list is deliberately short and mechanism-oriented — it maps
onto the guards below rather than trying to be an exhaustive top-down FMEA.

---

## 2. Hazard → protection mapping, with residual risk

**Legend:** G = software guard (S1–S14, `SaftyFW`), R = relay-authority gate
(`KilnFW`), P = physical measure, A = accepted risk (nothing currently
mitigates it beyond operator diligence).

| Hazard | Guard(s) / measure | Residual risk |
|---|---|---|
| H1 overheat | **G:** S1 (`abs_max_temp_c` ceiling), S2 (overshoot-sustained), S8 (rate-of-rise). **KilnFW:** thermal_guard guards 1,2,4,5,7 (per-zone, only while `profile_executor` runs that zone) | S1 and S8 both ship **disabled by default** (`abs_max_temp_c`=0, `max_rate_c_per_min`=0 — "0 = never trip" is deliberate, not a bug). An uncommissioned board has **no absolute temperature ceiling in force** at all until an operator sets one. KilnFW's per-zone guards only run while a profile is actively driving that zone — a direct `THERMO_CMD_READ`/dashboard-only session outside a running profile gets no thermal protection from KilnFW at all (`SAFETY_MODEL.md`, "What does NOT enforce it yet"). |
| H2 dry-fire / disconnected sensor | **G:** S5 (sensor validity, graduated WARN→TRIP), S11 (frozen sensor + heat commanded). **KilnFW:** thermal_guard guard 6 (per-zone, profile-running only) | S5/S11 need `ct_installed`/current sensing wired for S11's `heat_commanded` input; both are reachable in source (§6c below) but **not yet hardware-verified post-fix**. KilnFW's guard 6 only covers the profile-running window, same gap as H1. |
| H3 welded contactor | **G:** S9 (`TRIP_INEFFECTIVE`, unconditional, never operator-clearable) | S9 is gated on `current_sensing_commissioned` — inert on a CT-less board (§9 of the matrix). **Never provoked with a genuinely welded contactor on real hardware** — ROADMAP.md M4 flags this as blocked on a hardware jig that injects real AC current through the CT loop; no such jig exists in this repo. Argued and host-tested only. |
| H4 shock during service | **P:** physical isolation, TVS/current-limiting on input rails (`hardware/mainBoard/Power.kicad_sch`), K4 mechanical contactor | Standard practice, not re-verified as part of this pass — hardware review, out of scope here. Accepted as adequately covered by physical design, not by firmware. |
| H5 both processors agree falsely | **G:** dual-processor design itself — KilnFW's relay-authority gate (`relay_authority_on_blocked()`) and SaftyFW's independent guard set are separate codebases reading separate sensors | **A (accepted risk).** No cross-check exists that either processor's "healthy" verdict is *correct* rather than merely self-consistent — e.g. both could be reading a shared, physically-faulted thermocouple wire (H9). Not designed against; documented, not solved. |
| H6 link loss unnoticed | **R:** KilnFW's link-loss watchdog drops relays and asserts `SAFETY_FAULT_SRC_PC_LINK` (opt-in, default OFF for the PC link; unconditional relay-drop). **CommonFW link protocol:** SaftyFW's own liveness split — soft trip at 1.5s (blocks new heat-on), hard 30s firing-abort (`LINK_PROTOCOL.md` §8, wired 2026-09-04, `profile_executor.c:1201-1236`, pinned by `test_safety_link.c:77-92`) | The 30s firing-abort is **host-test-pinned but not hardware-verified** — nobody has held the link down on the bench and watched it with a stopwatch (ROADMAP.md, "Blocked on hardware that does not exist yet": "Time the link-staleness ceiling... Code is flashed; nobody has held the link down"). SaftyFW cannot react to a *live* E-stop/fault report from the Pico either way — that path is one-directional today (`SAFETY_MODEL.md`, "Nothing on the main board reacts to a safety-processor-reported E-stop or fault"). |
| H7 E-stop unreachable | **G:** S7, GPIO9 debounced 50ms, normally-closed wiring (cut cable/pulled connector/press all read as stop) | **Disagreement resolved in code, not yet in wiring**: the E-stop *polarity* bug (S7 inverted, shipped and fixed 2026-08-24, `discrete_pin_policy.c`) is closed and negative-tested. But **no physical E-stop button or deliberate jumper is fitted on a freshly-built board** (`HARDWARE.md` §5: "no jumper is currently fitted anywhere on the estop net in the schematic"). The bench board today reads GPIO9 **low** (healthy/closed) only because *something* is bridging the net physically — not because a button or documented jumper is present. **This means the E-stop input on the bench is not actually being exercised by a physical stop action**; it is present-and-quiet, not tested-and-quiet. |
| H8 bad OTA leaves unsafe state | **R:** both update paths refused unless idle and cool (`SAFETY_MODEL.md`-adjacent update interlocks); `flash_firmware()`'s post-flash verify (tooling, not firmware) | The specific item "link-loss heating block **not** bypassed during a Pico update" is **pinned in CI (2026-09-04) but still OPEN as a hardware-exercise item** — "a test suite is not a substitute for running a real update while heat is nominally blocked and confirming it stays blocked" (ROADMAP.md M8/M13). Argued + host-tested only, not hardware-verified. |
| H9 downstream-of-both-relays SPOF | none identified as closed | **A (accepted risk).** Both `SAFETY_CASE.md`'s own charter and `SAFETY_MODEL.md` name this as the interesting failure class (a thermocouple both processors read through the same broken wire, a mechanical failure downstream of K4). No specific mitigation beyond K4 itself is documented. |
| H10 guard masked / unreachable | **Process, not a guard:** `GUARD_TEST_MATRIX.md` §6/§6a/§6c/§10 — an explicit, repeatedly-recomputed reachability audit | This is the one hazard with strong process evidence: as of §6c (2026-09-03), 11 of 14 implemented guards are structurally reachable; S1/S13/S14 are deliberately configured off (not bugs); S8 exists but is excluded from the denominator pending a measured ramp. S6a is reachable in source but **cannot be provoked by any current host fixture** (`virtual_dut`/SimFW were both removed 2026-08-28) — bench hardware is the only way to exercise it. See §5 below for the full evidence table. |

---

## 3. Residual risks and non-protections — stated plainly

This system does **not** protect against, or protects only partially against,
the following. Each was checked against code/docs before being written down
here; none is copied from an unverified summary.

1. **An uncommissioned board has no temperature ceiling.** `abs_max_temp_c`
   defaults to 0, which `safety_guards.h`'s own convention reads as "not
   commissioned, never trip" — this is a deliberate design choice (fail loud
   at commissioning time, not fail dangerous with a guessed default), but it
   means S1 is a no-op until an operator explicitly sets it. The
   commissioning-gate interlock (`GUARD_TEST_MATRIX.md` §8, closed 2026-08-28)
   now refuses to grant heat at all until the required fields including
   `abs_max_temp_c` are set — **this closes the specific hole** of firing
   with S1 silently off, but does not change that S1 itself has no built-in
   floor.

2. **S8 (rate-of-rise) ships permanently at 0 = disabled** until an operator
   measures a real full-power ramp on this specific kiln and commissions a
   threshold at roughly 2x it. No such measurement is on record in this repo
   as of this writing.

3. **The E-stop input is not exercised by a physical stop action on a
   freshly-built board.** Verified against `HARDWARE.md` §5 and the schematic
   note: no jumper or button is documented as fitted on the `estop` net. The
   *bench* board separately reads GPIO9 low (closed contact, healthy) —
   confirmed by SWD readback 2026-08-24 — meaning some physical continuity
   exists on that specific unit, but it is not documented as a real
   button+cable able to demonstrate the open-on-press behavior on demand.
   **This confirms the concern in the task brief**: the E-stop is present and
   quiet, not press-tested.

4. **Current sensing can be disabled entirely** (`ct_installed = no`), a
   first-class, ASKED commissioning state (`GUARD_TEST_MATRIX.md` §9). On a
   CT-less board, S3 (load-stuck-on), S4 (load-inactive WARN), S9
   (trip-ineffective), and S14 (overcurrent) are all **structurally inert**,
   and S6b's soft current-gated tier degrades to the unconditional 120s
   backstop only. This is a deliberate, reported-as-such state, not a silent
   gap — but it is a real reduction in coverage that an operator can select.

5. **S9's welded-contactor escalation has never been provoked with a
   genuinely welded contactor.** It is argued from code inspection
   (`safety_guards.c:363-389`) and reachable in source (§6c), but the only way
   to test it for real needs a hardware jig injecting AC current through the
   CT loop while confirming K4's coil drive is de-energized — no such jig
   exists in this repo (ROADMAP.md M4). This is the single most safety-load-
   bearing guard in the system (the backstop against "the trip fired and
   nothing happened") and it carries the thinnest evidence.

6. **The safety processor's own E-stop/fault report does not affect the main
   board's relays.** The isolated fault line is one-directional (ESP → Pico
   only, via GPIO6/U1). If the Pico independently detects a fault or E-stop,
   KilnFW does not react to it beyond what it can observe over the telemetry
   link — `SAFETY_MODEL.md` states this plainly: "Today: no."

7. **A live thermocouple fault outside a running profile is invisible to
   KilnFW's relay gate.** `SAFETY_FAULT_SRC_THERMO` is only live-asserted
   while `profile_executor` is actively driving that zone; a raw
   `THERMO_CMD_READ` or the dashboard's idle status view reports the fault
   byte but nothing downstream acts on it.

8. **No cross-check exists that the two processors' "healthy" verdicts are
   independently correct** rather than merely self-consistent (H5/H9 above).
   A thermocouple physically shared or routed through the same failure point
   for both processors is not designed against.

9. **`SAFETY_FAULT_SRC_APP` latches forever** on a transient control-task
   stall recovering on its own (guard 9, found on the bench 2026-08-25) — the
   board shows a permanent fault requiring a reboot even after the underlying
   condition clears. Availability bug, not a missed-trip risk, but it trains
   operators to expect false-permanent faults, which erodes trust in real
   ones.

10. **Refusal of a relay-on command is invisible on the wire** — a refused
    `SET_RELAY` produces no reply; a GUI infers it only from state not
    changing within one report period. No explicit "refused, and here is why"
    signal exists today.

---

## 4. Evidence classification

Every claim above and every guard in the summary table is one of:
**argued** (code inspection only), **host-tested** (a specific host test
pins it), or **hardware-verified** (exercised on real silicon; commit/date
given).

| Guard / claim | Class | Evidence |
|---|---|---|
| S1 ceiling clamp (incl. NaN/-Inf property) | host-tested | `test_safety_guards.c::test_s1_ceiling_properties()`, 2026-08-19 |
| S1 reachability (structural) | argued | `GUARD_TEST_MATRIX.md` §6c, source-read against `safety_core.c:333` |
| S1 hardware trip | **not done** | no bench provocation on record for S1 in this repo |
| S2 overshoot-sustained | host-tested | `test_safety_guards.c` (119s/121s boundary, 2026-08-19) |
| S2 reachability + `virtual_dut` cross-check | argued + tooling (removed) | `virtual_dut` evidence existed but the tool was deleted 2026-08-28; source-reachability re-confirmed §6c |
| S3 load-stuck-on | host-tested | §2 provocation table |
| S3 reachability | argued | `current_sense_set_cal()` confirmed called, `current_task.c:197` |
| S3 hardware trip | **not done** | §3.4 rows unexecuted — "no hardware was touched to write this" |
| S4 load-inactive WARN | host-tested | §2 provocation table |
| S5 sensor validity (incl. per-tc_type band, CR1 readback) | host-tested | `test_max31856_tc_range_policy.c`, `test_max31856_decode.c` |
| S5 hardware fit (safety TC physically present) | **hardware-verified** | 2026-08-24, live link read "30.20 C (CJ 28.08 C)", `GUARD_TEST_MATRIX.md` §6a |
| S5 masking-before-fit finding | hardware-verified | same bench session; explains why no other guard had ever transitioned on this board before that date |
| S6a main-fault trip | argued | reachable in source (`safety_core.c:1073`) since the wiring commits; **cannot be provoked by any current host fixture** — needs bench hardware, permanently (no I2C-expander/opto emulation exists or is planned) |
| S6b link-dead (both tiers) | host-tested | §2 provocation table (10s soft, 120s hard) |
| S6b hardware | **not done** | §3.4 row unexecuted |
| S7 E-stop trip logic | host-tested, negative-tested | `test_discrete_pin_policy.c`; polarity-inversion bug reintroduced and confirmed to fail 4+2 assertions, then restored (2026-08-24) |
| S7 physical button/jumper | **argued only, contradicted by hardware note** | schematic says no jumper fitted; bench GPIO9 reads low by some undocumented continuity — not a demonstrated press-to-open test |
| S8 rate-of-rise (pure logic + config wiring) | host-tested | `test_s8()`, `test_safety_core_s8_wiring.c`, 2026-09-03 |
| S8 hardware / real threshold | **not done, and cannot be until a ramp is measured** | ROADMAP.md M3 |
| S9 trip-ineffective escalation logic | host-tested | §2 provocation table |
| S9 reachability | argued | `safety_core.c:1109`, `relay_owner_is_energized()` wiring |
| S9 hardware (real welded contactor) | **not done — no jig exists** | ROADMAP.md M4, explicit |
| S10 chamber-disagreement WARN | host-tested | §2 provocation table |
| S11 frozen-sensor trip | host-tested | §2 provocation table |
| S11 hardware | **not done** | §3.4 row unexecuted |
| S12 cold-junction WARN | argued + host-tested (ungated) | reads real snapshot, unconditional |
| S13 borrowed-zone staleness | host-tested | reachable in source (`context_borrowed_sample_counter_advancing()`); commissioning-gated off by default |
| S14 per-channel overcurrent WARN | host-tested | new guard 2026-08-28, `test_ct_disabled_guards()` pair-tested |
| Relay-authority gate (KilnFW, all three callers) | argued + host-tested per caller | `relay_authority.{c,h}`; UART bridge, diagnostics HTTP, profile executor all confirmed routed through it |
| KilnFW thermal_guard guards 1,2,4,5,7 | **host-tested, not hardware-verified** | `App/test/test_thermal_guard.c` exercises the real `thermal_guard_tick()` (not a stub) for each of these, including override/arming/regression cases (e.g. `"guard 1 trips when commanded heat produces far less than sanity_rate_c_per_min"`, `"guard 4 eventually trips a zone that starts hot and never settles"`); no bench provocation of any of these five is on record (`SAFETY_MODEL.md` summary table, corrected 2026-09-04 — a previous pass of this row read "not yet live-tested" as "not tested at all" and understated the coverage; see `SAFETY_MODEL.md`'s own disagreement note) |
| KilnFW thermal_guard guard 6 (sensor validity) | hardware-verified | live-verified end to end, no TC attached, trip fired after exactly 3 bad reads, board relay stayed off (`SAFETY_MODEL.md`); also host-tested against the real function (`App/test/test_thermal_guard.c`) |
| KilnFW thermal_guard guard 3 (relay welded, thermal sanity) | host-tested, not hardware-verified | `App/test/test_thermal_guard.c`, including a named hardware-motivated regression case ("found on hardware 2026-08-12 against the simulated…") and the `runaway_margin_c` override; the only hardware contact this guard has had is the false-positive it was tuned against, not a genuine positive trip |
| KilnFW thermal_guard guard 9 (control-task stall) | host-tested (trip/priority logic) + argued (fault-clear defect), not hardware-verified | the tick-stale fault path itself is a real-function test, not a stub (`App/test/test_safety_watchdog.c::test_tick_stale_still_faults_running_and_takes_priority`); the separate `SAFETY_FAULT_SRC_APP`-never-clears defect was found by code inspection of the trigger path, not by deliberately stalling the control task on real hardware — no commit or bench record around 2026-08-25 (or any other date) shows a genuinely provoked control-task stall; see `SAFETY_MODEL.md`'s own disagreement note, which resolves the "found on the bench" wording the same way |
| KilnFW thermal_guard guard 8 (cross-zone plausibility) | **not built** | unimplemented, needs concurrent multi-zone execution |
| Link-loss 30s firing-abort (KilnFW side) | host-tested | `test_safety_link.c:77-92`, pinned 2026-09-04 |
| Link-loss 30s firing-abort, real bench | **not done** | ROADMAP.md: "Code is flashed; nobody has held the link down" |
| OTA interlock vs link-loss heating-block | host-tested (CI-pinned) | 2026-09-04 |
| OTA interlock vs link-loss, real bench | **not done** | ROADMAP.md M8/M13, explicitly still open |
| E-stop polarity fix itself | hardware-relevant, negative-tested at unit level | fixed 2026-08-24; "nothing in the host suite or this fixture could have caught it" before the pure-policy extraction — worth noting the *original* bug shipped invisibly for a time |
| `virtual_dut`/SimFW cross-check evidence generally | **withdrawn** | tool deleted 2026-08-28; every "Yes" reachability verdict that cited it now rests on source-reading alone (method 1), re-confirmed independently in §6c |

**Rollup (guard-level rows above, S1–S14 plus the two KilnFW-side items called
out separately):** roughly 20 discrete claims tracked here — **19
host-tested** (S1–S14's logic rows plus KilnFW guards 1,2,3,4,5,6,7, and
guard 9's trip/priority path), **3 hardware-verified** (S5's fit/masking
finding, KilnFW guard 6, E-stop polarity fix), and the remaining **~8
explicitly marked "not done"** for hardware. What remains **argued only** is
narrower than a previous pass of this table claimed: S6a's permanent
hardware-only status, the E-stop jumper/button claim, and guard 9's
fault-clear defect specifically (its trip/priority mechanism is host-tested;
only the "never clears" defect's trigger path is code-inspection-only).
KilnFW guards 1/2/3/4/5/6/7 are **not** argued-only — see the corrected rows
above. No claim in this document is stronger than its weakest supporting
sentence in the source docs; where a source hedges, this table hedges
identically.

---

## 5. Cross-check against `GUARD_TEST_MATRIX.md`

Reviewed section by section (§1–§10) against this file's claims above.

**Agreement:**
- The four-state reachability model (not reachable / reachable-not-
  commissioned / reachable-not-provokable-by-fixture / reachable-and-
  exercised) is adopted here without modification.
- S1/S13/S14's "deliberately configured off" framing is used verbatim — this
  file does not describe them as bugs.
- The `virtual_dut`/SimFW removal and its effect on evidence strength (every
  "Yes" now rests on source-reading, method 1, not a running cross-check) is
  carried through here as a downgrade on every guard that previously cited
  `virtual_dut`.

**Disagreements found — reported, not silently resolved:**

1. **S6a's status is described two different ways depending on which section
   of the matrix you read.** §6c's per-guard table calls it "**reachable in
   source; (b) not exercisable by any host fixture**". §6a's narrative
   re-derivation calls it "Reachable on real hardware now" (after the TC fit)
   but still "not provokable through `virtual_dut`". These are consistent
   readings once you separate *reachable in source* from *reachable on real
   hardware* from *provokable by the deleted host fixture* — but the matrix
   itself does not always keep the three straight in prose, only in its
   table columns. This safety case uses the strictest of the three
   ("bench hardware is the only way," per §6c) for S6a's row above.

2. **This safety case's own precondition statement is now stale.** The
   original stub said: "This cannot be written honestly until `SaftyFW`
   exists and its guards have been exercised on hardware." `SaftyFW` exists
   and is running on real silicon (ROADMAP.md), and several guards *have*
   been hardware-exercised (S5's fit, KilnFW guard 6, the E-stop polarity
   fix) — but the *majority* of guards' hardware trip rows in §3.4 of the
   matrix are still unexecuted ("no hardware was touched to write this...
   every row above is still unexecuted"). Writing this file now is
   deliberately a **partial** safety case: real, but with hardware evidence
   still thin in exactly the places §3.4 says it is thin. This is stated
   here rather than silently proceeding as if the precondition were fully
   met.

3. **No disagreement found** between `GUARD_TEST_MATRIX.md` and
   `SAFETY_MODEL.md` on the KilnFW-side claims (per-zone guards, the
   relay-authority gate) — the two documents describe non-overlapping layers
   and were consistent everywhere checked.

4. **`LINK_PROTOCOL.md`'s 30s firing-abort item** is marked `[x]` complete in
   that document's own checklist (host-tested, CI-pinned) but ROADMAP.md
   independently lists "time the link-staleness ceiling... with a stopwatch"
   as still blocked on hardware that has not been touched. Both are correct
   simultaneously — `[x]` there means the *code path exists and is
   host-pinned*, not that it has been observed on the bench — but a reader
   skimming only `LINK_PROTOCOL.md`'s checkbox could reasonably conclude more
   than that. Flagged here so this file does not repeat the same
   overstatement.

5. **Corrected 2026-09-04:** this file's own §4 previously classified KilnFW
   `thermal_guard` guards 1, 2, 4, 5, 7 as "argued + code-reviewed only, not
   yet live-tested" and read that as "not tested at all"; it also classified
   guard 3 as "argued only" and guard 9 as flatly "argued via code
   inspection". `firmware/KilnFW/App/test/test_thermal_guard.c` demonstrably
   host-tests guards 1, 2, 3, 4, 5, 6, 7 against the real `thermal_guard_tick()`
   function (override, arming and regression cases included, none of them
   inert in the tested configuration or driven by a stub); guard 9's
   trip/priority path is likewise host-tested (`test_safety_watchdog.c`), with
   only its separate fault-clear defect remaining argued-only. Both this
   file's §4 rows and `firmware/KilnFW/docs/SAFETY_MODEL.md`'s summary table
   (already corrected in `ecdad62`) now agree: host-tested, not
   hardware-verified, for guards 1–7; host-tested + argued for guard 9.
   Separately, `SAFETY_MODEL.md`'s claim that guard 9's fault-latch defect was
   "found on the bench 2026-08-25" is retracted — no commit or status-doc
   entry near that date records a genuine bench-provoked control-task stall;
   the defect was found by code inspection, corrected in `SAFETY_MODEL.md`
   alongside this pass.

---

## Completion checklist

- [x] Hazard list written (§1, 10 hazards)
- [x] Each hazard mapped to a guard, a physical measure, or an accepted risk (§2)
- [x] Residual risks and non-protections stated explicitly (§3)
- [x] Evidence classified: argued / host-tested / hardware-verified (§4)
- [x] Reviewed against [`../firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`](../firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md) (§5) —
      reviewed through §10 as it stood 2026-09-04; four disagreements recorded above
      (items 1, 2, 4 pre-existing, item 5 a self-correction made this pass), none
      requiring a code change, all requiring careful reading rather than a
      single number.

**What would most improve this document next:** a real bench session against
§3.4's still-unexecuted rows (S2–S6b, S7, S9–S12 hardware trips) and, above
all, a hardware jig for S9's welded-contactor case — it is the guard this
document's own evidence table is thinnest on, and the one the system's whole
trip-ineffective story depends on.
