# kilnCtl Roadmap — both processors

> **Status:** planning · **Last reviewed:** 2026-09-18, CT commissioning bench
> check (nineteenth sweep) — live board read of channel 2's CT calibration
> and of S9/S14/S15 status; no source or firmware changed this sweep.
> - **Channel 2's CT calibration is correct and complete on the live board:**
>   `k_ct_v_per_a[2] = 1`, `zero_counts[2] = 63`, `gain[2] = 0.715`,
>   `ct_installed = 1`, `ct_topology = 1` (summed), `i_present_a = 2.0`;
>   `safety_get_commissioning` reports `commissioned = True`,
>   `stale = False`. This closes the earlier open question (below, eleventh
>   sweep) about whether `gain[ch] = 0.715` reconciles with the owner's CT
>   transfer function — it does, and nothing further needs to be written to
>   the board for channel 2's scale/zero.
> - **S14/S15 remain DORMANT, confirmed hardware-scale-limited, not a
>   firmware or calibration gap.** `i_normal_a[0..2]` are all still unset
>   because `zone_sweep_summed_normal_a()` refuses to record anything below
>   its 0.045 A noise floor, and this ~4 W bench fixture draws only about
>   70 mA total. No further code or calibration step is pending on this
>   fixture; arming either guard needs a kiln-scale load (or a different
>   bench fixture), not more software.
> - **Superseded 2026-09-18, CT-summed-topology fix:** the all-three
>   `k_ct_v_per_a > 0` gate recorded just above was a deliberate, documented
>   decision, not an oversight — but it permanently excluded any board wired
>   in SUMMED topology (one shared CT on channel 2 only) from ever reporting
>   commissioned, which downgrades S9 to a warning forever on exactly the
>   boards this bench represents. `s_current_sensing_commissioned` now calls
>   `config_store_current_sensing_commissioned()` (`firmware/SaftyFW/src/config_store.h`),
>   which requires `k_ct_v_per_a > 0` only on channels that are actually
>   *fitted* (all three for PER_ZONE, only channel 2 for SUMMED, via the new
>   `config_store_ct_channel_fitted()` helper), and requires at least one
>   fitted channel. Landing this alone would have been unsafe: `current_task.c`
>   and `safety_core.c`'s `any_current_present` also had to be masked to
>   fitted channels in the same change, because unfitted channels 0/1 read
>   16-17 raw ADC counts of idle noise against only a 25-count presence
>   margin — without masking, S9 (unclearable once latched) could arm and
>   then latch off that noise alone, strictly worse than the bug being fixed.
>   **Net effect on this bench: S9's `TRIP_INEFFECTIVE` is now armable here
>   for the first time** — channel 2's CT is fitted and calibrated (see the
>   commissioning check above), so `current_sensing_commissioned` now goes
>   true, and channels 0/1's noise no longer counts toward
>   `any_current_present`. This changes the bench's live safety posture, not
>   only its source: a welded-contactor exercise that could previously never
>   latch S9 here can now do so, once the fix is flashed.
> - **Latent, not active: the zone current sweep is still entitled to
>   overwrite `k_ct_v_per_a[2]`.** `safety_get_ct_cal` reports all three
>   channels `uncalibrated`, so the manual-provenance skip in
>   `zone_sweep_plan_k_ct()` (`firmware/KilnFW/App/drivers/control/zones_current_sweep_task.c`
>   around lines 794-806) never engages for channel 2 either, despite its
>   calibration being correct above. In practice the sweep cannot record
>   anything on this fixture (see the S14/S15 finding above), so this cannot
>   fire on the bench today — recorded as a known latent issue for whenever
>   a kiln-scale sweep becomes possible, not as an active defect.
>
> **Also reviewed 2026-09-18, claim-accuracy pass (twentieth sweep, docs only
> — no source, no firmware and no board touched).** Prerequisite P1 of
> `docs/PICO_AUTO_UPDATE_PLAN.md` is **CLOSED**: the bench RP2040 now boots
> through its two-slot bootloader, slot A active, with a `KLN1` metadata
> record (`firmware/SaftyFW/bootloader/metadata.h:69`) where a
> `debug_read_memory` scan previously read ordinary code. Every claim here
> and in the plan docs describing the Pico as a directly SWD-flashed image
> with no fallback slot was stale and is corrected. **A new open defect took
> its place the same day:** an ESP-driven Pico OTA cannot reach the data
> phase on this hardware — recorded under M8 below, diagnosis in
> `docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md`. The `kilnctrl`
> MCP tool count is now 170 as of `dd3ed13c` (`crash_report_ack`; earlier
> additions `safety_get_ct_cal_raw`, `bench_test_*`, `saleae_decode_kilnlink`,
> `safety_set_log_level`, `convert_config`); `CLAUDE.md` holds the
> authoritative count from here on.
>
> **Reviewed before that:** 2026-09-18, docs/ROADMAP sync
> (eighteenth sweep, docs-only — no source or firmware changed this sweep).
> Verified against the named commits and against directly-reported bench
> state, not against another doc's status marker.
> - **Bench board reflashed to `ae3160df` on both processors, 2026-09-18**,
>   ending a stretch of roughly 1057 commits behind HEAD (the last recorded
>   flash below this line pinned the ESP at `170f4b75` on 2026-09-10). Live
>   `/api/readiness` on the bench reports 18 of 19 items
>   `ok`; the sole `not_done` item is `safety_commissioned` (3 of 65
>   applicable safety parameters still unset — `i_normal_a[0..2]`, unchanged
>   from the earlier sweeps below, since no CT sweep has been run against
>   this build). `cross_zone_max_delta_c` and the thermocouple offsets read
>   `deliberately_off`, by choice, not by omission. The `cfg` LittleFS
>   partition now genuinely reports mounted on this board — earlier doc
>   language calling it inert was written from source inspection, not a
>   live read; see `docs/CONFIG_FILESYSTEM.md`.
> - **CT sampling: mains-phase-aliasing hypothesis for the fitted channel's
>   idle noise refuted, no code change** (`8ae0ca6c`) — the ADC sees a
>   rectified, RC-smoothed (τ = 1 s) peak envelope, not a raw current
>   waveform (`firmware/SaftyFW/docs/CURRENT_SENSE.md` §1/§3), so there is
>   no fast-sine phase for a microsecond-scale conversion burst to alias
>   against, and the RC values themselves bound even the more charitable
>   envelope-recharge-phase reading of the hypothesis roughly 10x below the
>   measured noise. The real, still-open anomaly points at pickup on the CT
>   lead itself and needs a bench oscilloscope, not a sampling-rate change;
>   see `docs/audits/ct_sampling_mains_aliasing_review_2026-09-18.md`, and
>   the CT-commissioning row below (this sweep corrected that row's stale
>   step-0 claim).
> - **`kilnctrl` MCP tool count is 160, `kicad` is 86** (`53c323d5`, three
>   new `zone_current_sweep_start/status/abort` tools wrapping
>   `zones_http.c`'s current-sweep routes) — `CLAUDE.md` and
>   `docs/MCP_SERVERS.md` were already updated by that commit; nothing
>   further to correct here. On this ~4 W bench fixture every zone's
>   current is structurally below `ZONE_SWEEP_NORMAL_NOISE_FLOOR_A`
>   (0.045 A vs. ~23 mA/zone), so an "unmeasured" sweep result here is
>   expected, not a bug — consistent with `i_normal_a` staying unset above.
> - **`screen_idle`'s stack ceiling is 3152 B**, raised from a stale 3008 B
>   baseline with cause cited to `bfa60679` (`24d4f465`, direct-measurement
>   comment fix `1f054189`) — no ROADMAP or other doc here still quoted the
>   old 3008 B figure; `docs/research/fuzzy_ramp_tracking_2026-09-13.md`'s
>   mention of 3008 is dated history describing an earlier baseline, not a
>   current-state claim, and is left as-is.
> - No unchecked ROADMAP item was closed this sweep on the strength of a
>   plan document alone; the one substantive content fix was the CT-
>   commissioning row's stale step-0 claim above.
>
> **Reviewed before that:** 2026-09-17, roadmap-upkeep audit
> (seventeenth sweep) — see the sweep note near the sixteenth sweep's below
> for what changed. Also folds in the fifteenth sweep that
> were "work in progress" as of the fourteenth sweep (dimensionless fuzzy
> bands, the overshoot re-measurement and its review, the firing_score fix
> sequence) and records an owner decision. No hardware touched, no firmware
> behaviour changed. Verified against the named commits, not against this
> list's own prose. Full detail: `docs/audits/session_summary_2026-09-14.md`
> (successor to the 09-11/09-13 summaries).
> - **OWNER DECISION: autotune-derived fuzzy bands ship.** `2c49465a` derives
>   `rate_band_c_per_s = model_k_dc / model_tau_s` and
>   `error_band_c = model_k_dc * 0.5` from each zone's own autotune model,
>   with the previous absolute constants (20.0 °C / 0.5 °C/s) kept only as an
>   explicitly-logged fallback for a never-autotuned zone. This meets the
>   standing requirement that nothing ship guessed for, or tuned to, a kiln
>   other than the installed one, at no material cost — orthogonal to the
>   "no demonstrated benefit" finding below, which concerns the rule table
>   and strength, not the axis units. `docs/FUZZY_CONTROLLER_PLAN.md` §2(i)
>   updated; `fuzzy_strength_pct = 0.0` on the live board still means this is
>   UNVERIFIED ON HARDWARE.
> - **The fuzzy layer has no demonstrated benefit UNDER MATCHED CONDITIONS ON
>   THIS BENCH — that scope qualifier is load-bearing, per an owner
>   objection, and must travel with the finding everywhere it is cited.**
>   `docs/audits/fuzzy_overshoot_measurement_2026-09-13.md` plus its
>   appended review (`1570a65a`) found the document's "equivalent fixed
>   retune" comparison arm was actually fuzzy_50's centre-cell MAXIMUM
>   (reachable only at error=0/rate=0), not its time-average — fuzzy's real
>   ramp-phase mean gain multipliers are roughly half that arm's. A plain
>   static gain rescale at fuzzy's true ramp-phase average reproduces
>   fuzzy_50 on all four objectives inside materiality, and the
>   overshoot/undershoot penalty tracks gain-change magnitude monotonically
>   with no discontinuity at the inference boundary. The claimed 0.42 °C
>   overshoot advantage came entirely from a ramp-down residual mislabelled
>   as overshoot (real delta: fuzzy is +0.15 °C worse); the settle-time
>   claim inverts at a 0.5 °C band (vs. the document's chosen 2.0 °C). **But
>   every measurement behind this finding — a ~4 W bench, a well-tuned PID,
>   a plant model matched to the plant — is the condition LEAST likely to
>   reveal a gain-adaptation layer's value, since such a layer earns its
>   keep precisely when the plant does not match what the PID was tuned
>   for. This is weak evidence against the layer, not evidence for removing
>   it — "untested where it would matter" is not "tested and found
>   useless."** Untested conditions where a benefit would be expected: a
>   changed thermal mass, a PID tune never good for the real plant, the
>   high-temperature regime (`k`/`tau` ~20x lower), and a thermocouple
>   placed near the elements rather than the load — shortening apparent
>   dead time and adding a fast mode the FOPDT fit does not represent, a
>   plant/model mismatch of exactly the kind this layer exists to absorb,
>   which the simulator cannot currently express at all.
>   `docs/SCENARIO_SIMULATION_PLAN.md` was being authored separately to scope
>   coverage of these. **Corrected 2026-09-15 roadmap claim audit: that plan
>   now exists and has partly run** — its own status line reads WI-1 through
>   WI-8 DONE (per-item status in
>   `docs/audits/scenario_simulation_implementation_2026-09-14.md`), WI-9
>   DROPPED (its premise, the fuzzy/Ki mutual-exclusion guard, was deleted by
>   `88bb4333`, not merely disabled). **Updated 2026-09-16: WI-10 is now DONE
>   and the plan is CLOSED** — a `strength_pct` cross-firing adapter design
>   plus its simulation arm (`sim_strength_pct_adapt.c`) found every scoped
>   scenario's 9-firing chain stayed `FIRING_COMPARE_INSUFFICIENT` (too few
>   matched-segment pairs per firing to clear the comparator's Bar 1 floor)
>   — a recorded, non-fatal finding per the plan's own acceptance criterion,
>   not a defect. Read that plan directly rather than this line; the
>   "no outcome exists" framing here was stale.
>   Owner principle, now decided: fuzzy constants, like PID gains, are
>   **derived per kiln, not shipped** — bench values may be anything
>   convenient precisely because they never ship, so no bench number in
>   this finding is evidence for or against a shipped default.
>   **`docs/FUZZY_CONTROLLER_PLAN.md` §2(iv)'s "for" case, previously noted
>   as weakened by the (retracted) IAE headline, has that weakening
>   WITHDRAWN and stands as originally written — but is NOT strengthened by
>   today's finding either**, for the same matched-conditions reason. Do
>   not flatten the plan's five-option structure — this corrects one
>   option's argument, not the ranking.
> - **Ramp tracking is CLOSED against the fuzzy layer** (`c002ceaf`): the two
>   ramp-lag rule cells are `{kp +1, ki 0, kd 0}` while steady-state ramp
>   error is set by `Kv = Ki·P(0)` — the wrong lever — and the layer has no
>   access to `d(setpoint)/dt` at all. The feedforward climb term is the
>   mechanism that targets this; unchanged from the 09-13 sweep.
> - **The firing_score scorecard was accept-permissive on two of the four
>   objectives, and is now fixed.** `2edbb6eb` found no settle-time
>   instrument and a clamped-to-zero undershoot; `d41da85f` added both,
>   raising `FIRING_SUBSCORE_COUNT` 3→6; `9a9afb25`'s review found that this
>   **silently enrolled** the new axes into `firing_compare`'s verdict via a
>   loop to `FIRING_SUBSCORE_COUNT`, moving the historical corpus REJECT
>   21→26 and INSUFFICIENT 615→610 while ACCEPT held at 24 only by
>   cancellation; `560cffe0` fixed the enrolment with an explicit voting
>   mask and `_Static_assert`s making an unclassified or signed axis unable
>   to silently vote. **A1 restored to the pinned 24/21/615.** The settle
>   band used by the corpus fit is **2.0 °C**, chosen from 33 real
>   dwell-zone instances — a different instrument from, and not to be
>   confused with, the 0.5 °C materiality critique in the overshoot-
>   measurement review above.
> - **Ratchet fixes, and a new mutual-exclusion constraint.** `97288659`
>   anchored the `adaptive_tune` K_dc ratchet to the original autotune
>   baseline rather than the live adapted value; `36f88d62` stopped an
>   ordinary whole-page zones save from silently zeroing that anchor;
>   `e78fbc5b` closed a live Ki ratchet loop (effective-vs-reference
>   divergence under `PID_FUZZY`, ~1.2x per run, 9 runs to the 5x
>   plausibility bound) by withholding the Ki correction while fuzzy is
>   active — that mutual exclusivity has since been **superseded**: the owner
>   decided fuzzy and the self-improving PID run concurrently with no
>   interlock (`docs/audits/concurrent_fuzzy_pid_adaptation_2026-09-14.md`),
>   and `88bb4333` removed the guard together with the Ki write path it
>   protected, making `adaptive_tune_ki.c` diagnostic-only and SIMC
>   (`adaptive_tune_model.c`) the sole AUTOMATIC gain writer; `0dbd7c6d`
>   exposed the anchor over `GET /api/zones`, closing the observability hole
>   where the fix's own value could not be read back.
> - **Open, no outcome asserted:** the coupling sign reversal remains
>   unexplained (no literature reports one); the level-scheduled coupling
>   class's do-not-retry gate is unchanged; the Pico heat-start reboots are
>   closed-pending-recurrence, not root-caused; the `ease_off_window_mult`
>   hardware A/B (`a57ca6f9`) was **INCONCLUSIVE** (within-run zone
>   confound) so the shipped 2.0 default stands unchallenged; the RP2040
>   fault-hook chain has never been observed end-to-end on hardware; and the
>   zones JSON response has only **161 bytes** of headroom in its 7360-byte
>   cap (`docs/audits/zones_json_headroom_plan_2026-09-14.md`, `375c9258`) —
>   enlarging the buffer is forbidden.
> - **CLOSED 2026-09-15 (was "parked deliberately"):** the two defects in
>   `adaptive_tune_ki.c`'s fuzzy guard — reading
>   `control_mode`/`fuzzy_strength_pct` at refine time instead of snapshotting
>   at capture time, and failing open when `zones_config_get_control_mode()`
>   returns false — were both fixed by `ac5c26a3` (dwell-entry snapshot +
>   fail-closed), and the guard itself was then removed outright by `88bb4333`
>   along with the Ki write path it protected. Neither defect exists at HEAD:
>   the file no longer calls `zones_config_get_control_mode()` at all. This
>   entry was stale, not open.
>
> **Claim audit, 2026-09-15** (`docs/audits/roadmap_claim_audit_2026-09-15.md`).
> Every claim in this file re-checked against code at HEAD and git history, not
> against this file's own prose. All 206 cited commit hashes resolve and no
> spot-checked commit was misdescribed. Six stale claims corrected in place
> (each marked "corrected 2026-09-15 roadmap claim audit" where it sits), two
> moved file paths fixed, and one **safety-evidence misattribution** corrected:
> the guard-evidence row named the E-stop polarity fix as hardware-verified
> when `docs/SAFETY_CASE.md` Â§4 classes it host-tested. The dominant shape was
> the one the 2026-09-04 audit predicted â€” shipped work still described as
> pending. Nothing was flashed, no board read, no `debug_*` call made, no
> production code changed. Seven topics could NOT be resolved without hardware
> or an owner decision and are left standing, marked, in that audit's
> "Undetermined" section.
> - **UNVERIFIED ON HARDWARE, unchanged â€” and NOT checkable by the 2026-09-15
>   claim audit either, which was forbidden to read the board:**
>   `fuzzy_strength_pct = 0.0`,
>   `approach_rate_cap_c_per_hr = 0.0`, and `adaptive_tune enabled=False` on
>   all three live zones — every fuzzy-band, overshoot-measurement, ramp-
>   tracking and ratchet finding above is a host-test/sim result only; none
>   has run on the physical kiln.
>
> A concurrent pass is separately auditing a settle-band mismatch in
> `sim_fuzzy_overshoot.c` as of this sweep — work in progress, no outcome to
> report; not cited further here.
> - **Fuzzy controller: Stage 0 of `docs/FUZZY_CONTROLLER_PLAN.md` has run,
>   and its downstream comparison was independently reviewed and partly
>   retracted the same day.** `ed854ac5`'s offline nine-cell probe recorded
>   the centre cell's exact effect — kp x0.75/ki x1.25/kd x0.75 at strength
>   50, the only cell ever observed on real hardware — but its "6 of 9 cells
>   unreachable" claim rests on an unmeasured profile-rate conversion
>   (`8a12521b`, `docs/audits/review_sim_fuzzy_commits_2026-09-13.md`) and
>   must not be cited. `ba230bca` originally reported the centre cell's
>   switching behaviour beating an equivalent flat, always-on retune by
>   ~7.8% IAE; that figure was measured against a stale, sabotaged build and
>   has been **retracted** by the same review — rebuilt from source, fuzzy
>   vs. an equivalent flat retune is a 0.011 degC MAE gap at strength 50 and
>   reverses sign at strength 25, indistinguishable from a fixed multiplier
>   on this scenario. Net: neither result changes `docs/FUZZY_CONTROLLER_PLAN.md`'s
>   standing position — no option is supported on materiality grounds, and
>   the deciding evidence cannot come from this bench fixture. See the plan's
>   §2(iv)/§4.3 for the current argument, not a copy here. A process finding
>   worth carrying forward: a negative test that hand-restores source and
>   proves an empty `git diff` can still leave a **built artifact** poisoned —
>   a full rebuild must follow any negative test before anything downstream is
>   measured against it.
> - **Coupling model: a flat-scale discriminator was run; its PARTIAL verdict
>   is itself overstated, pin unchanged.** `c9ce6b7c` tested section 9's
>   hypothesis that a flat 1.173x multiplier on the constant coupling matrix
>   would reproduce the level schedule's A1 regression, reporting a headline-
>   count match (38/660) but a different cluster shape, and called the result
>   PARTIAL. A same-day independent review (`8a12521b`,
>   `docs/audits/review_sim_fuzzy_commits_2026-09-13.md`) ran the sampling
>   statistics on all four factor readings against the pinned 24/660 rate and
>   found {18, 20, 24} form one indistinguishable cluster (<=1.25 sd apart)
>   and 38 is only marginally elevated (~1.8 sd, p~=0.07) — **not established
>   as a real effect**, and the per-subscore cluster-signature argument (splits
>   of 2-vs-11 and 22-vs-27) is below resolution the same way. The factor-1.0
>   control did reproduce the pin exactly, so the experiment's mechanics are
>   sound; its conclusion is not. Correct reading: **inconclusive**, not
>   PARTIAL. The pin stays at 24/660 regardless, and the level-scheduled
>   coupling class's "explain the reversal before retrying" gate is unchanged —
>   if this question is worth resolving, a larger or paired-trial-level
>   analysis is needed, not a re-read of the existing 660 trials.
>
> - **Coupling model class, not just the coefficients, is the defect** — this
>   sharpens, not reopens, the eleventh sweep's refutation. `fd8d7b93`
>   re-ran the 62-75 C under-prediction test against the fresh column-by-
>   column matrix (not the old mixed-provenance one) and **all nine
>   zone/plateau cells still under-predict, ~9-31%**. The sign reversal
>   against the low-level ~33% *over*-prediction is confirmed **real and
>   still unexplained** — do not assume the fresh matrix will resolve it.
>   `8ee40a7b`/`763acbdc` proposed and then **refuted** a total-power
>   superlinear enclosure-loss term from data already on hand (a single-
>   column sweep reached 1.48x the joint case's total power with a flat
>   response — refutes any gamma>1 for this signature). The surviving
>   signature is that the deficit tracks **how power is split across zones,
>   not the total power drawn**. ~~Also found in the same pass: `coil_power_w`
>   is `0.0f`/unset everywhere in the firmware, so total power in watts
>   cannot be evaluated at all today — a "consumer without producer"
>   instance, not yet fixed.~~ **Corrected 2026-09-15 roadmap claim audit:
>   this was already superseded on the day it was written.** `dbd8ff52`
>   (2026-09-10) added the per-coil nameplate wattage override
>   (`ZONES_CFG_VERSION` 24->25) the day before `fd8d7b93` recorded the
>   claim; the producer chain is complete at HEAD (`zones_http_post_parse.c`
>   parses it, `zones_config_set_coil_power_w()` stores it,
>   `zones_http_get.c` reports it), and `0.0` is a documented sentinel
>   meaning "use an equal share of the nameplate sum", whose own producer is
>   `ZONE_MAX_POWER_PARAM_ID`. Whether the LIVE BOARD has either value set is
>   a hardware question this audit could not check. Net effect on the eighth-through-twelfth
>   sweeps' buoyancy hypothesis (z0 fitted exponent 1.366, "leading
>   hypothesis: buoyant transport into the top zone, superlinear in
>   delta-T", still stated further down this file): buoyancy was already
>   refuted on hardware before today (single-column transport measured
>   linear, `cf1f8ce9`/`1b9afd4f`/`5844a3e8`/`947709a8`) and the superlinear-
>   power alternative is refuted now — **neither surviving hypothesis
>   explains the z0 shape error**; treat every "buoyant"/"superlinear"
>   phrase below this line as a retired hypothesis, not a live one. The
>   defect is the model class (`G*u = b`, additive duty-driven off-
>   diagonals), not a coefficient — unchanged conclusion, now with a second
>   eliminated alternative.
> - **`s_coupling_use_measured_diag_k_dc` arbitrates nothing on this board**
>   (`ef8a374f`): it compiles **true**, not false, and has moved to
>   `zone_coupling_solve.c`; the live diagonals already match the measured
>   constants to 3-4 significant figures. Any doc or MCP diagnostic string
>   still claiming this flag is `false` is stale — a PC-side MCP string with
>   that claim is being fixed in a separate pass, do not touch it here.
> - **`sim_iter_tune` A1 pin's exit condition was unfalsifiable, now fixed**
>   (`645551c2`, stale operator banners fixed by `250a0fef`): the old
>   condition ("when the coupling re-identification lands") was satisfiable
>   on a false trigger. A1 measured today at **24/660 (3.64%)**, unchanged
>   from the number quoted elsewhere in this file — the fix changes what the
>   pin *means*, not today's measured value.
> - **Fuzzy controller: confirmed still running plain PID on the bench**
>   (`7e669c18`) — `control_mode=3` but `fuzzy_strength_pct=0.0` on all
>   three live zones, which per `pid_fuzzy_adjust()`'s own contract
>   reproduces base PID bit-for-bit. ~~No closed-loop simulation exercises the
>   fuzzy path at all.~~ (Superseded `fbdc5bd0`/`7ef487ff`/`65fc6be9` — see
>   the corrected blocking-prerequisite bullet below.) The one hardware capture that did exercise it
>   (`fuzzy_ab_20260904d` arm B1, referenced in §3.6f below) is a single,
>   unpaired-A-arm run with 100% of its samples falling in one of the rule
>   table's nine cells — insufficient to characterize the layer, not just
>   "one capture short." **Two new owner requirements, not yet designed
>   against:** (1) the fuzzy controller must not ship with parameters tuned
>   on this bench fixture — it must bootstrap from the PID autotune result
>   on the *installed* kiln and keep adapting over heat cycles, because the
>   bench is a ~4 W/120 V fixture capped ~40 C above ambient while a real
>   kiln reaches ~1200 C where radiation dominates and both k and tau fall
>   ~20x from the bench-fitted values; (2) any controller change is to be
>   tested in simulation first, which today's finding shows the current sim
>   harness cannot do for the fuzzy path — this is a new, currently-unmet
>   prerequisite for further fuzzy work, see
>   `firmware/KilnFW/docs/PID_EXPANSION_PLAN.md` §10.
> - **Two unexplained Pico reboots at heat start: closed pending recurrence,
>   NOT root-caused** (`26505ce6`) — downgrades the eleventh/twelfth
>   sweeps' "remain unexplained" to a specific, falsifiable leading
>   candidate: most likely `link_task`'s stack overflow already fixed by
>   `c27484a2` (the SaftyFW stack-budget fix from the tenth sweep). The
>   direct `scratch[5]` evidence that would have confirmed this
>   self-erased, so this is not proven — treat as closed-pending-recurrence,
>   watch for a recurrence post-`c27484a2` before calling it fixed. A
>   relay-inrush brownout at the same moment remains undiscriminated: there
>   is no brownout detector distinct from the watchdog bit.
> - **RP2040 fault-hook diagnostics audited on the source, still unverified
>   on hardware** (`8267fab2`) — this is a narrowing, not a reversal, of the
>   "never been verified on hardware" claim below: all 3 in-scope bits have
>   real producers in code, but the producer-to-consumer chain has **never
>   been observed end-to-end on hardware**. `BOOT_BROWNOUT` is now confirmed
>   **dead** (no detector backs it — consistent with the brownout gap noted
>   above). A mirror-drift check was itself found missing the three fatal-
>   fault bits and has been fixed. `relay_owner` and `watchdog_task` remain
>   on the bare 256-word minimum stack, unrelated to this finding but noted
>   in the same audit.
>
> **Fuzzy controller — live workstream, owner requirements attached.** Full
> detail: `docs/audits/fuzzy_controller_improvement_scoping_2026-09-11.md`
> (`7e669c18`, `90abaf5f`, and a further same-day commit — read the file
> directly, it was still being extended as of this sweep). Summary, not a
> copy:
> - Live board state, a third inert configuration distinct from the two
>   already retired ones (mode-2 defect; withdrawn cell-crossing claim):
>   `control_mode=3` on all zones but **`fuzzy_strength_pct=0.0` on all
>   three** — by `pid_fuzzy_adjust()`'s own contract this reproduces base
>   PID bit-for-bit, so the board runs plain PID today.
> - Load-bearing defect found this pass: `ERROR_BAND_C_DEFAULT=20.0f` /
>   `RATE_BAND_C_PER_S_DEFAULT=0.5f` are absolute constants the code's own
>   comments admit are desk reasoning about "a mid-size kiln," never
>   measured on any plant — autotune already produces
>   `model_k_dc`/`model_tau_s`/`model_dead_time_s` per zone and none of it
>   reaches the fuzzy layer.
> - Owner requirements (stated as requirements, not suggestions): (a) must
>   NOT ship with parameters trained on this bench fixture — bootstrap from
>   the PID autotune result on the installed kiln; (b) must keep adapting
>   over subsequent heat cycles; (c) authority must grow with **measured
>   confidence**, driving the existing `strength_pct` up from zero rather
>   than through a new mechanism.
> - ~~Blocking prerequisite: **no closed-loop sim exercises the fuzzy path at
>   all** — neither `sim_iter_tune.c` nor `sim_credibility_gate_closedloop.c`
>   calls `pid_fuzzy_adjust()`. Nothing here is testable on the sim-first
>   requirement until that harness exists.~~ **NO LONGER BLOCKING, corrected
>   2026-09-15 roadmap claim audit.** The narrow half is still true (neither
>   of those two files mentions `pid_fuzzy` at HEAD), but the harness exists:
>   `fbdc5bd0` (2026-09-11) added `sim_fuzzy_closedloop.c`, a single-zone
>   closed-loop harness for `pid_fuzzy_adjust()`; `7ef487ff` added
>   `sim_fuzzy_overshoot.c`; `65fc6be9` added `sim_factorial_driver.c`. The
>   sweeps at the top of this file already cite results measured with those
>   harnesses, so this bullet contradicted them.
> - Baseline data is insufficient, not merely thin: one mode-3 capture,
>   unpaired A-arm, 100% of its samples in one of nine rule cells.
> - **Requirements (a) and (b) above are already met in code by
>   `adaptive_tune` — not by fuzzy, and not yet by anything new.**
>   `docs/audits/adaptive_tune_vs_owner_requirements_2026-09-11.md`
>   (`655da406`) independently verified `adaptive_tune.c`/`.h` bootstraps
>   strictly from an existing autotune result and keeps refining every
>   clean firing indefinitely — host-tested and reachable end-to-end, but
>   **disarmed on this board** (`enabled=False`, `observations_lifetime=0`
>   on all three zones), so this is a statement about the code, not
>   demonstrated hardware behaviour. Requirement (c), authority graduated
>   by measured confidence, is **not addressed by any shipped code** —
>   `adaptive_tune`'s guards are fixed constants, never graduated. A
>   ratchet defect in `adaptive_tune`'s bound anchoring is being fixed in a
>   separate, concurrent pass — in progress, not done, do not describe an
>   outcome here. Do not build a second bootstrap-and-adapt mechanism for
>   fuzzy: extend `adaptive_tune` for (c) instead. Full analysis and options:
>   `docs/FUZZY_CONTROLLER_PLAN.md`.
> - **Superseded by the fourteenth sweep, above:** the plan's Stage 0 probe
>   (listed as not-yet-run in earlier sweeps) has now run — see the
>   2026-09-13 bullet at the top of this file and `docs/FUZZY_CONTROLLER_PLAN.md`
>   directly rather than this paragraph, which predates that result.
> - **The controller objective is four-part, stated by the owner
>   (2026-09-13): rate/lag, settle speed, settle accuracy, over/undershoot —
>   fuzzy's job is specifically the last one.** `docs/FUZZY_CONTROLLER_PLAN.md`
>   §0.0 records this and the methodological fallout: every IAE/MAE figure in
>   that plan (including the retracted-and-replaced centre-cell-vs-flat-retune
>   comparison above) is an aggregate that collapses all four objectives into
>   one number and needs re-scoring on `firing_score.c`'s per-objective
>   subscores, not silent re-ranking. Ramp tracking (objective 1) is now
>   **closed against the fuzzy layer** — `c002ceaf`
>   (`docs/research/fuzzy_ramp_tracking_2026-09-13.md`) shows the two
>   ramp-lag rule cells push `Kp`, not the `Ki` that actually governs
>   ramp-following error, and the layer has no access to `d(setpoint)/dt` at
>   all; the feedforward climb term remains the mechanism for objective 1,
>   unchanged from before. A new, cheap, **NOT YET TESTED** candidate for
>   objective 4 was also recorded: the setpoint-weight `b` in `pid.c`
>   (hardcoded to 1.0, `PID_SETPOINT_WEIGHT_B`), one parameter, no rule-table
>   change — see plan §0.0.2.
>
> **Simulation fidelity — coupling-model replacement, in progress
> elsewhere, do not describe an outcome.** Sources:
> `docs/research/multizone_thermal_modelling_literature_2026-09-11.md`
> (`47cd0f28`) and the coupling audits cited earlier in this sweep. Summary:
> - Refuted model class: `G*u = b` with additive duty-driven off-diagonals;
>   superposition fails ~33% at low level; the sign reversal at 62-75 C is
>   real and unexplained (see above). Two candidate replacements are already
>   dead: buoyancy (refuted on hardware) and a total-power superlinear loss
>   term for any gamma>1 (refuted from data on hand).
> - **Level-scheduled coupling gain has now failed twice and is not the
>   current plan.** First attempt (`8cbd9d67`, reverted `9f054181`) hit A1 at
>   38/660; a second attempt meeting all four of that adjudication's retry
>   conditions still hit 43/660 (worse), was reverted with nothing committed,
>   and is recorded, with a labelled hypothesis for why, in
>   `docs/audits/coupling_level_schedule_adjudication_2026-09-11.md`'s
>   "Second attempt" section. A third attempt requires first explaining that
>   result, per that doc's gate — this is a precondition on the model class,
>   not an implementation detail to retry.
> - Acceptance criterion: the `sim_iter_tune` A1 false-accept bar, pinned at
>   **24/660 (3.64%)** against a 2.0% design target, to be re-measured after
>   the coupling change per the pin's own (now-falsifiable, `645551c2`) exit
>   condition.
> - Owner's standing sequencing rule: **controller changes are tested in
>   simulation first**, then single-zone hardware; multi-zone/joint work
>   stays blocked until the coupling defect is resolved. Single-zone sim is
>   on firm ground (single-column transport is measured linear); multi-zone
>   sim is not, and a sim result there must never be reported as hardware
>   evidence.
>
> - **Update, fourteenth sweep:** the flat-scale discriminator section 9 asked
>   for has now been run (`c9ce6b7c`) and reviewed -- inconclusive under
>   sampling statistics, not the PARTIAL result first recorded; see the
>   2026-09-13 bullet at the top of this file. Pin and gate both unchanged.
>
> **Reviewed before that:** 2026-09-10, roadmap-upkeep audit
> (twelfth sweep, updated in place after a same-day correction) — commits
> landed since the eleventh sweep (`dd67ec7e`, corrected by `e5b7fff9`).
> Verified against code and git history, not against a commit message's own
> self-assessment. **Check-suite tallies were unstable all session** (93/93,
> 92/1, 91/2 seen within one hour) because several sessions hold WIP in this
> shared tree — do not read any single count below as a property of current
> `HEAD`, only as what was true at the stated moment.
> - **`check_all_task_stack_budgets.ps1` (KilnFW) does NOT skip grading on
>   INDETERMINATE tasks — corrected from this sweep's own first pass.** A
>   second read of `firmware/KilnFW/App/test/check_all_task_stack_budgets.py`
>   (its own EXIT CODES doc at line 131, and the OK/FAIL branches around
>   line 566-602) confirms: INDETERMINATE means the reported total is an
>   explicit **lower bound**, not that the total goes unscored — an
>   over-ceiling total still fails regardless of INDETERMINATE status. Runs
>   earlier this session that reported it red were correct; my earlier
>   "OK by design" framing in this same sweep was wrong and has been
>   removed. `safety_poll` measured fresh at that time: 3136 B used, 4756 B
>   honest free of 8192 (58.1%) — a lower bound, not the true worst case.
>   That 3104 -> 3136 B ceiling move is now explained and is a deliberate,
>   dated, causal ratchet update (`24b12f0a`, see next bullet), not
>   unexplained drift.
> - **Ceiling-reconcile backoff: landed** (`24b12f0a`, superseding the
>   in-flight revert this sweep initially reported as uncommitted). ARMED is
>   now treated as the latch it actually is (fixed 30 s backoff, jitter and
>   the false PWM/de-energise claims removed from `safety_ceiling_policy.h`),
>   **and** the ceiling reconcile moved off the blocking
>   `safety_cfg_store_refetch()` onto a non-blocking `apply_pairs_ex()` path —
>   this second half is the more important fix, since it stops
>   `safety_poll_task` taking the blocking form `safety_cfg_store.c:1488-1510`
>   documents as forbidden (httpd-worker callers keep the blocking path).
>   Making that refetch function non-static cost one 32 B inlined frame on
>   `safety_poll`'s deepest path — 3104 -> 3136 B — which is exactly the
>   figure measured above.
> - **elf_archive producer leak fixed at the root** (`b693f6f1`): orphan
>   adoption now runs automatically inside `archive_kiln_elf()`, recovering
>   identity by scanning for the `esp_app_desc_t` magic word — 68 files / 47
>   manifest / 21 superseded / **0 unreferenced** at time of that commit.
>   `_prune()` now protects every referenced ELF and reports loudly when the
>   60-file cap is therefore unenforceable (archive is ~1.3 GB) — shedding
>   registered identities to make room is an **open owner decision**, not a
>   defect.
> - `91f0adfb` closed a word-boundary hole in `check_doc_hash_citations.ps1`'s
>   `sub:` tag matching.
> - **Web-GUI ceiling hard cutoff, confirmed in code** (`c99356f8`):
>   `safety_ceiling_policy_target_c()` (`firmware/KilnFW/App/drivers/safety/safety_ceiling_policy.c:16`)
>   returns the ESP's configured maximum with no added headroom;
>   `SAFETY_CEILING_HEADROOM_C` stays defined but unused, per that file's own
>   comment at line 39. Verified on hardware per the eleventh sweep;
>   unchanged this sweep.
> - `d141e152`, `df4da85b`/`7aefa049`, `9d796b57`, `6113859a`, `8489facf`,
>   `15a8d1a1`, `370391a0`/`ca48ae0a` — as described in the brief handed to
>   this sweep; each is a committed, clean change and no code inspected this
>   sweep contradicts their stated effect.
> - **Two items landed this sweep are already flagged as being reworked,
>   record accordingly, not as closed:** `a3c566bc`'s `k_ct`/`i_normal` fix
>   cleared the ESP's normals without telling the Pico, leaving the guard
>   armed on a stale scale and making a calibrating sweep unable to ever arm
>   S14/S15 — a rework is in progress. `6113859a`'s GET_STATUS timeout
>   gating is separately being revised because it made `stats.timeouts`
>   blind to partial loss.
> - The **coupling joint-identification capture is still running** on the
>   bench (`docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md`, not touched this
>   sweep per instruction) — the matrix remains refuted and the board runs
>   uncoupled feedforward until it lands; this also gates the iter_tune
>   dwell-offset bars, S8 auto-derivation, and the A1 pin's exit condition.
> - **Several firmware commits remain built but unflashed** (refusal
>   classification/backoff, GET_STATUS accounting, per-coil nameplate
>   wattage `dbd8ff52`) pending the board, which is occupied by the coupling
>   capture.
>
> **Reviewed before that:** 2026-09-10, roadmap-upkeep audit
> (eleventh sweep) — 27 commits landed since the tenth sweep (`c4b026d8`).
> Verified against code and git history, not against another doc's status
> marker; check suite re-run fresh this sweep, **90/90 passing** (confirmed
> against `run_all_checks.ps1` output, matching the tenth sweep's target of
> "84 -> 87" continuing to "87 -> 90"). Landed and confirmed:
> - **ESP flashed to HEAD `170f4b75`** (`c34fb2a5`, 2026-09-10, clean
>   detached worktree at `origin/main`) — the board had been stuck at
>   `6355c822` since 2026-09-06. `flash_firmware(..., verify=True)` confirmed
>   the running partition/build; Pico-side commissioning (S8 20 C/min,
>   `mains_voltage_v=120`) survived the ESP-only reset unchanged. Full record:
>   `docs/audits/esp_bring_up_to_head_2026-09-10.md`.
>   - `/api/readiness` item `safety_commissioned` now reports **exactly 3**
>     missing parameters, `i_normal_a[0..2]` — `ct_channel_map` is no longer
>     counted, confirming the summed-topology mirror fix (tenth sweep) is
>     live on both sides. The `i_normal_a` write path (`c0729e1e`) is now on
>     the board, so a CT sweep could arm S14/S15 — **none has been run; both
>     guards remain DORMANT**.
>   - **Side effect, since fixed: S1 `abs_max_temp_c` moved 80 -> 85 C, then
>     corrected back.** `safety_ceiling_sync.c` (new this HEAD) pushes the
>     ESP's configured zone ceiling to the Pico's S1 threshold on link-up,
>     and at the time of the flash that policy added 5 C of headroom above
>     the ESP's own 80 C ceiling. The owner decided a web-page ceiling must
>     be a hard cutoff (exact value, no added headroom), and the fix has
>     **landed and been verified on the bench**: `c99356f8` makes
>     `safety_ceiling_policy_target_c()` return the ESP's configured maximum
>     exactly — `SAFETY_CEILING_HEADROOM_C` (5.0f) stays defined but unused,
>     documented as historical. Verified live in both directions: 85 -> 80
>     to match the ESP, a raise of zone 0 to 90 took the Pico to exactly 90,
>     and a lower back to 80 took it to exactly 80. Negative-tested by
>     restoring the `+ SAFETY_CEILING_HEADROOM_C` line (turned 6 tests red),
>     then reversed by hand; 90/90 checks and 34/34 host-test executables
>     pass. **Practical consequence for operators**: with the ceilings now
>     equal, reaching the configured limit latches a safety-processor trip
>     requiring a manual clear, rather than the ESP stopping a few degrees
>     early — this is the deliberate trade for a hard cutoff, and it is why
>     profiles should stay below the ceiling (see the dashboard proximity
>     warning, `a9abd273`, and the owner's ~5 C profile-headroom guidance —
>     a separate mechanism, deliberately left untouched by `c99356f8`).
> - **The 45 C discriminating coupling plateau ran and finished** (`7c18a11e`):
>   settled 46 min, z2 residual **-1.58 C** — close to the -2.9 C SCALE
>   prediction and far from the -8.4 C OFFSET prediction from the ninth/tenth
>   sweep's own pre-registered criterion, so **SCALE is favored, OFFSET
>   excluded**, for z2 specifically.
> - **Eight-plateau shape-vs-scale analysis** (`b60a3f85`): the simulation-
>   and hardware-derived corrections **agree** to 0.030/0.037/0.040 once a
>   hardware drift allowance is applied, leaving a common ~0.035
>   row-independent residual. But **z0 is a shape error, not a scale error**:
>   fitted exponent 1.366 with an offset that survives leave-one-campaign-out,
>   crossing zero once across eight plateaus. Leading hypothesis: buoyant
>   transport into the top zone, superlinear in delta-T. **The adopted
>   coupling matrix is refuted against its own acceptance criterion** — this
>   updates, and is consistent with, the tenth sweep's `cplval75` refutation.
>   A uniform per-row rescale will not fix z0; joint re-identification is
>   still required (unchanged open item, see below).
> - **Simulator coupling model class was wrong, now fixed** (`d63a5591`): it
>   used a temperature-difference exchange term (`g*(T_j-T_i)`), identically
>   zero at a uniform dwell, where the firmware's `zone_coupling_solve.c`
>   couples by additive duty-driven source gain
>   (`diag(k)+coupling_coeff`) — two model classes that agree only in
>   differential mode, and the recorded dwell operating point is almost pure
>   common mode. This was the real cause behind the credibility gate's ramp
>   MAE bar failing outright; after the fix, ramp MAE passes on 5 of 6
>   zone-runs against the 3 C bar (`docs/audits/sim_credibility_gate_real_cause_2026-09-10.md`,
>   confirmed current in `docs/ITER_TUNE_REDESIGN_PLAN.md`'s own live status
>   block, itself already updated by a concurrent pass this sweep — re-read
>   that doc directly rather than trusting a fixed number here, since a
>   further rebuild during this same day moved the count from da9f3775's
>   "5 pass / 1 fail" framing to a 6-zone-run framing with the same shape).
> - **Credibility gate reports honestly now** (`da9f3775` and siblings,
>   `b351edf0`): previously-vacuous dwell-entry-peak passes fixed, ambient
>   leakage in the state stated. Gate overall verdict remains **GATE FAILS**
>   — dwell offset and dwell-entry peak are the open bars, per
>   `docs/ITER_TUNE_REDESIGN_PLAN.md`'s live status.
> - **Second-order plant hypothesis for the dwell-entry-peak bar: refuted as
>   tested** (`4f26a7f7`). **Closed-loop replay (candidate 3) explains dwell
>   offset but not the peak residual** (`170f4b75`). All three investigated
>   candidates for the peak residual are now eliminated — the leading
>   hypothesis reduces to the z0 coupling deficit above; do not re-propose
>   second-order plant or closed-loop replay as the peak explanation again.
> - **A1 false-accept regression root-caused and fixed** (`8b96b591`): dwell-
>   entry peak now smoothed over one PWM window, false-accept rate 3.64% ->
>   1.97%.
> - **Capture-scoring adapter added** (`49bb1123`, `firing_score_from_capture`
>   runs `firing_score.c`/`firing_compare.c` against real bench captures), and
>   a finding recorded: **profile 7's dwells are shorter than its entry
>   window**, so `steady_rms_c` can never produce a sample — Bar 2 (noise-
>   floor spread) is structurally unreachable for that profile.
> - Smaller fixes this sweep, each confirmed in git log: ceiling-reconcile
>   budget/backoff (`d2710518`), `climb_window_floor_s` wired into the
>   autotune-engine producer path (`bd77ffd1`), atomic ELF publish for
>   `check_00` (`25508277`), the ELF-archive test-write guard (`aa698221`),
>   `check_doc_hash_citations.ps1` now resolving submodule commit hashes
>   (`79d93233` — this is the fix that lets this very sweep's citations be
>   checked correctly).
> - **Coupling capture procedure revised again** (`c42d9384`): delta-T-
>   targeted plateaus, ambient measured at the thermocouples, a same-session
>   high-delta-T joint hold, and a new criterion B distinguishing scale from
>   shape errors — now an estimated 7.25-8.75 h capture, up from the tenth
>   sweep's 6.5-8 h.
> - Check suite: **87 -> 90 discovered, 90 passing, 0 failed**, re-confirmed
>   by a fresh run this sweep (see status line above).
>
> **Open, in flight, or owner-blocked as of this sweep** (do not mark any of
> these done): the S1 ceiling hard-cutoff fix has **landed and is verified**
> (`c99356f8`, see above) — no longer open, listed here only until the next
> sweep folds it into "closed items"; a long coupling-identification capture
> is now actually running on the bench (`docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md`)
> and the board runs uncoupled feedforward until it completes and the matrix
> is re-identified, and a per-row rescale will not fix z0's shape error;
> S14/S15 remain dormant — a sweep was attempted and did NOT arm them
> (`a2ce8aba`): the summed-topology `derived_mask` gap means the
> `k_ct_v_per_a` derivation path never executes on this wiring regardless of
> load size, and separately the fixture's ~33-90 mA draw sits at or below
> the sweep's 45 mA noise floor for a single pass; the credibility
> gate's dwell-offset and dwell-entry-peak bars are still open with all three
> investigated peak-residual hypotheses now eliminated; S8 stays at its
> hand-set 20 C/min until the matrix is re-identified; `abs_max_temp_c` must
> still be raised Pico-first-then-ESP before a real firing; the E-stop
> double-pole switch is still unwired; two Pico reboots at heat start remain
> unexplained; `safety_poll` (3104/3104 B) and `autotune_engine` (2944/2944
> B) both still sit at exactly zero stack margin; the Pico fault-hook
> diagnostics (malloc/assert latching, `boot_reason` bits) have never been
> verified on hardware; iter_tune persistence, HTTP surface and shadow mode
> remain unbuilt, deliberately behind the credibility gate. Full detail for
> closed items before this sweep lives in `docs/COMPLETED_2026-09.md`.
> **Reviewed before that:** 2026-09-10, roadmap-upkeep audit
> (tenth sweep) — 53 commits landed since the ninth sweep (`fa424d74`).
> Verified against code and git history, not against another doc's status
> marker. Landed and confirmed:
> - **SaftyFW stack checker: two correctness fixes** — margin was graded
>   against a ceiling set to exactly 2x its own measurement rather than the
>   declared budget (`05b48cce`), and a stale PC-relative register literal
>   could resolve and silently clear the INDETERMINATE tag (`82eecb0e`).
>   Following from that, **stack budgets raised and now 9/9 ok**:
>   `link_task` 6144->10240 B, `update_task` 3072->6144 B,
>   `configTOTAL_HEAP_SIZE` 40K->56K (`c27484a2`) — the stacks would
>   otherwise have consumed nearly the whole heap.
> - **Check suite: 84 -> 87 discovered, 87 passing, 0 failed** (confirmed by
>   a fresh run this sweep). New: `check_00_saftyfw_target_build.ps1`
>   (`cfd8e3ce`) and `check_01_kilnfw_pushed_build.ps1` /
>   `check_01_saftyfw_pushed_build.ps1` (`cbbec0fa`), which build the
>   fetched `origin/main` rather than the local tree — closing the gap that
>   let `main` break four times in a day.
> - **ELF archiving on every flash, both processors, with loud-failure
>   lookup** (`311047c2`) — the previous gap made a live panic
>   unsymbolizable.
> - **RP2040 `config_store` in-RAM-cache seqlock (`b202fe56`/`5671ee03`/
>   `cb1ba325`) is now FLASHED** — `ae23aba4` (2026-09-09, clean detached
>   worktree at HEAD; commissioning and S8 survived the reset) and again
>   `b88ea6ba` (2026-09-10, later HEAD; commissioning survived). Both
>   `docs/CONFIG_FILESYSTEM.md` and the ninth-sweep note above it had this
>   marked "NOT yet flashed" — that was stale as of this sweep and has been
>   corrected in `docs/CONFIG_FILESYSTEM.md`. Fallback-ABA fix and boot
>   seeding also landed (`985e41b4`).
> - **S8 (rate guard) auto-calculation from the identified plant model**:
>   write path wired (`a4398558`), then three review-found defects fixed —
>   sentinel-wins-selection, inverted margin, coupling-blind basis
>   (`431019ba`), range check gated on `fields_set` (`9345f722`), caller
>   rejects `FIT_TEMP_UNKNOWN` and feeds the real coupling gain
>   (`0820dfa6`), margin scoped to checked provenance with a 2.0x
>   uncoupled fallback and load-time range gate (`8f53d16c`, `fb495e05`).
>   S8 stays at its hand-set 20 C/min on the bench regardless — the auto
>   path is not trusted on this plant until the coupling matrix is
>   re-identified (see open items).
> - **S15/S14 arming**: `22eeab9f` gated `amps_valid_for_ct` on CT
>   commissioning plus a 45 mA noise floor, and `1feffdd8` made the sweep
>   back-out honest about a possibly-already-landed commit. `c0729e1e`
>   (2026-09-10) adds the missing write path pushing each zone's measured
>   normal current to the Pico's `i_normal_a[0..2]` — **this is new code,
>   not yet flashed to the ESP** (the ESP's bench build is still 33+
>   commits behind HEAD per `b88ea6ba`), so S14/S15 remain dormant on the
>   running board today; do not mark this item closed.
> - **Guard 1 climbing window** now derived as
>   `clamp(dead_time+tau, 120, 900)` ~= 317 s (`cf3b5adb`), replacing zone
>   0's spurious 60 s override (zones 1/2 never had one).
> - **Safety-ceiling link-down bypass closed**: reconciled on every
>   link-up tick, not just at boot (`1132d13a`). `zones` ceiling ->
>   Pico propagation added (`2d604d1d`); `backup_import` bypass closed
>   (`be25339b`).
> - `sw_reset_esp` MCP tool added for the non-JTAG ESP reboot-in-place path
>   (`8e10d9b0`).
> - **`safety_poll` frame size**: two identical-looking 16 B trims landed
>   back-to-back (`6d7454e8` then `edac93a2`, apparently the same fix
>   committed twice) — it sits at exactly 3104 B against a 3104 B ceiling,
>   zero margin; treat as fragile, not closed.
> - **Coupling matrix: `cplval75` capture (`1efbdc0c`) refutes the adopted
>   matrix against its own pre-registered criterion** (`d2e570ad`) — failing
>   2 of 3 zones at every plateau by 6-9x tolerance; `ff_hold_infeasible`
>   confirmed inert as a contributing cause. Joint identification procedure
>   revised to 37-minute column steps from the live 271 s tau, acceptance
>   step 6 inverted, 6.5-8 h total (`3120cca5`). A 45 C discriminating
>   plateau (predicted z2 residual: ~-2.9 C for a per-row SCALE error vs.
>   ~-8.4 C for a constant OFFSET) is **in progress on the bench board as of
>   this sweep** — do not report a verdict here; check the capture's own
>   record.
> - **iter_tune credibility gate**: still FAILS. Both previously-published
>   explanations were retired this sweep (`a5721a53`, exchange-vs-source
>   coupling investigated and also ruled out) — the real cause is
>   under investigation and currently **unknown**. Do not re-propose either
>   retired explanation.
> - Two SaftyFW double-reboot investigations: the first aborted mid-capture
>   (`238e0eb9`); the second flashed current HEAD and did **not** reproduce
>   the Pico reboot, instead surfacing an unrelated ESP panic on the ESP's
>   own stale build after a per-zone thermal-guard trip (`b88ea6ba`) — two
>   clean Pico runs is suggestive, not proof; the ESP panic itself was not
>   investigated further.
> - Plant constants centralized into `sim_measured_zone_constants.h`
>   (`4d42bafe`), fixing a stale preset diagonal baked into three sim
>   harnesses.
> - `mains_voltage_v` corrected 240 -> 120 on the board, read-back confirmed
>   (noted in the ninth sweep as landed; reconfirmed present at this HEAD).
> - **run_all_checks.ps1 `-ListOnly` bug fixed** (`f2a293f8`, this sweep):
>   `$SkipExitCode = 3` was assigned before `param()`, which PowerShell
>   silently refuses — no switch bound at all, so `-ListOnly` always ran the
>   full suite. `workbench.py`'s `repo_checks(list_only=True)` MCP tool has
>   been getting a full 900s run back instead of a listing this whole time;
>   same fix covers it. Verified `-ListOnly` now lists and exits promptly,
>   and a full run is still 87/87.
>
> **Open, in flight, or owner-blocked as of this sweep** (do not mark any of
> these done): the 45 C discriminating plateau is running now; the
> iter_tune credibility gate's real cause is unknown after two published
> explanations were retired; the coupling matrix needs joint
> re-identification and the board runs uncoupled feedforward until then; S8
> stays at its hand-set 20 C/min until the matrix is re-identified;
> `i_normal_a`/S14/S15 have a write path in code (`c0729e1e`) but it is not
> yet flashed, so both guards remain dormant on the bench board; `k_ct_v_per_a`
> is still uncalibrated and the owner's stated CT transfer function does not
> obviously reconcile with the committed `gain[ch]` of 0.715; the E-stop
> double-pole switch is still unwired; `abs_max_temp_c` must be raised
> Pico-first-then-ESP before a real firing; two Pico reboots at heat start
> remain unexplained (did not recur on a clean reflash, but one clean run is
> not proof); `safety_poll` has zero margin against its stack ceiling. Full
> detail for closed items before this sweep lives in `docs/COMPLETED_2026-09.md`.
> **Reviewed before that:** 2026-09-09, roadmap-upkeep audit
> (ninth sweep, afternoon) — a very large amount landed since the eighth
> sweep (`79080e0a`), 43 commits. Verified against code and git history, not
> against any other doc's status marker. Landed and confirmed:
> - **`/api/readiness` is now a real, no-override firing interlock**
>   (`d5170d54`) — the eighth sweep's "in progress" line above is now closed —
>   **and the same interlock was extended to autotune** (`cd43cc32`), since
>   autotune commands the same relays through a separate choke point
>   (`autotune_begin_run_locked()`) that the firing gate never touched.
> - **Executor stack overflow fixed, plus the budget check that would have
>   caught it** (`379f3fe6`); `backup_export`'s httpd-stack RED closed by a
>   frame-size cut (`8bd5684e`); **table-driven stack budgets added for all
>   28 previously-uncovered KilnFW tasks** (`316967b7`), then made honest
>   about tasks reached only through indirect dispatch, where the checker
>   cannot see the real worst case (`c726748e`) — read that commit before
>   trusting a green KilnFW stack-budget run as a full proof.
> - **SaftyFW: `current_task`/`discrete_task` stack overflow that could
>   deadlock both cores, fixed** (`3afc5ea6`); a per-task stack-budget check
>   added and `thermo_task`'s stack bumped (`5b8fc53d`).
> - **SaftyFW `config_store` RAM-cache seqlock** against a confirmed-live
>   cross-core torn read (`b202fe56`, barrier-primitive fix `5671ee03`, then
>   a review-found fallback-buffer race fixed writer-owned rather than
>   reader-written, `cb1ba325`) — **host-tested only, not yet flashed to the
>   bench Pico.** See `docs/CONFIG_FILESYSTEM.md` for detail, and note this
>   is a *different* defect from the RP2040 `config_store`'s still-open
>   `next_write_slot` torn-slot issue (untouched by any of the above).
> - **`check_00` target build added to the check suite** (`139debb5`,
>   closing the gap that let an unbuildable `main` reach it), then hardened
>   against an MSYSTEM no-op false-pass and given a freshness check
>   (`16f0563f`), then that freshness gate's own false-positive on a
>   legitimate no-op build fixed (`af0bb774`).
> - **Summed-CT topology exempted from the `ct_channel_map` commissioning
>   requirement** (`b5cb83a4`), plus an ESP-side mirror fix and a new
>   truth-table drift check between the two sides.
> - Commissioning render now exposes `estop_active_level` and four other
>   previously-unprinted params (`d5768552`).
> - **Serial-port identity: COM14 confirmed the MAIN BOARD** by excluding
>   CMSIS-DAP probes from UART-bridge autodiscovery (`3530e598`) then
>   identifying boards by USB serial number rather than chip family
>   (`9e9dfb45`).
> - **`iter_tune` redesign** (`8f80a4de`, three latent defects found in
>   review and fixed same day: `249ce287`, `ce55440d`) plus a new write-
>   surface guard and its own negative test (`f3fcd597`). Steps 1, 2 and 5
>   of the plan's 9 steps landed; 3-4 and 6-9 remain open/design-only — see
>   `docs/ITER_TUNE_REDESIGN_PLAN.md` itself for current step status (it is
>   being edited by another pass concurrently with this sweep; re-read it
>   rather than trusting a stale summary here).
> - Simulation harness gaps G1-G4 promoted out of `sim_iter_tune.c`
>   (`e0d2e006`).
> - **A real Pico reboot-in-place wire command** (`8b0e799a`, `d045cd64` —
>   the latter also reports a failed reboot task, names the S6a latch it
>   causes, and gates the Pico on transfers) plus sw-reset honesty
>   corrections.
> - `boot_guard_reset_counter()` wired into `flash_firmware()` via a new
>   HTTP route (`b09294fb`) — see `CLAUDE.md`'s boot_guard section for why
>   this exists (a tool-driven clear, never an unconditional boot-path one).
> - UI sweep: transient fetch/CDP harness errors no longer reported as
>   layout FAILs (`8018cfe3`); viewport height settle-and-verify fix before
>   the occlusion check (`1f91f500`).
> - Audits: the DC-gain "factor of ten" resolved as cross-zone attribution,
>   not an identification error (`3605f278`); S8 rate-guard's real
>   achievable ramp rate measured, re-tune recommended (`bd2ad739`); a
>   dual-processor flash + commissioning attempt (`7c3e6eac`); the owner's
>   K4-open hypothesis for `cplval75`'s zero-heat run investigated
>   (`8487d85d`); the commissioning gate blocking K4 traced to the
>   `ct_channel_map` summed-mode gap above it fixed (`2b3f1206`); a stale
>   RP2040 `config_store` atomicity claim and flash-status doc corrected
>   (`f0974a9a` — itself an instance of a doc's own status line going stale
>   a third way: "implemented, NOT YET FLASHED" after a later commit had
>   already recorded it flashed and bench-verified).
>
> **Open, in flight, or owner-blocked as of this sweep** (do not mark any of
> these done): commissioning the safety processor; S8's real-kiln rate value
> and auto-calculation (owner decision recorded, design in flight); **the
> bench heat path is still unconfirmed end to end** — today's no-heat run is
> *explained* by `calibration_missing` refusing the enable, but that has not
> been demonstrated as the actual mechanism, so do not record it as solved;
> the coupling matrix is mixed-provenance and a guard now refuses it, so the
> board runs uncoupled per-zone feedforward until a joint identification
> capture is taken; `i_normal_a[0..2]` needs live current before S14/S15 can
> arm; `abs_max_temp_c` must be raised Pico-first-then-ESP before a real
> firing, and the Pico's ceiling must never end up tighter than the ESP's;
> the E-stop double-pole switch is still not wired and polarity is still
> unset (compiled default ACTIVE_HIGH); manual relay control and the CT
> sweep deliberately bypass the readiness gate (owner decision, documented);
> `iter_tune` remains inert and unwired by design, credibility gate in
> flight; and `ff_hold` infeasibility above roughly ambient+38 °C is
> **structural on this power-limited rig** — both the radiative-term and
> fitted-slope explanations were checked and disproved, so do not re-propose
> either. Full detail for closed items before this sweep lives in
> `docs/COMPLETED_2026-09.md`.
> **Last reviewed before that:** 2026-09-05, roadmap-upkeep audit
> (seventh sweep) — landed the cone-unrated bucket (`1501f0c`+`3b0c82e`), the
> `safety_cfg_http.c` PSRAM move (`541b357`, flashed and re-baselined —
> `af17e3d` plus the follow-up dram_margin.h/doc pass),
> S8's compiled default (`c43323a`+`ea69efa`, bench value still gated on a
> GRACE-window write), and the >62 °C ff_hold-infeasible confirmation
> (`94b1a2a`); fuzzy bands are live on the board. Also recorded eight owner
> decisions from 2026-09-05 (`abs_max_temp_c` closed at 80 °C by design, CTs
> deferred, `UnitTestFixture` kept, M8's field-update exercise approved) and
> absorbed the previous sweep's
> LCD/display session plus a batch of review-finding fixes and a stack-
> margin capture. Full detail for every closed item lives in
> `docs/COMPLETED_2026-09.md`, per this file's own upkeep rule; earlier
> sweeps' audit trail lives in that file's edit history, not here.
> **Bench status, 2026-09-05:** the thermocouple swap is fixed, heater power
> confirmed close to previous levels (if a tuning run doesn't match earlier
> measurements, recalibration may be needed), and the test fixture kiln is
> available for firing again.
> **Start here:** the [What is actually left](#what-is-actually-left) section
> immediately below is the short answer; the milestones are the detail.
> **Keep this file current.** This is the top-level dispatch board: the place to
> start a task from when you do not already know which plan owns it. It holds
> *ordering and cross-processor dependencies only* — the detail lives in the
> per-area plans linked below, and duplicating their content here guarantees the
> two will drift. When a milestone lands, tick it here **and** in the owning
> plan. When the shape of the work changes, edit this file rather than letting it
> describe a project that no longer exists.
> **Sixteenth sweep, 2026-09-16 — roadmap-upkeep pass.** Two milestones were
> fully done but still presented as live work and were collapsed to one-line
> CLOSED entries: M12 (every row already ticked by 2026-09-15; cited commits
> `5cd56b6`/`64d0a8e`/`17ae4d9`/`b5cb83a4`/`c0729e1e`/`ddbd024`/`3149393`
> verified as ancestors of `origin/main`, full text moved to
> `docs/COMPLETED_2026-09.md`) and M16 (closed the same day by `61c75767`,
> already mirrored into `docs/HW_ABSTRACTION.md`, just never collapsed here).
> A third instance was a pure forwarding address rather than a milestone: the
> "Future work — KilnFW PC-link command acknowledgement" section had nothing
> left of its own — the work closed in `firmware/KilnFW/TODO.md` section 11
> back on 2026-08-24 (`5df2190`/`a458a8f`/`7b4c087`, all verified ancestors of
> `origin/main`) — so it now collapses to a one-line pointer too. No other
> milestone's tick state disagreed with its owning plan on this sweep; a
> sample of M12's cited commits and `commissioning_gate.c`'s
> `!calibration_missing && config_params_all_required_set()` check were
> verified against code, not just against plan prose.
> **Seventeenth sweep, 2026-09-17 — roadmap-upkeep pass.** No milestone's
> tick state disagreed with its owning plan (M0-M16 spot-checked against
> `docs/COMPLETED_2026-09.md`, `docs/HW_ABSTRACTION.md`, and each open
> milestone's own body text) and no ticked box or completion narrative was
> found that hadn't already been collapsed by an earlier sweep. Two stale
> claims found and corrected in place in the "Where each kind of task is
> planned" table, both about sequencing rather than tick state:
> `docs/RELEASE_HARDENING_PLAN.md`'s row said it "starts once
> `docs/KILN_PROFILES_PLAN.md` is finished," but the plan's own doc shows it
> opened 2026-09-16 with several BLOCKER sub-items already closed
> (`bfa60679` and others) — it already started, gate or no gate.
> `docs/WEB_AUTH_PLAN.md`'s row said it "Follows
> `docs/RELEASE_HARDENING_PLAN.md`," but both opened the same day and have
> been landing concurrently since, per `docs/WEB_AUTH_PLAN.md`'s own status
> line. Items left open and unverified because they need hardware or an
> owner decision, per this file's own upkeep rule against guessing: relay
> status LEDs, distinct thermocouple-daughterboard connectors, the I2C
> expansion connector, the DEBUG header, S9's welded-contactor exercise, the
> AP-fallback router test, and the Pico-update hardware exercise (M8) — none
> touched. Nothing flashed, no board read, no `.kicad_*` file touched.


The system is two firmwares that must agree with each other:

- **`KilnFW`** — ESP32-S3 main controller. Thermocouples, SSR heater outputs, PID,
  profiles, Wi-Fi, web GUI. Partly built and partly verified on hardware.
- **`SaftyFW`** — RP2040 safety processor (A1). Independent overheat and fault
  detection, owns the mechanical pilot relay K4. **Built and running on real
  silicon**; every guard input is now produced, and what remains is
  commissioning values plus the hardware-gated trip proofs.

They talk over an isolated UART (a digital isolator, U6, as of 2026-08-25;
previously an optocoupler pair). That link, and the rule that **the safety
processor must be alive for the main processor to heat**, is what makes this one
project rather than two.

---

## Index of what is left, by complexity

Every open item in this file, in one table, so the size of the remaining work
is visible without reading 1000 lines. **This is an index, not a second copy of
the plan** — each row points at the milestone that owns the detail, and when
the two disagree the milestone is right. Complexity is effort *once the item is
unblocked*; an XL that is blocked on a decision is still one sentence of your
time away from being startable.

| Size | Means |
|---|---|
| **S** | An hour or less. One file, or one number, or one question answered |
| **M** | A session. Several files, or a bench procedure with a known script |
| **L** | Multiple sessions. Touches persisted data, or a subsystem, or needs its own test pass |
| **XL** | A project. New hardware in the loop, or an unretired risk with no reproducer yet |

### Blocked on you — nothing in the code can answer these

| Size | Item | Where |
|---|---|---|
Six of the nine questions from 2026-08-28 became work rather than questions —
see [M12](#m12--commissioning-the-operator-can-actually-do--opened-2026-08-28).
Eight more were decided by the owner on 2026-09-05. What is still genuinely
open is short:

| Size | Item | Where |
|---|---|---|
| **M** | ~~CTs — deferred, 2026-09-05~~ — superseded: a summed CT was fitted on GPIO28, 2026-09-05. **CT commissioning, 2026-09-06** — owner wants user-entered probe rating (any rating; real probes 10-100 A, bench probe 1 A), user-entered or auto-measured idle offset, real-amps readout, and a `ct_topology` (per_zone / summed) so S14 works on the one summed CT. Plan with steps 0-6: `firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md`. **Steps 1-5 done** (`51c084f`, `7175078`, `9374e8a`, `8a124c4`, `b8f0f47`, `8239b87`: editable calibration fields, auto idle-offset over the wire, `ct_topology`/S15/summed sweep on both Pico and ESP, real-amps display on web/LCD/PcTools, docs). **Step 0 done, corrected 2026-09-18: this row was stale.** The noise-floor capture was actually taken and recorded 2026-09-06 (`firmware/SaftyFW/docs/CURRENT_SENSE.md`'s "Measured noise floor — RUN 2026-09-06" section: 262 samples/60.1 s via `safety_capture_ct_counts()`, fitted channel std ≈ 4.678 counts ≈ 3.8 mA), and that doc's own completion checklist already marked step 0 closed — this row alone had not been updated to match. ~~**Step 6** (bench run with the owner to actually arm S14/S15)~~ — **CLOSED by owner decision, 2026-09-19: "software walkthrough only."** The CT-lead-pickup investigation (`8ae0ca6c`) found the fitted channel's idle noise (std 9.329 counts vs. 0.180/0.223 on the two unfitted channels) traces to CT-lead pickup, not sampling rate, and would need a bench oscilloscope to chase further — moot now that the owner has decided not to attach a real load to this fixture. The software walkthrough ran end to end on the live board 2026-09-19: commissioning read back (`ct_installed=1`, `ct_topology=1` summed, channel 2 calibrated `k_ct_v_per_a=1`/`zero_counts=63`/`gain=0.715`, `i_normal_a` unmeasured on all channels), live status confirmed S14/S15 both DORMANT from the board's own description, and the current sweep (`zone_current_sweep_start`) ran all three zones and returned its expected **INCONCLUSIVE** verdict (`summed_unmeasured_mask=7`, no `i_normal_a` pushed) because this fixture's ~23 mA/zone sits below the firmware's 45 mA noise floor. A post-sweep re-read confirmed nothing changed (commissioning byte-identical, no trip latched, relays off). Full detail: `firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md`'s "Step 6 CLOSED" section. **This closes the whole CT commissioning step list (0-6).** A real load (or a different bench fixture) is the only thing that can ever let S14/S15 arm — that remains a hardware-gated, one-line follow-up, not further code. | `firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md`; `docs/CONTACTOR_FEEDBACK_OPTIONS.md`; `docs/audits/ct_sampling_mains_aliasing_review_2026-09-18.md`; M5 |
| — | ~~LittleFS for flash writes, 2026-09-06~~ — assessed **not adopted** for log retention, then **superseded 2026-09-07**: endurance review confirmed no wear problem exists either, but the owner directed the migration anyway for architectural reasons (structured, inspectable, backup-able user data). In progress — zones config, profiles, and prefs dual-write to a new `cfg` partition; see `docs/CONFIG_FILESYSTEM.md` for state and open items, `docs/LITTLEFS_ASSESSMENT.md`/`docs/FILESYSTEM_USER_DATA_PLAN.md` for the history. **RP2040 `config_store` A/B sectors (flash_endurance_review_2026-09-07.md R2): implemented AND FLASHED, `b7af9ebe` 2026-09-08** (bench-verified: commissioning config read back byte-for-byte across the migration, CRC unchanged) — this row previously read "NOT YET FLASHED" and was stale by a day. **The separate in-RAM-cache defect found 2026-09-09** (`s_cached_record` read with no synchronisation across cores, `safety_config_version` observed changing mid-read during a live heating run), fixed by a seqlock (`b202fe56`, barrier fix `5671ee03`, then a writer-owned-fallback correction `cb1ba325` after review found the first fix's own fallback snapshot could itself race two readers, 169+ host tests including a torn-read reproduction: 20,945/190k writes without the fix, 0 with it) — **IS now flashed** (`ae23aba4` 2026-09-09, `b88ea6ba` 2026-09-10; corrected 2026-09-14 roadmap truth-up — live `safety_get_fw_version` reads Pico build `d957d5fd`, far newer than either landing commit, commissioned). This row previously said "none of the three seqlock commits have been flashed" and was stale. See `docs/CONFIG_FILESYSTEM.md` for detail. | `docs/CONFIG_FILESYSTEM.md` |
| **XL** | **Whole-kiln setup wizard — NEW, owner request 2026-09-08.** One web page, `/setup`, guiding a new owner from a blank board to a kiln `/api/readiness` reports ready: network/time/units, zones + thermocouples + types, zone type (HEATER vs ON_OFF_DEVICE), relays and names, zone commissioning limits, the safety processor's own commissioning, current sensing + CT verification under load, autotune and the coupling matrix. Modelled on `safety_commissioning_page.html`'s guided flow (stepper, consequence-bearing radio cards, read-back-verified commit) and backed by the existing `/api/readiness` checklist rather than a new model. **DONE (2026-09-09).** All 13 wizard steps and all 11 implementation steps shipped; progress persisted in NVS (not `cfg`) so a filesystem problem cannot lose it. Two steps apply heat (CT sweep, autotune) and five need the owner present. **Follow-up (2026-09-19):** `check_no_bench_text_in_ui.ps1`'s jargon-class pass carries a temporary whole-file allowlist for `setup_wizard_page.html` (six doc/source-citation hits) pending the coupling-matrix-step-removal rewrite in `C:\wt\wizrework_koxpk7`; remove the allowlist entry once that rewrite lands and the file is clean. | `docs/SETUP_WIZARD.md` |
| — | **On/off device zones — owner request 2026-09-07, decisions settled 2026-09-14.** A zone may drive a non-heater on/off device (vent, damper, fan, water feed) instead of a heating element, switched by per-segment rules on ramp phase / direction / temperature / time, with a stalled ramp counting as a dwell. **Not "design only" — steps 1-8 of the 9-step plan are shipped and host-tested** (`d58492c9`, `3d740f78`, `dd1d6ada`, `172e3081`/`b46c120c`, `bf1db47f`, `83c8b28b`/`e8e32c7a`, `be27d461`); this row previously understated remaining work by ~8 steps. Safety core: guards 1/2/3/4/9 disabled for such a zone, per zone (`docs/ON_OFF_ZONE_PLAN.md` sec 1's guard table) — guard 1 (HEATING_FAILED) would otherwise false-trip on a *correctly working* vent, since "duty high, temperature flat" is both its trip condition and the device's normal signature. **2026-09-14: owner asked to decouple an on/off device from the 3-slot heating-zone array so it binds a spare relay instead — investigated and found genuinely large** (the zone array is hard-sized at `MAX31856_CHANNEL_COUNT` = 3 everywhere: guards, coupling matrix, firing records, persisted `zone_cfg_t`, HTTP surface; widening it needs a `ZONES_CFG_VERSION` schema bump, a frozen prior struct, a converter and a CRC check — `docs/audits/on_off_spare_relay_binding_2026-09-14.md`); **not implemented**, per D1 of `docs/audits/on_off_zone_decisions_2026-09-14.md`, which the owner has not yet reconsidered against this new request. Two engineering gaps from that decisions doc closed 2026-09-14: `adaptive_tune`/`firing_score`'s firing-stats snapshot now skip on/off zones as training data (`adaptive_tune.c`, `profile_executor_firing_stats.c`), and `docs/SAFETY_CASE.md` now carries the guard-3 coverage gap and the `max_temp_c == 0` relaxation. **Remaining open item: step 9, a supervised bench session with dry contacts — no on/off zone has ever actuated a physical relay.** **2026-09-20: step 5b added** — `profiles_page.html` previously had no editor for a profile's on/off rules at all (it only echoed `on_off_rules` back unchanged on save); an "On/off devices" section now lets an operator add/edit/remove per-segment rules from the browser, wired into save/load/preview, plus a real fixed gap (`rule%u_temp_source` was never sent, so any saved temperature condition was silently inert). **Follow-up 2026-09-20:** fixed a silent rule-destruction defect in that same editor -- a stale/absent zone used to serialize as an empty value and get silently dropped on save; it now round-trips via a flagged orphan option and save is refused client-side while any row is stale, plus a widened JSON byte budget and a new `check_page_js_tests.ps1` standing check; `4fe0a38d` then closed the Opus review nits on that pass (bounded temp_c import, stderr-safe check wrapper). | `docs/ON_OFF_ZONE_PLAN.md` |
| — | ~~Relay type and contact-life budget, 2026-09-06~~ — done. Per-zone `ssr/contactor/mercury`; rated-life table; budget math; >80% warning / >90% error, indication only, on LCD topbar + LCD Diagnostics and web dashboard + web diagnostics; confirmed reset both places; safety relay type select (`contactor/mercury` only) and K4 edge counting on the ESP (`c6d41fc`). | `docs/RELAY_LIFE_BUDGET.md` |
| — | ~~Zones page clean-up, 2026-09-06~~ — done: prose/tables behind a `<details>` info glyph, and "same as zone N" per group (schema v20→v21, `5672719`+`0126f24`). Chart.js assessed and declined; display-power Save button already fixed 2026-09-04. | `firmware/KilnFW/docs/ARCHITECTURE_DECISIONS.md#zones-page-clean-up-info-disclosure-schema-v20-v21-chartjs` |
| — | ~~Safety TC display audit, 2026-09-05~~ — done, `b90fcb3`: one predicate, `safety_tc_is_separate_physical_sensor()` (mirrored as `window.kcSafetyTcIsSeparate` in `app.js`), now gates the LCD diagnostics page, dashboard, zones page and `/api/status`; unknown/pre-protocol-10 status shows rather than hides. **2026-09-20 (`efcd7e01`): the web diagnostics page's safety-TC fault card now follows the same predicate** — `diagnostics_http.c` reports `tc_is_separate_sensor` in the `safety` block and `renderSafetyTcCard()` hides the card only when the link is up and the flag reads false (`test_safety_tc_diagnostics.js`, 24 cases). | `firmware/SaftyFW/src/safety_guards.h` (tc_source); `firmware/SaftyFW/docs/SAFETY_MODEL.md` §3 |
| S | S8 sanity rate — tool added `c49bb0e9`: `safety_set_rate_guard()`/`safety_get_rate_guard()` now expose config_store 0x0204/0x0205 over `POST`/`GET /api/safety/commissioning` (mirrors `safety_set_ct_cal`'s confirm-gated, read-back-verified pattern; refuses off `confirm`, a running firing/autotune, or an ARMED relay). **Corrected 2026-09-14 roadmap truth-up: this row said the guard "remains DORMANT (0)" — live `safety_get_rate_guard()` reads `max_rate_c_per_min=20 (ARMED)`, `rate_window_s=60`, i.e. already armed at a hand-set bench value, not the docs' 33.3 C/min (2x-fastest-rule) default.** **Open owner question, not resolved here: 20 C/min is tighter than the documented 2x-fastest rule and could nuisance-trip a 900 C/hr zone** — whether to raise it to 33.3 C/min (or the auto-derived value once the coupling matrix is re-identified, see the ninth/tenth-sweep notes above) is an owner decision, left open. | M3 |
| — | High-temperature validation firing — closed 2026-09-05, `94b1a2a` confirms ff_hold infeasible above 62 °C on hardware. | `PID_EXPANSION_PLAN.md` §3.6i |
| **S** | **Display items needing the owner's own hands/eyes, 2026-09-04.** Three separate (touch corner accuracy CLOSED `f028e2f` — see M1): (1) a residual blue tint on the ST7796 panel with every firmware cause eliminated by measurement — needs the owner's eye, or a colorimeter, or a second unit; (2) wake-on-touch, first-touch-swallow and error-dismissal behaviour on display power, which need a finger on the actual glass; (3) the STOP-block 5V I2C hazard measurement at meter-module pins 10/12, still not taken. | `DISPLAY_ST7796_PLAN.md` §4 |
| **M** | ~~Field-update hardware exercise~~ — **ESP half done 2026-09-05**: OTA into `ota_0` + rollback both verified on the bench (PID gains byte-identical before/after, no heat, no firing). **Pico half attempted 2026-09-06**: a raw `.bin` (`arm-none-eabi-objcopy -O binary` on `SaftyFW_slotA.elf`, no header-packaging step needed — the ESP builds `UPDATE_BEGIN`'s header itself) staged and the relay started, but the Pico refused `UPDATE_BEGIN` ("a safety trip is pending") before any flash write — a real Pico-side interlock the ESP's own cached status did not show. The actual over-the-wire transfer is still unexercised. **Updated 2026-09-18: the bootloader/metadata gap is closed** — the bench Pico now boots through its two-slot bootloader (slot A active, `KLN1` metadata present) — **and a fresh attempt got further and failed differently**: the RP2040 hardware-watchdog-reset while erasing the destination slot, so the ESP failed the relay at its 15000 ms erase timeout. The failure was safe (relays off throughout, no trip latched, configuration unchanged). See the M8 item below and `docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md`, plus `firmware/CommonFW/docs/UPDATE_PROTOCOL.md` "Hardware exercise 2026-09-05". **2026-09-20, in source (not yet exercised on the wire):** the SaftyFW build now emits per-slot raw images `SaftyFW_slotA.bin`/`SaftyFW_slotB.bin` (position-dependent, slot A 0x00011000 / B 0x000E1000), and the Pico's `update_task` rejects an image whose reset vector does not point inside the destination slot (`update_task_slot_linkage_check()`, state `UPDATE_TASK_STATE_REJECTED_SLOT_LINKAGE = 8`, no protocol bump) — `5310dd78`, `7fb0b15f`, `6d3d0ada`. The ESP-side mirror of state 8 and the embedded-image boot path are in flight under the M-row below. | M8 |
| XL | **Two accepted risks in `docs/SAFETY_CASE.md`, new 2026-09-04: nothing currently mitigates either.** (1) Whether the two processors' independently-"healthy" verdicts are actually *correct* rather than merely self-consistent — e.g. both could be reading a shared, physically-faulted thermocouple wire. (2) Whatever sits downstream of both relays (a mechanical failure past K4) has no mitigation beyond K4 itself. Not a code gap — no reproducer exists and none is proposed; owner decision on whether/how to mitigate | `docs/SAFETY_CASE.md` H5, H9 |
| — | **Guard evidence is mostly host-tested, not hardware-verified.** Of ~20 tracked guard-level claims, 19 are host-tested and only **3** are hardware-verified (**corrected 2026-09-15 roadmap claim audit** — the three rows are S5's *hardware fit*, S5's *masking-before-fit* finding, and KilnFW thermal_guard guard 6. The E-stop polarity fix was named here as the third and is **not** one: `SAFETY_CASE.md` §4 classes S7 as host-tested and negative-tested. The count was right, the attribution was wrong, and it credited the E-stop path with evidence it does not have) — everything else, including all of S1–S4/S6–S14's trip logic and KilnFW guards 1/2/3/4/5/7/9, has never been provoked on real silicon | `docs/SAFETY_CASE.md` §4 rollup; `GUARD_TEST_MATRIX.md` §3 |

**Current owner-dependent items, 2026-09-09 sweep** (nothing in the code can close these; listed together so they don't have to be re-derived per session):

| Item | Blocks | Where |
|---|---|---|
| ~~Attach the safety thermocouple to the safety processor's own MAX31856 (J7)~~ — **stale, corrected 2026-09-09: fitted 2026-08-24**, reading `30.20 C (CJ 28.08 C)`; see M3 above. This row was left behind after the fact | — | `firmware/SaftyFW/docs/SAFETY_MODEL.md` §S5 |
| Bench webcam re-aim + LCD colour verification (numeric pixel sampling, not eyeball) | Display power / colour items above | `CLAUDE.md` "Camera aim (2026-09-06)"; `DISPLAY_ST7796_PLAN.md` §4 |
| ~~`iter_tune.c` wire-vs-delete decision~~ — **decided 2026-09-08: keep it, redesign it.** Three open questions for the owner in `ITER_TUNE_REDESIGN_PLAN.md` §9 are settled by that section itself (auto-snapshot anchor, 6-trial budget, bench-fixture-only scope). **Steps 1, 2 and 5 landed 2026-09-09** (`8f80a4de`, three latent defects found in review fixed same day, `249ce287`): `control/firing_score.c`/`firing_compare.c` plus a rewritten `iter_tune.c` decision core (old whole-firing IAE path deleted, not left dual), validated by a Monte-Carlo sim harness (`sim_iter_tune.c`) — 24/24 converged, 660 null comparisons 0% false-accept, 660 mismatched-plant runs 13 better/0 worse/0 cage violations. **Corrected 2026-09-14 roadmap truth-up: step 3 (the G1-G4 sim-harness gaps, `e0d2e006`) and step 4 (the §6.5 credibility gate, `225d4b91`) also landed 2026-09-09, and step 7's write-surface guard (`check_iter_tune_write_surface.ps1`, `f3fcd597`) too** — `docs/ITER_TUNE_REDESIGN_PLAN.md` itself was corrected 2026-09-10 to say so; this row never followed. **Still open: the credibility gate FAILS against a real recorded firing for a currently-unknown reason** (two explanations investigated and retired — see the 2026-09-11 sweep note above, do not re-propose either), plus the noise-floor artifact, persistence/HTTP surface, shadow mode, and step 9's first hardware trial (owner present). | `docs/ITER_TUNE_REDESIGN_PLAN.md` §8/§9 |
~~CT commissioning steps 0 and 6 (noise-floor capture, bench run with the owner)~~ — **both closed.** Step 0 was closed 2026-09-18; step 6 closed 2026-09-19 by owner decision as a software walkthrough only (see the M-size CT commissioning row above). S3/S4/S9/S14/S15 stay DORMANT (`i_normal_a not measured`) — that is now expected to stay true on this bench permanently, not pending further work; only a real load can change it. | `firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md`; M5 |
| ~~E-stop double-pole switch not yet fitted~~ — **closed 2026-09-10, owner decision: "im not going to wire the double pole switch on the fixture. consider it closed so long as the signal is checked and acted on."** Pole 2 (GPIO9 → S7 → `relay_owner` trip) is read, debounced, tripped and relay-de-energized independently on the RP2040, end-to-end pinned by `firmware/SaftyFW/test/test_estop_deenergizes_relay.c`, and bench-verified via `firmware/SaftyFW/README.md`'s procedure + `estop_verified`. Pole 1 stays permanently unwired on this fixture, so the E-stop here is firmware-mediated only — immaterial on this ~4 W/120 V fixture; a real kiln should still wire pole 1 | — (was: full E-stop hardware coverage beyond GPIO9's software-visible pole) | `firmware/SaftyFW/docs/HARDWARE.md` §5.1/§5.2; `docs/SAFETY_CASE.md` H7 |
| `abs_max_temp_c` must be raised **Pico-first, then ESP**, before a real (non-bench) firing — and the Pico's ceiling must never end up tighter than the ESP's | Real-kiln firing readiness | `docs/SETUP_WIZARD.md`; `docs/ON_OFF_ZONE_PLAN.md` |
| S8 is ARMED at a hand-set 20 C/min bench value, not the docs' 33.3 °C/min default (corrected 2026-09-14, see M3 row above) — whether 20 is right for a real kiln, or should move to 33.3 or an auto-derived value, is an open owner decision | S8 (rate-of-rise) real-world accuracy | M3 row above; `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md` |
| ~~RP2040 `config_store`'s `next_write_slot` torn-slot reprogramming defect~~ — **stale, corrected 2026-09-17: fixed and bench-verified 2026-09-14 (`88bb4333`)**, not "flagged, untouched" as this row previously said. `config_store_next_write_slot()` now confirms the target slot is actually still erased before programming into it, and every program is followed by a read-back verify; see `docs/CONFIG_FILESYSTEM.md` and `docs/audits/rp2040_config_store_write_atomicity_2026-09-14.md` for detail | Config-store write atomicity | `docs/audits/flash_endurance_review_2026-09-07.md` R2 follow-on; `docs/CONFIG_FILESYSTEM.md` |

### Software, doable now — no hardware, no decisions

| Size | Item | Where |
|---|---|---|
| L | **Every fault says what was detected and what to do** — a standing rule, not a closing milestone, so it never fully closes: applies to every fault surface added from here on. All of S6a's own checklist items landed 2026-08-28 | M13 |
| L | **CT clamp attribution — built and verified under both topologies (individual per-zone CTs and shared/summed), re-confirmed 2026-09-19** (host suite 2499/2499, `run_all_checks.ps1` 113/0/0). Nothing software-only remains. Pending, and hardware-gated only: any real PASS or FAIL verdict, and the true envelope settling behaviour at real current — both require a real kiln; this ~4 W bench can only ever produce INCONCLUSIVE, by design. See `docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md`'s status line. | `docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md`; `docs/CT_CHANNEL_MASK_PLAN.md`; `firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md` |
| XL | **Source layering + hardware abstraction** — `drivers/` reorg applied in `9f18ca5` (2026-09-05); HAL Phases 0-4 all done (every interface has a real backend + host fake, every named consumer migrated, include-boundary enforcement is strict, `esp_random.h` classified 2026-09-06). **Closed 2026-09-16: the hardware timing re-check.** All three named measurements (safety-link reply, display frame time, thermo read latency) now have hardware numbers — safety-link reply against the pre-existing 345 ms budget (max 340 ms observed, thin margin, see the 2026-09-14 composition correction on what that counter actually measures); display/thermo have no prior figure to compare against and are recorded as fresh baselines against the ESP32-S3's 300 ms interrupt-watchdog ceiling (max 81 ms / 57 ms observed, comfortably under). No measurable cost from the HAL indirection against any of these bars. Full detail: `docs/HW_ABSTRACTION.md` | M16; `docs/HW_ABSTRACTION.md` |
| L | ~~**An uncommissioned safety processor must refuse heating enable.**~~ Landed `5cd56b6`. Resolved 2026-08-28 by making CTs **optional hardware**: `ct_installed` (param `0x0109`) is a new ASKED commissioning question, and answering *no* drops the CT-map requirement **and** switches S3/S4/S9/S14 off while reporting them off. Verified on the live board: `commissioned: true`, heat permitted | M12 |
| S | ~~`thermal_guard_cfg_t.progress_band_c` unwired.~~ Done (`992f3954`, review conditions landed `e5375594`): `zone_cfg_t::progress_band_c` (ZONES_CFG_VERSION 21->22), `zones_config_get/set_progress_band_c()`, both build sites (`profile_executor_run.c`, `autotune_engine.c`), zones GET/POST wire (`z%u_progressband`). 0 = 3 °C firmware default, matching `error_band_c`'s sentinel convention. See `docs/audits/consumer_without_producer_2026-09-06.md` | M13 |
| **L** | **`iter_tune` redesign — see the M-row above (this table, "iter_tune.c wire-vs-delete decision") for current step status; consolidated here 2026-09-14 roadmap truth-up to remove a duplicate that had drifted (this row still said "steps 3-4 and 6-9 remain design-only" after both plan doc and code had moved past it).** Design background kept: owner decision 2026-09-08 to keep and redesign rather than wire `control/iter_tune.c` as-is (`3bf773af` superseded); score measures matched profile segments (ramp lag in seconds, dwell-entry overshoot, steady dwell RMS) against the target profile rather than whole-firing IAE, validated first in simulation by extending `firmware/KilnFW/App/test/sim_plant.c` with the four gaps (PWM window, actuation lag, MAX31856 quantisation, real measured plant/coupling constants) — *not* the deleted `SimFW`/`kilnsim`. 10 ordered steps (0-9), no kiln time before step 8. See `docs/audits/consumer_without_producer_2026-09-06.md` for how the module got here | `docs/ITER_TUNE_REDESIGN_PLAN.md`; `PID_EXPANSION_PLAN.md` |
| L | **ESP32-S3 OTA: single 8 MiB `app` slot plus a ~1.9 MB non-firing `recovery` image** — replaces today's two 3 MB `ota_0`/`ota_1` A/B application slots with one large slot, giving the application headroom (today's image is ~2.29 MB against a 3 MB slot) instead of eating into it every release; `factory` becomes a small `recovery` image whose only job is to receive and write an application image over Wi-Fi, deliberately unable to fire the kiln. No data partition moves or resizes, so `coredump`, `cfg`, `logs`, `nvs`/`wifi_nvs`/`kiln_nvs`/`profiles_nvs` (config, profiles, Wi-Fi credentials) all survive the migration by construction. `otadata` must be erased and rewritten because the app-partition offsets and sizes change. In-app RECOVERY MODE is deleted outright in favor of the separate image — it is the source of three prior board brickings. Migration itself is a one-time, cable-attached, per-board operation; see the plan for the Wi-Fi-password owner action that gates it | M8; `docs/OTA_SINGLE_SLOT_PLAN.md` |
| **M** | **The ESP application checks the Pico's firmware version on every boot and updates it automatically if needed — owner requirement 2026-09-16.** Most of the machinery already exists (the whole `UPDATE_BEGIN`/`DATA`/`END` relay, the `pico_img` staging partition, the interlock check, the auth and the single update mutex); the real gaps are three — no `SaftyFW` image is embedded in the ESP application, so there is nothing to update from at boot; no *expected* Pico identity exists anywhere in the tree, so "if needed" is undecidable; and there is no boot-time trigger with a persisted, read-back-verified attempt bound. Favourable finding: the Pico's config store lives outside both application slots, so an update preserves `abs_max_temp_c`, the arming state and the CT normals `i_normal_a` by construction. Four of five owner decisions settled the same day: an ESP rollback downgrades the Pico; the attempt budget is 3, persisted; an unrecoverable version mismatch (OVERRIDE) refuses to fire until resolved at the bench, surfaced on both the LCD and `/readiness`; and a `CONFIG_STORE_FORMAT_VERSION` bump (OVERRIDE) is carried automatically via in-place migration-chain steps in the Pico's own config store, per a project-wide config-migration policy, rather than excluded. ~~Hard prerequisite: this bench Pico still runs `SaftyFW.elf` directly rather than through the two-slot bootloader~~ — **P1 CLOSED 2026-09-18: the bench Pico now boots through the two-slot bootloader, slot A active, with a `KLN1` metadata record present**, so a relayed image now lands in a slot the boot vector does consult. What blocks the feature end to end is a different, newly observed defect — an ESP-driven relay cannot reach the data phase (see M8). **Progress 2026-09-16**: the hardware-independent slice landed in two commits (`aec61cb9`, `1c41a9e8`) — the pure boot-time decision function and its persisted 3-attempt counter, and a genuine sixth `readiness_gate` key (`pico_update`) that structurally refuses every firing-start path (web, LCD, benchproto) on an unrecoverable mismatch, fed today by an honest stub that always reports "no mismatch." Deliberately stopped there: wiring the stub to a real verdict needs the embedded expected image (plan steps 1-2), since arming the decision function without one would resolve every board's boot to the unrecoverable "no image" outcome and, with the gate now real, brick firing fleet-wide — that wiring no longer waits on the two-slot bootloader (P1, closed 2026-09-18) but on the relay's erase-phase failure recorded under M8. Plan with steps 0-11 and owner decisions §10: `docs/PICO_AUTO_UPDATE_PLAN.md`. | `docs/PICO_AUTO_UPDATE_PLAN.md`; M8 |
| **L** | **One-step-at-a-time config migration — owner requirement 2026-09-16:** "Each new fw should support migration of the nearest configuration forward allowing a one way one step at a time config update path". From the next schema bump onward, a release that bumps a persisted config version ships exactly one new step (N-1 -> N) and carries **only** that step, so a board more than one version behind cannot read its own config and must be upgraded one release at a time (settled by the owner 2026-09-16 as "one way one step at a time"). **Settled by the owner the same day: forward-only, NOT retroactive** — `convert_versioned_blob_to_current()` (`firmware/KilnFW/App/drivers/persist/zones_config_migrate.c`) stays as the pre-v26 tail, unchanged, and the chain's input floor is v26, so the step table is empty until `ZONES_CFG_VERSION` moves to 27. `ZONES_CFG_VERSION` is **not** bumped by this work. Governs the zones config, the kiln-config slots (`KILN_CFG_STORE_VERSION`, already a real two-step chain and the shape to copy), fire profiles, and the RP2040's `CONFIG_STORE_FORMAT_VERSION`; not the boot-critical NVS items nor the `cfg` partition bridges, which re-use the same versioned blob. Carries three dependent pieces: a per-step `calibration_missing`/`fields_set` policy on the Pico — the actual mechanism that lets a `CONFIG_STORE_FORMAT_VERSION` bump carry the CT normals `i_normal_a` forward instead of forcing recalibration; a firing-blocking quarantine for a newer-than-known blob, closing the "runs on firmware-default PID gains after a rollback, unannounced" hazard; and `tools/check_config_migration_steps.ps1` (landed 2026-09-17 for the zones store; **extended 2026-09-19** to also require a current-version step/macro for kiln-config slots, fire profiles, and the RP2040 store — narrower than the zones rule set for those three, see `docs/CONFIG_MIGRATION_CHAIN_PLAN.md` §5.1 for exactly what's still deferred per store; **extended again 2026-09-19** to teach the fire-profiles rule the frozen-input `_Static_assert`(sizeof)/`crc32`-last-field discipline that store's code already had but the check did not yet inspect — `test_check_config_migration_steps.ps1` now 22 assertions, up from 18), failing a build that bumps `ZONES_CFG_VERSION` without its step, its frozen-struct asserts and its captured-blob test. Testing is deliberately asymmetric: the existing tail keeps the coverage it has, every new step owes a real captured blob at its input version from the day it lands. All four owner decisions are now settled (one step only; steps expire past a fixed age; quarantine firing; mandatory pre-bump blob capture). **The blocking prerequisite (a migrated blob was never written back on the ordinary `nvs_load()` load path) is CLOSED, corrected 2026-09-17** — `d3f74d67` persists a migrated blob immediately, read-back verified, and `6985c89b` surfaces a write-back verify failure to the operator (`zones_cfg_migration_persist_fault_t`, wired through `/api/status` and the LCD trip strip). See `docs/CONFIG_MIGRATION_CHAIN_PLAN.md` §1.6, itself corrected the same day. The step table itself is still empty pending the first schema bump past v26 | `docs/CONFIG_MIGRATION_CHAIN_PLAN.md`; `docs/PICO_AUTO_UPDATE_PLAN.md`; `docs/OTA_SINGLE_SLOT_PLAN.md` |
| **S** | **PC-side arbitrary-jump config converter — owner request 2026-09-17.** Firmware stays one-step-only (row above); this is the separate PC-side tool that jumps any version to any other, best-effort, file-only, never touches a board. Landed: `tools/PcTools/src/kilnctrl/config_convert.py` (CLI `tools/PcTools/scripts/config_convert.py`, MCP `convert_config`), covering the `kilnctl_backup` document and a new `kilnctl_profile_blob` raw-NVS wrapper (v1-v4), plus a mirror-drift check (`tools/check_config_convert_mirror.py`, negative-tested). `kiln_cfg_store`'s package format, SaftyFW's raw `config_store_record_t`, and the ESP's raw `zones_cfg_t` blob (`ZONES_CFG_VERSION`) are deliberately not yet implemented — refused by name, not attempted. Review before landing caught the CRC being computed over the body only, where firmware's `compute_profile_crc()` covers the whole struct with `crc32` zeroed; fixed, decode now verifies the CRC too (29 tests). See `docs/CONFIG_MIGRATION_CHAIN_PLAN.md` §7. | `docs/CONFIG_MIGRATION_CHAIN_PLAN.md` |
| **L** | **LCD dashboard and profiles rework — owner request 2026-09-19.** Six items, all LCD (480x320, no scrolling, no new colours): (1) dashboard shows the selected profile's name to the left of the Start button; tapping it opens a profile picker, favorites first with a star; (2) the LCD Profiles page becomes that same list, with a New button (profile icon) at the top of the screen and per-profile delete matching the web page; the My Profiles / Built-ins / Restore Hidden / New Profile buttons go away; (3) the Temperature page also shows the safety relay's state; (4) the LCD loses the ability to reset relay life (web keeps it); (5) the right quarter of the dashboard, beside the graph, shows relay states, zone temperatures and zone / zone-group power in the style of the web Thermocouples & Zones page, without the graphic; (6) the dashboard Settings button sits flush top-right (today it is offset left); (7, added 2026-09-19) the dashboard's `Kiln: <name>` line is shown only when the board holds two or more kiln configurations. Wave 1 in progress 2026-09-19. The profile list must share its favorites ordering with the web dashboard's `favorites` API rather than re-deriving it. **Planned 2026-09-19: `firmware/KilnFW/docs/UI_PLAN.md` Section 6 — "LCD dashboard and profiles rework, owner request 2026-09-19"** carries the per-item detail: files and functions, the data source and lock status for each (items 1, 3 and 5 read the two snapshots `ui_page_home_refresh.c` already fetches, so no new task, timer or HTTP route is needed), pixel arithmetic against `UI_THEME_PAGE_CONTENT_BUDGET_PX`, the host tests and `check_*.ps1` entries each item owes, a numeric `capture_lcd.ps1` verification recipe per item, a three-wave parallel split with the file collisions named, and five owner decisions, all answered 2026-09-19 — including the owner's override to four 64px rows per profile page, which fits the 268px budget exactly (`4*64 + 3*4 = 268`) at the cost of the page's header row, moving the New button and the page indicator into the topbar. Item 6 is root-caused there, not guessed: the topbar's hidden warning indicator is built after the gear and collapses to zero width under `LV_FLEX_ALIGN_START`, leaving the gear 40px short of flush right. **All seven items are in source at HEAD as of 2026-09-20** (item 5's LVGL rail widget tree included; this row's earlier "still open" caveat was stale). Not yet visible on the bench: both boards still run `73c1da94`, older than every commit in this rework, and flashing is blocked until the unacknowledged `touch_log_tap_targets` crash report is reviewed (fix `3f86e899` is in source). The per-item numeric `capture_lcd.ps1` verification is therefore still owed after the next flash. | `firmware/KilnFW/docs/UI_PLAN.md` |
| S | ~~Zone 0 gets the same per-group "Same as zone N" selectors as the other zones — owner request 2026-09-19.~~ Done: `zones_page.html` now renders the whole-zone `settingssrc` select and five `groupsrc` selects for zone 0 exactly as for zones 1..N-1 (`resolveTerminal()`/`resolveGroupTerminal()` no longer hardcode zone 0 as the fixed root); the server side needed no changes (`zones_http_post_parse.c`'s parser and the shared chain-walk were already generic per zone index). The one real fix was the cycle tie-break: `zones_config_json_normalize_settings_source_cycles()` now resets only the highest-indexed zone on a detected cycle (was: every member), so zone 0's own link survives a cycle it's part of. See `firmware/KilnFW/docs/ARCHITECTURE_DECISIONS.md`'s "Zone 0 gets the same per-group selectors" entry | `firmware/KilnFW/App/drivers/http/zones_page.html`; `firmware/KilnFW/App/test/test_zones_group_frames.js`; `firmware/KilnFW/docs/ARCHITECTURE_DECISIONS.md` |
| **M** | ~~**Visually simplify the Thermocouples & Zones web page — owner request 2026-09-19.**~~ **Done, 2026-09-19.** (a) Show-when-enabled landed for all five sections (Continuous Tuning, Cross-zone coupling matrix, Tuning quality, PID Autotune, Measure Zone Normal Current), gated on the `hidden` DOM property, never inline display. **Open owner question answered: yes** — each hidden section shows one muted `class="hint"` line in its place (e.g. "Enable continuous tuning on a zone to see learning results here"). (b) Relay-feedback test removed from the page. **Correction to this row's wire claim: no URI-handler slot was freed** — `/api/autotune/start` was always one shared route for both `method=step` and `method=relay`, so there was never a separate relay-only route to remove; cap stays 150/151. The firmware engine (`autotune_engine_relay.c`) is left in place, untouched, because `test_autotune_engine_prestart.c` host-tests it directly — dead-coded-only retirement did not apply since it is not dead, it is still tested. (c) Step test and PID Autotune collapsed into one panel; the tuning-method recommendation panel moved into that section's `<details>` info glyph. (d) Noise-floor section and its renderers removed outright (`test_noise_floor_panel.js` deleted with it); `tuning_recommendations.json`'s `noise_floor` field is untouched. Positive+negative gate tests added (`test_zones_page_visibility_gates.js`, 33 cases). See `firmware/KilnFW/docs/ARCHITECTURE_DECISIONS.md`'s "Zones page visual simplification and prose shortening (2026-09-19)" section | `firmware/KilnFW/App/drivers/http/zones_page.html`; `firmware/KilnFW/docs/ARCHITECTURE_DECISIONS.md` |
| **S** | ~~**Shorten and simplify the prose on the Thermocouples & Zones web page — owner request 2026-09-19.**~~ **Done, 2026-09-19**, shipped together with the show-when-enabled row above in the same pass: top-level section descriptions across the page were cut to one or two plain sentences, repository notes (commit hashes, plan-doc names, audit dates, bench-provenance "honesty note" paragraphs) were dropped from operator-visible text, the guard-suite/empty-kiln warning is now stated once at the top of the PID Autotune section instead of per sub-section, and existing `<details>` info glyphs were kept for the long form. Source-level HTML provenance comments were shrunk to one-line pointers at their owning doc. This pass covered the page's top-level sections that were touched by the removals/collapses above; per-zone/per-field prose elsewhere on the page (e.g. `INFO_HTML` entries, zone-type descriptions) was out of scope for this change | `firmware/KilnFW/App/drivers/http/zones_page.html` |
| **L** | **Thorough OTA testing of both processors — owner request 2026-09-19.** Not started. Exercise every field-update path end to end on the bench and record the evidence, rather than the single ESP round trip and the two failed Pico attempts recorded so far. ESP32-S3: update into the `app` slot over Wi-Fi from a real `.bin`, rollback, a deliberately corrupted image (bad CRC, wrong build, truncated), a power loss mid-write, an update attempted during a firing (must be refused), an update under web auth with and without credentials, the recovery image receiving an application image, and `otadata` state after each case; confirm PID gains, zones config, profiles, favorites and Wi-Fi credentials are byte-identical before and after each run. RP2040: the relayed `UPDATE_BEGIN`/data/commit sequence over the isolated link into the inactive slot, boot from that slot, fallback to the previous slot on a bad image, the erase-time watchdog case (`docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md`) and the CRC-variant defect that blocked every image, an update attempted with a trip pending (must be refused, already observed), and commissioning config read back byte-for-byte afterwards. Both: the dual-reflash S6a handshake trip and its `safety_clear_trip()` recovery, and a scripted `run_pctools_tests`-style regression so the whole matrix can be re-run from PcTools rather than by hand. Depends on the two open Pico defects (erase watchdog, CRC variant) being fixed first; the ESP matrix can start now. Record each case as PASS / FAIL / NOT RUN with the commit flashed, in a single audit doc, and update the M8 rows to match. Home for this matrix is now suite OT of `docs/BENCH_TEST_SYSTEM_PLAN.md` (Section 3.4), whose `summary.json` is the PASS / FAIL / NOT RUN record. | M8; `docs/BENCH_TEST_SYSTEM_PLAN.md`; `firmware/CommonFW/docs/UPDATE_PROTOCOL.md`; `docs/OTA_SINGLE_SLOT_PLAN.md` |
| **L** | **Standardized bench test system — owner request 2026-09-19.** Wave 0 (skeleton) landed: `bench_test_run`/`bench_test_list`/`bench_test_last` MCP tools plus `tools/bench_test.ps1`, the `smoke` suite (17 read-only cases), registry/runner/report scaffolding, and `tools/check_bench_test_registry.ps1`. One callable entry point that runs a chosen suite or the whole matrix against the test kiln through the existing `kilnctrl` tools only, and writes one structured log per run (`logs/bench_test/<run>/summary.json` plus a human transcript in `docs/BENCH_TEST_LOG.md`) with PASS / FAIL / SKIP / NOT_RUN / INCONCLUSIVE per case, the commit flashed on each processor, and timing. The plan catalogues 203 cases in nine suites: static/host layer (reuses `run_all_checks.ps1`), flash layout and image validity (partition table, running partition, `otadata`, boot_guard, app_desc, Pico slot), stack sizes (idle and after a firing, ESP and Pico), OTA on both processors (absorbs the row above, now allowed to include nightly JTAG flashes per decision 10 below), bounded autotune on the 4 W fixture, ~5 min heating profiles per mode via a hidden bench profile slot placed after the 100 user slots and the live-edit slot (owned by `slots100`, never user-visible), every web page feature across all pages including a generated per-route auth-tier sweep and now-permitted commissioning writes (decision 7), every LCD page and mode (post-Section-6 LCD) judged by webcam pixel sampling, and the safety-processor surface. Defines `smoke` (~10 min, no heat), `nightly` (~1 h, now including AP-fallback and opt-in JTAG flashes) and `full` (hours) subsets, the fixed ordering, twelve things the harness must never do, and a five-wave implementation with parallel waves. All ten owner open questions are now decided (`docs/BENCH_TEST_SYSTEM_PLAN.md` §7) — none remain open. Not meant to run whole often; pieces are for routine use. | `docs/BENCH_TEST_SYSTEM_PLAN.md` |
| **M** | **Edit the running profile mid-firing, from the web UI — owner request 2026-09-18. DELIVERED 2026-09-19 (pass 2): `live_profile_page.html`, five ADMIN routes in `profiles_live_http.c`, fork-on-edit, HARD-mode validation, executor pickup, end-of-firing prompt; `test_profiles_live_http.c` 104/104. This row's earlier "groundwork only" text was stale — see `docs/LIVE_PROFILE_EDIT_PLAN.md` §10 for what landed and its four handler-bug fixes. Still in flight 2026-09-20: a duplicate-name refusal shared by both profile save paths (`live_edit_name_collides()`), under review.** Design record kept below. A web-only page that changes the profile a firing is currently executing, so a firing can be improved in progress. Editing forks immediately into a new profile (an ordinary user slot, so it inherits NVS, the `cfg` dual-write and migration, and survives a reboot mid-firing), leaving the original untouched on disk; at the end of the firing the operator is asked to name-and-save the copy or overwrite the original, with overwriting a shipped/builtin schedule refused outright server-side — structurally, since builtin ids are `>= PROFILE_BUILTIN_ID_BASE` and back a `const` table with no writable storage at all, so a forged request cannot express the attack. **Two hard requirements shape it:** an edit that would exceed the running zones' `max_temp_c` (or their ramp ceiling) is *rejected* server-side before the working copy is written, not warned about in the browser — deliberately stricter than ordinary save-time validation, which is advisory because profiles are portable while a live edit is not — and re-checked inside the executor before the swap is adopted, closing the ceiling-changed-underneath window; and both profile editors must share one implementation, which means factoring `parse_profile_fields()` and one `profiles_validate_candidate()` out of `profiles_edit_http.c`/`profiles_http.c` and the segment editor out of `profiles_page.html` into a served `profile_editor.js`. **The shared-validator half of that factoring has landed (`73c322bb`): `profiles_validate_candidate()` now single-sources ceiling/ramp validation, with a HARD mode added for the coming accept/pickup paths while ADVISORY mode preserves existing save-time behavior byte-for-byte** — this is groundwork only, not the live-edit feature, which still has no UI, no fork-on-edit, and no pickup path. The Pico's `abs_max_temp_c` is never written, read as a limit, or derived from — there is no code path from this feature to any Pico parameter, which is how "never exceeded, never tightened" is met. Pickup is a generation counter polled by the control task, the same shape as the existing mid-run zones-config reload, and is continuous by construction because the ramp state lives in `s_exec`, not in the profile. **Sequencing dependency:** the shared factoring rewrites files the kiln-profiles work also touches; the profile-favorites work it also used to depend on has since landed (`acf36720`, `18e8b60a`). Five owner decisions were open and are now recorded as resolved in the plan doc (slot budget, a ceiling lowered mid-firing, name collisions, editing while PAUSED/FAULTED, the prompt under web auth) — none of them blocked starting | `docs/LIVE_PROFILE_EDIT_PLAN.md` |
| **L** | **100 user profile slots plus a live-edit slot — owner request 2026-09-19.** In source at HEAD 2026-09-20: `PROFILES_MAX_COUNT` 8 -> 100, `LIVE_EDIT_WORKING_SLOT_ID` = 100, hidden bench slot `PROFILE_BENCH_SLOT_ID` = 101, `PROFILE_BUILTIN_ID_BASE` 128 as the hard ceiling; 4-word slot bitmaps with legacy-blob read migration; chunked list handlers; `s_profiles.profiles[]` moved to PSRAM; `cfg` partition grown to 0x250000 (source only, not yet flashed); backup import PSRAM-buffered with 100/101-profile tests; web name filter plus Favorites / Recently fired grouping; LCD picker shares the web `favorites` ordering; `tools/check_profiles_capacity.ps1`. Plan tasks 1-11 done. **Open: task 12, the bench migration** (backup first, then flash the new partition table) — hardware-gated, its own session. | `docs/PROFILE_SLOTS_100_PLAN.md` |
| S | ~~Off-theme white button backgrounds on the web diagnostics danger zone~~ — done `e8772211` (2026-09-20): `background:#fff` -> `var(--bg)`, and `lint_pages.js` gained a `hardcoded_background_literals()` rule (enforced via `check_lint_pages.ps1`) so a literal background colour outside a `lint-color-ok`-annotated line fails the suite; negative-tested. | `firmware/KilnFW/App/test/lint_pages.js` |
| S | ~~Credit the research the control design draws on~~ — done `d833161c` (2026-09-20): `CREDITS.md` "Research and control literature" section plus `docs/research/README.md` mapping each source to the code it influenced (three-node sensor model, `zone_coupling_solve.c` candidate 3) and listing what was surveyed but not used. | `CREDITS.md`; `docs/research/README.md` |

### Blocked on hardware that does not exist yet

| Size | Item | Where |
|---|---|---|
| S | Time the firing abort (30 s) with a stopwatch during a real running firing — the 1.5 s staleness ceiling was bench-verified 2026-09-06 (`LINK_PROTOCOL.md` §8) with no firing needed | M6 |
| M | S9's welded-contactor escalation — by definition needs a welded contactor. **Checked 2026-09-03: SimFW cannot do this — SimFW itself no longer exists** (removed `8553244`, 2026-08-28; `firmware/UnitTestFw` took its place and is unrelated ESP32-S3 bench-instrument firmware — DAC/AD9833/OLED/PCF8575 — with no path to the safety processor's current-sense input at all). Even when SimFW existed, its own removal commit records that `ct_calibration` "needs the fixture to physically drive current into the CT" — S9 (`firmware/SaftyFW/src/safety_guards.c:363-389`) latches only on real `any_current_present`, gated by `in->context_valid`, `in->current_sensing_commissioned` and NOT `in->current_sensing_disabled`; that flag comes from the CT's analog current-transformer signal through `current_sense.c`, not a GPIO a simulator MCU could assert. What would actually be required: a fixture that injects genuine AC current through the CT sense loop while the K4 drive line is confirmed de-energized — i.e. a hardware jig, not firmware simulation — plus a CT actually fitted and commissioned (`ct_installed=yes`; this was `ct_installed=no` on the bare bench as of the checked date above). **Corrected 2026-09-18, then superseded the same day by the CT-summed-topology fix:** the board reads `ct_installed=1` (channel 2's summed CT fitted and calibrated, per the CT-commissioning bench check at the top of this file); `s_current_sensing_commissioned` used to require all three `k_ct_v_per_a` entries greater than zero regardless of topology — a deliberate decision at the time, but one that permanently blocked any SUMMED-topology board (only one CT, wired to channel 2) from ever reporting commissioned. It now instead requires `k_ct_v_per_a > 0` only on channels that are actually fitted for the board's topology (`config_store_current_sensing_commissioned()`, `firmware/SaftyFW/src/config_store.h`), landed together with masking `any_current_present` to fitted channels only (channels 0/1's idle ADC noise must not count) so the unclearable S9 latch cannot arm off noise. **S9's `TRIP_INEFFECTIVE` is now armable on this board for the first time** — this is a live change to the bench's safety posture, not only to source, once flashed: a welded-contactor exercise here can now actually latch S9, independent of the fixture-availability question above. | M4 |
| M | AP-fallback verified end to end (needs a router with correct *and* deliberately-wrong static config) | M6 |
| M | Per-channel CT-to-jack commissioning and the ADC noise-floor measurement — see the M-size CT commissioning row far above (M5's table), `CT_COMMISSIONING_PLAN.md` steps 0 and 6 | M5 |
| M | **HW changes:** relay status LEDs for K1–K4/S9, distinct connector types for the thermocouple daughterboards, I2C broken out on an expansion connector. (LCD backlight control's flying wire is fitted and confirmed — see M1, closed 2026-09-04.) | M1 |
| S | **Blocking, before the MSP4031 touches J2 at all**: meter module pins 10/12 (CTP_SCL/CTP_SDA) at 5V — confirms or clears a hazard that can back-feed the SX1509/ESP32-S3 through the shared I2C bus. `DISPLAY_ST7796_PLAN.md` §4 | M1 |
| M | DEBUG header and GP16/GP17 access before A1 is soldered down | M0 |
| L | Field updates exercised against real hardware — see the M-size "Field-update hardware exercise" row above (consolidated 2026-09-14 roadmap truth-up, was a duplicate): ESP half done; the Pico half's 2026-09-06 interlock refusal is superseded by the 2026-09-18 attempt, which reached the erase phase and failed there on an RP2040 watchdog reset | M8 |
| L | `GUARD_TEST_MATRIX.md` §3 — every enabled guard's real trip, safe-state power-on, sensor open-circuit, current-mapping commissioning | M4 |

---

## Where each kind of task is planned

| Plan | Owns |
|---|---|
| [`ROADMAP.md`](ROADMAP.md) (this file) | Milestone order, cross-processor dependencies |
| [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md) | Main firmware: web UI, profiles, PID, thermal protection, storage |
| [`firmware/KilnFW/docs/PROJECT_STATUS.md`](firmware/KilnFW/docs/PROJECT_STATUS.md) | What in `KilnFW` is built vs. verified — the honest ledger |
| [`firmware/KilnFW/docs/UI_PLAN.md`](firmware/KilnFW/docs/UI_PLAN.md) | LCD + web UI usability/cleanup plan — no-scroll LCD audit, phone/tablet web audit, prioritized fix queue |
| [`firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`](firmware/KilnFW/docs/PID_EXPANSION_PLAN.md) | Per-zone control-algorithm choice (Cohen-Coon rule, fuzzy-PID layer), cross-zone coupling measurement (RGA) and feedforward |
| [`firmware/KilnFW/docs/DISPLAY_ST7796_PLAN.md`](firmware/KilnFW/docs/DISPLAY_ST7796_PLAN.md) | Second LCD panel (ST7796/MSP4031) support, runtime panel auto-detection, display SPI async/DMA |
| [`firmware/KilnFW/docs/FLASH_BUDGET.md`](firmware/KilnFW/docs/FLASH_BUDGET.md) | The 16 MB flash: partition table, image size, what has been reclaimed |
| [`firmware/KilnFW/docs/DRAM_PSRAM_STATUS.md`](firmware/KilnFW/docs/DRAM_PSRAM_STATUS.md) | Internal SRAM reclamation — allocator threshold, stack sizing, PSRAM relocation |
| [`firmware/KilnFW/docs/WEB_UI_RESPONSIVE.md`](firmware/KilnFW/docs/WEB_UI_RESPONSIVE.md) | Browser UI across display sizes: token consolidation, shell layout, the responsive sweep |
| [`firmware/KilnFW/docs/ARCHITECTURE.md`](firmware/KilnFW/docs/ARCHITECTURE.md) | Tasks, priorities, owner-task queues, single-writer ownership doctrine |
| [`docs/SETUP_WIZARD.md`](docs/SETUP_WIZARD.md) | The whole-kiln setup wizard: step order, dependencies, which steps need heat or the owner, and where progress persists |
| [`docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md`](docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md) | Commissioning-time proof that each CT clamp is on the conductor the configuration claims, and that each zone's normal current is what it should be: the pass/fail/**inconclusive** verdict, its configuration fingerprint and staleness rule, where it is stored and surfaced, and what this ~4 W bench can and cannot ever determine |
| [`docs/LIVE_PROFILE_EDIT_PLAN.md`](docs/LIVE_PROFILE_EDIT_PLAN.md) | Editing the profile a firing is currently running, from the web UI: what is editable past/current/future, the fork-on-edit working copy and where it survives a reboot, the end-of-firing save-or-overwrite prompt and its builtin refusal, the server-side temperature and ramp bounds and their re-check at pickup, and the shared factoring that keeps both profile editors on one implementation |
| [`docs/HW_ABSTRACTION.md`](docs/HW_ABSTRACTION.md) | KilnFW `drivers/` layering into role directories, and the `firmware/hwAbstraction/` tree (interface/esp/pico/host) for both firmwares — M16 |
| [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) | Safety firmware, phases 0–10 |
| [`firmware/SaftyFW/docs/SAFETY_MODEL.md`](firmware/SaftyFW/docs/SAFETY_MODEL.md) | What trips, why, and the anti-nuisance doctrine |
| [`firmware/SaftyFW/docs/ARCHITECTURE.md`](firmware/SaftyFW/docs/ARCHITECTURE.md) | Tasks, priorities, core affinity, logging transports |
| [`firmware/SaftyFW/docs/HARDWARE.md`](firmware/SaftyFW/docs/HARDWARE.md) | The traced board, pin map, bench connections |
| [`firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`](firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md) | Guard-by-guard test coverage and real-input reachability |
| [`firmware/SaftyFW/docs/CONFIG_REFERENCE.md`](firmware/SaftyFW/docs/CONFIG_REFERENCE.md) | Every safety tunable, its default, and whether getting it wrong is dangerous |
| [`firmware/SaftyFW/docs/COMMISSIONING.md`](firmware/SaftyFW/docs/COMMISSIONING.md) | How those values get set from the web GUI and persist on the safety processor across OTA |
| [`firmware/CommonFW/README.md`](firmware/CommonFW/README.md) | Shared `kilnlink` code, used by both firmwares |
| [`firmware/CommonFW/docs/LINK_PROTOCOL.md`](firmware/CommonFW/docs/LINK_PROTOCOL.md) | The wire, both ends — the contract neither side may break alone |
| [`firmware/CommonFW/docs/UPDATE_PROTOCOL.md`](firmware/CommonFW/docs/UPDATE_PROTOCOL.md) | Field updates for both processors: interlocks, one-password auth, ESP OTA partitioning |
| [`firmware/SaftyFW/docs/BOOTLOADER.md`](firmware/SaftyFW/docs/BOOTLOADER.md) | The RP2040 bootloader, flash layout and recovery mode |
| [`tools/PcTools/TODO.md`](tools/PcTools/TODO.md) | GUI, MCP, GPIO probe, debug and logging for **both** processors |
| [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md) §§12–13 | M10's instrumentation: HTTP route-table cap, internal-DRAM low-water alarm, task-stack high-water reporting |
| [`docs/SAFETY_CASE.md`](docs/SAFETY_CASE.md) | The cross-processor safety argument: hazard list, guard/measure/accepted-risk mapping, residual risks, evidence classification (argued/host-tested/hardware-verified). Written 2026-09-04, still thin on hardware evidence — see its own §5 |
| [`docs/SYSTEM_ARCHITECTURE.md`](docs/SYSTEM_ARCHITECTURE.md) | The system level spanning both processors: end-to-end command/trip paths, the board-to-board interface, power domains and GND crossings, cross-processor boot/shutdown ordering. Written 2026-09-04 |
| [`docs/REPO_LAYOUT.md`](docs/REPO_LAYOUT.md) | The hardware/software reorganisation and its blockers |
| [`docs/SETUP.md`](docs/SETUP.md) | Fresh-clone setup: what is machine-specific, and how `tools/setup.ps1` handles it |
| [`docs/RELEASE_HARDENING_PLAN.md`](docs/RELEASE_HARDENING_PLAN.md) | What has to be true before this controls a real kiln unattended: coredump readback for the open `profile_executor` panic, a release-gate audit against this repo's own vacuous-pass history, long-duration soak with a machine-checked verdict, guard provocation on hardware split into what the 4 W bench can and cannot ever close, failure injection, OTA/recovery mechanics, and the first-firing checklist (**closed** — [`docs/FIRST_FIRING_CHECKLIST.md`](docs/FIRST_FIRING_CHECKLIST.md), blocker 8). Risk-ordered, remaining blockers marked. **Corrected 2026-09-17 roadmap-upkeep sweep: this row's "starts once `docs/KILN_PROFILES_PLAN.md` is finished" was stale** — the plan's own doc shows it opened 2026-09-16 and several BLOCKER sub-items already closed (e.g. `bfa60679`, Blocker 6's schema-downgrade-hazard sub-item), regardless of `KILN_PROFILES_PLAN.md`'s status; read that plan's own status line for what remains, not this gating note |
| [`docs/WEB_AUTH_PLAN.md`](docs/WEB_AUTH_PLAN.md) | Username/password and roles for the web GUI plus a numeric PIN for the LCD: an always-open Dashboard tier, `user` (start/stop only) and `administrator` (everything else), the full three-tier classification of all 140 HTTP routes, hashed and salted credentials in NVS, a fail-closed enforcement point with a mechanical route-tier check, an inactivity lock with a ten-second stay-unlocked prompt on both interfaces, and an E-stop-gated physical credential reset. Authentication ships defaulted OFF on both interfaces, so a field-upgraded board is unchanged. **Corrected 2026-09-17 roadmap-upkeep sweep: "Follows `docs/RELEASE_HARDENING_PLAN.md`" was stale — both plans opened the same day (2026-09-16) and have been landing concurrently since (route tiers, credential storage, LCD PIN entry, the web login surface, admin password/settings page, and the web-GUI inactivity lock all host-tested per `docs/WEB_AUTH_PLAN.md`'s own status line), not sequenced as this row claimed** |

---

## The dependency spine

Four facts set the order. Everything else can be shuffled.

1. **The safety UART pins are swapped in `KilnFW`.** Until that is fixed the link
   cannot pass a byte in either direction, so every link-dependent task is
   blocked behind it.
2. **The wire contract is shared code.** Writing it twice guarantees the two
   copies diverge, so `CommonFW` comes before either firmware consumes it.
3. **The relay is the last thing to wire up.** Guards get built and host-tested
   against synthetic inputs before anything can physically open a contactor.
4. **The liveness rule cuts both ways.** Once the ESP refuses to heat without
   safety telemetry, a main board with no Pico fitted cannot heat either — so
   that switch is thrown deliberately, with a documented bench escape hatch.

---

## What is actually left

The milestones below are the detail. This is the honest short answer, because
after M0 cleared, "what remains" stopped being a code question and became
mostly a hardware-and-decisions question. Reviewed 2026-08-28.

**The bench changed on 2026-08-28 and several long-standing "theoretical"
items became live.** The heating elements are now PHYSICALLY CONNECTED. Every
statement anywhere in this repo that reasons from "relays may be activated but
nothing will get warm" is now false, and that assumption is load-bearing in
more places than it looks.

Two consequences that need to be read together:

- **The fixture is limited to 80 °C, and that is a property of the FIXTURE,
  not of the kiln.** It is currently enforced by `max_temp_c = 80` on all
  three zones (set live over `/api/zones`, previous config saved). This is a
  TEST THRESHOLD and must come out before a real kiln — `SaftyFW/TODO.md`
  phase 9 already carries "confirm no test threshold was left in place", and
  this is now a concrete instance of it, not a hypothetical.
- **Only ONE processor is enforcing that limit.** The safety processor's
  `abs_max_temp_c` reads `set: true, value: 0`, and 0 on that field means
  *never trip*. **(Corrected 2026-09-15 roadmap claim audit: this 2026-08-28
  snapshot is superseded by work recorded higher in this file —
  `safety_ceiling_sync.c` pushes the ESP's configured ceiling to S1 on every
  link-up tick, and `c99356f8` makes that an exact hard cutoff with no added
  headroom, bench-verified in both directions. The present live value is a
  hardware reading this audit was not permitted to take.)** So the independent overtemperature guard is not armed, and the
  ceiling is enforced by the same processor that commands the heat. Note the
  reporting trap that hid this: `/api/readiness` says "all 58 safety
  parameters have values", which counts a zero as a value — the field next to
  it, `commissioned`, says `false`. Setting a Pico-side ceiling is offered and
  awaiting the owner's number.

**Blocked on you — mostly answered on 2026-08-28.** Six of the nine questions
that stood here were answered in one message, and the answers were largely
*"the operator should be able to enter that"*, which converted them from
questions into [M12](#m12--commissioning-the-operator-can-actually-do--opened-2026-08-28).
Summarised, because the answers matter beyond the milestone that implements
them:

- **An uncommissioned safety processor must NOT grant heating enable.**
  Unqualified NO. It grants it today, which is the only reason the bench can
  heat, so M12 lands this one **last** — see that milestone's ordering note.
- **Kiln maximum temperature** is entered on a safety web page, and feeds
  `abs_max_temp_c`. **Thermocouple maximum is inferred from the thermocouple
  type**, not asked for.
- **Every relay is rated for 100% duty and inrush is negligible — the board is
  designed for it.** This closes the SSR-vs-contactor-coil question and the
  2 A/125 VA duty-window check outright.
- **Breakers are assumed sized for the full kiln load at 100% duty.** Instead
  of per-zone element power, the operator enters maximum expected kiln power
  on the safety page, and the zones page gets a button that measures each
  zone's normal current by energizing them one at a time. That measurement is
  then used at runtime to check each CT is on the zone it is configured for,
  and to warn when it is not.
- The twelve stale `display_*` MCP tools were left to my judgement: delete.

All three items once listed here as still unanswerable are now closed: physical
zone arrangement (owner-confirmed 2026-09-03), the deferred S8 sanity rate
(decided 2026-09-05 — see the table above), and `hardware/UnitTestFixture/`
(owner said KEEP, 2026-09-05). **Scope firmed up 2026-09-05: `UnitTestFixture`
is a control device only** — a PcTools/MCP surface driving its I/O expanders
to flip relays, for shorting/opening thermocouples, opening heater
connections, and simulating SSR lock-ups. It stays excluded from the HAL
boundary and the `drivers/` reorg (M16/`HW_ABSTRACTION.md`). See
`docs/UNIT_TEST_FIXTURE_STATUS.md` for the relay inventory, wire protocol, and
the MCP control surface.

**Blocked on hardware that does not exist yet.** All of this is scripted and
waiting, not unwritten:

- `GUARD_TEST_MATRIX.md` §3's trip rows — every enabled guard's real trip,
  safe-state power-on, sensor open-circuit, current-mapping commissioning.
- ~~The safety processor's own MAX31856~~ — **fitted 2026-08-24 and verified
  reading 30.2 °C.** ~~Still absent: any CT~~ — **2026-09-05: a CT is now
  fitted on `Current3` (GPIO28/ADC2) and confirmed working** — 1A:1V CT,
  ~+59 mV DC offset, reading the summed current of ALL heaters (not
  per-zone); `Current1`/`Current2` remain unpopulated. See the new items
  below and `firmware/SaftyFW/docs/HARDWARE.md` §9.
- S9's welded-contactor escalation, which by definition needs a welded
  contactor. **Checked 2026-09-03**: not a SimFW task — SimFW was removed
  (`8553244`, 2026-08-28) and its replacement, `firmware/UnitTestFw`, is
  unrelated ESP32-S3 bench-instrument firmware with no connection to the
  safety processor's current sense. S9 latches on real `any_current_present`
  from the CT's analog signal (`safety_guards.c:363-389`), which requires
  physically driving current through the CT — a hardware jig, not something
  any firmware simulator asserts over GPIO — plus a CT actually fitted and
  commissioned, since `ct_installed=no` switches S9 off on today's bare bench.
- CT hardware: split-core current transformer probes, 1 V output at full
  scale (amp rating = full-scale value); no coupling/isolation transformer
  is used. See `docs/CONTACTOR_FEEDBACK_OPTIONS.md` §7.
- Link-staleness *timing* — the 30 s firing abort only. The 1.5 s ceiling was
  bench-verified 2026-09-06 (`firmware/CommonFW/docs/LINK_PROTOCOL.md` §8):
  `debug_reset(peer="pico")` silenced telemetry and the device log recorded
  the fault firing at the coded 1500 ms threshold (observed at 2250 ms of
  silence, within 500 ms poll-granularity slack), self-clearing on the next
  good frame. The 30 s abort needs a real running firing to observe and is
  still untimed.

**Genuinely still software, and doable without you or the fixture.** This is
now a short list, which is the point:

- **Display power (brightness/idle-timeout/keep-on-while-firing/display-on-
  error), 2026-09-04.** New feature, not an M11 reopen — see
  `firmware/KilnFW/docs/UI_PLAN.md`'s "Display power" section for the full
  writeup. Pure decision core + persisted settings + HTTP API + settings-page
  UI are built and host-tested. **Brightness is no longer inert
  (`be02d34`):** the flying wire (GPIO15 → panel pin 8) is fitted, owner-
  confirmed by meter, `KILNCTL_BACKLIGHT_PWM_ENABLE` now defaults on, and ON
  duty is driven from the operator's brightness setting rather than a
  Kconfig constant — not yet confirmed by meter/eye that the panel actually
  dims, that's the next check. `keep_on_while_firing`/`display_on_error` now
  default **true** (`f028e2f`, owner decision from a real finger on the
  glass), so both are already right the day a timeout is chosen. The
  `screen_idle.c`/`lvgl_port.c` touch-swallow/timeout hookup landed the
  same day (`7fc17cc`), bound to real producers — the executor for
  `firing_active`, the RP2040 DIAG trip for `error_active`. Review then found
  both producers too narrow (`192eb7d`): keep-on-while-firing blanked the
  screen mid-autotune, since autotune holds relay authority with the executor
  IDLE, and display-on-error missed the ESP's own guard aborts, which never
  touch the RP2040's `diag_state`. Both widened. The 1-minute timeout is
  hardware-verified via `touch_get_state()` ("screen on" → "screen blanked"),
  as is NVS persistence across a reboot. Wake-on-touch, first-touch-swallow
  and error dismissal still need the owner's finger.
- ~~Touch was mirrored top-to-bottom on the ST7796 glass~~ **CLOSED
  (`f028e2f`)** — Y-invert was the wrong knob (X was fine); capacitive
  orientation knobs are now conditional on `KILNCTL_DISPLAY_PANEL_ST7796`.
- ~~LVGL's wake-edge invalidate reentered its own flush callback~~
  **CLOSED (`51e1ef5`) — killed a live firing on the bench before the fix.**
  This is the root cause behind the `safety_poll` panic/`configASSERT`
  chain; full postmortem in `CLAUDE.md` "Firmware gotchas".
- ~~LCD diagnostics 9 pages, profile-detail layout, builtin-catalogue
  browse-by-family, web display-settings styling, cone/firing-type
  verification~~ **all CLOSED 2026-09-04** (`1cf200f`, `445a78e`, `0470185`,
  `70ef683`, `9d73c8f`) — owner-feedback fixes on the real panel plus a
  source-checked correction of the builtin catalogue's cone metadata (3
  fixed, 15 confirmed). The remaining 10 unrated cones closed 2026-09-05
  (`1501f0c`+`3b0c82e`): `PROFILES_BUILTIN_CONE_UNRATED`, LCD catalogue
  "Unrated" bucket sorted last (owner decision).
  Full detail: `docs/COMPLETED_2026-09.md`. Not yet flashed.
- **`screen_idle` held its own lock across the producer reads, 2026-09-04
  (`7a8594d`).** The policy tick called `dashboard_get_status()` (five
  MAX31856 SPI bursts), `kiln_io_owner_command_read()` (blocks up to 200 ms on
  another task) and four interrupts-disabled heap walks at 20 Hz, all under a
  lock the LVGL task takes on every tick and touch — against the module's own
  documented invariant. Reads moved outside the lock and throttled to 1 Hz,
  policy now reads a cached snapshot; five mutation tests, all red.
- ~~**The LCD back buttons do not work**~~ **CLOSED (`1982ed6`).** Root cause
  was the topbar's z-order-first-match hit test: icons are built left-to-right
  (Back, Home, Prev, Next, Gear) so every icon except the last in a row was
  shadowed by whichever came after it, and Back was *always* shadowed since
  something always follows it. Fixed by capping the touch-area extension at
  `UI_THEME_PADDING_PX/2` per side (`ui_theme_apply_touch_area()`) and
  registering the icon row as a touch group (`ui_topbar.c`).
- ~~Diagnose the HTTP concurrency reset~~ — **stale, corrected 2026-09-04: this
  shipped and was already moved out.** Root cause was
  `CONFIG_LWIP_TCP_ACCEPTMBOX_SIZE` defaulting to 6 — a fixed-size mailbox, not
  a heap failure or the backlog/socket-table limits three prior passes chased.
  Raised 6→16; reset rate 22.5%→0.0% at the same concurrency levels. This
  bullet itself was left behind when the finding moved to
  `docs/COMPLETED_2026-09.md#http-connection-resets-under-concurrency--root-cause-and-fix-2026-09-04`
  on 2026-09-04 — the upkeep rule says a finished item leaves this file, and
  the one-line pointer was missing until now.
- ~~A guard that every `src/**.c` is in its CMakeLists or explicitly
  excluded.~~ **Stale, corrected 2026-09-03: this shipped**, predating this
  roadmap's last review — `tools/check_c_files_in_cmakelists.ps1`, wired
  into `run_repo_checks`/`run_all_checks.ps1`. `tick_timing.c` (the incident
  that motivated it, 2026-08-28: added to the host-test list but not
  `SaftyFW/CMakeLists.txt`, so the host suite compiled it happily while the
  real target link failed) is itself already fixed too.
- ~~**PID Expansion Plan Phase 3b — cross-zone coupling feedforward.**~~
  **Stale, corrected 2026-09-02: this shipped.** The coupling matrix persists
  per-zone (`coupling_coeff[]`, `zones_config_accessors.c`/
  `zones_config_json.c`, config store) and `zone_coupling_solve.c` /
  `profile_executor_feedforward.c` apply it as a full 3x3 directed matrix —
  the asymmetric-pair schema concern this entry raised is already resolved by
  storing a full row per zone rather than one scalar coefficient. A re-solved
  asymmetric matrix was adopted onto the board 2026-09-02 — see
  `PID_EXPANSION_PLAN.md` §2/§3.2 for the coefficients and the caveats.
- ~~**New, scoped but not yet in a plan doc: per-zone enable/disable**~~ —
  **stale, corrected 2026-09-03 (second pass): the feature already existed.**
  `zones_cfg_t.thermo_count` is already exactly this: a contiguous-prefix
  `[0, thermo_count)` zone count, validated by `zones_config_json_validate()`
  and `parse_zone_fields()`. A zone dropped by shrinking the count keeps its
  stored config rather than losing it, so raising the count restores it.
  `profiles_http.c`'s `valid_zone_bits` already stops any profile targeting a
  zone outside the prefix, and guards, autotune and the coupling matrix are all
  already bounded by the same value. `zones_post_handler()` already refuses a
  structural change (409) while a firing is RUNNING/PAUSED **or** while any
  affected zone is still hot or has a relay commanded on — a stronger interlock
  than driving the relay off at toggle time.
  The scoping note above was doubly stale: `ZONES_CFG_VERSION` is at **15**, not
  11 — five further migrations have landed since that prerequisite was written.
  **Shipped 2026-09-03:** `zones_page.html` now offers per-zone checkboxes as
  UI sugar over `thermo_count` (checking zone *i* sets the count to *i+1*,
  unchecking sets it to *i*), so the contiguous-prefix rule holds by
  construction instead of relying on the operator editing a number by hand.
  No config-version bump and no migration were needed, and adding a second
  parallel "enabled" field would have duplicated `thermo_count`'s meaning
- ~~**New: Pico rollback from the OTA page**, plus a link-protocol reply
  frame so a Pico rollback refusal is visible~~ — **stale, corrected
  2026-09-03: this shipped.** `kilnlink_rollback_result.h` is exactly that
  reply frame (`link_task.c`'s `link_task_handle_rollback()` sends it instead
  of the old fire-and-forget path), and `ota_http.c`'s
  `ota_pico_rollback_post_handler()` / `ota_pico_rollback_status_get_handler()`
  drive it from `POST /api/ota/pico/rollback`. Not yet exercised against a
  live mismatch (M8)
- ~~**New: safety processor build identity** (commit + build date) shown on
  the OTA page~~ — **stale, corrected 2026-09-03: this shipped.**
  `saftyfw_build_info.h` is regenerated every build (`CMakeLists.txt`'s
  `saftyfw_build_info` target) and reported over the link; `dashboard_http.c`
  exposes `safety_build_commit`/`safety_build_datetime`/`safety_build_dirty`
  and `ota_page.html` renders them

**Closed 2026-08-27 through 2026-09-04, no longer open** — DRAM/stack
reclamation, PID Cohen-Coon/fuzzy-layer landing, SNTP sync, the first
hardware coupling-matrix/RGA measurements, the withdrawal of coupled/Ki
adaptive-tuning hardware clearance (harvest-layer defect), the shelving of
dynamics-from-ramps, ramp assist landing end to end (default OFF, gated on a
real cone-temperature firing — see the GATED table below), a round of
relay-autotune/measurement-tooling/web-UI hardening, and the fuzzy-PID A/B
campaign's discovery-and-fix-and-second-discovery (`8906686` then `778ad64`,
see [Blocked on you](#blocked-on-you--nothing-in-the-code-can-answer-these)
for the resulting owner decision) plus the 2026-09-04 safety-case/guard-
coverage hardening pass (27 payload-decoder fuzz targets, S3/S4/S10/S9
coverage, `SAFETY_CASE.md`/`SAFETY_MODEL.md` corrections, and
`wire_protocol_fingerprint_check.py`). Full postmortem detail for all of the
above:
[`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#what-is-actually-left-closeout-narratives-moved-2026-09-04).

**What is currently GATED, and on what** (the short answer for planning):

| Item | Gated on | Where |
|---|---|---|
| DRAM/PSRAM allocator-threshold work | A full soak (cold firing through cooldown) plus a Pico OTA relay-path measurement that has never been taken | `DRAM_PSRAM_STATUS.md` §5/§6/§9 |
| Second LCD panel (ST7796/MSP4031) | The physical panel, and its pre-power STOP-block 5V I2C hazard check before the module ever touches J2 | `DISPLAY_ST7796_PLAN.md` §0/§4 |
| Ramp assist default (OFF → ON) | A real firing at cone temperatures — everything measured so far is bench-range (0–80 °C), well below where the cone table's heat-work weighting matters | `PID_EXPANSION_PLAN.md` §7 |

**Two operational facts a future session must not miss (verified by READING
THE BOARD, 2026-09-04 — not inferred from commit messages):**
- **Both zone-config migrations are ALREADY on the live board and have already
  run against its real config.** `GET /api/zones` returns
  `ease_off_window_mult: 2.0` and `approach_rate_cap_c_per_hr: 0.0` on all
  three zones; `GET /api/status` reports `fw_build: Sep 4 2026 14:53:53`.
  This was NOT deliberate: an agent authorised to flash while diagnosing a
  watchdog reset built from a shared working tree carrying another session's
  in-progress schema work, and the v16→v17→v18 chain rode along. It landed
  correctly — v16→v17 carried the prior global `ease_off_window_mult` of 2.0
  verbatim to every zone (the intended default; had it been left at 3.0 by the
  withdrawn A/B, every zone would silently have inherited 3.0), and v17→v18
  added the cap at 0/uncapped. PID gains, plant models and the 80 °C ceilings
  are intact. Note `d800a60`'s own commit message says it stayed unflashed —
  true of *that agent*, and wrong about the board. **Read the board.**
- **The accidental flash briefly broke every preset apply**, because the
  firmware emitted `approach_rate_cap_c_per_hr` while `zones_http_client.py`
  had no POST mapping, so `load_config_preset` refused rather than risk
  silently zeroing it — correct behaviour, and the reason the board still sits
  on the `fuzzy_ab_strength50_20260903` arm preset rather than the baseline.
  The mapping landed in `d800a60` (`z%u_approachratecap` /
  `_PRESET_ZONE_OVERRIDE_FIELDS`), so the tree is whole; a long-running MCP
  server started before that commit will still refuse until restarted. The
  trap for the next flash: never flash a build carrying the GET half without
  the matching client POST half. `7afd2e6`'s flash-provenance guard now
  refuses when the dirty tree touches config-schema files, which is what
  should have stopped this.

**What is done and should not be reopened:** the link itself, the wire
contract and its two independent version numbers, the PC-link acknowledgement
convention, every guard's input plumbing, and the instrumentation that now
reports DRAM, stack and link health. See the decisions table at the foot of
this file before re-litigating any of them.

---

## M0 — Unblock the link · *the only milestone with no alternatives*

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phase 0.

- [x] **Link confirmed working end-to-end (2026-08-23).** The bench
      measurement found the byte actually died on baud mismatch, not idle
      level, framing or opto polarity: the TCMT1109 optocoupler pair then
      fitted could not switch fast enough for 115200. Walking the rate down
      settled on 9600, hardcoded on both sides at the time, and
      `safety_get_status()` returned live telemetry. **That optocoupler pair
      was replaced by a non-inverting digital isolator (U6) on 2026-08-25;
      the 9600 figure was a property of the retired parts, not of either
      firmware, and the baud sweep is now complete — see
      `KILNCTL_SAFETY_BAUD_RATE` in `KilnFW/App/drivers/Kconfig` for the
      committed value.** See `firmware/KilnFW/docs/SAFETY_LINK.md` "Transport"
      and `firmware/SaftyFW/docs/HARDWARE.md` §1.
- [x] Tier 0 pin test settles ESP TX/RX by measurement (`HARDWARE.md` §1) —
      moot as a separate step: the Pico is attached and the link carries live
      telemetry both ways (`safety_get_status()` returns fresh frames,
      `tx_dropped 0`), which settles TX/RX by observation rather than by
      probing pins
- [x] ESP TX/RX GPIO assignment and pull-up fixed in code, docs corrected,
      `UART_PROTO_MSG_BROADCAST` added, stale netlist removed, bench path
      decided (Debug Probe SWD + UART bridge on GP16/GP17) — all 2026-08-16
- [ ] DEBUG header and GP16/GP17 access provided before A1 is soldered down

System-wiring decisions gating any bench trip test (not firmware work) are
recorded in the decisions table below, with reasoning in
[`firmware/SaftyFW/docs/HARDWARE.md`](firmware/SaftyFW/docs/HARDWARE.md) §3/§5.

## M1 — Tooling that makes everything after it cheaper

Owned by [`tools/PcTools/TODO.md`](tools/PcTools/TODO.md). Worth doing early precisely
because it is what turns later hardware questions into a script instead of a
soldering session.

- [~] **Known-good config presets, so a test always starts from the same
      board** (owner request, 2026-08-28): factory-default then load a named
      config as one step, so a run is reproducible instead of depending on
      whatever the last session left behind. Presets live as DATA under
      `tools/`, never compiled into firmware — the bench fixture's 80 °C
      ceilings must never be capable of being left behind in a real kiln build.
      **No longer scoped to KilnFW-side config:** the safety-parameter write
      path is trustworthy now (SET_PARAM/COMMIT_CONFIG confirmed by a live
      read-back), so a preset carries a `"safety"` section written over
      `POST /api/safety/commissioning` by `safety_cfg_http_client.py`, with
      `load_config_preset(safety_host=...)` reaching it. Applied for real to
      the bench 2026-08-28: `tc_source`, `tc_placement_mode`,
      `abs_max_temp_c`, `mains_voltage_v`, `tc_type`, `max_rate_c_per_min`,
      `borrowed_zone_index` all committed and confirmed by read-back.
      **What is left is hardware, not software:** `ct_channel_map[0..2]`
      states which relay each CT is clamped around, and no CT is fitted on
      this bench (M5), so it has no measured truth. Committing a guess would
      clear `calibration_missing` and make the safety processor report itself
      COMMISSIONED on a mapping nobody verified, so the assumed identity map
      sits in a separate `"safety_ct_channel_map_backup"` preset section that
      applies only on an explicit `use_ct_map_backup=True`. Readiness still
      reports "Safety processor commissioned" as not-done, correctly, until
      the CTs are fitted and the zone current-sweep derives the real map
- [~] **Live-bench regression suite built on that preset** (2026-08-28):
      `tools/PcTools/tests/bench_fixture_session.py` +
      `tests/conftest.py` turn "start from `bench_fixture.json`, confirmed
      loaded" into a pytest fixture, and
      `tests/test_live_bench_firing.py` runs a real, bounded (45 °C, ≤80 °C
      ceiling checked four independent ways) firing attempt through the same
      `POST /api/profile_exec/start` the dashboard's Start button uses. Ran
      green against the live bench 2026-08-28, 4/4. **What it proves today:**
      the start endpoint accepts (the gate is downstream of
      `profile_executor.c`), and with the safety processor not commissioned
      no relay ever energizes and `safety_heating_enabled` stays false — a
      regression test for `commissioning_gate.c` working, not for it
      existing. The test branches on the board's own live verdict, so the day
      the CTs are fitted and the sweep commits a measured `ct_channel_map` it
      begins exercising the real heat path with no edit. Skips entirely
      unless `KILNCTRL_BENCH_HOST` is set; no mock stands in for the board
- [x] Live-bench **step and PID-tuning** regression tests
      (`tools/PcTools/tests/test_live_bench_tuning.py`), on the same harness —
      green against the live bench 2026-08-28, 4/4 in 6m10s. A closed-loop
      setpoint step (35 → 45 °C on zone 0) with the PV trace sampled at 2 s,
      and the open-loop step autotune (`autotune_engine.c`'s
      `AUTOTUNE_METHOD_STEP`) driven end to end through
      `/api/autotune/{start,abort}` under a 300 s budget. **What it proves
      today:** the start is accepted, the engine steps, and with heat refused
      PV is flat (−0.03 °C over 61 s, no relay ever closed) and the engine
      aborts itself with *"response too small to fit (trace flat or
      noise-dominated)"* — the correct verdict, said out loud. Also a
      negative test that a second concurrent autotune is refused
      (`begin_run_locked()`). `/api/autotune/accept` is never posted: a
      regression test must not retune the bench.
      **Superseded 2026-08-29 — the flat trace had two causes, and both are
      now fixed.** The first was K4: nothing in the firing path ever asked
      for heat enable (`heat_enable.c`, above). The second was found the
      moment the first was: zone 0's `heater_window_ms` was 2000 ms against
      the 10 s minimum on-time, so `heater_output_duty()` quantized every
      on-time a duty could compute to OFF. Autotune commanded `step_duty
      0.4` for 40 minutes and the relay never closed once — the "response
      too small to fit" verdict was correct about the trace and said nothing
      about why it was flat. With the window at 60 s the same run measures
      relay 1 closing for **24.0 s of a 60.9 s period (duty 0.394 against a
      commanded 0.40)** and PV climbing ~2 °C/min — and the step **completed
      with a model for the first time**: `state=done`, `model_valid=true`
      after 390 s / 39 samples, PV 31.9 → 45.0 °C, fitted `K = 32.95
      °C/duty`, `tau = 166.9 s`, `dead time = 36.9 s`, SIMC proposal
      `kp = 0.0343 / ki = 0.000206 / kd = 0.633`. The gains were **not**
      accepted, on this bullet's own rule. This bullet's "what it proves
      today" describes the old, heat-refused behaviour and is superseded by
      the run above. The relay-feedback method is
      **not runnable on this fixture at all** —
      `AUTOTUNE_RELAY_SETPOINT_HEADROOM_C` (50 °C) under an 80 °C ceiling
      admits only setpoints below the bench's own 35 °C ambient
- [x] **Live-bench verification pass, 2026-08-29** — step tuning, PID tuning,
      zone interaction and a full profile firing, each turned from an
      observation into an assertion, plus the cooldown gate that lets them run
      back to back honestly.
      **The gate:** `BenchSession.wait_for_cooldown()` +
      the `cold_bench` pytest fixture. The target is derived from the board's
      **lowest live cold-junction reading** plus a tolerance, not hardcoded —
      the room here runs around 100 °F and a 25 °C gate would never open. It
      raises on budget expiry rather than proceeding onto residual heat, and
      its policy is split into two pure functions with negative tests
      (`tests/test_cooldown_policy.py`): no cold junction refuses rather than
      defaulting, and **NaN is not cool**. Deliberately *not* an MCP tool — a
      multi-minute blocking wait does not fit `kiln_batch`'s one-round-trip
      contract. Measured in use: 50.4 → 37.5 °C in 646 s.
      **Two harness defects found by asserting instead of observing.** The
      step test's profile carried a 1-minute dwell against a 900 °C/h ramp, so
      the run ended at ~105 s with PV at 38.6 °C and the remaining ~380 s of
      the sweep sampled a *cooling* jig — the trace read 0.38 °C/min, four
      times too slow, for a reason that is not the plant. And
      `sample_response()` indexed channels positionally in a list already
      filtered to valid ones, which would have relabelled every channel after
      a gap — the exact failure that turns a cross-zone measurement into
      fiction. Both fixed; every sample now carries `channels_c` for all
      zones.
      **Results.** Step response from an enforced cold start: PV
      37.51 → 44.37 °C over 487 s, executor `running` throughout, relay 1 and
      K4 closed, **driving-phase rise 1.82 °C/min**, arrival at t=135 s,
      worst post-arrival deviation 4.48 °C. Autotune: `state=done` at 450 s,
      **K = 31.36 °C/duty, τ = 184.7 s, L = 46.1 s**, SIMC
      `kp = 0.03196 / ki = 0.00017 / kd = 0.73614` — an independent second fit
      agreeing with the first (32.95 / 166.9 / 36.9) to within 5 % on K, which
      is the first time this board's plant model has been *reproduced* rather
      than merely measured. Gains still not accepted.
      **Zone interaction — the RGA's first run on real data.**
      `autotune_engine.h` said "no input cell has ever been filled on hardware
      ... this has never run on measured data"; it has now. Two cold-start
      autotune runs (zones 0 and 1, same power cycle — the matrix is RAM-only)
      filled `K = [[30.181, 5.519], [11.822, 23.266]]`, and the board computed
      `Λ = [[+1.1024, −0.1024], [−0.1024, +1.1024]]`, det 636.9. Every row and
      column sums to 1.0000 — Bristol's identity, which is what the test
      asserts, so it is a check on the implementation rather than on a number
      someone typed. **Verdict: the loops interact mildly and independent
      per-zone PID is legitimate on this jig.** Raw thermal coupling, measured
      alongside and **asymmetric**: firing zone 0 raises ch1 by 17.5 % and ch2
      by 8.7 % of its own rise; firing zone 1 raises ch0 by **43.5 %** and ch2
      by 17.7 %. Zone 1 leaks into zone 0 about 2.5× as hard as the reverse,
      which matters for any multi-zone schedule on this enclosure.
      **Superseded 2026-08-30 by a full 3x3 coupling matrix and RGA over all
      three zones, and again 2026-09-02 by a re-solved matrix** — current
      numbers live in `firmware/KilnFW/docs/PID_EXPANSION_PLAN.md` §2, not
      restated here.
      **Full multi-segment profile.** 42 °C dwell 6 → 52 °C dwell 6 → down-ramp
      to 46 °C, run end to end through `POST /api/profile_exec/start`: all
      three segments entered in order, all dwelled, peak 56.30 °C, K4 closed on
      **112/113 samples**, no heat commanded during the down-ramp (PV
      56.16 → 54.18 °C with relays open), ramp-lock never engaged, and every
      relay plus K4 released by the completion path rather than by teardown.
      *(NOT a test of multi-zone ramp-lock coordination — that needs two zones
      in closed-loop control, and zones 1/2 are `control_mode 0` here.)*
- [x] **Guard 1 aborted a healthy firing for settling.** Found by the profile
      run above, on its second dwell: zone 0 holding 50.7 °C against a 52.0 °C
      setpoint at full duty — the steady-state offset any finite-gain PID
      leaves — and `thermal_guard.c` tripped with *"heating but rose only
      −0.2C in 1min"*, aborting the whole firing. Guard 1 treated `error > 0`
      as "still climbing toward setpoint"; those are different questions, and
      once a loop arrives, high duty + positive error + not rising **is** the
      correct state. Every sufficiently long dwell on a sufficiently lossy
      zone would eventually abort. Fixed with an arrival band
      (`progress_band_c`, default 3 °C): outside it guard 1 is unchanged, so a
      dead element on a ramp — errors of tens of degrees — is still caught;
      inside it the zone must not *fall*, which is guard 2's rate test applied
      to a case that previously had no test at all. Five host tests, proven
      load-bearing by forcing the band to 0 (two fail, both pass at 3).
- [x] `pc_tools` moved to `tools/PcTools/`; GPIO probes built for both chips
      (ESP: deny-list incl. GPIO6; Pico: over SWD, GPIO6 read-only). **Pico
      probe bench-tested 2026-08-19, PASS.** ESP probe bench-tested 2026-08-19
      but blocked: the PC↔ESP command UART (COM9) is unresponsive independent
      of the M0 isolated link — see bench state below
- [x] Coordinated two-board test script (`tools/PcTools/scripts/
      coordinated_gpio_test.py`) — reaches each board by a path independent of
      the link under test; run 2026-08-19 confirmed the Pico half works and
      the ESP half hits the same COM9 fault above
- [x] OpenOCD wrapper covering both chips (program/reset/halt/read/write) —
      2026-08-17, `kilnctrl.debug_probe`
- [x] Per-processor console capture + interleaved log file
      (`kilnctrl-console-capture`) — host-verified only; the SAFETY log-relay
      wire path is still unimplemented in firmware
- [x] **HW change: LCD backlight control — CLOSED 2026-09-04 (`be02d34`).**
      Flying wire (GPIO15 → module pin 8) is fitted, owner-confirmed by
      meter; `KILNCTL_BACKLIGHT_PWM_ENABLE` now defaults on and ON duty
      comes from the operator's brightness setting. Not yet confirmed by
      meter/eye that the panel actually dims — that's the next check, not a
      firmware gap. Full buildup history in `docs/COMPLETED_2026-09.md`.
- [~] **Second LCD panel (ST7796/MSP4031), auto-detection, display SPI
      async/DMA.** `firmware/KilnFW/docs/DISPLAY_ST7796_PLAN.md`, sequenced
      Phase 0 (bench facts/hazard measurement) through Phase 7 (UI). Phases 1
      (single-owner cleanup, bounded SPI-owner timeout — also closes
      `TODO.md`'s spi_owner unbounded-wait entry), 2 (panel codec extracted),
      3 (`panel_spi`/`st7796_panel` split, Kconfig-selectable) and 5 (FT6336U
      touch abstraction, unvalidated on hardware) landed 2026-09-01/02.
      Phase 4 (auto-detection) is done ahead of hardware: the detection logic
      is host-tested and wired into `panel_spi_blit.c` (`panel_spi.c`'s
      2026-09-04 split, see the 1500-line-rule item below), and is correctly inert
      today because both descriptors' `id_matches` stay NULL — the ILI9488's
      Sec.4 RDDID bytes were captured 2026-09-03 (`0x00 0x00 0x00`, MISO
      undriven, i.e. permanently unmatchable) and the ST7796's have never
      been read since the module has not touched J2. Phase 6 (SPI DMA) is
      also done ahead of hardware: 9.2/9.5/9.9 landed; 9.3 (PSRAM DMA), 9.4
      (hardware CS), 9.6 (async flush) and 9.7 (zero-copy flush) landed
      compiled-in but default-OFF behind their own Kconfig symbols; 9.1's
      instrumentation landed (`flush_last_us`/`flush_max_us`/`flush_count` on
      `GET /api/status`) though 9.1b's number is not yet recorded live; 9.8
      needed no code. **The owner connected the module to J2 on 2026-09-04 and
      it is now the panel in service** — the "has not yet touched J2" caveat
      above is superseded for everything except the STOP-block 5V I2C hazard
      measurement, which was never taken. Bring-up that day:
      `KILNCTL_DISPLAY_PANEL` was still pinned to ILI9488, so the board ran the
      wrong init table against real ST7796 silicon; and `main.c` only ever
      constructed an `NS2009Class`, so the fully written, host-tested FT6336U
      driver was never instantiated — a textbook consumer-without-producer.
      Both fixed (`16fe9ed`); touch answers at I2C 0x38. The real colour bug
      was neither inversion nor MADCTL: `panel_codec.c`/`panel_spi.c` sent
      LVGL's RGB565 little-endian where MIPI-DCS RAMWR wants MSB-first, and the
      code's own comment had the wire order backwards (`8174db5`). Every
      INVON/BGR experiment run before that fix was confounded and is not
      evidence. With byte order correct, MADCTL was re-measured and set to RGB
      `0x00` (`f493bf8`) and INVON is deliberately absent (vendor
      transcription, guarded by `test_st7796_panel.c`). Touch axis mapping now
      lives on `touch_dev_t`, so panel and touch controller select
      independently (`KILNCTL_TOUCH_FT6336U`). ST7796 RDDID reads `0x00 0x00
      0x00` like the ILI9488 — MISO undriven — so `id_matches` stays NULL
      permanently on both and Phase 4 is inert by design, not by omission.
      **Still open:** a residual blue bias, with every firmware cause
      eliminated by measurement (camera response refuted by a neutral
      off-screen bezel sample; RGB565 field boundaries unit-tested against
      known values; LVGL double-swap ruled out; blit paths compiled out; SPI
      clock swept 20/15/10 MHz with the blue *fraction* flat at 0.62/0.61/0.58
      — `07cad60`, table in `DISPLAY_ST7796_PLAN.md` §4). Treat as a module
      characteristic needing the owner's eye, a colorimeter or a second unit,
      not more firmware. Touch corner accuracy CLOSED 2026-09-04 (`f028e2f`
      — Y was mirrored, `KILNCTL_TOUCH_CAP_INVERT_Y` now defaults on).
      Still open: the 5V I2C hazard measurement. Rendering can now be
      checked without a person at the bench
      via `tools/PcTools/scripts/capture_lcd.ps1` (`5fd9761`) — sample pixels
      numerically, never by eye, and always include an off-screen reference.
      The only hardware change required remains a custom
      harness; the main board itself needs no modification
- [ ] **HW change: relay status LEDs** for K1–K4, S9
- [ ] **HW change: distinct connector types** for the thermocouple daughterboards
      vs. main-board connectors
- [ ] **HW change: I2C broken out on an expansion connector** (owner request,
      2026-08-28). For a future board revision, not the current one. Worth
      deciding alongside it: whether the expansion header carries power and at
      what rail, and whether the bus is the same one the SX1509 and the
      MCP23017 expanders sit on or a separate segment — an expansion connector
      that shares the relay expander's bus lets an add-on wedge relay control

**Bench state (2026-08-20):** ILI9488 LCD, ESP32-S3 JTAG, and Pico SWD all
verified working. Three MAX31856 ICs + thermocouples now fitted on the ESP32-S3
board (channels 0/1/2 reading correctly, `thermo_owner.c` unblocked); the
safety processor (RP2040) still has none fitted.

**Bench state (2026-08-24):** both UARTs work — the two blockers named above
are cleared. The isolated Pi↔ESP link is up and carrying telemetry at 9600
(M0; that figure was specific to the optocoupler pair fitted at the time and
does not describe the digital isolator that replaced it on 2026-08-25 — see
M0's note), and the PC↔ESP command UART on COM9 is responsive. Both firmwares were
flashed over JTAG/SWD today and verified running.

**Later the same day the safety processor's MAX31856 and thermocouple were
fitted** (this paragraph's earlier revision said "still not populated", which
was true when written and stopped being true a few hours later — the two
statements are hours apart, not a contradiction). Verified live:
`safety thermocouple valid | 30.20 C (CJ 28.08 C)`. It required a Pico reset,
because SPI init runs once at boot. The E-stop net measures low (a contact IS
fitted, contrary to `HARDWARE.md` §5's older note) and S7 is now genuinely
evaluated rather than masked by S5 — and correctly stays quiet, which is the
first real test of the `discrete_task.c` polarity fix (`642dd54`): with the
old inverted read this healthy board would now be latched on S7.

## M2 — `CommonFW`, before either firmware depends on it · *done, 2026-08-19*

Owned by [`firmware/CommonFW/README.md`](firmware/CommonFW/README.md). `kilnlink` builds
under both pico-sdk and ESP-IDF, both firmwares consume the same codecs for
every `LINK_PROTOCOL.md` sec 4/6 command, `pc_tools` cross-checks the same
byte vectors, and a CI grep (`tools/check_no_duplicate_crc.ps1`) keeps a
second CRC/framing implementation from reappearing outside `CommonFW`. Detail
in `firmware/CommonFW/README.md` and `firmware/CommonFW/docs/LINK_PROTOCOL.md`.

## M3 — Safety processor to first trustworthy reading

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phases 2–4. Independent of the
link, so it can run in parallel with M1 and M2 once M0 is out of the way.

- [x] FreeRTOS SMP skeleton, GPIO6 driven low first, watchdog with latched trip
      reason — all done 2026-08-16, build-verified
- [x] **The SAFETY processor's MAX31856 and thermocouple are now fitted**
      (2026-08-24, by the user). Verified on hardware the same day:
      `link up; safety thermocouple valid | 30.20 C (CJ 28.08 C)`, status
      flags `0x21` = `LINK_UP | TEMP_VALID`. This unblocked the real-reading
      work below it and immediately changed guard reachability — S5 (sensor
      invalid) stopped latching, which had been masking every later guard
      because `safety_guards_tick()` early-returns while any trip is latched.
      **Bring-up trap worth keeping:** the IC must be present BEFORE the Pico
      boots, since its SPI init runs once at startup. Fitted under power it
      reads `safety TC invalid` with nothing pointing at the real cause; a
      `debug_reset` over SWD is the fix.
      **And it exposed a decision that needs you** — see
      `firmware/SaftyFW/TODO.md`, "An uncommissioned safety processor grants
      heating enable": with the sensor real and the stale S5 latch cleared,
      the board granted heating enable while still reporting
      `commissioned: false`, i.e. with S1's absolute temperature ceiling
      disabled for want of `abs_max_temp_c`
- [ ] ~~The SAFETY processor's MAX31856 is not populated~~ — superseded by the
      line above. Kept for one revision so anyone mid-task on the old wording
      sees why it changed.
      **Read the word "safety" carefully** — this item is about the RP2040's
      own thermocouple, not the main board's. `KilnFW`'s three channels ARE
      fitted and working: verified 2026-08-24 with all three reading ~35 °C,
      `SR 0x00`, and open-circuit detection confirmed genuinely enabled
      (`CR0 = 0x90`, so `OCFAULT[1:0] = 01`) — which is what makes "no fault"
      mean "a thermocouple is attached" rather than "detection is switched
      off". An absent TC on those channels would fault. The two sets of
      thermocouples are easy to conflate from this line alone, and doing so
      leads to "correcting" a true statement
- [~] MAX31856 driver + config plumbing (tc_type via flash-backed
      `config_store`, commissioned over `SAFETY_CMD_SET_CONFIG`) built and
      wired end-to-end in code (2026-08-19). ~~The part itself is not
      physically populated~~ — **fitted 2026-08-24 and reading correctly.**
      **Still open**: there is no LCD/web commissioning surface yet, and the
      four no-default section-1 fields remain unset, which is what keeps
      `commissioned: false` and leaves S1's ceiling disabled
- [x] 13 of 13 guards (`SAFETY_MODEL.md` §4) implemented as pure functions and
      host-tested against synthetic inputs. **S8 (rate-of-rise), the last
      holdout, gained its pure-module implementation and integration on
      2026-09-03** (`safety_guards.c`'s S8 block, `safety_core_load_guard_cfg()`
      wiring gated on `CONFIG_STORE_SET_MAX_RATE_C_PER_MIN`, `test_s8()` +
      `test_safety_core_s8_wiring.c`, `SaftyFW/docs/GUARD_TEST_MATRIX.md`).
      It ships deliberately off — `max_rate_c_per_min` defaults to 0.0f,
      the same "no default by design" shape S1 uses for `abs_max_temp_c` —
      so **commissioning `max_rate_c_per_min` is now what enables it**,
      superseding the earlier finding here that the field would enable
      nothing. **Input wiring now complete (2026-08-24):**
      `safety_core_build_input()` populates every field the guards read —
      `context_valid`, `any_current_present`, `relay_commanded_recently`/
      `_continuously`, `zone_count`, the setpoint/measured reductions,
      `sample_counter_advancing`, both discretes and `reboot_grace_active`.
      The older "only S5/S6b/S7/S12 are reachable, the other 8 have no
      producer" finding is superseded: what still holds a guard dormant is a
      missing **commissioning value** (S1's `abs_max_temp_c` defaults to 0 =
      never trip; S13 needs `tc_source`/`borrowed_zone_index`), which is a
      different kind of gap from a missing producer. Two real producer bugs
      were found and fixed on 2026-08-24 — the E-stop polarity was inverted
      (S7 could not fire) and `current_sense_set_cal()` was never called, so
      `any_current_present` was permanently false (S3/S9/S11 and S6b's
      current-gated trip could not fire). ~~The reachability count in
      `GUARD_TEST_MATRIX.md` predates both fixes; re-establish it rather than
      trusting the old number.~~ **Re-established 2026-09-03**
      (`GUARD_TEST_MATRIX.md` §6c): old count was 6 of 13 structurally
      reachable (computed before either fix and before S8 existed); S8
      itself gained its implementation and integration on 2026-09-03 and is
      now the same class as S1/S13 — implemented and integrated but
      deliberately configured off pending commissioning, not unreachable.
      See `SaftyFW/docs/GUARD_TEST_MATRIX.md` for the current reachable-count
      recomputation (S14 is new since 2026-08-28 and tracked separately). S3,
      S6a, S7, S9 and S11 moved from blocked to reachable — S6a's own
      `main_fault_asserted` wiring is a third fix in the same window, beyond
      the two named above. S1, S13 and S14 remain deliberately blocked by
      commissioning gaps, not producer bugs — §6c distinguishes that from an
      unreachable guard explicitly. One stale claim inside the matrix itself
      was also caught and corrected in the same pass: S13's row said
      `sample_counter_advancing` had no producer at all; it does now
      (`safety_core.c:957-960`), and `tc_source` alone is what still blocks
      S13. Host suite re-run in the same pass: 1983/1983 checks pass (the
      matrix's old "452/452" checklist line was itself stale, from before
      the suite grew ~4.4x).
- [x] CI grep: `safety_core.c` never includes the link header — 2026-08-16,
      `firmware/SaftyFW/tools/check_isolation.ps1`

## M4 — Relay authority

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phase 5. First milestone that can
physically stop a kiln, and the first that can nuisance-trip one.

- [x] Relay owner task is the sole writer of GPIO6 — confirmed by whole-tree grep
- [x] Trip latches and requires an explicit `CLEAR_TRIP` to clear, refused if
      the tripping condition re-fires on a one-tick retest
      (`safety_guards_try_clear()`); wired end to end PC→ESP→Pico and
      ESP-side send (`safety_link_send_clear_trip()`) plus web/UART surfaces —
      all built and host-tested (2026-08-19). **Known scope limit** (documented,
      not a bug): a graduated/windowed guard's elapsed-time accumulator resets
      on clear, so it re-trips on its own timescale rather than instantly.
      **Hardware-verified 2026-08-23/24**: the physical link has carried real
      CLEAR_TRIP frames. Refused correctly against a latched S5 with the
      safety thermocouple genuinely absent, accepted once the tripping
      condition cleared, and the board never rebooted in either case — which
      it originally did, via a `log_task` stack overflow on the refusal path
- [x] **K4 now has a real energize path.** The "zero callers" finding of
      2026-08-20 is stale: `safety_core_request_enable()` calls
      `relay_owner_command_energize()` (`safety_core.c`), reached from
      `link_task`'s enable handler, and it refuses ON when the safety
      thermocouple is declared absent or a trip is latched.
      **Enable is now GRANTED on real hardware (2026-08-24)** — the
      thermocouple is fitted, the stale S5 latch cleared, and the board
      reports `heating enable granted`. ~~The bench cannot demonstrate a
      genuine heat-enable until the safety thermocouple is populated~~ — that
      blocker is gone.
      **Observed closing, 2026-08-29.** Not with a meter and not with an LED
      — with the element. Until this date nothing in the normal firing path
      ever sent `SAFETY_CMD_REQUEST_ENABLE` at all: `profile_executor.c` and
      `autotune_engine.c` had *zero* calls to `safety_link_request_enable()`,
      so every firing and every autotune this firmware ever ran closed K1 and
      left K4 open. That is what "40 minutes of commanded heat moved this jig
      0.67 C" was actually measuring. With `heat_enable.c` wired in, the same
      jig on the same profile went 32.1 → 47.5 C, `safety_relay_energized`
      true on every poll of the run and false again the moment it stopped.
      A contact that passes enough current to move a thermocouple 15 C is
      closed. What is still unobserved is the *mechanical* state under a
      fault — a welded contact reading closed while the request is released
      — which is M1's LEDs, not this.
      And note what granting it exposed —
      `SaftyFW/TODO.md`'s "An uncommissioned safety processor grants heating
      enable": permission is given while S1's absolute ceiling is disabled for
      want of `abs_max_temp_c`
- [x] Rule engine drives relays through the existing owner arbitration —
      `rules_task.c` claims `RELAY_OWNER_RULE` and never writes the SX1509
      directly, so precedence is PROFILE/AUTOTUNE > RULE > MANUAL. Fails safe
      on safety fault, down link, OTA in progress, and on its own stale-tick
      watchdog. **Rules may never command a zone-assigned (PID/thermocouple)
      relay** — owner's scope rule, enforced in the evaluator, the task and
      the POST handler; heater relays stay readable as rule conditions.
      2026-08-22, verified on hardware before the bench was disassembled.
      **The rules engine was deleted on 2026-08-28 (M11)** — this bullet is
      kept because the arbitration it describes is still live and is what
      firing-profile relay/IO segments now claim through. The owner's scope
      rule survived the deletion intact: a segment may not command a
      zone-assigned relay either
- [x] Dashboard/readiness no longer report the safety link as up merely
      because the driver object exists — both call sites now consult the real
      staleness-gated `link_up`. This was a live false positive: the board
      reported "safety=up" with the UART unplugged. 2026-08-22
- [ ] S9 trip-ineffective escalation proven with a deliberately welded contactor
      (hardware-gated). **Not a SimFW task, checked 2026-09-03**: SimFW is gone
      (removed `8553244`); even the tool it was checked against for this exact
      question required "physically driv[ing] current into the CT," per that
      removal commit's own audit. S9's latch needs the CT's real analog
      current signal (see the M4 table row above for the code path), which
      only a hardware jig can supply, plus a CT fitted and commissioned
- [ ] Every guard exercised per
      [`GUARD_TEST_MATRIX.md`](firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md) —
      §2's host-provocation table is fully audited (452+/452+ checks pass);
      §3's hardware-trip rows remain open (no bench hardware attached)

## M5 — The link carrying real traffic

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phases 6–8, contract in
[`firmware/CommonFW/docs/LINK_PROTOCOL.md`](firmware/CommonFW/docs/LINK_PROTOCOL.md).

- [~] Current sensing: load-active detection and a power estimate (not an
      over/under-current trip) built and wired onto `SAFETY_CMD_POWER`.
      **2026-08-24: the calibration was never actually loaded** —
      `current_sense_set_cal()` had no caller anywhere, so `amps[]` was
      permanently 0 and `any_current_present` permanently false, silently
      disabling S3/S9/S11 and S6b's current-gated trip while S4 warned
      forever. Now loaded at boot and on every `COMMIT_CONFIG`, and presence
      detection is decoupled from `k_ct_v_per_a` (deciding whether current
      flows never needed a volts-per-amp scale). `CURRENT_SENSE.md` §5 and
      `CONFIG_REFERENCE.md` both claimed a wrong `k_ct_v_per_a` could not
      affect guard behaviour; that was false and is corrected. `context_valid`
      is also computed now — see M3's guard bullet. **Still open**: the
      per-channel CT-to-jack commissioning check on real hardware, and a bench
      measurement of the ADC noise floor to confirm the uncommissioned
      presence fallback margin (25 counts) sits above it
- [x] ESP → Pico context frames (`SAFETY_CMD_PUSH_CONTEXT`, incl.
      `relay_recent_mask`) built from live board state — 2026-08-18.
      **Hardware-verified 2026-08-24**: the Pico is on the bench, the link is
      up, and `context_valid` is computed from frames that actually arrive.
      The "no Pico on this bench" caveat this bullet used to carry is retired
- [~] Pico → ESP telemetry (status, diagnostics, firmware version, trip events,
      power) — all five frame types have working codecs, send paths, and
      `KilnFW`-side decode/dispatch, plus PC-facing `GET_DIAG`/`GET_TRIP_EVENT`
      subcommands (2026-08-18–19). **Now hardware-verified (2026-08-23/24)**:
      M0 is cleared, status/diag/power frames cross the real wire, and the
      diag/power applied counters were observed climbing at the expected
      0.5/s over a 90 s soak. Getting there needed a UART TX self-start fix —
      an edge-triggered TX interrupt that never re-armed stranded a frame in
      the ring silently, with no counter tripped
- [x] Pico never blocks on the link — all five no-wait rules audited clean
      2026-08-18 (no ACK/retry, bounded non-blocking TX, `safety_core.c` never
      calls into the link, correct task priority/core affinity)
- [x] Mutual version handshake: `ANNOUNCE_VERSION` both ways, `min_compatible`
      checked in both directions, both firmwares on the shared codec
- [x] TX ring reserves capacity for telemetry; log frames dropped above a 50%
      watermark and the drops counted
- [x] Borrowed-thermocouple staleness split correctly across S11/S13/S6 —
      audit-confirmed 2026-08-18, no code fix needed
- [x] `SAFETY_CMD_COMMIT_CONFIG_REJECTED` (0x20) — a refused commissioning
      commit used to be indistinguishable from an accepted one. The Pico now
      names the offending `param_id` and a reason code (RANGE /
      CONTRADICTION), and `safety_cfg_http.c` surfaces it in the page's error
      text instead of "sent, awaiting confirmation". 2026-08-22, host-tested
      both ends; not hardware-verified (M0)
- [x] `SET_LOG_LEVEL` (0x1B) reachable from the ESP — the codec and the Pico
      consumer existed with no caller, so the feature was dead. Now
      `POST /api/safety/log_level`, deliberately API-only (a bench knob, not
      an operator control). 2026-08-22
- [x] LCD stopped showing a raw `reason 0x%02X` where the web showed decoded
      words — the two same-language copies are one shared table
      (`safety_trip_words.h`). 2026-08-22

## M6 — Throw the liveness switch

The point at which the two processors become one system. Deliberately separate,
because it changes what a bare main board will do.

- [x] Link staleness → fault at a fixed 1.5 s ceiling, 30 s silence aborts a
      running firing, boot-time `FW_VERSION` request retried until answered,
      and a documented bench escape hatch (`safety_link_fault_on_link_loss`) —
      all built 2026-08-18/19. Code-verified and flashed. **The Pico is now on
      the bench (2026-08-24) so the "no Pico" caveat is gone, but the TIMING
      half is still unverified**: nobody has held the link down with a
      stopwatch to confirm the 1.5 s ceiling and the 30 s firing abort fire
      when they should. That is a bench procedure, not a code gap
- [~] GUI (web + LCD) surfaces safety temperature, enclosure temperature, and
      power — built and wired to the same status cache the wire frames land
      in. **The reason for the blanks changed on 2026-08-24 and the
      distinction matters**: frames now arrive every 500 ms, so this is no
      longer "no Pico has ever sent them". Safety temperature reads null
      because the safety MAX31856 is genuinely not populated (M3), and the
      three current channels read 0.00 A because no CT is fitted. Both are
      honest reporting of absent hardware, not a code gap and no longer an
      M0 consequence
- [ ] **AP-fallback fix unverified end to end** — needs a router with both
      correct and deliberately-wrong static config; see `KilnFW/TODO.md` Wi-Fi

**2026-08-20: a large batch of UI, Wi-Fi, and boot-stability bugs were found
and fixed during a full hardware test pass** — profile/readiness reporting,
the LCD no-scroll rewrite, captive-portal DNS hijack, gzip content
negotiation, the Digital Fire built-in schedules, a thermocouple-fault page,
web DHCP/static-IP toggle, and others. Full list, and the ongoing ledger, is
in `firmware/KilnFW/TODO.md` and `firmware/KilnFW/docs/UI_PLAN.md`; two hazards worth reuse
were promoted to the decisions table below (internal-SRAM exhaustion at task
creation, and the UART owner's per-transfer heap churn).

## M7 — Repo reorganisation · CLOSED 2026-09-05

Owned by [`docs/REPO_LAYOUT.md`](docs/REPO_LAYOUT.md). All items done,
including `hardware/UnitTestFixture` (owner decision: KEEP, 2026-09-05). Full
detail: [`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m7--repo-reorganisation-closed-2026-09-05).

## M8 — Field updates

Owned by [`firmware/CommonFW/docs/UPDATE_PROTOCOL.md`](firmware/CommonFW/docs/UPDATE_PROTOCOL.md)
and [`firmware/SaftyFW/docs/BOOTLOADER.md`](firmware/SaftyFW/docs/BOOTLOADER.md);
tracked as `KilnFW/TODO.md` section 9 and `SaftyFW/TODO.md` phase 10.

Last, and genuinely last: a bootloader is new code in the component with nothing
behind it, and it is only defensible because SWD sits underneath as the recovery
path. Two facts set the shape of this milestone:

- **The RP2040 mask ROM has no UART bootloader.** Updating the Pico over the
  isolated link means writing one. It is written once over SWD and never
  updates itself.
- **You cannot OTA your way into being OTA-capable.** The ESP needs a new
  partition table with two app slots, and a partition table can only be written
  over a cable.

- [x] **Measure the isolated link's real error rate — done, and 115200 did
      not work at all under the old optocoupler pair.** The optocoupler pair
      capped the link at 9600 (see M0); that pair was replaced by a digital
      isolator (U6) on 2026-08-25 and the ceiling no longer applies, so the
      committed baud is being re-measured (`KILNCTL_SAFETY_BAUD_RATE` in
      `KilnFW/App/drivers/Kconfig`). The update transfer's error rate at
      whatever the current baud is, over a sustained multi-megabyte run, is
      still unmeasured. Retry cost is still 200 ms × up to 10
- [x] Real flash size established (N16R8, 16 MB/8 MB PSRAM) and declared in
      `sdkconfig` — 2026-08-17. **Both follow-ups are now done**: the
      two-app-slot table exists (`otadata`/`ota_0`/`ota_1`/`factory`/
      `pico_img`/`coredump`, and `factory` moved to 0x810000 on 2026-08-21),
      and the bootloader + partition table were flashed and verified on the
      physical board over JTAG on 2026-08-24. **Consequence worth knowing:**
      the sanctioned JTAG path writes the app into `factory`, so a
      bench-flashed build always boots `factory` and the rollback/boot-confirm
      machinery below never executes — it can only be exercised by a real OTA
      into `ota_0`/`ota_1`
- [x] `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, app confirms itself only after
      NVS + safety link + web server are up — host-build-verified, not yet
      hardware-flashed
- [~] Pico flash layout/metadata frozen (reserved `signature[64]`, `sig_required`,
      pubkey region) and implemented in `metadata.c`/`flash_layout.h` — design
      and code done 2026-08-19; not flashed or hardware-verified (no RP2040
      attached)
- [~] Pico bootloader: GPIO6 low first, per-boot CRC, `boot_attempts` fallback,
      and a real recovery-mode UART1 receiver (not beacon-only) — all built
      and host-build-verified 2026-08-19; not flashed or exercised over a live
      UART1 link
- [x] Mutual protocol-version check landing with M5 — both firmwares on the
      shared `kilnlink_announce` codec, GUI shows both sides' versions and
      names which is older. Not verified against real mismatched hardware
- [~] Compatibility floor (frame ids `0x00`–`0x0F` reserved, never gated on
      `peer_version_compatible`) — frozen and implemented; not verified
      end-to-end against a live mismatch
- [x] Image header validated before the first erase (2026-08-17)
- [x] Challenge–response on the AP password, never crosses the wire, 3-failure
      lockout (2026-08-17)
- [x] Both update paths refused unless idle and cool, with the specific
      blocker named (2026-08-17)
- [ ] Link-loss heating block **not** bypassed during a Pico update — now
      pinned in CI on both sides (2026-09-04), still OPEN as a
      hardware-exercise item (a test suite is not a substitute for running a
      real update on a real board):
      - KilnFW side: `firmware/KilnFW/App/test/test_safety_link_compile.c`
        now links the REAL `relay_authority_on_blocked()` (App/drivers/
        relay_authority.c — previously stubbed everywhere else in the host
        suite) against a real `SafetyLinkClass`, and proves
        `safety_link_set_update_in_progress()` (the call `ota_pico_relay.c`'s
        relay task makes around a Pico relay) does not relax an asserted
        `SAFETY_FAULT_SRC_SAFETY_LINK` fault, and that a link going stale
        mid-update still denies heat. Negative-tested: temporarily made
        `safety_link_set_update_in_progress(true)` clear `fault_sources`,
        confirmed 4 checks fail by name, reverted.
      - SaftyFW side: `firmware/SaftyFW/test/test_update_task_relay_wiring.c`
        source-scans the real, non-host-compilable `update_task.c` (same
        precedent as `test_safety_core_s8_wiring.c`/
        `test_safety_core_polarity_wiring.c`) and fails closed if it cannot
        locate either file or `relay_owner_command_energize()`'s real
        signature; pins that `update_task.c` calls no relay_owner_* mutator
        and touches no relay GPIO. Negative-tested: added a call to
        `relay_owner_command_energize()` into `update_task.c`, confirmed the
        scan fails by name, reverted.
      - Still needed: an actual Pico OTA exercised on real hardware with the
        link deliberately dropped mid-update, confirming no relay ever
        energizes and the fault stays latched after the update ends.
        **Blocked, 2026-09-18, by the two serially-blocking defects in the
        next items — not by prerequisite P1, which is closed.** The test design
        is settled
        (drop the link by halting the ESP mid-relay and hold past the 120 s
        `link_dead_hard_s` backstop — `firmware/SaftyFW/src/config_store.c:900`
        — so `SAFETY_TRIP_LINK_DEAD` latches, polling the relays throughout)
        and the slot image is built. The only missing thing is that the
        update cannot reach the data phase at all.
- [x] **An ESP-driven Pico OTA cannot reach the data phase — observed on real
      hardware 2026-09-18, FIXED the same day by `e59b0328`; only the hardware
      exercise remains.** The fix erases the Pico's destination slot in 4K
      sectors instead of 64K blocks (`UPDATE_TASK_ERASE_CHUNK_SIZE`, pinned to
      `HAL_FLASH_ERASE_SIZE`, the smallest unit the HAL can express) and feeds
      the hardware watchdog between sectors through one shared owning
      function, `watchdog_task_feed_if_all_within_deadline()`, which
      `watchdog_task_fn()` now calls too rather than carrying its own copy of
      the gate-then-feed sequence. The feed policy is unchanged — it happens
      only if every registered task is within its own deadline, so a wedged
      safety processor still starves the watchdog and still reboots, including
      mid-update. **Still needed: a real ESP-driven Pico OTA on hardware,
      reaching and completing the data phase.** The original observation, kept
      because it is the evidence the fix is graded against: the ESP staged the
      image and started the
      relay; the RP2040 then hardware-watchdog-reset partway through erasing
      the destination slot, so it never confirmed `RECEIVING`, and the ESP
      failed the relay at its 15000 ms erase timeout with `Pico did not
      confirm RECEIVING within 15000 ms of erase` (`RELAY_ERASE_TIMEOUT_MS`,
      `firmware/KilnFW/App/drivers/net/ota_pico_relay.c:111`; the message is
      formatted at `:448`). Evidence: `safety_get_diag()` immediately
      afterwards reported `boot reason: watchdog | state grace | uptime
      24007 ms`, against an uptime of 2552017 ms and state `armed` moments
      before. **The failure was safe** — relays R1-R4 read 0 throughout, no
      trip latched, the configuration survived (`config_version` 162,
      `config_crc` 53177, both unchanged) and the board returned to `armed`
      on its own — but the ESP-driven Pico update path is non-functional on
      this hardware today. Observed facts only, no root cause asserted here;
      the diagnosis lives in
      `docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md`
- [x] **A second, independent defect blocked the same path: the ESP staged the
      Pico image with the wrong CRC-32 parameterization — 2026-09-18, FIXED
      the same day by `fabd270f`; only the hardware exercise remains.** The
      arithmetic moved into its own module,
      `App/drivers/http/ota_image_crc.c`, specifically so it could be pinned
      by a known-answer test (`test_ota_image_crc.c`) — HTTP handlers in this
      project are target-build-only and do not link into the host suite, so a
      test written against `ota_pico_do_stage()` would never have run. Both
      ends of the comparison are now pinned to one value: SaftyFW's half has
      been held to the standard CRC-32 check value by
      `test_bootloader_metadata.c` the whole time. Reproduced numerically
      before the fix over a real 114796-byte image — the firmware's
      parameterization yielded `0xBA38A716`, exactly what the board reported
      when staging, against `0x83C472EF` for true CRC-32/zlib; the file was
      never wrong. **Still needed: an ESP-driven Pico update on hardware
      passing the verify step it could never previously pass.** The original
      diagnosis, kept as the evidence the fix is graded against:
      `ota_http_pico.c` seeded the accumulator with `0xFFFFFFFF` and applied a
      final XOR on top of `esp_rom_crc32_le()`, which already performs both
      inversions internally, so the ESP sent a different CRC-32 variant of
      the same, correct bytes and the Pico's verify step could never agree. It
      failed closed — a corrupt image could not reach the active slot — but every
      ESP-driven Pico update ended in `ERR_CRC_MISMATCH` regardless of link
      quality. **The two defects were serial, not alternative: fixing the
      erase/watchdog failure alone only advanced the failure to the CRC check
      rather than producing a working update — which is why both fixes are
      prerequisites of the one remaining hardware exercise.** Diagnosis:
      `docs/audits/pico_ota_staged_crc_mismatch_2026-09-18.md`. This also
      identifies the cause behind `firmware/SaftyFW/TODO.md` Phase 10's open
      "reconcile host-side vs ESP-side UPDATE_BEGIN image CRC" item, which
      recorded the symptom in September
- [x] Four MCP tools for OTA (challenge, ESP update, Pico update, status),
      plus explicit ESP and Pico rollback and a web `/ota` page — built and
      unit-tested against mocked HTTP (2026-08-18–19); not yet exercised
      against a physical board
- [x] `SAFETY_CMD_ANNOUNCE_REBOOT` sent before a routine ESP reboot so a
      firmware update doesn't trip S6(b) — 20 s grace window, host-tested;
      not hardware-verified (no board attached to confirm a real reboot
      suppresses the trip)

## M11 — The UI the owner actually asked for · *opened and CLOSED 2026-08-28*

Owned by [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md) and
[`firmware/KilnFW/docs/UI_PLAN.md`](firmware/KilnFW/docs/UI_PLAN.md). **CLOSED
— all items landed and are hardware/migration-verified.** A batch of direct
owner requests that changed persisted data structures and deleted the rules
engine subsystem: relay control consolidated into the diagnostics Danger
Zone; board-health/thermocouple-fault pages folded into `/diagnostics`;
safety pages grouped into one nav group; shared per-zone timing profiles
(`ZONES_CFG_VERSION` 8→9); relay/IO segments added to firing profiles
(`PROFILE_VERSION` 2→3); the rules engine deleted in favor of segments; LCD
pages consolidated the same way, plus a planned-profile preview. Full
detail, including the durable `zones_cfg_t`/`profile_t` migration-hazard
note (persisted structs that embed arrays by value displace every element
after an insertion — read this before touching either struct again):
[`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m11--ui-consolidation-full-detail-moved-2026-09-04).

## M12a — The commissioning surface lied · *opened and closed 2026-08-28*

**CLOSED.** An opus audit found the safety commissioning page reported
`{"ok":true}` on writes the Pico had actually rejected (four independent
defects: an ACK that couldn't fail, a rejection race that got dropped, a
stale NVS cache presented as current, and an unconditional `"set": true`).
All fixed and hardware-verified the same day (`ddbd024`, `3149393`);
`abs_max_temp_c = 80` is committed and confirmed on the bench, and fixing it
exposed a second defect (config page 1 always timing out — the ESP had never
once fetched a config page in the project's history), also fixed the same
day. The write-window constraint this uncovered (config writes refused
outside the ~60s boot GRACE period) shaped M12's design below. Full
postmortem: [`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m12a--the-commissioning-surface-lied-full-postmortem--opened-and-closed-2026-08-28).

## M12 — Commissioning the operator can actually do · *opened and CLOSED 2026-08-28*

**CLOSED.** All items landed and are hardware/code-verified, including the
2026-08-28 same-conversation additions and the 2026-09-15 S14/S15 correction
(`b5cb83a4`, `c0729e1e`). Every row in this milestone was already ticked by
2026-09-15 — found stale-presented-as-live during the 2026-09-16
roadmap-upkeep sweep and collapsed here. Full detail:
[`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m12--commissioning-the-operator-can-actually-do-full-detail-moved-2026-09-16).

## M13 — Every fault says what was detected, and what to do · *opened 2026-08-28*

A standing requirement from the repo owner, not a one-off fix: **"all faults
reported to the user should come with instructions on how to fix them or more
importantly what was detected wrong."** Note which half he called more
important — the cause, not the remedy. Treat this as a rule that applies to
every fault surface added from here on, not a milestone that closes.

It came from S6a, which is the worst case and therefore the right example. S6a
is `mainFault` (GPIO10) LOW, debounced 200 ms. The safety processor sees ONE
BIT and cannot know why — that independence is deliberate and is not to be
traded away. But the ESP does know: `fault_sources` is a bitmask of MANUAL /
PC_LINK / THERMO / SAFETY_LINK / APP / THERMAL_SANITY (`safety_link.h:139-142`),
and `dashboard_http.c:261` already reads it. So the cause was measured, held,
and simply never shown next to the trip.

**Landed 2026-08-28, all of it — S6a's fault-source decode (shared
`safety_trip_words.h` table across web/diagnostics/LCD, captured AT TRIP
TIME not read live), real numbers on every `safety_trip_t` cause line,
KilnFW-side fault coverage, and an explicit non-clearable notice for S9.**
Full postmortem: [`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m13m14--fault-reporting-and-verification-findings-full-detail-moved-2026-09-04).

**Second sweep, 2026-09-16** (following `6985c89b`'s zones-config
migration-persist fix, which explicitly did not review the rest): widened the
hunt for give-up paths whose only output is `ESP_LOGE` with no web/LCD/API
surface. Found and fixed two gaps, both in `kiln_cfg_swap.c`'s
`kiln_cfg_swap_boot_recover()` (the two-processor config-swap boot-recovery
state machine) — its seven failing branches (unreadable/CRC-bad pending
record, no safety link for PICO_OPEN/PICO_DONE, rollback-failed,
ESP_DONE re-read-failed with fallback-rollback-failed, ESP_DONE
esp/pico-mismatch, unrecognised marker) only ever logged and left heaters
alarmed with no operator-visible cause or remedy. Added
`kiln_cfg_swap_boot_fault_t`/`kiln_cfg_swap_get_boot_fault()` (same
first-one-latched-wins convention as `zones_cfg_load_fault_t`), wired through
`dashboard_http.c` -> `dashboard_status_http.c`'s `/api/status` JSON ->
`main_page.html`'s new banner -> `ui_page_home_refresh.c`'s LCD trip strip.
Also found, while wiring this, that `6985c89b`'s own
`zones_config_migration_persist_fault` fields were populated in
`dashboard_http.c` and consumed by `main_page.html`'s JS but were **never
serialized** into the `/api/status` JSON in `dashboard_status_http.c` — that
banner was dead code since it landed; fixed in the same pass. Host tests:
two new cases in `test_kiln_cfg_swap.c` (fault latches with an actionable
reason; first-one-wins across two different failures), both negative-tested
by hand against the production latch guard. Other candidate surfaces
reviewed and judged already adequate or out of scope this pass:
`estop_verification_clear()`/`safety_cfg_write.c` (already returns an
actionable reason to the HTTP caller), `ota_pico_relay.c`'s retransmission
give-up (already surfaced via OTA status), `safety_cfg_store_flush_if_dirty()`
(self-heals on next refetch), and `thermo_task.c`'s SWD-only
`s_reconfig_gave_up` counter, ~~not conclusively checked against the generic
TC-invalid path this pass — left open for a future sweep~~. **Closed the same
day, `9d600ac8`: `thermo_task_reconfig_gave_up()` is now wired into
`kilnlink_diag_t` bit4 (`link_task.c`) and surfaced on the ESP side as
`safety_tc_reconfig_gave_up` through `dashboard_http.c` ->
`dashboard_status_http.c`'s `/api/status` JSON -> `main_page.html`'s banner.**
This remains a
standing rule, not a milestone that closes.

**The clearing semantics, recorded here because they were only discoverable by
reading `safety_guards.c`:** an S6a trip LATCHES. It does not clear on its own,
a new firing does not clear it, and an ESP reboot does not. Only an explicit
CLEAR_TRIP does — and that clear is REFUSED while the cause persists, because
`safety_guards_try_clear()` (`safety_guards.c:209`) clears the state and
immediately re-runs the guard, and an unwindowed guard with the line still LOW
re-trips on that same tick. So the operator sequence is: identify the source,
remove it, then clear. None of that is currently told to the operator, and the
owner had to ask.

## M14 — Verification you can trust · *opened and CLOSED 2026-08-28*

Not a feature milestone. It exists because on 2026-08-28 the sentence "tests
pass, build clean, flashed and verified" could be true and worthless, and
almost every defect found that day was something reporting success it had
not earned. **CLOSED, all findings landed** — a `build_kilnfw` wrapper that
reported OK on a failed build, a `flash_firmware` that didn't check the
binary matched HEAD, two stack overflows found via unregistered margins, and
two new CI guard scripts that both caught real violations on their first
run. Full postmortem: [`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m13m14--fault-reporting-and-verification-findings-full-detail-moved-2026-09-04).
**The same bug class recurred 2026-09-09** (see the header of this file):
`profile_executor` (KilnFW, `379f3fe6`) and `current_task`/`discrete_task`
(SaftyFW, `3afc5ea6`) both overflowed real task stacks with no budget check
registered, same shape as the two 2026-08-28 findings — each fix landed
alongside the missing check (`379f3fe6`'s own budget row; `5b8fc53d` for
SaftyFW). `316967b7` closed the KilnFW-wide gap (28 previously-uncovered
tasks got budgets in one pass), then `c726748e` found even that coverage
overstates its own confidence for indirect-dispatch tasks — read that commit
before treating a green stack-budget run as proof.

## M10 — Instrumentation: make the board tell you when it is wrong · *CLOSED 2026-09-04*

Not a feature milestone. It exists because four separate defects in this
project were invisible for weeks not because they were subtle, but because
nothing on the board was counting the right thing — and in three of the
four, something *was* counting and reported the comfortable answer.
**All findings landed and are hardware-verified.** Full postmortem for each
(route-table overflow, safety-poll false timeouts, DRAM/stack-margin
instrumentation and its own blind spots, truncated-JSON readiness checks,
the heartbeat-contract guard, and the HTTP-concurrency-reset root cause):
[`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m10--instrumentation-findings-full-detail-moved-2026-09-04).

Owned by [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md) §§12–13.

## M15 — Architecture hardening · *opened and CLOSED 2026-09-04*

**CLOSED.** All 22 findings from the four-agent architecture review landed
the same day (12 files over the 1500-line rule split move-only, five new CI
drift/lint checks added, the SX1509/DRAM/duty-struct/mode-state/JSONL/
quantize/mcpkit/frame-A findings all fixed) — `build_kilnfw` + 21/21 host
tests green throughout. Informational carry-forward: the Pico is still on
protocol v8, which makes S13's BORROWED-zone indicator unreachable in
practice (fails closed and visibly, not a defect). Full per-item detail,
file maps and the "patterns worth copying" list:
[`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m15-architecture-hardening-findings-full-detail).

---

## M16 — Source layering and hardware abstraction · *opened 2026-09-05, CLOSED 2026-09-16*

**CLOSED.** Both reorganisations landed: the `drivers/` directory move
(`9f18ca5`, 2026-09-05) and all five HAL phases, and the one item that stayed
open past that — the hardware timing re-check for safety-link reply, display
frame time, and thermo read latency — got its last two live-board numbers on
2026-09-16 (`link_reply_us` measured 2026-09-14; `display_flush_us`/
`thermo_read_us` measured 2026-09-16). Everything named as unmigrated in the
plan (Wi-Fi/httpd/LVGL, OTA partition writes, the SaftyFW bootloader,
`firmware/UnitTestFw`) is an owner-decided permanent holdout, not open work.
Full detail, numbers and bars: [`docs/HW_ABSTRACTION.md`](docs/HW_ABSTRACTION.md).

---

## M17 — The zone graphic: configuration you can look at · *opened 2026-09-18; stages 1 and 3–5 landed*

An owner request, and a specific kind of instrument rather than decoration. At
the top of the **web** zones page sits a cartoon of a stacked kiln — octagonal
brick ring sections on a tube-steel stand with a hinged lid, no controller box
and no branding of any kind — drawn with exactly as many rings as
`thermo_count`, annotated per ring with the heaters, thermocouple and current
sensor that zone is configured for, in icons *and numbers*, plus icons for any
extra relays. Errors and warnings appear as badges that open detail on click.
A second, smaller piece: extra relays gain a **device type** — damper, outlet,
valve, fan, light, other — a fixed code-defined enum, one byte per relay,
rendered as a dropdown.

The point is at-a-glance confirmation that the right settings exist, which sets
an unusually harsh acceptance standard: **a graphic that renders plausibly
while the configuration is wrong or unknown is worse than no graphic**, because
the operator stops checking the fields under it. The plan's controlling section
is the one that decides how "unknown" and "not reported" are drawn so neither
can ever read as "configured and healthy", and its negative test is the one
that guards it — feed the renderer a response with the sensor associations
deleted and assert the unknown glyph appears and no configured-state icon does.
A well-meaning "sensible fallback" is exactly how this feature turns into a
confident lie.

**Decided, so they are not re-opened:** web only — the LCD zones page is
explicitly out of scope, it is 480x320 and must not scroll. The artwork is
inline SVG generated from the zone count inside `zones_page.html` itself, which
is one `EMBED_TXTFILES` blob, so a separate asset would cost new embed, gzip
and route plumbing to save bytes it would not save. The data contract extends
`GET`/`POST /api/zones` by a single `relay_types` array rather than adding a
route; **no httpd stack buffer and no zones JSON buffer is enlarged**, and the
graphic contributes zero bytes to any response because it is built in the
browser from numbers already on the wire. The relay device type lives in
`relay_names_cfg_t` with its own `RELAY_NAMES_CFG_VERSION` bump — **not** in
`zones_cfg_t`, even though a zones-schema bump is now authorized for the
parallel CT-channel work, because cosmetic per-relay data should not share the
PID gains' rollback fate.

**The one blocking hazard, and it was a data-loss one — CLOSED by stage 1,
2026-09-18.** `relay_names_validate()` silently discards the entire blob on any
version mismatch, resetting every relay name to blank, and only one version of
that blob had ever existed so no migration function existed for it. Bumping it
without writing one would have wiped every operator-entered relay name on the
next firmware update, with no notice. The migration landed in the same commit
as the bump, with the host test that proves a real v1 blob (frozen v1 layout,
v1 CRC) still yields every name under v2 firmware — negative-tested by breaking
the name copy and confirming that test goes red.

A second instance of the same hazard, not in the original plan, was found and
closed alongside it: the generic `pref_cfg_fs` bridge to the `cfg` filesystem
is parameterised by one fixed item size, so a v1-length *file* would have been
dropped silently rather than migrated. `relay_names_load()` now upgrades such a
file in place at the same rev before the divergence tie-break runs, with its
own host test. Inert today — no board mounts `cfg` yet.

**Stage 1 landed**: `relay_device_type_t` (with `unset` as enum 0), the
`types[]` array, `RELAY_NAMES_CFG_VERSION` 1 → 2, the frozen
`relay_names_cfg_v1_t`, both migrations, and the accessors. **Note the
accessor names**: `zones_config_get/set_relay_device_type()`, because
`zones_config_get/set_relay_type()` already exists and means something else
entirely — the zone's *switching hardware* (SSR/contactor/mercury) behind the
relay-life budget. Two meanings of "relay type" now coexist; the collision
surfaced only as a duplicate-definition compile error.

Two faults the front end genuinely cannot see are recorded rather than faked:
there is no per-zone heater-load fault (only `ct_warn_mask`, which is silent on
any zone whose normal current was never measured, and which cannot assert at
all on this ~4 W fixture), and there is no live per-channel CT presence or
calibration-health flag as distinct from the config fields. The three questions
that were open for the owner are all answered as of 2026-09-18 and recorded in
the plan's section 10: an un-set relay type **does** get its own enum value 0,
`unset`, so a migrated board says "nobody has told me what this relay does"
rather than quietly claiming "other" (built in stage 1); a global safety trip
is drawn as **one banner across the whole graphic**, never per-zone badges; and
the per-zone heater-load badge **ships dormant and labelled "not measured"**
rather than being deferred.

Full detail, five-stage landing sequence and test strategy:
[`docs/ZONE_GRAPHIC_PLAN.md`](docs/ZONE_GRAPHIC_PLAN.md).

**Stages 3–5 CLOSED 2026-09-18 — the graphic itself now exists.** A cartoon
kiln sits at the top of the web zones page, drawing one glowing ring section
per configured zone from `thermo_count` — z0 top, z2 bottom — annotated with
each zone's heater relays, thermocouple channels and CT, plus icons for any
extra relay (derived as any relay bit no zone's `relay_mask` claims, never a
declared list). Every annotation has three renderings, not two: configured,
deliberately none, and not reported. Badges carry click-popups saying what they
mean and at what scope. A safety trip draws ONE banner for the whole kiln,
keyed to the live latched `diag_state` rather than the last trip event — the
bench board reports a ~39-minute-old `trip_reason` alongside a healthy live
state, which an event-keyed banner would have painted as a permanent false
trip.

**Owner corrections, 2026-09-18.** Two things the drawing got wrong once it
was in front of the owner. The large orange slabs under the top rim and at the
foot of each upper ring — the per-ring `kg-glow` ellipses, mostly painted over
by the ring bodies so only their top arcs showed — are gone; the small orange
heater indicator circles stay, one per ring. The lid was drawn before the rings
and floated above them on its own geometry, reading as an open ring rather than
a lid: it is now emitted last, in the body's own projection (same centre axis,
same `rx`/`ry` arc as the ring rim), filled with the card background so it
reads as a solid cap. `tools/check_zone_graphic_render.ps1` grew assertions for
all three facts and each was negative-tested by sabotage and a hand restore.

The same pass answered a second owner request about the settings below the
graphic: each per-group inheritance selector now shows or hides a bordered
frame containing exactly the fields that selector governs, visible only while
it says "Custom settings for this zone". Hiding is the element's own `hidden`
property, never an inline display style, and submission semantics are
unchanged — an inherited zone already saved through the terminal zone's stack.
`firmware/KilnFW/App/test/test_zones_group_frames.js` covers the rule,
including that zone 0's frames never hide.

**Stage 2 CLOSED 2026-09-18 — the type is on the wire and settable.**
`"relay_types"` is reported by `GET /api/zones` as one small integer per relay,
dense and 0-based like `relay_names`; `relay_type_N=` on the `POST` stores it,
refusing an out-of-range or non-numeric value with a 400 rather than coercing it
to `UNSET`; and each unowned relay gained a device-type dropdown beside its name
field, with `UNSET` ("not set") a real selectable choice that renders as the
unknown glyph. Measured cost: 24 bytes on a max-width response (6624 → 6648
against `json_cap` 7360), so no zones JSON buffer and no httpd stack buffer
grew. Web only; the LCD gained nothing. Not yet verified on hardware — the
set-over-HTTP/reboot/read-back check still needs a board flashed with it.

The controlling requirement is the plan's section 6: a graphic that renders
plausibly while the configuration is unknown is worse than no graphic. So the
render function is a pure JSON → HTML-string function with DOM insertion kept
separate, and `tools/check_zone_graphic_render.ps1` drives it under node
against fixture JSON — asserting that ring count follows configuration, that
zone order is top-down, that a missing field renders as unknown rather than
taking the page's own form default (`thermo_mask = 1 << i`), and that an
invalid config emits **no `<svg>` at all**. Both of those last two were
negative-tested by sabotage, hand restore and a re-run.

Colour never carries a badge's meaning alone: each badge gets a shape-distinct
glyph (octagon for a fault, triangle for a warning, dashed hollow circle for
unknown) plus an SVG `<title>` and the click-popup text. The two new badge
rules are therefore recorded in
`firmware/KilnFW/App/test/ui_status_color_allowlist.json` with that call-site
cue written out, which is what `check_ui_status_color.ps1` asks for — it can
see the CSS shape but not the JS-injected glyph.

Cost: +29,273 bytes raw, **+9,480 gzipped** on the embedded page — over the
plan's own 12 KB / 4 KB budget, recorded as a measured overrun in
[`docs/ZONE_GRAPHIC_PLAN.md`](docs/ZONE_GRAPHIC_PLAN.md) §5 rather than
restated. No httpd stack buffer and no zones JSON buffer grew; the live half
rides the page's existing 3 s `/api/status` poll instead of adding a fetch.
Not hardware-verified: another task owned flashing, so the board runs firmware
without this page — what is verified is the render function against real
captured board JSON.

---

## Future work — KilnFW PC-link command acknowledgement

**CLOSED**, moved out of this file 2026-08-24, closed in the owning doc
2026-08-24 (`5df2190`, `a458a8f`, `7b4c087` — all verified ancestors of
`origin/main`; `check_bridge_reject_reason.ps1` and
`test_bridge_reject_reply.py` both pass). This section had become a pure
forwarding address with no open row of its own — collapsed here 2026-09-16
so it can't be mistaken for live work. Two deliberate, still-visible gaps
live in the owning doc, not here: `display_bridge_task` awaiting an owner
decision (delete vs. restore), and `BLIT_DATA` staying fire-and-forget by
design. Full detail: [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md)
section 11.

---

## Decisions taken, so they are not re-litigated

| Decision | Date | Where the reasoning lives |
|---|---|---|
| **The web drop-down is three groups -- Firing, Kiln setup, System -- and every page is nested under exactly one of them.** A group carries no href and never navigates; it is a disclosure only. The former top-level "Safety" group is dissolved into Kiln setup at full depth, because the menu supports exactly one level of nesting. No destination was dropped, and the menu stays a static array -- it is not auth- or state-aware. | 2026-09-18 | `firmware/KilnFW/App/drivers/http/nav.js`, the NAV_LINKS header comment |
| **A zone's time-proportioning window and its minimum on-time are one constraint, not two settings.** `heater_window_ms >= 3 * max(heater_min_on_ms, 10 s)`, enforced at every config door. Below that ratio no fractional duty can be rendered and a PID zone silently becomes a bang-bang one — this bench ran a 2 s window against the 10 s floor and every commanded duty rendered as OFF. | 2026-08-29 | `firmware/KilnFW/docs/PID_CONTROL.md` "The window and the minimum on-time are not independent" |
| Pico bench path is the Debug Probe: SWD plus its UART bridge on GP16/GP17. **The Pico's own USB is not used.** | 2026-08-16 | `firmware/SaftyFW/docs/ARCHITECTURE.md` §1 |
| **On-board relays K1-K4 are for galvanic isolation and switch other relays only — never element current.** They are EE2-12NUH signal relays (2 A, 125 VA max), so element switching was never physically possible. This retires the duty-window contact-life worry; the downstream device's own life and coil inrush now set the limit. | 2026-08-24 | `firmware/KilnFW/TODO.md` §6A.0, `hardware/datasheets/mainBoard_Relay/EE2-12NUH.pdf` p7 |
| ~~**PSRAM stays disabled** on the ESP32-S3~~ — **reversed 2026-08-17: PSRAM is ENABLED** (octal, 8 MB) and used for LVGL draw buffers + heap + several task stacks | 2026-08-16, reversed 2026-08-17 | `firmware/KilnFW/TODO.md` §9.1a |
| Library paths use `${KIPRJMOD}/../lib`, not a KiCad path variable | 2026-08-16 | `docs/REPO_LAYOUT.md` B1 |
| OTA authentication is challenge–response on the AP password, never a form POST | 2026-08-16 | `firmware/CommonFW/docs/UPDATE_PROTOCOL.md` §2 |
| Update frames and the version handshake are a **frozen compatibility floor** | 2026-08-16 | `firmware/CommonFW/docs/LINK_PROTOCOL.md` |
| **A request and its reply may never share a command id.** Telling them apart by payload length structurally blocks a short refusal reply, which is why SAFETY/DISPLAY/TOUCH's driver-error paths stayed silent. `GET_CT_CAL`/`GET_PARAM`/`GET_CONFIG_PAGE` moved to `0x22`/`0x23`/`0x24`; `GET_FW_VERSION` keeps its shared `0x0B` as the documented exception, being inside the frozen floor where a refusal is never needed. | 2026-08-24 | `firmware/CommonFW/docs/LINK_PROTOCOL.md` |
| **The two links version independently.** `UART_PROTOCOL_VERSION` (PC↔ESP) was an alias of `KILNLINK_PROTOCOL_VERSION` (ESP↔Pico) behind a hard-equality gate, so an isolated-link bump refused every PC command until pc_tools moved. Bitten three times before being split. | 2026-08-24 | `firmware/KilnFW/App/drivers/common/uart_task_ids.h` |
| K4 → line-contactor interlock: J10 pin 1 = NO, pin 2 = COM, pin 3 = NC (read from the K4 symbol's rest position, not silkscreen) — still wants a continuity check against the physical part | 2026-08-16 | `firmware/SaftyFW/docs/HARDWARE.md` §3 |
| E-stop circuit is normally-closed by design. ~~No jumper fitted, so an as-built board reads permanent STOP~~ — **corrected 2026-08-24 by measurement**: `pico_gpio_read(9)` reads LOW on this bench, i.e. a contact IS fitted and S7 correctly stays quiet. Do not plan around needing to fit one; measure instead. The stale note also masked a real inversion — `discrete_task.c` had `!gpio_get()` on an active-HIGH pin, so this healthy reading decoded as *pressed*, hidden because S5 latched first and `safety_guards_tick()` early-returns while any trip is latched | 2026-08-16, corrected 2026-08-24 | `firmware/SaftyFW/docs/HARDWARE.md` §5, commit `642dd54` |
| ESP32-S3 boot-loop (repeating stack overflow in the main task, right after LVGL's boot banner) fixed by raising `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 3584→8192 | 2026-08-19 | `firmware/KilnFW/App/main.c`, `sdkconfig.defaults` |
| Internal SRAM exhaustion: `xTaskCreatePinnedToCore()` always takes TCB+stack from internal SRAM, and Wi-Fi/lwIP + LVGL had claimed nearly all of it by the time later tasks tried to start (caused the AUTOTUNE/WIFI UART-task registration failures). Fixed at the source — moved LVGL's allocator and the Wi-Fi/lwIP pools to PSRAM — not by shrinking the tasks that were failing | 2026-08-20 | `firmware/KilnFW/TODO.md` §1 |
| `uart_owner_transfer()` called `xSemaphoreCreateBinary()` (a heap alloc) on every single UART transfer; under real interactive load this exhausted internal SRAM (`ESP_ERR_NO_MEM` bursts every ~40s). Fixed with a static, stack-resident semaphore | 2026-08-18 | `firmware/hwAbstraction/esp/uart/uart_owner.c` |
| LVGL hit-testing cannot escape a parent that doesn't contain the touch point, and a non-`LV_OBJ_FLAG_FLOATING` child of a flex column silently joins the flow and eats the page's content budget | 2026-08-20 | `firmware/KilnFW/App/drivers/ui/ui_topbar.h` |
| benchproto returned a **stale reply from a different command** after every PC reconnect: the host's `msg_index` restarts at 0 per connection while the firmware's dedup ring and per-task ACK cache live for the MCU's boot lifetime. `FAULT_LIST` reported "0 faults" while 8 were armed — a clean-decoding wrong answer, diagnosed by reply *length* (3 bytes is `FAULT_SCHEDULE`'s shape, not an empty list's 2). Fixed with a `SYS_SESSION_RESET` handshake that must itself bypass dedup | 2026-08-23 | `firmware/CommonFW/src/benchproto_link.c` |

---

## How the work gets done — delegate to Sonnet subagents

Standing instruction from the repository owner, 2026-08-21. It applies to every
milestone below and to any new work filed against this roadmap.

**The coordinating session should not implement roadmap work itself.
Implementation is assigned to Sonnet subagents, and the coordinator oversees
them.** In practice that means:

- The coordinator reads enough of the code to write an accurate brief, splits
  the work into non-overlapping file scopes, dispatches Sonnet subagents, and
  then verifies what comes back. It does not sit down and write the feature.
- **Sonnet is the default worker model.** Opus and Haiku are available when a
  task genuinely calls for them; Fable coordinates and reviews only, and is
  never a worker.
- **Scopes must not overlap.** Two agents editing the same file collide
  silently and the loser's work is lost. Every brief names the files it owns
  and the files it must not touch.
- **Builds stay central.** Concurrent `idf.py` runs against one build
  directory clobber each other, so subagents write code and the coordinator
  compiles once. Briefs say "do not build" explicitly.
- **Verification is not delegated.** A subagent's report is a claim, not
  evidence. The coordinator builds, flashes, and exercises the change against
  the four levels in "What 'done' means" below before any item here is ticked.
- The narrow exception is shared scaffolding that encodes a hazard already paid
  for in a debugging session — the kind of thing a fresh agent reliably gets
  wrong. Writing that once, centrally, so every delegated task inherits the fix
  is cheaper than briefing the hazard into every agent.
  `firmware/KilnFW/App/drivers/ui/ui_topbar.h` is the worked example: it exists
  because LVGL hit-testing cannot escape a parent that does not contain the
  touch point, and because a non-`LV_OBJ_FLAG_FLOATING` child of a flex column
  silently joins the flow and eats the page's content budget. Both cost a
  session before they were understood.

---

## Working from the repository root

Claude and the editor are opened at `kilnCtl/` from 2026-08-16 onward. This is
now a load-bearing assumption rather than a preference:

- **Open `kilnCtl.code-workspace`, not the folder.** The ESP-IDF extension needs
  a `CMakeLists.txt` in the workspace folder it is pointed at, and the repository
  root does not have one. The multi-root workspace gives it `firmware/KilnFW`
  while keeping the root open alongside.
- `.mcp.json` uses root-relative paths, including `-C firmware/KilnFW` for the
  ESP-IDF server.
- `idf.py` needs `-C firmware/KilnFW`; `uv` needs `--project tools/PcTools`.

What it changes, and what still needs doing, is
[`docs/REPO_LAYOUT.md`](docs/REPO_LAYOUT.md) "Working from the repository root".

---

## Cross-processor invariants

Any change that touches these needs both plans read, not one:

- The safety processor **never blocks on the link**. A frozen ESP core must not
  be able to stall it.
- The safety processor is **RX-only while firing**; the telemetry relaxation
  applies outside firing and to unacknowledged broadcast frames.
- **The safety processor must be alive to start or continue any heater-on event.**
- Current measurement is **load-active detection and a power estimate only**.
- Guards clear two independent bars — a magnitude correct operation cannot reach,
  and a duration a transient cannot sustain.
- All board relays are **pilot relays**, driving external contactors and SSRs.
- **Neither processor is updatable while the kiln can heat**, and the Pico
  enforces that itself rather than trusting the ESP.
- **Each processor checks the other's protocol version**, in both directions, and
  neither trusts the other's data until both agree. The frames that establish
  that, and the ones that carry an update, are frozen so a mismatch can never
  lock out the fix.

## What "done" means

Same four levels everywhere, and they are not interchangeable:

**planned** → **built** (compiles, `-Wall -Wextra -Werror`) → **host-tested**
(synthetic inputs, negative paths) → **hardware-verified** (observed on the real
board). [`firmware/KilnFW/docs/PROJECT_STATUS.md`](firmware/KilnFW/docs/PROJECT_STATUS.md) keeps
built and verified distinct; every plan here is expected to do the same.

## Roadmap upkeep

Standing rules, not a to-do list — apply them every time this file changes,
they never get "checked off":

- Milestone ticks mirrored into the owning plan, not only here
- `Last reviewed` date bumped whenever a milestone changes state
- New work filed under a milestone, or a new milestone added with its owner
- **A finished item leaves this plan.** Either it moves to a
  reference/doc file (a decision, hazard, or reasoning someone will re-hit
  — a one-line pointer here is enough) or it is deleted. Ticked boxes and
  completion narratives do not accumulate here; a milestone that is fully
  done collapses to one line
