# Joint coupling-matrix identification capture, 2026-09-10 — matrix REFUTED, plus an ESP panic in `profile_executor`

**Status: capture halted early on a hard abort (new ESP `reset_reason`), not completed to the
full five-plateau protocol in `docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md`. Four of five
pre-registered plateaus obtained (low-ΔT joint hold run fresh this session; 62/70/75°C reused
as genuinely held-out data from `cplval75`, `docs/audits/cplval75_coupling_verdict_2026-09-10.md`,
`d2e570ad`). Both acceptance criteria evaluated and FAIL decisively. Board left safe and idle:
relays off, no trip, executor and autotune idle, confirmed by direct read-back after the panic.**

Preceded by `docs/audits/safety_link_get_status_timeout_counter_2026-09-10.md` (`514b785a`,
`96b48d67`), which cleared the safety link's GET_STATUS timeout counter as a benign,
protocol-predates-today accounting artifact before this capture was allowed to proceed.

## Preconditions confirmed before heating

- No unacknowledged crash, `recovery_mode` false, link up, `trip_mask 0x0000`, `commissioned=True`.
- Pico `abs_max_temp_c` = 80°C exactly matches ESP `max_temp_c` = 80.0 on all three zones
  (`safety_get_commissioning`, `control_get_zones`) — the hard-cutoff fix (`c99356f8`) confirmed
  on the running Pico image (`c27484a2`, clean, `config_crc` matching the ESP's cache).
- S1 (80°C) and S8 (20°C/min) both ARMED, `mains_voltage_v=120`.
- `profiles_get_exec_status`/`autotune_get_status` both idle at session start.
- `T_amb0` (session-start ambient, mean of three zone TCs): 36.85°C.
- Live `model_tau_s`/`model_k_dc`/`model_dead_time_s` re-confirmed from `GET /api/zones`
  matching the capture doc's cited values exactly: z0 263.8s/39.2459/52.8, z1 269.8/31.9669/43.5,
  z2 270.9/31.6810/33.9. 8τ floor = 270.9×8 = 2167s ≈ 37 min (worst-case zone, per doc).

## Column steps — all three accepted, whole-column-per-trace as designed

| Zone | Attempt | Elapsed | `k_gain_c_per_duty` | `tau_s` | `dead_time_s` | Quality flags |
|---|---|---|---|---|---|---|
| z0 | 1 (rejected) | 871s | 44.46 | 268.26 | 35.93 | all true |
| z0 | 2 (accepted) | 861s | 42.731 | 255.6 | 40.3 | all true |
| z1 | 1 (accepted) | 751s | 32.397 | 258.9 | 31.3 | all true |
| z2 | 1 (accepted) | 811s | 33.849 | 247.1 | 26.0 | all true |

**z0 attempt 1 was rejected, not because the fit was bad, but for process reasons**: a
rate-limit killed the session mid-run; on resume the run had already reached
`state=done` (871s, all quality flags true), but its rested-baseline provenance across the kill
could not be independently confirmed to this capture's standard. Re-run clean (attempt 2) from
a verified-rested baseline; the two independent runs agree within 4-11% on all three fitted
parameters, which is what justifies trusting the firmware's own settling gate
(`model_settled`/`model_extrapolation_converged`/`model_tau_consistent`, all true well before
the doc's own conservative 37-minute/8τ *planning* floor) rather than re-deriving a hard time
floor from a now-superseded diagonal estimate. This is not the `coupid6` truncation failure mode
(duty still monotonically climbing, extrapolation never converging) — every run here showed a
smooth, clean rise and a genuinely converged extrapolation well inside the doc's floor.

Column bookkeeping verified internally consistent after all three accepts — exactly the three
off-diagonal cells in each column changed relative to the pre-capture matrix, matching which
zone was stepped:

```
Fresh matrix (row i = affected zone, column j = stepped zone; diagonal is coupling_diag_k_dc):
  z0: [42.731, 25.42, 21.52]
  z1: [12.44, 32.397, 26.08]
  z2: [8.08, 10.81, 33.849]
```

Rested-baseline discipline: before each step, all three zones' `actual_c` were confirmed within
~1°C of the running `T_amb0` (re-measured at each cooldown, since ambient itself drifted down
~3°C over the ~3-hour session — 36.85°C at session start to ~33.7°C by the low-ΔT hold. This
matches project memory's standing note that a fixed absolute number is the wrong target when
ambient itself drifts; each cooldown was judged against its own freshly-measured anchor, not
the session-start value, and not `is_rested()` alone).

## Low-ΔT joint hold (new this session)

Borrowed profile slot 7 (`pv08311918`) for a single-segment, all-three-zone hold: target 45.4°C
(session ambient 35.4°C + 10°C), ramp 60°C/hr, dwell 40 min. **Original slot 7 content saved
before use and restored byte-for-byte after, confirmed via `profiles_get` read-back**
(`target=40.0C ramp=300.0C/hr dwell=45min` / `45.0/120.0/8` / `60.0/120.0/8`, zone_mask 0x7).

Ramp and dwell both clean: no trip, no rate excursion, temperatures and duty both flat for the
final ≥10 minutes of a dwell that ran ≥39 minutes (past the 37-min/8τ floor). Settled averages,
final 10 minutes (n=107 samples at 5s cadence):

| Zone | mean temp (°C) | mean duty | ΔT (vs. mean ambient 33.70°C) |
|---|---|---|---|
| z0 | 45.503 | 0.1499 | 11.81 |
| z1 | 45.404 | 0.1728 | 11.71 |
| z2 | 45.379 | 0.1853 | 11.68 |

(Realized ΔT is ~11.7-11.8°C rather than the targeted ~10°C because ambient kept drifting down
through the 40-minute dwell after `target_c` was fixed at hold start — still well inside the
doc's "below ΔT≈10°C" bracket and clearly separated from the 33-46°C high-ΔT band.)

## Acceptance test — pinned criteria applied as written

**Condition number**: `cond(G)` (2-norm) = **5.036** (< 10 — passes on its own).

**Criterion A (point prediction, tol = max(0.05 abs, 15% rel)), all four available plateaus**
(existing 62/70/75°C `cplval75` points plus the new low-ΔT point; the doc's own falsifying
prediction, `u(62)=[0.168,0.371,0.591]`, `u(70)=[0.176,0.479,0.759]`, `u(75)=[0.176,0.545,0.853]`,
kept unchanged):

| ΔT plateau | z0 pred/obs/diff | z1 pred/obs/diff | z2 pred/obs/diff |
|---|---|---|---|
| low (~11.7) | 0.070/0.150/**0.080 FAIL** | 0.094/0.173/**0.078 FAIL** | 0.298/0.185/**0.113 FAIL** |
| 62°C | 0.184/0.168/0.016 PASS | 0.265/0.371/**0.106 FAIL** | 0.845/0.591/**0.254 FAIL** |
| 70°C | 0.230/0.176/**0.054 FAIL** | 0.330/0.479/**0.149 FAIL** | 1.050/0.759/**0.291 FAIL** |
| 75°C | 0.259/0.176/**0.083 FAIL** | 0.370/0.545/**0.175 FAIL** | 1.178/0.853/**0.325 FAIL** |

**11 of 12 checks FAIL, several by 2-3× tolerance.** z2's predicted duty at 70/75°C
(1.05, 1.18) is infeasible — over-saturated — against a modest observed 0.76/0.85. **Criterion A
FAILS decisively.**

**Criterion B (per-row `ΔT = s·(G·u) + c` least-squares fit, all four plateaus)**:

| Row | s | c (°C) | Verdict |
|---|---|---|---|
| z0 | 1.362 | **−8.00** | FAIL — needs a large offset, `|c|` far over the 1.5°C bar |
| z1 | 1.150 | **−2.03** | FAIL — also needs a real offset (new finding, see below) |
| z2 | 1.277 | −0.04 | PASS — pure scale, `|c| ≈ 0` |

**Sign-change check (the buoyancy discriminator)**: z0's residual (`pred − obs`) is **negative**
at low ΔT (−0.080 at ΔT≈11.7) and **positive**, growing, at high ΔT (+0.016/+0.054/+0.083 at
ΔT 33/41/46) — it **crosses zero** between ΔT≈12 and ΔT≈33°C, the same qualitative signature
that refuted the previously-adopted matrix (`cplval75_coupling_verdict_2026-09-10.md`). z1 and
z2 show no sign change across the tested range (z1 consistently negative, z2 consistently
positive) — scale-type behavior for those two rows, though z1's offset is large enough to still
fail the ≤1.5°C bar once the low-ΔT point is included (it looked like a pure scale using only
the three high-ΔT points; adding the low-ΔT point shows it is not).

## Verdict

**Both criteria FAIL. This freshly, correctly column-by-column identified matrix is refuted by
the same held-out data (plus one new low-ΔT point) that refuted the previous mixed-provenance
matrix — for a different reason.** The previous matrix's failure was mixed-provenance gain
deficit; this one's diagonal and off-diagonals are all measured, same-session, and
internally self-consistent, and it *still* fails, because **single-zone column steps do not
superpose additively onto the real three-zone joint-hold response.** z2 in particular is
predicted to need near-saturated or infeasible duty at high ΔT that the plant never actually
needs — the opposite direction of error from before, and evidence that a joint hold's real
coupling is sub-additive relative to the sum of three independent single-zone excitations
(plausibly because a joint hold raises the whole enclosure's ambient/insulation temperature at
once, changing the loss term every zone sees, where a lone zone's step only warms its
neighbours by direct transport with the rest of the enclosure still comparatively cool).

**The buoyancy hypothesis for z0 (`docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md`, "Hypothesis:
z0's shape error is buoyant transport, not a bad fit") SURVIVES**, now confirmed against an
independently, freshly identified matrix rather than only the original mixed-provenance one:
z0's residual sign-flips across the same discriminator range predicted, meaning **no
re-identification of a linear `G`, however carefully done, can fix z0's row** — per the
hypothesis's own stated consequence, the fix is an added plant term (plausibly a superlinear
convective-transport term with its own lag), not a better-fitted linear matrix. z1 is now also
shown to need more than a scale correction, which the doc's own prior "z1 intermediate"
characterization anticipated but had not confirmed with a low-ΔT point until this pass.

**Do not persist this matrix as an activated solution.** It is moot regardless: `control_get_zones`
reports the diagonal values as "measured, but firmware's `s_coupling_use_measured_diag_k_dc` is
compiled false — not currently used even though present," confirmed in source
(`firmware/KilnFW/App/drivers/control/zone_coupling_solve.c:292`,
`if (!use_measured_diag_k_dc) return false;` inside `zone_coupling_matrix_provenance_ok()`).
Even a matrix that HAD passed would not activate on this image without a separate, deliberate
flag flip and reflash (`PID_EXPANSION_PLAN.md`'s own section on this flag) — out of scope for
this no-flash capture, and now moot given the failure above. The per-column values are left
persisted on-hardware (unavoidable — `autotune_accept()` persists one column per call, before a
joint acceptance verdict is even possible) but are inert and should not be read as "the adopted
matrix."

## Hard abort: ESP panic in `profile_executor` while stopping the low-ΔT hold

While calling `profiles_stop()` to end the low-ΔT joint hold — which had already settled
cleanly for the required duration — the ESP panicked and rebooted. `get_heap_status`
immediately after showed `reset_reason='panic/exception'` (unclean boot), `uptime_s=17`.
`GET /api/crash_report`:

```
present=true, exc_cause_str="IllegalInstruction", exc_pc=0xfffffffd, exc_addr=0x00000000,
exc_task="profile_executo" (profile_executor), frame_trustworthy=false,
backtrace=["0xfffffffd"], backtrace_corrupted=true, found_on_boot_reset_reason="PANIC",
acknowledged=true (unclear whether this reflects this crash or a stale prior one — not
resolved in this pass)
```

This is the same symptom shape as the documented 2026-09-04 `safety_poll` panic (project
memory `project_safety_poll_panic_thermo_slot_corruption`: `IllegalInstruction`, `exc_addr 0x0`,
corrupted backtrace, real cause a neighbouring task's stack overrun corrupting shared state) —
here the faulting task is `profile_executor` instead, and it happened at the exact moment
`profiles_stop()` was called after a genuine, sustained, all-three-zone PID-controlled dwell
(the kind of load this task rarely sees for 40+ continuous minutes in routine use). Whether
`profiles_stop()` landed before the panic, or the panic is itself what ended the firing, is not
resolved in this pass — but the **outcome** is confirmed by direct read-back after the reboot:
`io.relays = 0`, `profiles_exec_status.state = 0`, `trip_mask 0x0000` on both processors, Pico
`boot_id` unchanged at 38 (the Pico did not reset — only the ESP did), Pico uptime and
`config_crc` continuous throughout.

**This halted the capture** per the standing hard-abort rule ("a new ESP `reset_reason`"). The
high-ΔT joint hold was not run; the existing 62/70/75°C `cplval75` data substituted for it in
the acceptance test above (and — since the acceptance test had already failed decisively at
every other tested plateau — running a fourth high-ΔT point would most likely have added a
fourth confirmation of an already-clear pattern, not changed the verdict). **This crash is a
more important open finding than the matrix result and should be root-caused before any future
multi-hour joint hold is attempted** — this project has an unresolved history of moving on from
panics without root-causing them first (see `project_screen_idle_brick_real_cause` and the
`safety_poll` panic above), and `profile_executor` is exactly the task that would be running
during any future joint-hold capture too.

## Board state at end of session

Verified by direct read-back after the panic and reboot: `io.relays = 0`, executor `state = 0`,
autotune `state = idle`, `trip_mask 0x0000` on both processors, Pico armed and stable
(`boot_id` 38 unchanged, uptime climbing normally post-ESP-reboot). Borrowed profile slot 7
restored to its exact original content, confirmed via `profiles_get` read-back. **Board left
safe and idle.**

## What this capture establishes, and what it does not

**Establishes**: the column-by-column identification method itself is sound and reproducible
(two independent z0 runs agreed within 4-11%); the resulting matrix is well-conditioned
(`cond=5.04`) but is **refuted** by held-out joint-hold data via both pre-registered criteria;
the buoyancy hypothesis for z0's shape error survives a second, independent test; z1 is now
also shown to need a shape correction, not just a scale correction. **Does not establish**: a
working replacement coupling matrix (none is being persisted as active), or a root cause for
the `profile_executor` panic (flagged for separate, dedicated investigation).

## Raw data

`.jsonl` captures are local-only (`.gitignore:100`): `logs/coupling/coupling_col_z0_20260910*.jsonl`,
`coupling_col_z1_20260910.jsonl`, `coupling_col_z2_20260910.jsonl`, `coupling_jointlo_20260910.jsonl`,
`cooldown_after_*_20260910.jsonl`, and the running `CHECKPOINT.md` narrative. Derived summary:
`docs/audits/coupling_joint_identification_capture_2026-09-10_observations.tsv`.
