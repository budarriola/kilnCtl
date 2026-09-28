# Safety Case — the argument that this kiln controller is safe enough

> **Status:** first pass, synthesized from existing docs · **Last reviewed:** 2026-09-04 (topology/H3 pass)
>
> **Superseded 2026-09-24:** every `ceiling = min(abs_max_temp_c, firing_max_c
> + firing_margin_c)` formula quoted below describes code that has since been
> reverted (owner decision: "the safty limits should be the same the safty
> processor is a backup incase the esp fails" — SET_FIRING_CEILING/0x09 and
> the Pico-side tightening it drove are removed). S1's ceiling is now
> unconditionally `abs_max_temp_c`, with no `firing_max_c`/`firing_margin_c`
> term at all — the formula text below is historical, not current behavior.
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
| H1 overheat | **G:** S1 (`abs_max_temp_c` ceiling), S2 (overshoot-sustained), S8 (rate-of-rise). **KilnFW:** thermal_guard guards 1,2,4,5,7 (per-zone, only while `profile_executor` runs that zone) | **Verified live 2026-09-04 (`GET /api/safety/commissioning`, read-only): S1 is ARMED, `abs_max_temp_c`=80, `set:true`, `commissioned:true` on the current bench board — this has changed since the 2026-08-28 note below and in `firmware/KilnFW/TODO.md`, which described `abs_max_temp_c`=0 (never-trip).** S8 (`max_rate_c_per_min`) remains at 0 = disabled, deliberately, pending a measured full-power ramp (see §3.2). With S1 armed, a runaway is independently backstopped: `safety_guards.c` (`SAFETY_TRIP_OVERTEMP`, ~line 603) computes `ceiling = min(abs_max_temp_c, firing_max_c + firing_margin_c)` and trips on the 3rd consecutive over-ceiling reading (~300ms), which de-energizes K4 on the RP2040 side — independent of the ESP32-S3 regardless of what the ESP keeps commanding. Today that ceiling (80°C) equals, not undercuts, the ESP-side zones' own `max_temp_c` (also 80), so S1 is real protection against a runaway but not a *tighter* independent limit — see the owner-decision recommendation in §3.1. KilnFW's per-zone guards only run while a profile is actively driving that zone — a direct `THERMO_CMD_READ`/dashboard-only session outside a running profile gets no thermal protection from KilnFW at all (`SAFETY_MODEL.md`, "What does NOT enforce it yet"). |
| H2 dry-fire / disconnected sensor | **G:** S5 (sensor validity, graduated WARN→TRIP), S11 (frozen sensor + heat commanded). **KilnFW:** thermal_guard guard 6 (per-zone, profile-running only) | **Corrected 2026-09-04 — S11 is currently INERT, not merely unverified.** `safety_guards.c:657`'s gate is `in->tc_valid && in->heat_commanded`, and `heat_commanded` is wired straight from `any_current_present` (`safety_core.c:1090`), which `safety_core.c:1020-1024` forces to `false` whenever `ct_installed = no`. **Live-confirmed the same day** (`safety_get_status`, `dc39b09`): `ct_installed = 0`, all three channels reading `0.00 A`. So on the bench rig **today**, S11's elapsed-time accumulator never starts, regardless of how long a reading sits frozen — this is not "reachable in source, awaiting a hardware pass," it structurally cannot fire while CTs are absent. S5 is unaffected by this (its gate is `tc_valid`/read-freshness only, no current dependency) and remains armed. KilnFW's guard 6 only covers the profile-running window, same gap as H1. See `GUARD_TEST_MATRIX.md` §9's now-added S11 row for the full trace. |
| H3 welded contactor | **G:** S9 (`TRIP_INEFFECTIVE`, unconditional, never operator-clearable). **Topology:** the line contactor sits electrically upstream of the per-zone SSRs (`mains → line contactor → SSRs → elements`), so K4 (RP2040) and K1/K2/K3/K5 (ESP) gate two *different, staged* points rather than one shared point — see §3 item 4a for the full topology finding | **No active detection today, but the topology tolerates a single weld of the wrong device.** S9 is gated on `current_sensing_disabled`/`current_sensing_commissioned` (`safety_guards.c:405-416`) — inert on a CT-less board (§9 of the matrix), and **live-confirmed inert on the bench rig 2026-09-04**: `safety_get_status` reports `ct_installed = 0`, currents `0.00 A, 0.00 A, 0.00 A`. `safety_core.c:1020-1024` forces `any_current_present` false, so S9's block at `safety_guards.c:403-416` takes the `current_sensing_disabled` branch every tick — streak held at 0, no warn, no path to `TRIP_INEFFECTIVE`, ever, until CTs are fitted and commissioned. No other guard substitutes for it (S3/S4/S14 are equally CT-gated; nothing else observes contactor state), and KilnFW provides no independent check — its own `current_a[]` (`safety_link_frames.c:632-634`) is decoded from the *same* RP2040 CT telemetry over the link, not a separate ESP-side sense path (no ESP-side current ADC exists; `hardware/mainBoard/CurrentSense.kicad_sch`'s ADC0/1/2 feed the RP2040's `current_task.c`, not the ESP32-S3). **But this hazard row's title is narrower than the real finding.** A welded *contactor* alone does not, by itself, put current through an element — the per-zone SSRs (ESP-driven) still individually gate each element downstream of it, so heat only flows if an SSR is also on. What a welded contactor actually does — silently — is disable the *entire* independent RP2040 veto: every SaftyFW guard (S1 overtemp, S2, S6a/b link/main-fault, S7 e-stop, all of them) acts by de-energizing K4, and K4 dropping only opens the contactor; if the contactor's own contacts are welded, dropping K4 achieves nothing. See §3 item 4a for the full single-failure table. **Plainly, in order of how bad it actually is:** (1) if K4 (the pilot relay) welds, or the contactor coil path sticks, the contactor stays closed regardless of firmware — SaftyFW's entire trip-actuation path becomes cosmetic (still logs/reports, stops nothing) until an operator notices or a second failure (a stuck SSR, a runaway the ESP itself doesn't catch) turns that into real heat with zero independent backstop; (2) nothing today detects either half of that on this CT-less rig. Also **never provoked with a genuinely welded contactor on real hardware** even when armed — ROADMAP.md M4 flags this as blocked on a hardware jig that injects real AC current through the CT loop; no such jig exists in this repo. Argued (schematic + `firmware/SaftyFW/docs/HARDWARE.md` §3, itself schematic-traced 2026-08-16) and host-tested only; the topology claim was cross-checked against `hardware/mainBoard/kiln.kicad_pro` via the KiCad MCP server this pass (`get_kicad_component_connections` on K1/K4) but the live netlist returned no populated nets for either reference (likely a stale/unbuilt netlist, not evidence against the schematic-documented topology) — so the topology claim rests on the cited schematic-derived documentation, not on a netlist this pass independently re-derived. |
| H4 shock during service | **P:** physical isolation, TVS/current-limiting on input rails (`hardware/mainBoard/Power.kicad_sch`), K4 mechanical contactor | Standard practice, not re-verified as part of this pass — hardware review, out of scope here. Accepted as adequately covered by physical design, not by firmware. |
| H5 both processors agree falsely | **G:** dual-processor design itself — KilnFW's relay-authority gate (`relay_authority_on_blocked()`) and SaftyFW's independent guard set are separate codebases reading separate sensors | **A (accepted risk).** No cross-check exists that either processor's "healthy" verdict is *correct* rather than merely self-consistent — e.g. both could be reading a shared, physically-faulted thermocouple wire (H9). Not designed against; documented, not solved. |
| H6 link loss unnoticed | **R:** KilnFW's link-loss watchdog drops relays and asserts `SAFETY_FAULT_SRC_PC_LINK` (opt-in, default OFF for the PC link; unconditional relay-drop). **CommonFW link protocol:** SaftyFW's own liveness split — soft trip at 1.5s (blocks new heat-on), hard 30s firing-abort (`LINK_PROTOCOL.md` §8, wired 2026-09-04, `profile_executor.c:1201-1236`, pinned by `test_safety_link.c:77-92`) | The 30s firing-abort is **host-test-pinned but not hardware-verified** — nobody has held the link down on the bench and watched it with a stopwatch (ROADMAP.md, "Blocked on hardware that does not exist yet": "Time the link-staleness ceiling... Code is flashed; nobody has held the link down"). SaftyFW cannot react to a *live* E-stop/fault report from the Pico either way — that path is one-directional today (`SAFETY_MODEL.md`, "Nothing on the main board reacts to a safety-processor-reported E-stop or fault"). |
| H7 E-stop unreachable | **G:** S7, GPIO9 debounced 50ms, normally-closed wiring (cut cable/pulled connector/press all read as stop); firmware also independently commands `relay_owner` into TRIPPED on assertion, pinned end-to-end by `firmware/SaftyFW/test/test_estop_deenergizes_relay.c` | **Resolved in code and closed by owner decision on wiring.** The E-stop *polarity* bug (S7 inverted, shipped and fixed 2026-08-24, `discrete_pin_policy.c`) is closed and negative-tested. A contact is physically fitted on this bench board (GPIO9 confirmed LOW/healthy 2026-09-08, superseding the earlier "no jumper fitted" note — `HARDWARE.md` §5). The double-pole design's pole 1 (hardware-interrupting the line contactor coil, `HARDWARE.md` §5.1) will **not** be wired on this fixture — owner decision 2026-09-10, "consider it closed so long as the signal is checked and acted on" — so the E-stop here is **firmware-mediated only**: it depends on the safety processor running and reaching the trip path, unlike a wired pole 1 which cuts power regardless of firmware state. Immaterial on this ~4 W/120 V fixture (`HARDWARE.md` §5.1); a real kiln installation should still wire pole 1. |
| H8 bad OTA leaves unsafe state | **R:** both update paths refused unless idle and cool (`SAFETY_MODEL.md`-adjacent update interlocks); `flash_firmware()`'s post-flash verify (tooling, not firmware) | The specific item "link-loss heating block **not** bypassed during a Pico update" is **pinned in CI (2026-09-04) but still OPEN as a hardware-exercise item** — "a test suite is not a substitute for running a real update while heat is nominally blocked and confirming it stays blocked" (ROADMAP.md M8/M13). Argued + host-tested only, not hardware-verified. |
| H9 downstream-of-both-relays SPOF | none identified as closed | **A (accepted risk) — now concretized (§3 item 4a).** The relay stages are not literally one shared point; they are staged in series (contactor, then per-zone SSRs) and each is genuinely single-point for a different reason. The line contactor is the single point whose failure disables K4's *entire* real-world effect (every SaftyFW guard trip becomes cosmetic, not just S9's), while each per-zone SSR is the single point downstream of *both* authorities for that zone's element specifically (a stuck SSR keeps that element hot regardless of the ESP's own command, and is normally still caught by K4/contactor dropping — unless the contactor has *also* welded). Neither failure alone routinely causes uncontrolled heat; a welded contactor plus a welded/stuck SSR (or an ESP-side command left on with no independent overtemp backstop) does. No specific mitigation beyond K4 itself is documented, and this system's evidence for K4→contactor actually being wired as documented is schematic-derived, not bench-proven (§3 item 4a). |
| H10 guard masked / unreachable | **Process, not a guard:** `GUARD_TEST_MATRIX.md` §6/§6a/§6c/§10 — an explicit, repeatedly-recomputed reachability audit | This is the one hazard with strong process evidence: as of §6c (2026-09-03), 11 of 14 implemented guards are structurally reachable; S1/S13/S14 are deliberately configured off (not bugs); S8 exists but is excluded from the denominator pending a measured ramp. S6a is reachable in source but **cannot be provoked by any current host fixture** (`virtual_dut`/SimFW were both removed 2026-08-28) — bench hardware is the only way to exercise it. See §5 below for the full evidence table. |
| H11 the two processors silently hold different configurations | **G/R:** the standing config-divergence invariant — the ESP compares its saved snapshot of the Pico's commissioning parameters against the Pico's live ones (format version + hash identity) and, on any mismatch, raises an alarm and disables the heaters. The two-processor apply is transactional (`kiln_cfg_swap.c`), with a persisted marker so a crash mid-apply is detected and recovered at the next boot rather than being left half-applied | **Detection is designed and latched, but the dangerous *precursor* is a different row's problem.** Divergence is deliberately un-dismissable: there is no operator control to acknowledge it, only re-pushing a config until the halves genuinely agree, so it cannot be clicked away and forgotten. The residual risk is scope, not mechanism: this compares the config the Pico *reports*, so it catches a failed/partial push and a stale Pico, but not a Pico whose stored config is itself wrong (a bad value correctly pushed and correctly echoed reads as perfectly converged — that is H1/§3.1's problem, not this one). An **UNCONFIGURED** Pico is the worst case here, because `abs_max_temp_c` defaults to `0.0f` which S1 reads as "never trip" (§3 item 1): such a board is a missed-trip state that *looks* quiet, and the re-push on a detected Pico reboot is what closes it. Host-test and source evidence only; not provoked on hardware by desynchronizing the two processors and watching the heaters actually go down. |

---

## 3. Residual risks and non-protections — stated plainly

This system does **not** protect against, or protects only partially against,
the following. Each was checked against code/docs before being written down
here; none is copied from an unverified summary.

1. **An uncommissioned board has no temperature ceiling — but this board is
   now commissioned.** `abs_max_temp_c` DEFAULTS to 0, which
   `safety_guards.h`'s own convention reads as "not commissioned, never
   trip" — a deliberate design choice (fail loud at commissioning time, not
   fail dangerous with a guessed default). That was this bench board's
   documented state on 2026-08-28 (`firmware/KilnFW/TODO.md`, and this
   file's own hazard row above, both said `set: true, value: 0`). **Verified
   live 2026-09-04, read-only, via `GET /api/safety/commissioning`
   (`firmware/KilnFW/App/drivers/http/safety_cfg_http.c`'s
   `commissioning_get_handler`) against the running board: `abs_max_temp_c`
   is now `{"set":true,"value":80}`, `commissioned:true`, config CRC
   matched (not stale).** Confirmed against source, not assumed: reading
   `firmware/SaftyFW/src/safety_guards.c` (READ ONLY) shows S1's trip test
   is gated on `cfg->abs_max_temp_c > 0.0f`
   (`SAFETY_TRIP_OVERTEMP`/`abs_max_temp_c`, ~line 603-631) — with the value
   now 80, S1 is live: it computes
   `ceiling = min(abs_max_temp_c, firing_max_c + firing_margin_c)` and trips
   on the 3rd consecutive tick over that ceiling, de-energizing K4
   independently of the ESP32-S3. The commissioning-gate interlock
   (`GUARD_TEST_MATRIX.md` §8, closed 2026-08-28) is presumably what caused
   this to get set, though this pass did not trace exactly when/how it was
   commissioned — only that it now is. The
   commissioned ceiling (80°C) is identical to, not tighter than, the
   ESP-side zones' own `max_temp_c` (80°C) — real backstop against the ESP
   continuing to command heat, but not a second, lower line of defense.
   **Decided by the owner, 2026-09-05: `abs_max_temp_c` STAYS at 80°C, by
   design.** Intent stated directly: the Pico's ceiling is meant to be the
   same as or looser than the ESP's — a second set of eyes on the same
   limit, never a tighter envelope of its own. This closes the item; do not
   re-raise it as a gap that S1 "only mirrors" the ESP — that is the
   intended relationship, not an oversight.

2. **S8 (rate-of-rise), S13 (borrowed-TC-stale) and S14 (overcurrent) remain
   commissioned OFF today**, confirmed by the same live read: `max_rate_c_per_min`=0
   (S8, disabled, same 0-means-never-trip convention as S1 used to be),
   `tc_source`=0/`OWN_J7` (S13's BORROWED_ZONE/BOTH gate, not engaged), and
   `ct_installed`=0 with `i_normal_a[0..2]` unset (S14 — WARN-only by design
   regardless, not a trip guard, `safety_guards.c` ~line 930-960). These are
   dormant by design/commissioning gap, not defects — see
   `GUARD_TEST_MATRIX.md` §6/§9 for the existing analysis, unchanged by this
   pass.

3. **Stale as of 2026-09-10 — superseded, see H7 above.** This item
   originally read: "the E-stop input is not exercised by a physical stop
   action on a freshly-built board... present and quiet, not press-tested."
   That premise ("no jumper or button... documented as fitted") was itself
   corrected 2026-09-08 (`HARDWARE.md` §5: a contact is physically fitted),
   and `firmware/SaftyFW/README.md`'s bench verification procedure plus
   `estop_verification.c`'s durable operator record now give pole 2 an actual
   demonstrated press-to-open test, not an assumption. Pole 1 (the physical
   interlock) will not be wired on this fixture — owner decision 2026-09-10,
   see H7 — so it is never press-tested here by design, not by oversight;
   that is the firmware-mediated-only consequence H7 states.

4. **Current sensing can be disabled entirely** (`ct_installed = no`), a
   first-class, ASKED commissioning state (`GUARD_TEST_MATRIX.md` §9). On a
   CT-less board, S3 (load-stuck-on), S4 (load-inactive WARN), S9
   (trip-ineffective), S11 (frozen-sensor — added to this list 2026-09-04,
   see below), and S14 (overcurrent) are all **structurally inert**,
   and S6b's soft current-gated tier degrades to the unconditional 120s
   backstop only. Four of these five (S3, S4, S9, S14) are deliberately
   held inert, reported as such via `ct_guards_disabled`, with the reasoning
   recorded at `GUARD_TEST_MATRIX.md` §9. **S11 is different: it is a real
   side effect (`heat_commanded` is wired from the same `any_current_present`
   these guards share, `safety_core.c:1090`), not a decision anyone recorded
   — until this pass it was not even listed here or in §9's table, and H2's
   row above described it as merely "not yet hardware-verified" rather than
   "cannot fire in the current configuration." That gap in the documentation,
   not the underlying behavior, is the unintentionally-dormant finding.**
   **Live-confirmed 2026-09-04** (`safety_get_status`, `dc39b09`):
   `ct_installed = 0`, currents `0.00 A, 0.00 A, 0.00 A` on the bench rig
   right now — every guard in this paragraph is inert today, not
   hypothetically. This is a real reduction in coverage that an operator can
   select, and on this rig it is currently selected.

   **The practical consequence, stated plainly: with no CTs fitted, nothing
   in this system detects a welded contactor today.** S9 is the guard H3's
   hazard row rests on, and it cannot fire while `current_sensing_disabled`
   is set. KilnFW provides no independent backstop — its `current_a[]` is
   relayed RP2040 CT telemetry, not a second sensor (`safety_link_frames.c:632-634`;
   `hardware/mainBoard/CurrentSense.kicad_sch`'s ADC feeds the RP2040 via
   `current_task.c`, not the ESP32-S3). **Owner decision, not acted on here:**
   arming S3/S4/S9/S11/S14 requires (a) physically fitting CTs per
   `hardware/mainBoard/CurrentSense.kicad_sch`/`CURRENT_SENSE.md`, (b) running
   the one-relay-at-a-time channel mapping check (`CURRENT_SENSE.md` §5 step
   2 — the gate `GUARD_TEST_MATRIX.md` §3.3 already documents), and (c)
   commissioning: setting `ct_installed = 1`, a valid `ct_channel_map`, and
   loading a real per-channel calibration via `safety_set_ct_cal` (the
   read-only counterpart, `safety_get_ct_cal`, and the calibration record
   itself, `config_store.h`'s CT-cal fields, already exist as the intended
   commissioning path — neither was called or written as part of this
   verification pass, per this task's own read-only constraint).

   **Owner decision, framed against the topology, not fear:** the case for
   fitting CTs should not rest on "an undetected weld exists" alone — item 4a
   below shows the series topology already tolerates a *single* weld of
   either device without immediate uncontrolled heat. What CTs concretely add
   is (a) S9 — direct detection of "K4 was just dropped and current is still
   flowing," which is exactly the residual case the topology admits it cannot
   defend against (a welded contactor), and (b) S3 — detection of a stuck SSR
   with no heat commanded, closing the *other* half of the double-failure
   pair before it becomes double. That is a precise, load-bearing addition,
   not a hedge against an unquantified fear — it is the one check this board
   structurally cannot perform any other way, because nothing on either
   processor can otherwise tell "the relay/contactor did what I asked" from
   "it didn't."

4a. **Topology finding, added this pass (2026-09-04): the two relay
   authorities gate two different, staged points, not one shared point** —
   `firmware/SaftyFW/docs/HARDWARE.md` §3 (schematic-traced 2026-08-16,
   `SSD.kicad_sch`, cross-checked this pass against
   `hardware/mainBoard/kiln.kicad_pro` via the KiCad MCP's
   `get_kicad_component_connections`; that live query returned no populated
   nets for K1 or K4, so it neither confirms nor contradicts the documented
   topology — the netlist appears stale/unbuilt, not evidence of anything).
   Every relay on the board (K1, K2, K3, K5 on the ESP; K4 on the RP2040) is
   a **pilot relay only** — none carries element current:

   ```
   mains ──> [ line contactor ] ──> [ SSRs, one per zone ] ──> heating elements
                    ^                        ^
                    │ coil                   │ control
              K4 (RP2040, pilot)      K1/K2/K3/K5 (ESP, pilot, one per SSR)
   ```

   K4 energized = contactor coil energized = contactor closed; K4 dropping is
   *supposed to* open the contactor and kill power to the entire SSR stage,
   independent of what the ESP is commanding. The ESP's K1–K3 separately gate
   each zone's own SSR downstream of the contactor. **Single-failure table**
   (argued from this schematic-derived topology, not bench-proven):

   | Failure | Does heat flow? | What (if anything) stops it | Detected today? |
   |---|---|---|---|
   | K1 (or K2/K3) welds closed, K4/contactor healthy | Only if that zone's SSR also conducts, same as normal commanded-on operation | **Yes** — SaftyFW cannot see K1's coil state, but it *can* see the downstream effect (overtemp via its own thermocouple → S1; or current-with-no-context via S3 if CTs were fitted) and drop K4, which cuts power to the whole SSR stage including the stuck channel. This is the series protection working as designed. | Overtemp path yes (S1, CT-independent); S3's more specific signature is CT-gated, currently inert |
   | K4 welds closed (or its coil-drive path sticks), contactor coil stuck energized, SSRs healthy | Not by itself — SSRs still individually gate their elements | **Nothing.** SaftyFW's every guard trip (S1, S2, S6a/b, S7, all of them) still computes and still commands K4 off, but that command no longer has any real-world effect once the contactor is welded. This is the finding that matters most: it is not "S9 doesn't fire," it is "the entire independent veto authority is silently disabled while every other guard keeps reporting as if it still worked." | **No** — no guard checks that a K4-off command actually opened the contactor; that check *is* S9, and S9 is CT-gated |
   | Line contactor's own mains contacts weld (the contactor itself, not K4) | Same as the K4-welds row — contactor stuck closed regardless of K4's state | Same as above — nothing, until a second failure (a stuck SSR, or the ESP alone running past its own limits) turns it into real uncontrolled heat with zero independent backstop | **No** — same gap; this and the K4-weld row are operationally indistinguishable from SaftyFW's point of view |
   | A per-zone SSR welds/shorts closed, contactor healthy | Yes, for that zone, regardless of ESP command | **Yes** — K4/contactor dropping cuts input power to the whole SSR stage; this is explicitly why the contactor sits upstream of the SSRs (`HARDWARE.md` §3: "It does defeat a welded/shorted SSR... the reason guard S3 exists") | S1 overtemp path yes; S3's specific "current with no heat commanded" signature is CT-gated, currently inert |
   | Contactor welded **and** a per-zone SSR also welded/stuck (or ESP commands heat with no independent ceiling) | **Yes, uncontrolled** | **Nothing independent of the ESP.** This is the true double failure the series topology is meant to make rare — one authority alone failing is tolerated, both failing (or one failing plus the surviving authority never being asked to intervene) is not | **No** on this rig — S9/S3 (the guards that would catch exactly this) are both CT-gated and inert |
   | Safety link lost (UART/isolator failure) | No new heat commanded; a running firing continues until ESP-side 1.5s/30s timers act (H6) | K4 defaults to whatever SaftyFW's own guards decide independently — link loss does not by itself change K4/contactor state, it changes whether the ESP will *ask* for more heat | Covered by H6, not a new SPOF for H3 |

   **The corrected headline:** "nothing detects a welded contactor" (established
   `8955763`) is true and unchanged. It is also incomplete on its own — a
   welded contactor in isolation does not immediately cause uncontrolled
   heat, because the SSR stage downstream still individually gates each
   element. What it *does* do is quietly convert this board from a
   dual-authority system into a single-authority (ESP-only) one for every
   hazard SaftyFW's guards address — S1 overtemp, S2, S6a/b, S7 included —
   because their only real-world effect runs through the contactor. The
   genuinely single-point failure is therefore twofold, not one thing: the
   **line contactor's own contacts** (whose failure disables the *entire*
   RP2040 veto, not just H3's guard) and **each zone's own SSR** (whose
   failure is normally still caught by that same veto, unless the contactor
   has independently failed too). A fire requires the double failure, not
   the single one — but today, on this CT-less rig, there is no independent
   signal that the first half of that pair has already happened, so an
   operator has no way to know they are one failure away from the double
   case rather than two.

   **What this pass verified versus inferred:** the topology (contactor
   upstream of SSRs, all relays pilot-only) is read from
   `firmware/SaftyFW/docs/HARDWARE.md` §3, which cites its own schematic
   trace (`SSD.kicad_sch`, 2026-08-16) and includes the schematic's own
   "not rated for use with heaters" annotation on K4 as corroboration. This
   pass did not independently re-derive the mains-side wiring from the
   netlist (the KiCad MCP query above came back empty) and, per this task's
   constraints, did not and could not touch a real contactor or SSR to
   confirm the failure modes above on hardware. **Open question for the
   owner, not answered by any document in this repo:** what contactor and
   SSR model numbers are actually wired at J8/J3/J4/J11/J10 on this specific
   physical rig, and has either ever been inspected for contact condition?
   Nothing in the schematic or firmware can answer that — it is a fact about
   the owner's own external wiring.

   **Detection options beyond CTs, assessed 2026-09-04:**
   `docs/CONTACTOR_FEEDBACK_OPTIONS.md` — owner decision, no recommendation.
   Headline: every K1–K5 pilot relay (K4 included) is DPDT with one entire
   Form-C contact set wired to nothing (schematic-confirmed, `SSD.kicad_sch`),
   and the ESP already has a spare opto-isolated input landed on an external
   screw terminal (`IO_1`/J24) doing nothing today — either could carry a
   contactor auxiliary-contact signal if the physically fitted contactor has
   one (unverified — owner question). Also confirms
   `relay_owner_is_energized()` (`firmware/SaftyFW/src/tasks/relay_owner.c`)
   is a software mirror of the command, never a pin read-back, so it cannot
   itself detect a weld. CTs and contactor feedback are argued as
   complementary, not alternatives: CTs catch a welded SSR (via the current
   symptom) and confirm after the fact; contactor feedback catches the
   contactor specifically and can do so before any current-based symptom
   appears — neither substitutes for the other, and landing a feedback
   signal on the ESP rather than the RP2040 trades away the independence
   that makes it valuable in the first place.

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

9. **RESOLVED, corrected 2026-09-04 (was stale, not open).** This item
   previously claimed `SAFETY_FAULT_SRC_APP` latches forever on a transient
   control-task stall recovering on its own (guard 9, "found on the bench
   2026-08-25" — that bench-discovery claim was already retracted elsewhere
   in this document as unsupported). Re-checking the underlying claim itself
   against the current tree shows it is also stale: the fix
   (`guard9_assert_stale_tick_fault()` ORing into `s_exec.global_fault_source`,
   cleared by `profile_executor_halt()` → `clear_this_runs_faults()`) landed
   in `0d85dbb` on 2026-08-27, two days after this item was written, and this
   document was never revisited. It is host-tested against the real
   functions (`App/test/test_profile_executor_prestart.c`, "the operator's
   halt deasserts `SAFETY_FAULT_SRC_APP`, closing the loop this defect left
   open"). The remaining behavior — the fault requires an operator halt to
   clear rather than self-clearing the instant the tick resumes — matches
   every other `thermal_guard` trip's acknowledgment-required pattern and is
   not treated as a defect elsewhere in this document; see
   `firmware/KilnFW/docs/SAFETY_MODEL.md` for the full correction.

10. **`/api/readiness` is a REAL interlock, on both firing and autotune, for
    five of its items; advisory for the rest** (owner decision 2026-09-09,
    firing gate implemented first, autotune gate extended the same day, a
    fifth item — `safety_ceiling_match` — promoted into the same blocking set
    2026-09-14; supersedes this item's previous text, which stated that
    readiness gated nothing at all — that was accurate when written and is no
    longer, and a version of this item that named only the firing path would
    itself now be stale).

    - **What changed.** `App/drivers/safety/readiness_gate.h` turns five
      checklist items into a refusal on every firing-start path AND every
      autotune-start path: `recovery_mode`, `safety_trip`, `crash_report`,
      `estop_verified` and `safety_ceiling_match`. There is exactly one decision function
      (`readiness_gate_refuses_start()`), called from two choke points:
      - `profile_executor_run()` — the single choke point all firing-start
        paths funnel through (POST `/api/profile_exec/start`, both LCD start
        buttons, and the benchproto RUN command).
      - `autotune_begin_run_locked()` — the single choke point all
        autotune-start paths funnel through (the step-test, target-step and
        relay-feedback entry points all call it before doing anything else,
        which in turn are reached from POST `/api/autotune/start` and the
        benchproto AUTOTUNE command; there is no LCD autotune-start button).
      Autotune commands heat through the same relays a firing does, so a gate
      on firing alone left a second, equally capable path to heat completely
      unguarded — an unverified E-stop or an unacknowledged crash report
      blocked a firing but not an autotune run until this extension.
      `profile_exec_start_post_handler()` and `autotune_start_post_handler()`
      each additionally check it before reading the request body, purely so
      the HTTP client gets a `409` naming the item rather than a generic
      `400` — a legibility duplicate of the same call, not a second rule.
    - **The rest of the board's heat-causing surface — checked, NOT fully
      covered.** Firing and autotune are not the only ways this board can
      energize a heater relay. `kiln_io_owner.c`'s `relay_on_blocked()` names
      itself "the ONE choke point every MANUAL relay-ON command reaches":
      `kiln_io_owner_command_set_relay()`/`_set_relay_mask()`, reached from
      the LCD's manual override (`ui_page_temperature.c`), the benchproto
      `SET_RELAY`/`SET_RELAY_MASK` commands (`uart_bridge_io.c`), and — only
      while danger mode is armed — POST `/api/diagnostics/danger/relay`
      (`diagnostics_http.c`).

      **Re-verified 2026-09-15, one route name corrected.** This bullet
      previously named POST `/api/relay` as a live manual-relay route. That
      endpoint does **not** exist at HEAD: `relay_post_handler()` was deleted
      2026-08-27 together with its only caller `manual_page.html`
      (`dashboard_http.c`, the comment block where the handler used to be).
      The shared gate + write it called, `dashboard_set_relay()`, is
      untouched and still shared — only the HTTP door is gone. The surviving
      manual-relay surface is the three callers named above. `zones_current_sweep_engine.c`'s CT-calibration
      sweep (`zone_sweep_hw_energize()`) also drives relays directly, through
      `kiln_io_owner_command_set_relay_mask()`, outside both
      `profile_executor_run()` and `autotune_begin_run_locked()`.
      **Neither path calls `readiness_gate_refuses_start()`.** Both are gated
      only by `relay_authority_on_blocked()` (a latched safety trip — one of
      the five items, enforced here independently of the readiness checklist)
      and the OTA-update interlock — `recovery_mode`, `crash_report` and
      `estop_verified` do not block either one. Manual relay control is
      additionally the one place `danger_mode_active()` (the diagnostics
      page's explicit-accept bench-test mode) deliberately bypasses even
      that safety-trip/OTA gate, by design, so an operator can bench-test a
      relay/contactor with nothing fighting the test — this is pre-existing,
      documented behavior (`danger_mode.h`), not a gap introduced here.
      This was a real gap, **re-confirmed in code at HEAD 2026-09-15**: an
      unverified E-stop or an unacknowledged crash report would refuse a
      firing or an autotune run but would NOT refuse a manual relay-ON (LCD
      override, benchproto `SET_RELAY`, or the danger-mode route) or a
      CT-sweep write. Raised in
      `docs/audits/manual_relay_readiness_gating_options_2026-09-15.md`,
      which laid out four options; **the owner chose Option B the same day,
      and it has now been implemented.** `relay_on_blocked()` gains a fourth
      check, `crash_report_has_unacknowledged()` — a cached, no-I/O flag
      (`App/drivers/safety/crash_report.c`/`.h`) kept valid from
      `crash_report_init()` in early boot (before `boot_guard_init()`, before
      `kiln_io_owner_start()` — including throughout RECOVERY MODE) and
      refreshed by `crash_report_acknowledge()`/`crash_report_clear()`. It
      refuses with a distinct `KILN_IO_OWNER_RELAY_ERR_CRASH_UNACK` /
      `DASHBOARD_RELAY_ERR_CRASH_UNACK` result (same pattern as
      `..._ERR_UPDATING`) so callers do not misreport it as a live safety
      fault, and covers the LCD, benchproto and CT-sweep paths in this one
      choke point. Danger mode still bypasses it, logged the same way it
      already logs bypassing the OTA-update interlock. **`recovery_mode` and
      `estop_verified` remain deliberately ungated on manual control** — the
      owner's stated reasoning (option doc §8): `recovery_mode` names the
      exact situation manual control exists for, and gating
      `estop_verified` risks locking an operator out of the procedure that
      sets it. Relay-OFF is never blocked by any of this.
    - **NO OVERRIDE.** There is deliberately no password bypass, no
      confirm-dialog escape and no `force` parameter. If the E-stop interlock
      is unverified, the board does not fire. This was the owner's explicit
      instruction, not an implementation default.
    - **One definition of each rule.** The gate does not decide, for itself,
      what "tripped" or "unverified" means. Each item is decided by calling
      the SAME `readiness_*_status()` pure predicate `readiness_http.c`'s JSON
      handler calls to render that item, and blocks on exactly one condition:
      *the gate blocks item X iff item X's displayed status is
      `READY_NOT_DONE`*. That makes the page and the interlock structurally
      incapable of disagreeing — the failure this repo has hit four times as
      the "reset one side of a pair" class. Held in place by
      `test_readiness_gate.c` (the biconditional, over all 64 fact
      combinations) and `check_readiness_gate_display_agreement.ps1` (that
      both files still route each item through the same predicate and the same
      item key).
    - **What is genuinely NEW enforcement.** Only `estop_verified`. The other
      three were already refused on the start path or just before it, and
      making them explicit here removes surprise rather than adding
      protection — with one real gap closed: an unacknowledged
      **`crash_report`** was previously enforced ONLY by the PC-side
      `capability_preflight`, so a start from the LCD or the web UI on a
      crashed board was accepted. It is now refused by firmware too.
      Per item, as verified 2026-09-09:
      - `recovery_mode` — already enforced: `main_control_bringup.c` never
        calls `profile_executor_start()` in recovery mode, so `s_exec.lock` is
        NULL and every entry point refuses; plus the explicit, shared-wording
        refusal in `system_mode_gate.c` (docs/SYSTEM_MODE_GATE_PLAN.md slice 2,
        2026-09-27 -- formerly a separate, HTTP-only `recovery_start_refusal.h`).
      - `safety_trip` — already enforced: `profile_executor_run()`'s
        `relay_authority_on_blocked()` check refuses a start while heat
        authority is blocked, which a latched trip does; and the Pico refuses
        to enable heat regardless.
      - `crash_report` — was **not** enforced in firmware (only
        `tools/PcTools/src/kilnctrl/capability_preflight.py`). Now enforced.
      - `estop_verified` — was enforced **nowhere**. Now enforced. Pole 1 of
        the E-stop (in series with the external contactor coil) is wiring
        firmware structurally cannot observe, so the honest coverage is a
        documented bench procedure plus a durable operator record
        (`estop_verification.h`); this gate is what makes that record matter.
    - **What stays advisory, deliberately.** `guard_max_temp`, `hardware`,
      `safety_context`, `cfg_fs`, `network`, `commissioning` and the rest do
      NOT block. `guard_max_temp` in particular already has a better refusal
      deeper in `profile_executor_run()` (the guard-5 zone-ceiling check) that
      knows which zones the profile actually uses; the checklist item is a
      whole-board summary and would refuse firings guard 5 correctly allows.
      Adding an item to `readiness_http.c` does not add it to the interlock.
    - **Operator legibility.** A refused start names the single blocking item
      and the action that clears it, front-loaded so a truncating LCD dialog
      still shows which item refused. The HTTP `409` carries the item's
      `/api/readiness` key as `readiness_item`, and the dashboard shows the
      message and links straight to that row on the readiness page, for both
      the firing and the autotune start endpoints. The readiness page itself
      states which five items refuse a firing (not yet reworded to also
      mention autotune explicitly — the wording predates this extension).
    - **Interaction with the reboot/S6a sequence.** Rebooting reliably latches
      an S6a main-fault trip (see `sw_reset_http.c` and the Reboot section of
      `settings_page.html`). With `safety_trip` a hard gate, a reboot now
      leaves the board unfirable AND unable to start autotune until the
      operator clears the trip. That is correct, and the sequence — reboot,
      clear trip, start — is stated in the reboot copy and repeated in the
      refusal message itself.
    - **Not hardware-verified.** Implemented and host-tested only (the
      autotune extension's own coverage:
      `test_autotune_engine_prestart.c`'s `test_begin_run_refused_by_
      readiness_*` tests and `test_begin_run_passes_the_readiness_gate_
      when_ready()`, driving the real `readiness_gate_refuses_start()` the
      same way `test_profile_executor_prestart.c`'s equivalent tests do); no
      board
      was flashed for this change.

11. **Refusal of a relay-on command is invisible on the wire** — a refused
    `SET_RELAY` produces no reply; a GUI infers it only from state not
    changing within one report period. No explicit "refused, and here is why"
    signal exists today.

12. **On/off zones (`ZONE_TYPE_ON_OFF`) have no welded-contactor detection on
    the ESP — accepted coverage gap, added 2026-09-14.** Guard 3 (`RUNAWAY`)
    infers a welded output from "temperature rising while the zone is
    commanded off"; an on/off zone's channel cannot express that signature
    (it may be driving a vent/damper/fan with no rise-while-off relationship
    to its own duty at all), so guard 3 is disabled outright for this zone
    type (`docs/ON_OFF_ZONE_PLAN.md` sec 1's guard table, guard 3 row). No
    replacement runs on the ESP. The only welded-contactor coverage that
    could apply to this output is CT-based (S14/S15 on the Pico) or physical
    contactor feedback, both out of scope for this feature and, per item 4
    above, **already structurally inert on this bench rig today**
    (`ct_installed = 0`). This is an accepted, argued gap, not a defect: a
    correctly-working on/off device's whole operating signature ("duty high,
    measurement flat or falling") is indistinguishable from guard 3's own
    trip condition, so no correct implementation of guard 3 could run on
    this zone type without false-tripping on ordinary use. **Argued only —
    no host test proves an absence, and no board on this bench has ever run
    an on/off relay.**

13. **`max_temp_c == 0` no longer unconditionally means "uncommissioned,
    refuse to start" — the one relaxation ever made to that settled
    semantics, added 2026-09-14.** For a `ZONE_TYPE_HEATER` zone, `max_temp_c
    == 0` still means exactly what it always has: an unmeasured heating
    ceiling is lethal, so the executor refuses to start
    (`profile_executor_start.c`/`profile_executor_run.c`'s existing
    `zone_needs_ceiling()` check). For a `ZONE_TYPE_ON_OFF` zone with **no
    thermocouple assigned** (`thermo_mask == 0`, legal only for this zone
    type per `docs/ON_OFF_ZONE_PLAN.md` sec 2), `max_temp_c == 0` does
    **not** block the run — there is no measured channel for a ceiling to
    apply to, so treating an unmeasured fan/damper the same as an unmeasured
    heater would refuse a legitimate, harmless configuration for no safety
    reason. The single predicate `zone_needs_ceiling(zi) = (zone_type ==
    HEATER) || (thermo_mask != 0)` is the one place this distinction is
    made, so a TC-equipped on/off zone (recommended whenever a temperature
    trigger is used) is unaffected and still gets a ceiling requirement like
    any measured channel. **Host-tested**: `test_zones_http.c:7174`'s
    `test_zone_is_on_off_and_zone_needs_ceiling()` exercises `zone_needs_
    ceiling()` directly for all three cases (HEATER always needs one, on/off
    WITH a TC still needs one, on/off with NO TC does not). Not
    hardware-verified — no on/off zone has ever actuated a physical relay
    (step 9 of `docs/ON_OFF_ZONE_PLAN.md`, still open).

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
| S3 reachability | argued | `current_sense_set_cal()` confirmed called, `current_task.c:243` (line drifted from a prior `:197` citation, re-confirmed 2026-09-17) |
| S3 hardware trip | **not done** | §3.4 rows unexecuted — "no hardware was touched to write this" |
| S4 load-inactive WARN | host-tested | §2 provocation table |
| S5 sensor validity (incl. per-tc_type band, CR1 readback) | host-tested | `test_max31856_tc_range_policy.c`, `test_max31856_decode.c` |
| S5 hardware fit (safety TC physically present) | **hardware-verified** | 2026-08-24, live link read "30.20 C (CJ 28.08 C)", `GUARD_TEST_MATRIX.md` §6a |
| S5 masking-before-fit finding | hardware-verified | same bench session; explains why no other guard had ever transitioned on this board before that date |
| S6a main-fault trip | argued | reachable in source (`safety_guards.c:459-462`, the S6a check itself — line drifted from a prior `safety_core.c:1073` citation, which is a different backstop entirely; re-confirmed 2026-09-17) since the wiring commits; **cannot be provoked by any current host fixture** — needs bench hardware, permanently (no I2C-expander/opto emulation exists or is planned) |
| S6b link-dead (both tiers) | host-tested | §2 provocation table (10s soft, 120s hard) |
| S6b hardware | **not done** | §3.4 row unexecuted |
| S7 E-stop trip logic | host-tested, negative-tested | `test_discrete_pin_policy.c`; polarity-inversion bug reintroduced and confirmed to fail 4+2 assertions, then restored (2026-08-24); relay-deenergize end-to-end pinned by `firmware/SaftyFW/test/test_estop_deenergizes_relay.c` |
| S7 pole 2 (telemetry) press-test | bench-verified procedure + durable record | `firmware/SaftyFW/README.md` "Bench verification procedure"; `estop_verification.c`, gated by `estop_verified` on `/api/readiness` |
| S7 pole 1 (physical interlock) | **not applicable to this fixture — closed by owner decision, not a gap** | owner, 2026-09-10: pole 1 will not be wired here; E-stop is firmware-mediated only on this fixture, `HARDWARE.md` §5.1 |
| S8 rate-of-rise (pure logic + config wiring) | host-tested | `test_s8()`, `test_safety_core_s8_wiring.c`, 2026-09-03 |
| S8 hardware / real threshold | **not done, and cannot be until a ramp is measured** | ROADMAP.md M3 |
| S9 trip-ineffective escalation logic | host-tested | §2 provocation table |
| S9 reachability | argued | `safety_core.c:1278` (`.relay_deenergized = !relay_owner_is_energized()` input wiring; line drifted from a prior `:1109` citation, which is the unconfigured-armed backstop, not S9 — re-confirmed 2026-09-17) |
| S9 hardware (real welded contactor) | **not done — no jig exists** | ROADMAP.md M4, explicit |
| S10 chamber-disagreement WARN | host-tested | §2 provocation table |
| S11 frozen-sensor trip | host-tested | §2 provocation table |
| S11 hardware | **not done** | §3.4 row unexecuted |
| S12 cold-junction WARN | argued + host-tested (ungated) | reads real snapshot, unconditional |
| S13 borrowed-zone staleness | host-tested | reachable in source (`context_borrowed_sample_counter_advancing()`); commissioning-gated off by default |
| S14 per-channel overcurrent WARN | host-tested | new guard 2026-08-28, `test_ct_disabled_guards()` pair-tested |
| S15 shared-CT commanded-sum-vs-measured deficit WARN | host-tested | new guard 2026-09-06, `test_s14_s15_summed_topology()` in `firmware/SaftyFW/test/test_safety_guards.c`; inert unless `ct_topology = summed`, which this board has not commissioned (`GUARD_TEST_MATRIX.md` §3.3/§3.4) |
| Relay-authority gate (KilnFW, all three callers) | argued + host-tested per caller | `relay_authority.{c,h}`; UART bridge, diagnostics HTTP, profile executor all confirmed routed through it |
| KilnFW thermal_guard guards 1,2,4,5,7 | **host-tested, not hardware-verified** | `App/test/test_thermal_guard.c` exercises the real `thermal_guard_tick()` (not a stub) for each of these, including override/arming/regression cases (e.g. `"guard 1 trips when commanded heat produces far less than sanity_rate_c_per_min"`, `"guard 4 eventually trips a zone that starts hot and never settles"`); no bench provocation of any of these five is on record (`SAFETY_MODEL.md` summary table, corrected 2026-09-04 — a previous pass of this row read "not yet live-tested" as "not tested at all" and understated the coverage; see `SAFETY_MODEL.md`'s own disagreement note) |
| KilnFW thermal_guard guard 6 (sensor validity) | hardware-verified | live-verified end to end, no TC attached, trip fired after exactly 3 bad reads, board relay stayed off (`SAFETY_MODEL.md`); also host-tested against the real function (`App/test/test_thermal_guard.c`) |
| KilnFW thermal_guard guard 3 (relay welded, thermal sanity) | host-tested, not hardware-verified | `App/test/test_thermal_guard.c`, including a named hardware-motivated regression case ("found on hardware 2026-08-12 against the simulated…") and the `runaway_margin_c` override; the only hardware contact this guard has had is the false-positive it was tuned against, not a genuine positive trip |
| KilnFW thermal_guard guard 9 (control-task stall) | host-tested (trip/priority AND fault-clear), not hardware-verified | the tick-stale fault path itself is a real-function test, not a stub (`App/test/test_safety_watchdog.c::test_tick_stale_still_faults_running_and_takes_priority`); the `SAFETY_FAULT_SRC_APP` fault-clear path is also a real-function test, not a stub (`App/test/test_profile_executor_prestart.c`, "the operator's halt deasserts `SAFETY_FAULT_SRC_APP`, closing the loop this defect left open") — the "never clears" defect this row used to describe was fixed in `0d85dbb` (2026-08-27), before this table's most recent correction pass, but that pass missed it; corrected 2026-09-04. No commit or bench record around 2026-08-25 (or any other date) shows a genuinely provoked control-task stall on real hardware — see `SAFETY_MODEL.md`'s own correction, which resolves the "found on the bench" wording the same way |
| KilnFW thermal_guard guard 8 (cross-zone plausibility) | **not built** | unimplemented, needs concurrent multi-zone execution |
| Link-loss 30s firing-abort (KilnFW side) | host-tested | `test_safety_link.c:77-92`, pinned 2026-09-04 |
| Link-loss 30s firing-abort, real bench | **not done** | ROADMAP.md: "Code is flashed; nobody has held the link down" |
| OTA interlock vs link-loss heating-block | host-tested (CI-pinned) | 2026-09-04 |
| OTA interlock vs link-loss, real bench | **not done** | ROADMAP.md M8/M13, explicitly still open |
| E-stop polarity fix itself | hardware-relevant, negative-tested at unit level | fixed 2026-08-24; "nothing in the host suite or this fixture could have caught it" before the pure-policy extraction — worth noting the *original* bug shipped invisibly for a time |
| `virtual_dut`/SimFW cross-check evidence generally | **withdrawn** | tool deleted 2026-08-28; every "Yes" reachability verdict that cited it now rests on source-reading alone (method 1), re-confirmed independently in §6c |

**Rollup (guard-level rows above, S1–S15 plus the two KilnFW-side items called
out separately):** roughly 21 discrete claims tracked here — **21
host-tested** (S1–S15's logic rows plus KilnFW guards 1,2,3,4,5,6,7, and
guard 9's trip/priority path **and** its fault-clear path, corrected
2026-09-04 — see the guard 9 row above), **3 hardware-verified** (S5's
hardware fit, S5's masking-before-fit finding, and KilnFW guard 6 — the
three rows in the table above actually labeled "hardware-verified"; the
E-stop polarity fix is deliberately excluded here, since its own row is
labeled "hardware-relevant, negative-tested at unit level," not
"hardware-verified"), and the remaining
**~8 explicitly marked "not done"** for hardware. What remains **argued
only** is narrower than a previous pass of this table claimed, and now
narrower still: S7's pole-2 press test is bench-verified (not argued), and
S7's pole 1 is not a gap at all — closed 2026-09-10 as not applicable to this
fixture (owner decision). What's left argued-only is S6a's permanent
hardware-only status. Guard 9's
fault-clear path is no longer argued-only — the underlying "never clears"
defect it used to describe was fixed in `0d85dbb` (2026-08-27) and is now
host-tested against the real function. KilnFW guards 1/2/3/4/5/6/7 are
**not** argued-only — see the corrected rows above. No claim in this
document is stronger than its weakest supporting sentence in the source
docs; where a source hedges, this table hedges identically.

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

6. **Corrected again 2026-09-04 (same day, later pass):** the above still
   treated guard 9's `SAFETY_FAULT_SRC_APP`-never-clears defect as open and
   argued-only. It is not — the fix landed in `0d85dbb` on 2026-08-27, two
   days *before* this file's original hazard-list item 9 was even written,
   and is host-tested (`App/test/test_profile_executor_prestart.c`, "the
   operator's halt deasserts `SAFETY_FAULT_SRC_APP`, closing the loop this
   defect left open"). Every prior correction pass re-litigated the "found
   on the bench" phrasing without re-checking whether the underlying defect
   still existed in code; it did not. See §3 item 9 and the §4 guard 9 row,
   both corrected in this same pass, and `SAFETY_MODEL.md`'s matching
   correction.

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
