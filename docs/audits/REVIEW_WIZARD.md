# REVIEW_WIZARD: setup / commissioning wizard, end to end (2026-10-16)

Read-only review on origin/dev. Scope: `/setup` (`setup_wizard_page.html`, `setup_wizard_http.c`),
`/api/setup/progress` (`setup_progress_http.c`, `persist/setup_wizard_progress.c`), the shared
commit helper `commissioning_shared.js`, the safety-processor write paths it calls
(`safety_cfg_http.c`, `zones_http_post*.c`, `zones_http_pid.c`), the PcTools live driver
(`_web_commission_cdp.mjs`, `web_commission_row.py`, `check_web_commission_cdp_driver.ps1`) and the docs
`firmware/SaftyFW/docs/COMMISSIONING.md`, `firmware/KilnFW/docs/COMMISSIONING_UX.md`, `docs/SETUP_WIZARD.md`.
Paths below are under `firmware/KilnFW/App/drivers/http/` unless given in full.

Totals: HIGH 0, MED 2, LOW 11, INFO 6.

Checked and clean:
- Auth: `/setup`, GET+POST `/api/setup/progress`, `/api/safety/commissioning*`, `/api/zones/pid`,
  `/api/zones/ct_channel_map` and current-sweep start are all `ROUTE_TIER_ADMIN` (`route_tier_table.h:234-302,477`).
- Mode gate (HR-02): every config write the wizard makes goes through a handler that calls
  `system_mode_gate_check()` (`zones_http_post.c`, `zones_http_pid.c:115`, `safety_cfg_http.c:68-77` for params,
  relay_type, ct_cal, ct_trim). The client `checkBusy()` in `commissioning_shared.js` is only a pre-check.
  The progress POST is not gated. That is fine: it is metadata only.
- SR-31: the wizard writes only ids 260, 261, 265, 266, 799 and 262-264. It never offers 0x0401/0x0404/0x0405.
- Partial whole-page zone saves: `zoneToPostParams()` replays every required field. `coilpower`,
  `coupling_diag_k_dc`, `fuzzy_strength` and `coupling_c*` are omitted, and the parser keeps the stored value
  when they are omitted. `expected_generation` is sent, so a stale step gets 409 `zones_config_stale`.
- Zone cal bounds: step 3 uses -50..50, which matches `ZONE_CAL_OFFSET_MIN_C/MAX_C`. The PID 0..1000 range
  matches `ZONE_PID_GAIN_MAX`. The on/off hysteresis range 0.5..25 matches `ZONE_HYST_C_MIN/MAX`.
- Resume after reboot: progress is persisted (cfg file, plus a legacy NVS migration chain v1..v5). Step state
  is re-derived from `/api/readiness` on each load (`computeStepState`), and a step whose readiness has
  regressed shows "regressed".
- LCD: there are no LCD wizard pages, so the no-scroll rule does not apply.

## MED

**MED-1: step 9 (PID by hand) reports a false "different gains" warning for ordinary values, and does not mark the step done.**
`setup_wizard_page.html:1389`. The read-back comparison uses absolute tolerances: 1e-6 for Kp/Kd and 1e-9 for Ki.
The board stores float32 and prints `%.9g` (`zones_http_get.c:447`). Checked with `Math.fround`:
Ki=0.3 comes back 1.2e-8 off, and Kp=100.3 comes back 3.0e-6 off. Both fail the check.
Scenario: the operator types Ki=0.3. The board stores it correctly. The page shows
"WARNING: board reports different gains than sent" and skips `postStepState(9,'done')`.
The operator now does not trust a correct write.
Fix: compare against `Math.fround(sent)`, or use a relative tolerance (|a-b| <= 1e-6 * max(1,|b|)).
Add a host test with Ki=0.3 and Kp=100.3.

**MED-2: step 7 sends tc_offset_c and abs_max_temp_c without the Pico's A5 bounds, so an out-of-range value gives a partial commit.**
`setup_wizard_page.html:1640` checks only that tc_offset_c is finite. `:1643` checks only that abs_max_temp_c > 0.
The Pico's SET_PARAM refuses |tc_offset_c| > 50 (`CONFIG_PARAMS_TC_OFFSET_ABS_MAX_C`) and abs_max > 2500
(`CONFIG_PARAMS_ABS_MAX_TEMP_MAX_C`). Under SR-32 the other pairs in the same POST still go through COMMIT_CONFIG.
Scenario: the operator enters tc_offset_c=60 with a new tc_type and abs_max. The tc_type and abs_max persist on
the Pico. The page reports a failure that blames staging, so the operator believes nothing changed.
Fix: mirror the Pico bounds in `validateStep7` (and in the `/safety/commissioning` GROUPS table), with tests.
The SR-32 protocol fix stays the real cure. The note on `safety_commissioning_page.html:521`
("No fixed cap beyond a finite number") has been wrong since A5 and should be corrected too.

## LOW

**LOW-1: step numbers in user-visible text use the 0-based id, but headings show id+1.**
The heading is `(stepId + 1) + '. '` (`:3107`). But `computeCompleteness` says `'Step ' + s.id` (`:500,507`).
"finish step 2 first" (`:2179` and siblings at `:1325,2309,2481,2644`) points at id 2, whose heading reads "3.".
`validateStep6` says "raise abs_max_temp_c in step 7" (`:925`), but that step's heading reads "8.".
`validateStep7` says "lower ... in step 6" (`:1653`).
Scenario: the operator follows "step 7" and lands on the zone-limits screen.
Fix: one helper, `stepLabel(id) = id+1`, used in every message. Add a test that no message text cites a raw id.

**LOW-2: the step 9 status line shows step 10's detail.**
`:1360` filters `m.id === 10`. The step is id 9, so it shows the profile/readiness detail of the next step.
Fix: use `9`, or better, the current step id.

**LOW-3: step 9 marks done after one zone is saved.**
`:1393` posts `done` when any single zone saves with kp > 0. Other zones may still have kp=0. Readiness then
shows the step as "regressed". Fix: mark done only when every heater zone in `cfg2` has kp > 0, or leave the
state to readiness.

**LOW-4: the step 6 refusal is stricter than the firmware and the zones page.**
`validateStep6` (`:921`) refuses a zone max_temp above the Pico abs_max. `zones_http_post.c:670-690`
(owner request 2026-09-10) raises the Pico ceiling automatically when a zone max is raised, and
`/settings/zones` lets that happen. The same edit succeeds on one page and is refused on the other.
Fix: allow it and say that the Pico ceiling will be raised to match. Or document why the wizard stays stricter.

**LOW-5: the per-zone CT map is described as "Required" but can be committed blank.**
At `:2860` the hint says required. `:3002`/`:3044` commit ct_installed=1 with per-zone topology and no map when all
three selects are blank. Readiness `safety_commissioned` should catch this, but the screen contradicts itself.
Fix: refuse a blank map when ct_installed=1 and the topology is per-zone, or reword the hint.

**LOW-6: a successful CT cal Apply throws away unsaved step 7 edits.**
`:2772` calls `renderStep7()` after a successful CT cal Apply. That rebuilds the form from the board and drops
anything typed but not committed (tc_type, offset, abs_max, CT map).
Fix: update only that row's result text, or warn when `kcDirty` is set.

**LOW-7: CT cal error path hides the server reason.**
`:2766`: `safety_cfg_http.c:1121,1140,1153` answer 400 with `httpd_resp_send_err` as text/plain, not JSON.
`r.json()` fails, and the user sees "FAILED: bad response" instead of the A_fs or zero_mv range text.
Fix: fall back to `r.text()`. Also mirror the A_fs/zero_mv bounds client side.

**LOW-8: step 3 verification can pass on a reading taken before the TC type change.**
`:2220` polls `/api/status` `actual_valid` straight away. A valid reading converted under the old type counts as
"verified converting". The page already says CR1 verify is not transmitted, but the first sample has no settle
delay. Fix: wait at least one conversion period plus the thermo task period first, or require a reading
timestamp after the POST.

**LOW-9: step 3 Save has no `lockSave`.**
`:2191`. Steps 2, 5 and 11 lock their save button; step 3 does not. A double click posts twice. The second POST
gets 409 `zones_config_stale`, and the page says "changed elsewhere ... Nothing was saved", although the first
POST did save. Fix: wrap it in `lockSave('step3Save')`.

**LOW-10: `|| 0` replay turns a missing GET field into an explicit value.**
`zoneToPostParams` (`:705-747`) sends `z.x || 0` for every field. For `xzone` (`:732`), an explicit 0 disables
guard 8. Today GET reports every field, so this is latent. Any future GET omission (version skew, a renamed
field) would silently disable guard 8 or zero a value on every wizard save.
Fix: omit optional-preserve fields when they are `undefined`, and send the stored value only when it is present.

**LOW-11: step 11 can save half a change.**
`:1514-1545`: `set_web_password` succeeds, then `set_policy` fails. The password has changed, but the error says
"Save refused", and the step-level text earlier says "login was NOT turned on".
Fix: say that the credentials were saved and only the policy change failed.

## INFO

- **INFO-1: coverage gaps.**
  - The safety relay type (contactor/mercury, `POST /api/safety/commissioning/relay_type`) is not offered.
    Steps 5 and 7 cover only the zone relay type.
  - E-stop verification (readiness `estop_verified`) is not offered, linked, or mapped to any step. It is still
    counted in the final gate, as a reason with a `/safety` link.
  - Other readiness items are not mapped to any step: `hardware`, `safety_ceiling_match`, `pico_update`,
    `ct_leak_alarm`, `zone_aux_conversion`. They block the gate, but no step explains them.
- **INFO-2: auth step order.** Step 11 (authentication) comes after the "final readiness gate" step 10. The
  heading "First profile & final readiness gate" reads as the end of setup. Consider moving the auth step earlier
  (after network), or renaming step 10.
- **INFO-3: stale wizard doc.** `docs/SETUP_WIZARD.md` still says 13 steps (`:29,56,228`). It names "step 12"
  as the final gate and "step 10" as PID. The wizard has 12 steps (ids 0-11, shown as 1-12): PID is "10." and
  the gate is "11.".
- **INFO-4: live driver coverage.** The PcTools live CDP driver covers only W49 (page load) and W50 (step 1 save,
  `web_commission_row.py:345-368`). No live or CDP row exercises steps 2-11. In the JS host tests,
  `test_setup_wizard.js` covers the pure validators but not the step 9 read-back, the step 7 bounds or the CT
  cal error path.
- **INFO-5: when step 7 is blank.**
  - If GET reports tc_offset_c as unset, a blank field sends 0. This is intentional: the placeholder says
    "0 = no correction".
  - With ct_installed=0, ct_topology and the map are not sent, so stale Pico values remain. The Pico ignores
    them while no CT is installed.
- **INFO-6: sheath-lag step (in flight, not reviewed).** It belongs in step 3 ("Thermocouple types & calibration
  offsets"), per zone, beside `cal_offset_c`. That keeps the step count and the progress mirror unchanged.
  Hazards:
  - (a) it must be added to `zoneToPostParams()`, or steps 2-6 whole-page saves will drop or reset it. Prefer
    the omit-preserves parser convention (see LOW-10).
  - (b) it must change only the ESP control estimate, never the Pico's own reading or `tc_offset_c`. A lag
    compensation that leads the raw reading must not let the ESP ceiling run ahead of the Pico abs_max
    relationship.
  - (c) its bounds must match the firmware parser's bounds, with a host test.
  - (d) step 3's verify text must not imply that the lag is verified.
