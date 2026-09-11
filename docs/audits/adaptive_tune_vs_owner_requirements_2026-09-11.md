# Audit: `adaptive_tune` vs the owner's three adaptive-control requirements (2026-09-11)

Independently verified against source (`firmware/KilnFW/App/drivers/control/adaptive_tune*.c/.h`)
and the live board (`kiln_call(name="adaptive_tune_get_status")`, host 192.168.1.156), not taken
on the word of `docs/FUZZY_CONTROLLER_PLAN.md`'s review.

## 1. What `adaptive_tune` actually does

Files: `adaptive_tune.c` (lock/state, Layer 1 harvesting, run-end dispatcher, opt-in flag,
Ki-baseline persistence), `adaptive_tune_model.c` (diagonal K_dc refine + full coupled-matrix
refine), `adaptive_tune_ki.c` (Ki diagnosis from within-dwell trace), `adaptive_tune_http.c`
(HTTP surface), headers `adaptive_tune.h` / `adaptive_tune_internal.h`.

**Trigger.** Two entry points, both driven by `profile_executor.c`:
- `adaptive_tune_zone_tick()` — called every control tick per active zone
  (`profile_executor.c:829`), purely observational: detects a settled dwell (slope-floor test,
  `adaptive_tune.c:319-327`) and a duty-stability test (`adaptive_tune.c:340-355`, closes a
  real defect where a slope-only test was insufficient evidence of steady state), then records
  one (duty, rise-over-ambient) point into a per-zone ring and, if every zone has a fresh
  above-floor duty reading, one joint (all-zone) row for the coupled solve.
- `adaptive_tune_run_end()` — called once per finished run, **after** `s_exec.lock` is released
  (`profile_executor.c:380`, matching `firing_stats_persist()`'s own call site), i.e. run-end
  only, never per-segment or mid-firing.

**Opt-in.** Per-zone boolean, default OFF (struct-zero), `zone_cfg_t::adaptive_tune_enabled`
(`adaptive_tune.c:706-735`, `zones_config_set/get_adaptive_tune_enabled()`). Set only via
`POST /api/adaptive_tune/enable` (`adaptive_tune_http.c`) → `kiln_call(name="adaptive_tune_set_enabled", ...)`.

**What it reads.** Live actual/duty/ambient/dwelling per tick (from the executor); at run-end,
`profile_firing_run_record_t` — per-zone active flag, `stats.sample_count`/`excluded_sample_count`
(skip if faulted/stopped-early or >5% excluded, `ADAPTIVE_TUNE_MAX_EXCLUDED_FRACTION`,
`adaptive_tune.c:559-575`), and the within-dwell trace for Ki diagnosis.

**What it computes — the SIMC path, named.** `adaptive_tune_refine_zone_locked()`
(`adaptive_tune_model.c:34-173`) fits `K = Σ(u·ΔT)/Σ(u·u)` (ordinary least squares through the
origin, `adaptive_tune_fit_gain()`, `adaptive_tune.c:151-166`) over the ring of settled-dwell
observations, blends it into the existing K_dc (`k_blended = k_dc + 0.15·(k_fit - k_dc)`,
capped to ±20%/run), then recomputes Kp/Ki/Kd via **`pid_autotune_tune_from_fopdt(&model,
AUTOTUNE_RULE_SIMC, 0.0f)`** (`adaptive_tune_model.c:113`) — explicitly "the SAME rule autotune's
Accept path uses ... never a second, looser tuning formula" (comment, line 99). τ and dead-time
are carried over unchanged (dwell data cannot inform dynamics). A separate, independent Ki-only
path (`adaptive_tune_ki.c`) diagnoses Ki-too-small/too-large from the within-dwell trace shape
(zero-crossing regularity, offset-vs-floor discrimination) when the model refine did **not** fire
this run (`adaptive_tune.c:580-633`, "D5": the two never both act in one run — the model refine
wins when both would apply). A third path (`adaptive_tune_refine_coupled_locked()`,
`adaptive_tune_model.c:335-464`) does a full least-squares coupled-matrix identification from the
joint-dwell ring and blends off-diagonal `coupling_coeff[i][j]` cells the same way.

**What it writes, and where it persists.** Kp/Ki/Kd and K_dc/τ/dead-time go through the
*existing, already-validated* setters — `zones_config_set_model()`/`set_pid()`/
`set_coupling_cell()` — into the **ordinary zone config blob**, the exact place autotune's own
Accept path writes (comment, `adaptive_tune.c:22-26`): "a reader cannot tell a learned gain from
a hand-tuned or autotuned one." The opt-in flag lives there too. Only the Ki-diagnosis baseline
(`ki_baseline`/`ki_baseline_valid`) and this module's own observation rings live in a private NVS
namespace (`adap_tune`) plus a cfg-filesystem mirror, dispatched through the flash worker
(`save_kibase_job()`, `adaptive_tune.c:506-534`) — never called directly from a PSRAM-stacked
task (see the file's top comment and `project_psram_stack_nvs_panic`).

**Revert.** `adaptive_tune_revert()` (`adaptive_tune.c:993-1102`) is a one-click, per-boot,
one-shot undo of the *last* applied change per zone (Kp/Ki/Kd, K_dc/τ/dead-time, and the
Ki-baseline, restored together to avoid re-creating the "reboot ratchet" defect its own comment
names). Refused outright (`ADAPTIVE_TUNE_REVERT_FIRING_ACTIVE`) while any firing anywhere is
RUNNING or PAUSED (`adaptive_tune.c:1012-1019`), board-wide, because a coupled multi-zone firing
can have every zone's feedforward depend on every other zone's model.

**When it runs, precisely:** run-end only (never mid-firing, never per-segment) — this is the
module's entire safety argument (see §5). It is opt-in per zone, off by default.

## 2. Is it live on this board? — the most important question

**No. It is reachable (correctly wired end-to-end) but has never actually run.**

Live read, `kiln_call(name="adaptive_tune_get_status")`, host 192.168.1.156, 2026-09-11:

```
zone 0: enabled=False observations=0 (lifetime=0) ... last refusal: zone not opted into adaptive tuning
zone 1: enabled=False observations=0 (lifetime=0) ... last refusal: zone not opted into adaptive tuning
zone 2: enabled=False observations=0 (lifetime=0) ... last refusal: zone not opted into adaptive tuning
```

All three zones: `enabled=False`, `observations_lifetime=0`, `has_applied` unset ("no refinement
applied yet this boot"), `revert_available=False`. This is not the "consumer without producer" or
"inert mode flag" failure classes this repo has shipped before (`project_consumer_without_
producer_class`, `project_fuzzy_ab_inert_control_mode`) — the code path is real, correctly
wired, and would activate the moment an operator flips the opt-in flag; it is simply **off by
design and has never been turned on on this board**, so it has zero observations, zero applied
changes, and nothing in persisted config it could have written. This board's PID gains today are
exactly whatever autotune (or hand-tuning) last wrote — `adaptive_tune` has contributed nothing.

## 3. Requirement-by-requirement scoring

**(a) Must NOT ship trained on the bench fixture — must bootstrap from this kiln's own autotune.**
**Satisfied, fully, by construction.** `adaptive_tune_refine_zone_locked()` refuses outright if
there is no existing step-test model to refine (`adaptive_tune_model.c:70-73`: `"no existing
step-test model -- learning refines, it does not create one"`), and its very first fit is checked
for plausibility **against** the existing (autotuned) K_dc (`k_fit > k_dc * 5.0` refused, line
75-79) before any blend happens. It never invents starting gains; it can only ever nudge gains
that autotune (or a hand-tune) already established. Same story for Ki (`adaptive_tune_ki.c:205-208`:
refuses if there is "no existing positive Ki to refine") and for coupling
(`adaptive_tune_model.c:390-394`: refuses if there is "no existing coupling row to refine").

**(b) Must continue adapting over subsequent heat cycles.**
**Satisfied.** `adaptive_tune_run_end()` runs at the end of *every* clean, opted-in, active-zone
firing, indefinitely — there is no cap on how many firings it will keep refining across (only a
per-run material-move floor, `ADAPTIVE_TUNE_MIN_MATERIAL_MOVE_FRAC`, so a run producing an
immaterial nudge is refused rather than written). Blend factor 0.15 and ±20%/run cap
(`ADAPTIVE_TUNE_BLEND_ALPHA`/`ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE`) mean each cycle's contribution
is deliberately small and cumulative — exactly "per-cycle refinement," run-end-only, as the task
brief describes.

**(c) Authority must grow as measured confidence rises.**
**Not at all — the sharpest gap.** There is no confidence variable, no notion of accumulated
trust, and no mechanism whose authority *changes* over time. Every guard in the module is a
**fixed** threshold checked fresh each run: `ADAPTIVE_TUNE_MIN_OBSERVATIONS=4` (an on/off gate,
not a graduated one — 4 or more observations and the fit is accepted with the *same* blend
factor and cap it would get with 400), `ADAPTIVE_TUNE_BLEND_ALPHA=0.15` (constant), and
`ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE=0.20` (constant, every run, forever). The revert path is
similarly binary: `revert_available` is a single per-boot flag, not a graduated rollback. Nowhere
does the module widen its own per-run move cap, shrink its guard margins, or otherwise let a
long, consistent track record earn more authority than a first-ever application gets. It is
correctly bounded (see §5) but the bound is *static*, not *rising with confidence* — this is the
one owner requirement `adaptive_tune` does not address in any form.

## 4. Gaps

To fully meet (a)+(b)+(c), `adaptive_tune` itself would need, at minimum:

1. **A confidence/authority state per zone** — e.g. a counter of consecutive accepted,
   materially-small refinements, or a rolling measure of fit residual/consistency across runs.
   Nothing today accumulates evidence across runs beyond the fixed-size observation ring
   (`ADAPTIVE_TUNE_RING_CAPACITY=12`, evicts oldest) and the Ki-baseline latch.
2. **A graduated cap schedule** driven by that state — e.g. `MAX_FRACTIONAL_MOVE` starting
   tighter than 20% and widening (or `MIN_OBSERVATIONS` starting higher and relaxing) as
   confidence accumulates, rather than the current fixed constants applied identically to the
   1st and the 100th accepted refinement.
3. **A confidence reset on discontinuity** — an autotune re-run, a `revert()`, or a
   fault/abort should plausibly reset accumulated confidence, not just clear the Ki baseline
   (already done, `adaptive_tune_clear_ki_baseline()`) — this needs a defined policy, not just a
   value clear.
4. **Anchoring the plausibility/blend bound to the original autotune result**, not the
   most-recently-adapted one (see §5) — otherwise "confidence" computed against a self-moving
   reference is not measuring what it claims to.

None of this needs a second, competing mechanism: it is additive state and a scheduling rule
layered onto the same run-end call, the same persisted gains, and the same revert path that
already exist. A **separate** mechanism attempting the same goal would have to independently
re-implement everything in §1 that already exists and is host-tested (`test_adaptive_tune*.c`,
6 files) — the bootstrap-from-autotune guard, the run-end-only safety boundary, the SIMC
recompute path, the persistence/dual-write/migration machinery, and the revert semantics — before
it could even begin adding confidence-based authority. That is strictly more work and more
surface area for the "reset one side of a pair" bug class this repo has shipped four times.

## 5. Safety review of what exists

**Bounds on adapted parameters.** K_dc: ratio gate ±5x (`ADAPTIVE_TUNE_MAX_JUMP_RATIO`) against
the *current* model, blend factor 0.15, per-run move cap ±20% (`ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE`),
minimum-material-move floor 0.5%. Ki: per-run correction capped at ±20%
(`ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE`), plus a **cumulative** ±5x ceiling/floor around
`ki_baseline` (`ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT`, `adaptive_tune_ki.c:258-300`). Coupling
cells: ratio/absolute plausibility gate, ±6.0 absolute move per run
(`ADAPTIVE_TUNE_COUPLING_MAX_ABS_MOVE`), and a hard ceiling `ZONE_COUPLING_COEFF_MAX` regardless
of ratchet (`adaptive_tune_model.c:447`) plus non-negativity and diagonal-dominance physical
plausibility (`adaptive_tune_matrix_plausible()`, `adaptive_tune_model.c:245-265`).

**Behaviour on fault/trip/abort.** `adaptive_tune_run_end()` is only ever called with real data
to act on when `clean == true`; a faulted or operator-stopped-early run is refused per-zone
(`"run was faulted or stopped early -- not used as training data"`, `adaptive_tune.c:563-564`)
and its per-run status fields are explicitly reset, not left stale (`reset_run_status_locked()`,
`adaptive_tune.c:449-460`, the "K1" fix documented in-file for exactly this "stale status surfaces
as this run's result" hazard).

**Suppressed during autotune?** Not applicable in the sense of a mutex — autotune and
adaptive_tune write through the same setters but at disjoint times (autotune Accept happens
independent of a profile run; adaptive_tune only ever writes at a profile run-end). There is no
explicit interlock preventing an operator from running autotune mid-firing while adaptive_tune
also wants to write, but adaptive_tune's own writes are gated to run-end (after relays are off)
and `adaptive_tune_revert()` is explicitly refused while any firing is running/paused
(`ADAPTIVE_TUNE_REVERT_FIRING_ACTIVE`) — the safety argument is "never write to a live control
loop," which holds regardless of autotune's own timing.

**Anchored to original autotune values, or to the most recently adapted ones?**
**To the most recently adapted ones — this is the documented ratchet hazard
(`project_bound_relative_to_persisted_state`), present here.** `adaptive_tune_refine_zone_locked()`
reads the CURRENT live model every single call — `zones_config_get_model(zi, &k_dc, &tau_s,
&dead_time_s)` (`adaptive_tune_model.c:70`) — and uses that same `k_dc` as **both** the ±5x
plausibility reference **and** the blend/cap baseline (`adaptive_tune_model.c:75-84`). Because a
successful refine immediately persists `k_blended` as the new live K_dc
(`zones_config_set_model()`, line 131), the *next* run's plausibility/move bounds are computed
relative to that just-written value, not the original autotune result. Over N successive
accepted refinements the gain can walk arbitrarily far from where autotune first put it, each
single hop individually bounded (±20%/run, ±5x ratio) but the reference point itself drifting
with every acceptance — exactly the shape flagged as "a bound justified against a test constant
is justified against nothing" once repeated. The coupled-matrix cells have the identical
structure (`prior_row[j]` from `zones_config_get_coupling()`, the current live value,
`adaptive_tune_model.c:390` and `410`) but are additionally capped by the absolute ceiling
`ZONE_COUPLING_COEFF_MAX`, which puts a hard floor under how bad the ratchet can get regardless of
path. K_dc has **no such absolute ceiling** — only the relative ratio/move bounds — so it is the
more exposed of the two. The Ki-diagnosis layer is the partial exception: `ki_baseline` is
explicitly documented as "latched once, never overwritten" by the diagnosis layer itself
(`adaptive_tune_ki.c:210-239`), but it IS re-latched by the *other* writer — every successful
model-refine SIMC recompute re-latches `ki_baseline` to its own fresh Ki
(`adaptive_tune_model.c:157-167`, "Q4") — so across repeated successful diagonal refinements, the
Ki cumulative-bound reference also walks forward rather than staying pinned to the original
autotune Ki. None of this has been exercised on real hardware yet (§2: zero observations,
zero applied changes on this board), so the ratchet is a **structural** risk proven by reading
the code, not yet an **observed** one.

## 6. Interaction with the fuzzy layer

They compose rather than conflict, and do not double-apply, but there is no explicit guard
preventing both from being enabled — the non-collision is architectural, not enforced.
`adaptive_tune` writes the **persisted, run-end** Kp/Ki/Kd/K_dc into the zone config blob
(`zones_config_set_pid()`/`set_model()`). The fuzzy layer (`pid_fuzzy_prepare_gains()`,
`firmware/KilnFW/App/drivers/control/profile_executor_pid_tick.c:311-362`) reads that **same**
persisted base gain set live, every tick, and applies a multiplicative adjustment scaled by
`zones_config_get_fuzzy_strength_pct()` (line 329) via `pid_fuzzy_adjust()`. They act on the same
values but at different layers and different times: adaptive_tune changes what is *stored* as
the zone's base gains (at run boundaries); fuzzy changes what is *actually applied* on top of
whatever base is currently stored (every tick, in proportion to `fuzzy_strength_pct`). There is
no code path where both write the same field at the same instant, so "double-apply" in the sense
of a race or an overwrite does not occur. As stated in the task brief, the live board currently
has `fuzzy_strength_pct = 0.0` on all three zones, so fuzzy reproduces base PID bit-for-bit today
(per `project_fuzzy_ab_inert_control_mode`/live config) — meaning this composition is entirely
**latent**, never yet exercised with both mechanisms simultaneously live. No code was found that
refuses to enable fuzzy on a zone with adaptive_tune enabled or vice versa; an operator can turn
both on today with nothing in the firmware stopping them.

## 7. Verdict

**Extend `adaptive_tune`; do not build a new mechanism, and `docs/FUZZY_CONTROLLER_PLAN.md`'s
§8.2 should be dropped.** `adaptive_tune` already, verifiably, satisfies (a) and (b) in full —
it bootstraps strictly from an existing autotune/hand-tune result and refuses to operate without
one, and it refines every subsequent clean firing indefinitely, both host-tested
(`test_adaptive_tune*.c`, 6 files) and reachable end-to-end on real hardware (confirmed live,
§2), even though it has not yet been turned on. The only owner requirement it does not meet is
(c), confidence-graduated authority, and that is a bounded, additive gap — a confidence/state
field plus a schedule that adjusts the *existing* fixed constants (`ADAPTIVE_TUNE_MIN_
OBSERVATIONS`, `ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE`, the ±5x ratio gates) — not a reason to
duplicate the run-end safety boundary, the SIMC recompute path, the persistence/migration
machinery, or the revert semantics this module already has host-tested and hardware-verified.
Building a second mechanism beside it would either have to re-derive all of that (real risk,
real time, the exact "second, competing mechanism" the review warned about) or leave a firing
touched by two independent gain-adjustment systems with no shared bookkeeping — itself a fresh
instance of this repo's "reset one side of a pair" class the moment one system's state needs to
account for the other's changes. The one thing worth fixing **before** extending it for (c) is
the ratchet in §5 (anchor K_dc's plausibility/move bounds — and, transitively, the Ki cumulative
bound's re-latch — to the original autotune result rather than the most recently accepted one,
and add an absolute ceiling on K_dc analogous to `ZONE_COUPLING_COEFF_MAX`), since a
confidence-driven authority increase on top of a self-moving reference point would only compound
that hazard rather than mitigate it.
