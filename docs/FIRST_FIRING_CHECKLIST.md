# First-Firing Commissioning Checklist

Closes `docs/RELEASE_HARDENING.md` blocker 8. This is the procedure an
owner follows the first time a REAL kiln — not the bench fixture — is fired
under this controller. It is written for the owner, not a firmware
developer: every step names what to do, what to read back, and what to do if
that reading is wrong. Nothing here requires the MCP developer tools
(`kiln_call`/`kiln_batch`); every check uses the LCD, the web UI, or a plain
HTTP GET/POST (curl or a browser).

**Do not skip a step to save time on a kiln that is about to be loaded with
ware.** Every step exists because the bench fixture (a genuine ~4 W load at
120 V, `docs/audits/guard_bench_provocations_2026-09-16.md`) cannot arm the
guard the step is checking. `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md` and
`docs/SAFETY_CASE.md` are the two authorities this checklist draws its
guard-status claims from; `docs/audits/guard_bench_provocations_2026-09-16.md`
records exactly which guards a 2026-09-16 bench session did and did not
manage to trip, and the ones it could not are this document's subject
matter.

Do this once, in order, before the first firing of any kind — including a
"just to see if it heats" test. Stop and fix, not work around, on any failed
observation.

---

## 0. What this checklist does not cover

It does not cover configuring zone counts, thermocouple placement mode, or
profile data — that is `docs/SETUP_WIZARD.md`'s job, and it must be done
first. This checklist assumes the wizard has already run once and the
controller boots to a normal (not recovery-mode, not crash-report-pending)
state. Check that now:

- Open the dashboard home page. **Pass:** no red banner, no "RECOVERY MODE"
  text, no "unacknowledged crash" text.
- **If wrong:** do not proceed. A board in recovery mode or with an
  unacknowledged crash report is refused by the readiness gate anyway
  (`recovery_mode`, `crash_report` are hard-blocking items there), and
  chasing a commissioning failure on top of an unrelated boot problem wastes
  time. Resolve the boot issue first.

---

## 1. Pre-power: wiring and continuity

Do this with the kiln UNPLUGGED from mains and the controller unpowered.

1. **Contactor coil circuit.** Confirm with a multimeter that the K4
   contactor coil is wired in series with the controller's relay output, not
   directly across mains. **Pass:** continuity from the relay output
   terminal to one coil terminal, and from the other coil terminal back to
   the controller's return path, with the relay in its de-energized
   (open) rest state reading open circuit across the coil.
   **If wrong:** stop. This is the single most safety-critical wiring fact
   in the system and cannot be verified any other way once mains is live.
2. **Thermocouple continuity per zone, per processor.** Trace each
   thermocouple's two leads from its physical position in the kiln to its
   labeled connector (three channels feed the ESP's `ThermocoupleBoard`,
   one separate channel feeds the RP2040 safety processor's own
   `SaftyThermocoupleBoard` — four MAX31856 devices total, per
   `CLAUDE.md`'s "Multi-Channel Thermocouple Interface" section). **Pass:**
   each physical zone position matches its intended connector label.
   **If wrong:** relabel or rewire before power-up — a swapped pair is
   caught electrically in step 5 below, but tracing it here is cheaper than
   diagnosing it live.
3. **E-stop button and pole 2 wiring.** Confirm the E-stop's pole 2 contact
   is wired into the safety processor's GPIO9 input path (`R10`/`C3` per
   `firmware/SaftyFW/docs/HARDWARE.md`'s pinout table), and that pole 1 (the
   second, physically independent contact) is wired in series with the
   contactor coil itself — not merely to another GPIO. **Pass:** visual and
   continuity confirmation of two mechanically separate contacts on two
   separate paths. **If wrong:** do not fire. Pole 1 is the one guard this
   entire codebase cannot verify in software at all
   (`docs/SAFETY_ARGUMENT_WITHOUT_BENCH.md:161`: "the installed kiln's
   first-boot checklist (plan item 8) must include verifying pole 1's
   physical interlock, since this bench can say nothing about it at all").

---

## 2. Power up, confirm link and clear the boot trip

1. Power the controller only (mains to the kiln elements still off /
   contactor de-energized is fine — the controller runs on its own low-
   voltage supply). Wait for the LCD to show its normal home screen.
2. Open `/api/safety/commissioning` in a browser (GET) or the dashboard's
   safety page (`/settings/safety`, registered
   `firmware/KilnFW/App/drivers/http/zones_http.c:766`). **Pass:** the page
   loads and shows a safety-link status of "up"/connected.
   **If wrong:** the RP2040 safety processor is not answering. Do not
   proceed to any of the checks below — none of them are meaningful without
   a live safety link.
3. A dual power-up commonly leaves a `SAFETY_TRIP_MAIN_FAULT` (S6a) trip
   latched while the link handshake finishes — this is expected, not a
   defect. Confirm the trip reason is exactly this and nothing else, then
   clear it: `POST /api/safety/clear_trip`
   (`firmware/KilnFW/App/drivers/http/dashboard_http.c:718`). **Pass:**
   after the POST, the safety page shows no active trip. Clearing is a
   request the safety processor re-validates, not an order it obeys blindly
   — if the underlying fault is still present the trip will not clear.
   **If wrong (trip persists after clearing, or a different trip reason is
   shown):** stop and diagnose that specific fault before continuing; do not
   repeat the clear call hoping it goes away.

---

## 3. E-stop function, both poles

1. **Pole 2 (firmware-visible).** With the controller idle (no heat
   commanded), press the E-stop button. **Pass:** the safety page's status
   flips to E-stop asserted within a couple of seconds, and any commanded
   relay output drops. Release the button and confirm the flag clears.
   **If wrong:** pole 2's wiring or GPIO9 read is faulty — fix before
   proceeding, since this is the only E-stop path this firmware can see at
   all.
2. **Pole 1 (physical, not firmware-visible — attest, don't trust a
   reading).** With mains live and the contactor test-energized (see step 6
   below for how to do this safely at low temperature), press the E-stop
   and confirm by direct observation — listening for the contactor to
   physically drop, or measuring continuity across its output — that power
   to the elements is cut. This step cannot be automated or read off any
   API, because pole 1 is deliberately outside the firmware's visibility
   (`docs/SAFETY_ARGUMENT_WITHOUT_BENCH.md:161`). **Pass:** the contactor
   audibly/mechanically opens on E-stop press, independent of anything the
   firmware reports.
   Once confirmed, record the attestation in software:
   `POST /api/estop/verify` (registered
   `firmware/KilnFW/App/drivers/http/diagnostics_http.c:520`, backed by
   `estop_verification_confirm()` in
   `firmware/KilnFW/App/drivers/safety/estop_verification.h`). Confirm via
   `GET /api/readiness`
   (`firmware/KilnFW/App/drivers/http/readiness_http.c:915`) that the
   `estop_verified` readiness item now reads true.
   **If wrong (contactor does not open on E-stop):** do not verify — leave
   `estop_verified` false and do not fire. This attestation is durable (NVS-
   persisted) and only clears itself on an E-stop polarity change or a
   factory reset, so it is meant to be done exactly once, correctly.

---

## 4. Thermocouple identity and type, per zone, on both processors

Zones are stacked vertically in this kiln: **zone 2 is the bottom, zone 0 is
the top** (`docs/audits/high_temperature_transfer_analysis_2026-09-08.md:158`:
"Zones are stacked vertically with z2 at the bottom
(`project_zone_physical_arrangement`)"). A swapped thermocouple pair between
two zones is a realistic miswire and must be caught here, not discovered
mid-firing as a control anomaly.

1. With the kiln at room temperature and no heat commanded, read each
   zone's temperature from the dashboard or LCD. **Pass:** all zones read
   within about 1-2 °C of each other and of ambient room temperature (a
   room-temperature kiln has no reason for the zones to disagree).
2. Warm ONE zone's physical location only — e.g., hold a heat gun or a warm
   object briefly near the BOTTOM of the kiln (zone 2's physical position)
   without touching the thermocouple itself, then remove it. **Pass:** the
   zone 2 reading on the dashboard rises and the others do not (or rise far
   less). **If wrong (the TOP zone's reading — zone 0 — rises instead while
   you warmed the bottom):** the zone 2 and zone 0 thermocouples are
   swapped. Correct the physical wiring and repeat this step before
   continuing — do not attempt to fix a swap in software by relabeling
   zones, since the safety processor's own thermocouple (a separate,
   independent sensor) still needs to agree with whichever zone it is
   configured to watch.
3. **Thermocouple type verification on the safety processor.** A
   thermocouple type mismatch on the RP2040 safety processor does not show
   up as a bad temperature reading — it presents as a specific internal
   flag, `s_tc_type_verified`, going false
   (`firmware/SaftyFW/src/max31856.c:79` defines the flag;
   `firmware/SaftyFW/src/max31856.c:286` computes it from a CR1 register
   readback; `firmware/SaftyFW/src/max31856.c:295` is the accessor that
   downgrades the reading to invalid on a mismatch). The observable
   consequence is the same shape as an open circuit: the safety-side
   temperature reads invalid, not merely wrong. Check the commissioning
   page (`GET /api/safety/commissioning`) or the safety-processor status
   shown there. **Pass:** the safety thermocouple channel shows a valid,
   plausible reading with no "invalid"/fault indication.
   **If wrong:** confirm the physical thermocouple type (e.g. type K vs
   type S) matches the configured `tc_type` for that channel before
   assuming a wiring fault — a correct wire into the wrong type setting
   produces exactly this symptom, not a temperature error.

---

## 5. Ceiling agreement between processors: `abs_max_temp_c`

The Pico's `abs_max_temp_c` must always equal the ESP's — there is no
supported configuration where the Pico's ceiling is tighter, looser, or
simply different from the ESP's; a "second set of eyes" only works if both
eyes are looking at the same number. This equality is checked continuously
in firmware (`tools/check_volatile_ceiling_write_callers.ps1:187`: "the
Pico's abs_max_temp_c ALWAYS equals the ESP's"), but the owner must confirm
it explicitly at commissioning because this value must be raised
Pico-first-then-ESP and the bench's placeholder value is meaningless on a
real kiln.

1. Set the real kiln's maximum allowed temperature. Set it on the Pico
   (safety processor) side FIRST, then on the ESP side, matching the
   documented ordering.
2. Read back both values from `GET /api/safety/commissioning`. **Pass:** the
   page shows one ceiling value and does not show a divergence warning
   between the two processors.
   **If wrong (the two disagree, or the page shows a divergence flag):** do
   not fire. Re-set both sides in the correct order and re-check. Never
   raise the ESP side above what the Pico currently holds, even
   temporarily — an unequal window is a window where the Pico is, in
   effect, not the guard it is supposed to be.
3. Note for anyone importing a saved configuration from another board
   later: importing forces `calibrated = false` and clears the per-zone
   `i_normal_a[]` current baselines
   (`docs/WEB_AUTH_PLAN.md:679`: "the board-identity comparison that forces
   `calibrated = false` and clears `i_normal_a[]` when `source_board` does
   not match the running board"). This import behavior does not touch
   `abs_max_temp_c` itself, but it is called out here because both facts
   live in the same commissioning workflow: an import is not a substitute
   for doing steps 5 and 6 of this checklist on THIS kiln.

---

## 6. Low-temperature dry run, contactor and relay proof

Do this before any real load (no ware in the kiln) and before the CT
calibration in step 7, since a working contactor is a precondition for a
meaningful current reading.

1. With the kiln empty, command a low setpoint (well under any glaze
   temperature — enough to confirm the contactor closes and elements draw
   current, not enough to fire anything). **Pass:** the dashboard shows the
   relay/contactor commanded on, and you can hear/feel the contactor
   physically close.
   **If wrong (relay commands on but the contactor does not close):**
   today this claim is schematic-derived, not bench-provable — this is
   exactly the fact this step exists to establish for the first time. Stop
   and fix the relay/contactor wiring before any further step.
2. Let the low-temperature run continue for a few minutes and confirm
   temperature actually rises on the zone(s) nearest the elements.
   **Pass:** a visible, steady rise (not flat, not erratic).
   **If wrong:** do not proceed to CT calibration or a full firing — a
   controller that cannot produce a confirmed temperature rise under
   commanded heat has a more basic problem than anything below.
3. Command heat off and confirm the contactor drops on command (not just on
   E-stop). **Pass:** contactor opens within the expected control interval.

---

## 7. CT (current sensing) calibration

Current-sensing commissioning is real, owner-performed work, not a
configuration checkbox — an uncalibrated CT leaves every CT-dependent guard
dormant. Per `docs/RELEASE_HARDENING.md`'s blocker-8 text, current-
sensing commissioning under real load is what unlocks guards S3, S4, S9,
S14, and S15 out of their commissioning-blocked state.

1. With the low-temperature dry run from step 6 still drawing real current
   through the installed CTs, run the calibration procedure:
   `POST /api/safety/commissioning/ct_cal`
   (`firmware/KilnFW/App/drivers/http/safety_cfg_http.c:1868`) and, if the
   procedure calls for a zeroed baseline first,
   `POST /api/safety/commissioning/ct_auto_zero`
   (`firmware/KilnFW/App/drivers/http/safety_cfg_http.c:1872`).
2. Read back the result from `GET /api/safety/commissioning`. **Pass:** the
   page reports `calibrated: true` for each installed CT channel, with a
   nonzero, plausible per-zone current baseline (`i_normal_a[]`) recorded
   for the load actually drawn in step 6.
   **If wrong (still shows `calibrated: false` after running the
   procedure):** do not treat an imported/copied configuration as a
   substitute — importing a config from another board deliberately forces
   `calibrated = false` and clears `i_normal_a[]`
   (`docs/audits/kiln_profiles_robustness_2026-09-14.md:485`: importing a
   mismatched board's config "degrades to uncalibrated" rather than
   silently trusting a foreign number). Re-run calibration on THIS board
   with THIS kiln's real load until the page reports `calibrated: true`.
3. Record, per the guard-provocation audit
   (`docs/audits/guard_bench_provocations_2026-09-16.md`), which CT-
   dependent guards are now verifiable for the first time on this
   installed kiln versus which still require a supervised firing or a
   dedicated jig (see section 8 below) before they can be considered
   proven.

---

## 8. Guard-by-guard accounting: what is proven, what is not

Use this table to know, honestly, what this checklist has and has not
established. "Bench-provoked" means the 2026-09-16 bench session
(`docs/audits/guard_bench_provocations_2026-09-16.md`) actually tripped the
guard and confirmed recovery; it does not by itself mean the guard is proven
on THIS kiln's real wiring.

| Guard | Bench-provoked already? | What this checklist does about it |
|---|---|---|
| S1 (`abs_max_temp_c` ceiling) | Yes, executed/matched/cleared/restored on the bench. | Step 5 re-establishes the real ceiling and its Pico/ESP equality on this kiln; the trip mechanism itself does not need re-proving. |
| S6a (main fault / opto path) | Yes, executed/matched/cleared/restored on the bench. | Step 2 exercises the same clear-trip path at real power-up. |
| S13 (commissioning-gap arming) | Yes, executed/matched/cleared/restored on the bench. | Steps 5-7 are exactly the commissioning fields S13 checks; completing them is what arms it here. |
| S6b (link-dead) | Attempted on the bench but not confirmed — the OpenOCD halt used to provoke it does not persist across the guard's wait window. | Not independently re-attempted by this checklist; a genuine link-loss event (unplugging the safety-link cable) during a supervised firing is the honest way to see it, and is deferred to the first attended firing, not this pre-firing checklist. |
| S5 (sensor validity / SPI failure, safety-side TC) | Attempted but not verified on the bench — no register-level fault-injection tool exists for the safety processor's own MAX31856. | Step 4.3 exercises the adjacent, verifiable case (type mismatch via `s_tc_type_verified`), which is the observable this checklist can actually produce. A genuine SPI/wiring failure of the safety TC is only provable by disconnecting it and confirming the invalid-reading behavior described in `firmware/SaftyFW/docs/HARDWARE.md` §8.1 — do this once, deliberately, as an explicit extra check if you want S5 proven before firing. |
| S2, S7, S9, S11 | Not attempted on the bench — need a human at a real bench/kiln, or a supervised firing. | S9 specifically needs a jig that injects real AC current while confirming the contactor de-energizes (`firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md` §3.4); this checklist's low-temperature dry run (step 6) proves the contactor opens on command, which is necessary but not sufficient for S9's full claim. S2, S7, and S11 remain to be exercised during the low-temperature and full-temperature attended firings that follow this checklist, per `docs/RELEASE_HARDENING.md` blocker 8's closure criterion. |
| S3, S4, S14, S15 (CT-dependent) | Cannot arm on the bench at all — the bench has no real current to sense. | Step 7's calibration is what unlocks these from dormant; confirm each shows a live, non-dormant status on `GET /api/safety/commissioning` after step 7, and treat any that still shows dormant as unverified going into the first firing. |
| KilnFW thermal_guard 1/2/3/4/5/7/9 | Not attempted on the bench — need real thermal mass and ramp behavior a 4 W fixture cannot produce. | Left to the low-temperature attended firing (S8's real rate-of-rise threshold, per blocker 8's second bullet, is a related, separate item that also needs a full-power ramp measurement and is explicitly out of this pre-power checklist's scope). |

---

## 9. What comes after this checklist

Completing sections 1-8 above makes the kiln ready for, in order: a
low-temperature attended firing, then a full-temperature attended firing,
then the first unattended firing — each a separate, later step with its own
refusal conditions, per `docs/RELEASE_HARDENING.md` blocker 8's closure
criterion. Do not treat completion of this document as clearance for an
unattended firing by itself.
