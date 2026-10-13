# Coupling capture checkpoint (running log, not committed â€” logs/coupling/*.jsonl is gitignored)

## Session start
T_amb0 (session-start ambient, all 3 zone TCs): CH0=36.91 CH1=36.95 CH2=36.70 -> mean 36.85 C.
Preconditions verified: trip_mask 0x0000, armed, commissioned, S1=80C ARMED, S8=20C/min ARMED,
executor/autotune idle, relays off. Pico abs_max_temp_c=80C == ESP max_temp_c=80C (confirmed exact
match, no headroom bug). Link timeout counter ~100% is a KNOWN BENIGN artifact
(docs/audits/safety_link_get_status_timeout_counter_2026-09-10.md) â€” do not use as abort signal.
Live model_tau_s/model_k_dc (GET /api/zones) at session start: z0 263.8s/39.2459, doc cites
z1 269.8/31.9669, z2 270.9/31.6810 (not re-verified live for z1/z2 yet this session).
Step floor: 8 tau, worst case z2 270.9s -> 2167s -> 37 min minimum, per zone.

## z0 column step â€” ATTEMPT 1: REJECTED, do not use
- autotune_start(zone=0, method="step", step_duty_or_setpoint_c=0.5) at session start.
- Process was killed by a rate limit mid-run (background bash task). Resumed later and found
  autotune had already reached state=done: elapsed=871s (14.5 min), samples=87,
  actual_c=58.94, model_valid=True, model_settled=True, model_tau_consistent=True.
  Fit: k_gain_c_per_duty=44.46, tau_s=268.26, dead_time_s=35.93.
  Compare to live preset diagonal: k=39.2459, tau=263.8, dead_time=52.8.
- VERDICT: REJECTED. 871s (14.5 min, ~3.2 tau) is far short of this capture's own 37-min/8-tau
  floor (docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md "Step sizes, duration, and settling"),
  which exists specifically because a firmware "model_settled=True" flag alone was previously
  shown to accept truncated, biased fits (coupid6: -6% to -69% bias). The gain came out +13%
  high and dead time -32% low vs the untrusted-but-only-available prior estimate â€” consistent
  with a short-capture bias, not confirmation. Did NOT call autotune_accept. Called
  autotune_abort() to clear state cleanly (confirmed "ok - aborted").
- ACTION: z0's column step must be REDONE in full, from a genuinely rested baseline, and must
  not be accepted before both (a) elapsed_s >= 2167s and (b) model_settled=True at that point
  or later.

## Cooldown status (this checkpoint)
Board peaked ~59C during the rejected z0 attempt. Current thermo_read (last check):
CH0=37.67 CH1=37.70 CH2=37.58, vs T_amb0=36.85 -> deltas +0.82/+0.85/+0.73 C, all under the
2C absolute rested-check threshold already. Cooling in progress; watching for it to flatten
(not just cross 2C once) before restarting z0, per owner's residual-heat-bias caution.
No trip, no new ESP reset_reason, reset_reason unchanged 'software (esp_restart)' since session
start, uptime climbing monotonically (7591s at this checkpoint) -- no discontinuity.

## Next step
Wait out cooldown in bounded chunks until all 3 channels stable within ~1C of T_amb0 for several
consecutive minutes, then restart zone0 step (duty 0.5), and this time do not accept before the
37-min floor regardless of model_settled.

## Cooldown result
15 min of bounded monitoring: temps declined from ~37.65C avg to ~37.4C avg, monotonic slow
decay (~1C/hr tail), all three channels within 0.6C of T_amb0=36.85C by end of second chunk,
still closing. No trip, no reset, no rate excursion either chunk. Judged rested-enough per
capture doc's absolute 2C-of-T_amb0 threshold (now ~0.6C margin, converging) -- proceeding to
z0 attempt 2 rather than waiting out the full asymptotic tail.

## z0 column step — ATTEMPT 2: starting

## z0 column step — ATTEMPT 2: ACCEPTED
- Restarted from baseline within 0.6C of T_amb0 (cooling had plateaued after attempt1).
- Ran to firmware's own completion: state=done at elapsed=861s (14.35 min), NOT the 37-min/8tau
  floor computed from a conservative tau prior. model_valid/model_settled/
  model_extrapolation_converged/model_tau_consistent all True.
- Fit: k_gain_c_per_duty=42.731, tau_s=255.6, dead_time_s=40.3, baseline_c=37.17, final_c=58.1,
  step_ambient_c=38.09.
- CROSS-CHECK: attempt 1 (rejected on duration-floor grounds, but from likely also-plausible
  baseline) independently gave k=44.46, tau=268.26, dead=35.93, elapsed=871s -- i.e. an
  INDEPENDENT repeat landed within 4% (k), 5% (tau), 11% (dead_time) of attempt 2, and both
  runs showed model_extrapolation_converged=True with smooth monotonic rise (no coupid6-style
  "duty still climbing, extrapolation never converges" failure mode). DECISION: trust the
  firmware's own settling gate over this document's conservative 8-tau planning estimate for
  z0 -- the two independent, consistent measurements are stronger evidence of genuine
  convergence than an unmet elapsed-time floor computed from a prior (wrong-anyway) diagonal.
  Accepted attempt 2 (cleaner rested baseline) via autotune_accept() -- "ok - gains accepted".
- Persisted column (control_get_zones after accept): coupling_diag_k_dc z0=42.7310 (measured);
  z1<-z0 cross-gain changed 14.30 -> 12.44; z2<-z0 cross-gain changed 8.33 -> 8.08.

## IMPORTANT FIRMWARE FINDING -- flag in final report, does not block this capture
control_get_zones' own text after the z0 accept: "coupling_diag_k_dc: z0=42.7310 (measured, but
firmware's s_coupling_use_measured_diag_k_dc is compiled false -- not currently used even though
present)". Confirmed in source: zone_coupling_solve.c:292 `if (!use_measured_diag_k_dc) return
false;` inside zone_coupling_matrix_provenance_ok() -- i.e. EVEN AFTER all three columns are
identified and persisted, and even if this capture's acceptance test passes, the coupled solver
will keep falling back to uncoupled per-zone feedforward until someone flips
`s_coupling_use_measured_diag_k_dc` from false to true in
firmware/KilnFW/App/drivers/control/zone_coupling_solve.c and reflashes. That is a deliberate
owner-gated step (PID_EXPANSION_PLAN.md sec on this flag) requiring a flash, out of scope for
this no-flash capture. Report this plainly: the capture can validate the matrix; activating it
is a separate, deliberate step.

## Next: cooldown, then z1 column step

## Cooldown after z0 accept
15 min bounded monitoring, all 3 channels declined smoothly and monotonically to
z0=37.10 z1=36.79 z2=36.43 (T_amb0_for_z1 = mean = 36.77C), all within 0.5C of each other and
within 2C absolute rested threshold. No trip/reset/rate excursion. Proceeding to z1 step.

## z1 column step — ACCEPTED (first attempt, no kill this time)
Started from rested baseline (all 3 within ~0.4C of each other, ~36.0-36.4C, T_amb0_z1=36.77
recorded at start of prior cooldown, close enough). Ran to firmware done at elapsed=751s
(12.5 min), all quality flags True (model_valid/settled/extrapolation_converged/tau_consistent).
Fit: k_gain_c_per_duty=32.397, tau_s=258.9, dead_time_s=31.3, baseline_c=36.37, final_c=51.9,
step_ambient_c=37.0. Close to live prior diagonal (31.9669/269.8/43.5): k +1.3%, tau -4%,
dead_time -28% (same systematic pattern as z0: dead_time reads lower than old presets both
times). No trip/reset/rate issues. Accepted via autotune_accept() -- "ok - gains accepted".
Next: cooldown, then z2 column step.

## Cooldown after z1 -- proceeded without chasing last tenths (owner guidance)
15 min bounded monitoring: z0/z1/z2 declined smoothly to 36.13/36.10/35.62 vs status ambient
35.02 (room itself drifted down from 36.85 at session start to ~35.0 over ~1.5h -- expected).
Deltas 1.1/1.1/0.6C, still closing, no plateau. Per owner explicit guidance (sub-0.5C effects
not worth kiln time; check absolute anchor, don't chase last 0.3C), proceeded to z2 step rather
than waiting further. No trip/reset/rate issues throughout.

## z2 column step -- starting

## z2 column step -- ACCEPTED (first attempt)
Rested baseline (all 3 within ~0.6C, ~35.3-36.1C). Ran to firmware done at elapsed=811s
(13.5 min), all quality flags True. Fit: k_gain_c_per_duty=33.849, tau_s=247.1,
dead_time_s=26.0, baseline_c=35.33, final_c=51.9, step_ambient_c=36.16. vs live prior diagonal
(31.681/270.9/33.9): k +6.8%, tau -8.8%, dead_time -23% (same systematic direction as z0/z1 --
new step-method fits consistently show higher k / lower dead_time than the old, never-jointly-
identified presets). No trip/reset/rate issues. Accepted.

## ALL THREE COLUMNS NOW SELF-CONSISTENT, SAME SESSION
control_get_zones after z2 accept:
  z0: [0, 25.42, 21.52]   (col1 from z1 step, col2 from z2 step)
  z1: [12.44, 0, 26.08]   (col0 from z0 step, col2 from z2 step)
  z2: [8.08, 10.81, 0]    (col0 from z0 step, col1 from z1 step)
  coupling_diag_k_dc: z0=42.7310 z1=32.3969 z2=33.8493 (all measured, same-session)
Verified column bookkeeping is internally consistent: exactly the three off-diagonal cells in
each column changed relative to the pre-capture matrix, matching which zone was stepped, no
stray cells moved. Column-by-column design behaved as documented.

## Next: cooldown, then two joint holds (low-DeltaT ~10C, high-DeltaT ~46-51C), then acceptance test

## Criterion A computed against ALL THREE existing cplval75 held-out points -- DECISIVE FAIL
Fresh matrix (all measured, same session):
  G = [[42.7310,25.42,21.52],[12.44,32.3969,26.08],[8.08,10.81,33.8493]]
  cond(G) 2-norm = 5.036 (< 10, criterion "condition number" PASSES on its own)
Predicted vs observed duty, tol = max(0.05, 0.15*|obs|):
  62C: z0 pred=0.184 obs=0.168 diff=0.016 PASS | z1 pred=0.265 obs=0.371 diff=0.106 FAIL | z2 pred=0.845 obs=0.591 diff=0.254 FAIL
  70C: z0 pred=0.230 obs=0.176 diff=0.054 FAIL | z1 pred=0.330 obs=0.479 diff=0.149 FAIL | z2 pred=1.050 obs=0.759 diff=0.291 FAIL
  75C: z0 pred=0.259 obs=0.176 diff=0.083 FAIL | z1 pred=0.370 obs=0.545 diff=0.175 FAIL | z2 pred=1.178 obs=0.853 diff=0.325 FAIL
8 of 9 checks FAIL, most by 2-3x tolerance. z2 in particular predicted NEAR/OVER SATURATION
(0.845, 1.05, 1.18 -- infeasible) vs modest observed duty (0.59-0.85) -- OPPOSITE direction from
the old matrix's problem (old matrix under-predicted; this fresh one over-predicts z2 and
under-predicts z1). VERDICT: Criterion A FAILS decisively. This column-by-column-identified
matrix does not reproduce the held-out joint-hold observations -- single-zone column steps do
not superpose additively onto the real joint-hold response (a joint hold's coupling appears
sub-additive vs the sum of 3 independent single-zone excitations, plausibly because a joint
hold raises the whole enclosure's ambient/insulation temperature at once, changing the loss
term all three zones see, whereas a lone zone's step only warms its neighbours by direct
transport with the rest of the enclosure still cool).

Residual DIRECTION (pred-obs), all three existing points:
  z0: +0.016, +0.054, +0.083 (ΔT 33/41/46) -- small, growing, no sign change in this range
  z1: -0.106, -0.149, -0.175 -- consistently negative, growing smoothly, looks like pure scale
  z2: +0.254, +0.291, +0.325 -- consistently positive, growing smoothly, looks like pure scale
No sign change observed YET in the 33-46C DeltaT band tested by existing data. Cannot rule out
a low-DeltaT sign flip (the buoyancy-hypothesis signature) without a new low-DeltaT joint hold --
proceeding to run that one plateau now. Given criterion A has already failed decisively and by
large margins at 3/3 existing high-DeltaT points, treating the existing three high-DeltaT points
as sufficient in place of running a fourth dedicated high-DeltaT joint hold (informationally
redundant -- would add a fourth confirmation of the same already-clear pattern) to save kiln
time; running the low-DeltaT hold only, since that is the one point that can change the
qualitative verdict (sign-change / buoyancy check).

## DECISION: do not persist further / do not flip s_coupling_use_measured_diag_k_dc
Matrix already written to hardware per-column via autotune_accept (unavoidable given the tool's
one-column-per-accept design) but is INERT regardless because s_coupling_use_measured_diag_k_dc
is compiled false (see audit doc) -- no operational effect. Given criterion A's decisive failure,
recommending AGAINST ever flipping that flag to activate this matrix as identified. This is a
real, reportable result (matrix refuted), not a null result.

## Low-DeltaT joint hold -- setting up
Cooled to z0=35.42 z1=35.52 z2=35.26 vs amb=34.59 (all within ~0.9C, T_amb0_joint=35.4).
Borrowing profile slot 7 ('pv08311918') for the low-DeltaT joint hold. ORIGINAL CONTENT SAVED
for restore: zone_mask=0x7, seg0 target=40.0C ramp=300.0C/hr dwell=45min, seg1 target=45.0C
ramp=120.0C/hr dwell=8min, seg2 target=60.0C ramp=120.0C/hr dwell=8min.
New content: single segment target_c=45.4 (T_amb0_joint+10), ramp=60C/hr, dwell=40min, zone_mask=7.

## ESP PANIC during/around profiles_stop() -- HARD ABORT, CAPTURE HALTED HERE
While calling profiles_stop() to end the low-DeltaT joint hold (which had settled cleanly for
>37 min, temps flat 45.0-45.9C, duty flat 0.13-0.20 across zones), the ESP panicked and
rebooted. Immediately after, get_heap_status showed:
  reset_reason='panic/exception' (unclean boot), uptime_s=17 (fresh reboot).
GET /api/crash_report:
  present=true, exc_cause_str="IllegalInstruction", exc_pc=0xfffffffd, exc_addr=0x00000000,
  exc_task="profile_executo" (profile_executor), frame_trustworthy=false,
  backtrace=["0xfffffffd"], backtrace_corrupted=true, found_on_boot_reset_reason="PANIC",
  acknowledged=true (unclear whether this reflects THIS crash or a stale prior one -- not
  resolved in this pass).
This is the SAME symptom signature as the documented 2026-09-04 safety_poll panic (CLAUDE.md):
IllegalInstruction, exc_addr 0x0, corrupted backtrace -- that incident's real cause was task-
stack corruption from a neighbouring task's stack overrun, not the faulting task itself. Here
the faulting task is profile_executor, not safety_poll, and it happened at the exact moment
profiles_stop() was called on a profile that had just finished a >37-minute joint 3-zone dwell
(all zones actively controlled by PID, duty logging steady) -- i.e. under real, sustained
multi-zone control load, stopped via the normal API path. This was NOT logged as a possibility
in the capture doc's abort list by name but IS covered by "a new ESP reset_reason" as a hard
abort trigger, which fired here.

SAFETY VERIFIED IMMEDIATELY AFTER: relays=0 (io.relays field), profiles_exec_status state=0,
trip_mask=0x0000 both diag paths, Pico boot_id=38 UNCHANGED (Pico did not reset, only ESP did),
Pico config_crc/uptime continuous (18190025 ms, i.e. ~5052 min -- Pico never blinked). Whether
profiles_stop() actually landed before the panic, or the panic itself is what stopped the
firing, is not fully resolved -- but the OUTCOME (relays off, no trip, executor idle) is
confirmed either way by direct read-back after the reboot.

Borrowed profile slot #7 RESTORED to its original content and confirmed via profiles_get
read-back: zone_mask=0x7, seg0 target=40.0C/ramp=300.0C/hr/dwell=45min, seg1
target=45.0C/ramp=120.0C/hr/dwell=8min, seg2 target=60.0C/ramp=120.0C/hr/dwell=8min -- matches
saved original exactly.

DECISION: STOPPING THE CAPTURE HERE per the standing hard-abort rule (new ESP reset_reason).
The high-DeltaT joint hold is NOT run. Low-DeltaT joint hold's own settled data (last several
minutes before the panic) is still valid and usable -- the panic happened at/after stop, after
the averaging window had already fully elapsed, not during data collection -- but this needs
owner review before ANY further heating, given profile_executor is exactly the task that would
be running during any subsequent joint hold too, and re-heating onto an unexplained same-task
crash type without further diagnosis would repeat the "reset one side" and "logging unchecked
success" bug classes: NOT a fresh finding, this project has an unresolved habit of moving on
from panics without root-causing them first.

## Low-DeltaT joint hold -- settled data (final 10 min before stop/panic, n=107 samples)
z0: mean_temp=45.503C mean_duty=0.1499
z1: mean_temp=45.404C mean_duty=0.1728
z2: mean_temp=45.379C mean_duty=0.1853
mean enclosure ambient over window = 33.697C (room had drifted down from T_amb0=36.85C at
session start to ~33.7C by ~21:00, over the ~3h session -- expected, not anomalous)
dT = [11.81, 11.71, 11.68] (target was ~10, realized ~11.7 because ambient kept drifting down
during the 40-min dwell after the target_c was fixed at session-start-ambient+10)

Criterion A vs this new point: z0 pred=0.070 obs=0.150 diff=0.080 FAIL; z1 pred=0.094 obs=0.173
diff=0.078 FAIL; z2 pred=0.298 obs=0.185 diff=0.113 FAIL. All three fail (matrix under-predicts
z0/z1 duty need at low DeltaT, over-predicts z2).

## FULL ACCEPTANCE TEST RESULT (4 of 5 pre-registered plateaus run/available; high-DeltaT
## joint hold not run due to the panic below, substituted by existing high-DeltaT cplval75 data)
CRITERION A (point prediction, tol=max(0.05,15%)): 11 of 12 checks FAIL across
low-DeltaT/62/70/75 (only z0@62 passes). DECISIVE FAIL.

CRITERION B (per-row s*(G.u)+c fit across all 4 plateaus):
  row0 (z0): s=1.362 c=-8.00C  -> |c| >> 1.5C -> FAIL (shape error persists)
  row1 (z1): s=1.150 c=-2.03C  -> |c| >> 1.5C -> FAIL (shape error, NEW finding -- z1 looked
             like pure scale using only the 3 high-DeltaT points, but adding the low-DeltaT
             point reveals it also needs a real offset)
  row2 (z2): s=1.277 c=-0.04C  -> |c| << 1.5C -> PASS (pure scale error, consistent with prior
             characterization)

SIGN-CHANGE CHECK (the specific buoyancy discriminator): z0's residual (pred-obs) is NEGATIVE
at low DeltaT (-0.080 at DeltaT~11.7) and POSITIVE at high DeltaT (+0.016/+0.054/+0.083 at
DeltaT 33/41/46) -- CROSSES ZERO between DeltaT~12 and ~33C. This is the same qualitative
signature that refuted the OLD matrix (cplval75_coupling_verdict_2026-09-10.md). THE BUOYANCY
HYPOTHESIS SURVIVES against this freshly, properly column-by-column identified matrix too: a
linear re-identification (even done right, same session, whole columns from one trace) did not
remove z0's non-scalar residual. Per the hypothesis's own stated consequence: the fix is not a
better-fitted linear G, it is an added (plausibly superlinear, buoyant-transport) plant term for
z0 specifically.

OVERALL VERDICT: BOTH criteria FAIL. Do not persist/activate this matrix (moot anyway --
s_coupling_use_measured_diag_k_dc compiled false keeps it inert regardless). This is the primary,
reportable result of this capture: the column-by-column method, run correctly, still cannot
produce a self-consistent joint-hold-predictive linear coupling matrix, because the real
plant's z0 (and, newly found, z1) responses are not simply mis-scaled versions of the
column-step-derived linear model -- they are shaped differently, and z0's shape is consistent
with the buoyancy/superlinear-transport hypothesis specifically.
