# `iter_tune` redesign (pending work only)

Why the old design could not be wired, the tracking metric, accept/reject
rule, perturbation strategy, safety cage, simulation harness, acceptance bars
(sec 7 A1-A8), implementation steps 1-7 and the settled owner decisions
(sec 9) live in `docs/ITER_TUNE_REDESIGN.md`. Section numbers cited elsewhere
refer to that file. Do not re-dispatch the A8 A1-half gap (45/660, 6.82 %):
closed by owner acceptance.

Pending, in order:

1. **Sec 6.5 credibility gate (needs a sensor tau capture).** The simulator
   still FAILS dwell-entry peak (ramp MAE passes 5 of 6 zone-runs). Gate to
   re-run: a sensor-tau capture on the bench, then re-fit and re-measure per
   `docs/audits/credibility_gate_dwell_offset_2026-09-14.md` (2026-09-21
   addendum) and `docs/audits/credibility_gate_scalar_adoption_2026-09-14.md`.
   If it still fails, stop and report; do not proceed on a model known not to
   reproduce the plant.
2. **Step 8, shadow mode (hardware-gated).** `firing_shadow.c` is wired; Bar 2
   is decided only after >= 5 real firings have been scored. Nothing to build
   until those firings exist.
3. **Step 9, enable trials (owner-gated).** One zone, one parameter, cage
   active, owner present, bench fixture only (sec 9.3), operator able to
   restore commissioned gains at any point. Nothing proposes gains on
   hardware until the owner signs off.
