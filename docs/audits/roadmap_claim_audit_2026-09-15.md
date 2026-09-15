# ROADMAP.md claim audit, 2026-09-15

**Scope.** Every load-bearing claim in `ROADMAP.md` checked against code at
`HEAD` and against git history — not against the file's own prose, and not
against other docs that cite it. No board was read, no firmware flashed, no
`debug_*` tool called, no heating run started. No production code was changed.

**Why this pass exists.** Two stale entries were found by accident earlier the
same day (a "parked" `adaptive_tune_ki.c` TOCTOU/fail-open defect that had been
fixed by `ac5c26a3` and then deleted outright by `88bb4333`, and a
"mutually exclusive by design" fuzzy/Ki interlock superseded by the same owner
decision). Both were corrected in `bfbb8c9e`. This pass looks for more of the
same shape deliberately.

**Headline.** The file is mostly accurate. Its header's own assertion
("Verified against the named commits, not against this list's own prose") holds
up: **all 206 distinct commit hashes cited resolve**, and every commit
spot-checked said what the roadmap says it says. The rot that exists is the
shape the 2026-09-04 nine-pass audit predicted — **shipped work still described
as pending**, six instances, plus two path-rot references and one
safety-evidence misattribution.

---

## Prominent: one claim stale in the dangerous direction

**The guard-evidence row misnames which three guard claims are hardware-
verified.** `ROADMAP.md`'s "Guard evidence is mostly host-tested, not
hardware-verified" row reads:

> only **3** are hardware-verified (S5's sensor fit/masking finding, KilnFW
> thermal_guard guard 6, **the E-stop polarity fix**)

`docs/SAFETY_CASE.md` §4 — the source the row itself cites — classifies the
S7 E-stop polarity fix as **host-tested, negative-tested**, not
hardware-verified, and names the third hardware-verified item as S5's
*masking-before-fit* finding (S5 fit and S5 masking are two separate rows).
The roadmap row therefore credits the E-stop path with a grade of evidence it
does not have. That is the wrong direction to be wrong in for a safety claim,
even though the count (3) happens to come out right.

Corrected in this pass to match `SAFETY_CASE.md` §4 exactly.

**Related, left as undetermined (see below):** the roadmap's E-stop closure row
says pole 2 is "bench-verified via `firmware/SaftyFW/README.md`'s procedure +
`estop_verified`". `estop_verified` is an operator-confirmed NVS record
(`9bc155ea`), never inferred from a GPIO read, so whether the confirmation was
actually performed cannot be established from code or git — only by reading the
board, which this audit is forbidden to do. Left standing, marked undetermined.
Mitigating fact from code, not hardware: the `/api/readiness` item is
unconditionally blocking with no override, so a firing cannot have started
without it; but `docs/SAFETY_CASE.md` records that it does **not** block manual
`/api/relay` commands or the CT sweep.

---

## Stale claims corrected (shipped work described as pending)

| # | Claim in ROADMAP.md | Verdict | Evidence |
|---|---|---|---|
| 1 | "Blocking prerequisite: **no closed-loop sim exercises the fuzzy path at all** — neither `sim_iter_tune.c` nor `sim_credibility_gate_closedloop.c` calls `pid_fuzzy_adjust()`. Nothing here is testable on the sim-first requirement until that harness exists." | **SUPERSEDED** | The narrow half is still true (neither of those two files mentions `pid_fuzzy`), but the broad claim and the "blocking prerequisite" framing are false: `fbdc5bd0` (2026-09-11) added `sim_fuzzy_closedloop.c`, "single-zone closed-loop harness for `pid_fuzzy_adjust()`"; `7ef487ff` added `sim_fuzzy_overshoot.c`; `65fc6be9` added `sim_factorial_driver.c`. The roadmap's own top-of-file bullets cite results measured *with* those harnesses, so the file contradicted itself. |
| 2 | "`docs/SCENARIO_SIMULATION_PLAN.md` is being authored separately … cited here as in-progress only, **no outcome exists**." | **SUPERSEDED** | The plan exists and its own status line reads "IN PROGRESS — WI-1 through WI-8 are DONE", with per-item status in `docs/audits/scenario_simulation_implementation_2026-09-14.md`; WI-9 is DROPPED (its premise, the fuzzy/Ki mutual-exclusion guard, was deleted by `88bb4333`); WI-10 not started. |
| 3 | "`coil_power_w` is `0.0f`/unset everywhere in the firmware, so total power in watts cannot be evaluated at all today — a 'consumer without producer' instance, **not yet fixed**." | **SUPERSEDED, and it was already superseded on the day it was written** | `dbd8ff52` (2026-09-10) added the per-coil nameplate wattage override (`ZONES_CFG_VERSION` 24→25) one day *before* `fd8d7b93` (2026-09-11) recorded this. The producer chain is complete at HEAD: `zones_http_post_parse.c` parses `coil_power_w`, `zones_config_set_coil_power_w()` stores it, `zones_http_get.c` reports it. `0.0` is a **documented sentinel** meaning "use an equal share of the nameplate sum", and the sum has its own producer (`ZONE_MAX_POWER_PARAM_ID`, consumed in `zones_current_sweep_task.c`). Whether the live board has either value set is not checkable here. |
| 4 | M12: "S14 still can't be armed until `ct_channel_map` has a real producer." | **SUPERSEDED** | `b5cb83a4` exempted the summed-CT topology from the `ct_channel_map` commissioning requirement; the roadmap's own eleventh-sweep note records `/api/readiness` reporting exactly three missing parameters, `i_normal_a[0..2]`, with `ct_channel_map` no longer counted, and `c0729e1e` added the `i_normal_a` write path. The remaining blocker is the uncalibrated CT / `i_normal_a`, not `ct_channel_map`. |
| 5 | "What is actually left" (2026-08-28): "The safety processor's `abs_max_temp_c` reads `set: true, value: 0` … So the independent overtemperature guard is not armed." | **SUPERSEDED** (stale in the *safe* direction — understates coverage) | Contradicted further up the same file: `safety_ceiling_sync.c` pushes the ESP's configured ceiling to S1 on link-up, and `c99356f8` makes `safety_ceiling_policy_target_c()` return that ceiling exactly, verified on the bench in both directions (85→80, 90, 80). The present live value is not checkable here. |
| 6 | M16 gate: "all **23** host executables green after every commit". | **STALE COUNT** | `build_host_tests.ps1` references at least `$exe37`; the eleventh sweep in this same file quotes 34/34 and `CLAUDE.md` quotes 37. Replaced with an unnumbered form rather than inventing a current figure. |

## Path rot (M7 repo reorganisation, 2026-09-05)

Both files exist, at different paths than the roadmap cites. All other
`docs/`, `firmware/`, `tools/` file references in the file resolve.

- `docs/LINK_PROTOCOL.md` → `firmware/CommonFW/docs/LINK_PROTOCOL.md` (the
  file cites the correct path in four other places)
- `docs/UI_PLAN.md` → `firmware/KilnFW/docs/UI_PLAN.md` (likewise)

---

## Claims checked and found STILL TRUE

Each verified against code at `HEAD`, not against the cited commit's message.

- **`2c49465a` derives the fuzzy bands from each zone's own autotune model.**
  `pid_fuzzy_derive_bands()` computes `model_k_dc * ERROR_BAND_K_FRACTION` and
  `model_k_dc / model_tau_s`, falling back to `ERROR_BAND_C_DEFAULT` /
  `RATE_BAND_C_PER_S_DEFAULT` and returning false on an invalid or pathological
  model. Exactly as described.
- **`88bb4333` made SIMC the sole *automatic* gain writer.**
  `adaptive_tune_ki.c` no longer calls `zones_config_get_control_mode()` at all,
  sets `ki_applied = false` unconditionally, and its own header comment carries
  the same careful "AUTOMATIC" qualifier the roadmap uses (five operator/import
  paths still call `zones_config_set_pid()` directly).
  `adaptive_tune_model.c` is the writer. The interlock the roadmap describes as
  removed is genuinely gone, not merely bypassed.
- **`firing_score` fix sequence.** `FIRING_SUBSCORE_COUNT = 6`;
  `firing_compare.c` gates each axis on `FIRING_COMPARE_VOTING_MASK` rather than
  looping to the count. As `560cffe0` is described.
- **`s_coupling_use_measured_diag_k_dc` compiles `true` and lives in
  `zone_coupling_solve.c`.** Confirmed at `zone_coupling_solve.c:304`. (The
  roadmap's own warning that stale docs still call it `false` is itself
  vindicated — `zone_coupling_solve.h:293` still says "the shipped default …
  = false". Reported, not touched: header comment, production code, and another
  session owns a related fix.)
- **`SAFETY_CEILING_HEADROOM_C` defined but unused.** Defined at
  `safety_ceiling_policy.h:113`; `safety_ceiling_policy.c` references it only in
  a comment describing what used to be added.
- **`PID_SETPOINT_WEIGHT_B` hardcoded to 1.0, untested.** Still
  `#define PID_SETPOINT_WEIGHT_B 1.0f`, one call site in
  `profile_executor_run.c`. No production knob.
- **`relay_owner` and `watchdog_task` on the bare 256-word minimum stack.**
  Both still 256 in `stack_margin_poller.c`'s table.
- **Zones JSON headroom.** `json_cap = 7360` in `zones_http_get.c`, with the
  file's own running "STILL 7360 as of …" ledger and an explicit note against
  enlarging it.
- **Every cited plan-doc section exists and says what is claimed:**
  `FUZZY_CONTROLLER_PLAN.md` §0.0 / §0.0.1 / §0.0.2 / §2(i) / §2(iv) / §4.3,
  `HW_ABSTRACTION.md` "Still open",
  `coupling_level_schedule_adjudication_2026-09-11.md` §9 "Second attempt",
  `zones_json_headroom_plan_2026-09-14.md`,
  `concurrent_fuzzy_pid_adaptation_2026-09-14.md`,
  `session_summary_2026-09-14.md`.
- **All 206 cited commit hashes resolve** in this repo or its submodule.
- **`commissioning_gate.c` exists on the safety processor**, as M12 claims —
  the gate is on the Pico, not in the KilnFW UI.

---

## Undetermined — cannot be resolved without hardware or an owner decision

Left standing in `ROADMAP.md`, explicitly marked. Not dropped, not resolved.

1. **Every "UNVERIFIED ON HARDWARE" value**: `fuzzy_strength_pct = 0.0`,
   `approach_rate_cap_c_per_hr = 0.0`, `adaptive_tune enabled=False`,
   `ease_off_window_mult = 2.0`, `control_mode = 3` on all three zones. These
   are assertions about live board state. This audit is forbidden to read the
   board, and a stale capture is not evidence. **Unverifiable by this audit.**
2. **`estop_verified`** — whether the bench procedure was actually performed
   (see the prominent section above).
3. **S8 ARMED at a hand-set 20 °C/min** vs. the documented 33.3 °C/min default:
   a live `safety_get_rate_guard()` reading, plus an open owner decision on the
   right value. Both out of scope.
4. **`abs_max_temp_c` current live value** on the Pico.
5. **Whether the ESP's flashed build matches `HEAD`** — several roadmap rows
   turn on this ("built but unflashed", "not yet flashed"). Flash state is board
   state.
6. **Any claim about a capture still running on the bench**
   (`COUPLING_JOINT_IDENTIFICATION_CAPTURE.md`) — the file was left untouched
   per its own instruction, and progress is not observable from git.
7. **`ZONES_CFG_VERSION` 26 → 27** — an open owner decision, correctly recorded
   as such.

---

## Counts

- Distinct commit hashes cited: **206**; resolved: **206**; misdescribed: **0**
  found among those spot-checked in depth (12).
- Substantive claims checked: **41**.
- Still true: **23**. Superseded/stale and corrected: **6**. Path rot
  corrected: **2**. Safety-evidence misattribution corrected: **1**.
  Could not be resolved (hardware or owner): **7** (topics, covering a larger
  number of individual value assertions).
- Never-was-true: **0**. One claim (#3, `coil_power_w`) was already false on the
  day it was written, which is the closest thing found.

## Code defects found and NOT fixed (four other agents are in this tree)

1. `firmware/KilnFW/App/drivers/control/zone_coupling_solve.h:293` still
   documents `s_coupling_use_measured_diag_k_dc` as "the shipped default …
   false" when the constant compiles `true` in the `.c` beside it. A doc
   comment, but on the safety-adjacent coupling path, and the roadmap already
   flags this exact class of staleness elsewhere.
2. `docs/SAFETY_CASE.md` records an open gap this audit did not evaluate:
   `estop_verified` / `recovery_mode` / `crash_report` gate a firing and an
   autotune run but do **not** gate manual `/api/relay` commands or the CT
   sweep. Recorded there already; repeated here because it bears on finding #1
   above.

## Method notes

- Hash resolution was mechanical (`git log -1` per hash). Commit *content* was
  read as a diff or as the resulting code at `HEAD`, never from the commit
  message alone — this repo has a documented instance (`e7b8efc`) of a commit
  message being wrong about its own change.
- Historical "Reviewed before that" sweep blocks were treated as **dated
  snapshots**, not as live claims, and were not rewritten. A statement like
  "the E-stop double-pole switch is still unwired" inside the eleventh sweep's
  own open-items list was true on its date and is correctly superseded by a
  later, dated row; rewriting history blocks would destroy the audit trail the
  file exists to keep.
