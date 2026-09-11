# `s_coupling_use_measured_diag_k_dc` audit — 2026-09-11

Audit only. No firmware, config, or board state changed. Board read-only via
`kilnctrl` MCP (`control_get_zones`, `wifi_get_status`) and a direct
`GET /api/zones` at the board's reported LAN IP (`192.168.1.156`).

## 1. Where it lives and what it does

**The flag moved, and changed shape, on 2026-09-10 (opus review round 2,
defect B) — before that date it was a private compile-time constant in
`profile_executor_feedforward.c`; today it is a function.**

Current location: `firmware/KilnFW/App/drivers/control/zone_coupling_solve.c:303-312`

```c
/* 2026-09-10 (opus review round 2, defect B): moved here from a private
 * `static const bool s_coupling_use_measured_diag_k_dc = true` in
 * profile_executor_feedforward.c -- see zone_coupling_solve.h's doc comment
 * on the declaration for why. Same value, same rollout-gate reasoning
 * (PID_EXPANSION_PLAN.md sec 3.2); only the storage location changed, so
 * this is a no-op for the control path. */
bool zone_coupling_use_measured_diag_k_dc(void)
{
    return true;
}
```

Declared `firmware/KilnFW/App/drivers/control/zone_coupling_solve.h:330`. Callers,
both in `profile_executor_feedforward.c` (lines 206, 231 — `zone_coupling_solve_hold`
and `_climb`), and the S8 rate-guard mirror (`s8_rate_guard_estimate.h`, per the
2026-09-10 dedup fix), all call `zone_coupling_use_measured_diag_k_dc()` instead of
reading a private constant, closing a prior "reset one side of a pair" gap where the
safety estimator had its own textually-separate copy of the provenance rule that
omitted this check entirely.

**So as of this commit the flag itself is compiled `true`, board-wide, with no
remaining private copy.** The commit history's own name for it ("the flag") is
retained here because two things it once controlled are now split apart — see
below.

### What actually changes when it is true

Two call sites read it:

1. **`coupling_diagonal_k_dc()`** (`zone_coupling_solve.c`, used by both the n×1
   fallback and the n>1 matrix's `G[row][row]`): when true AND
   `coupling_diag_k_dc` for that zone is finite and `> 0`, the diagonal cell is the
   coupling identification's own measured diagonal (`coupling_diag_k_dc`); otherwise
   (flag false, or the field unset/non-finite/≤0) it falls back to `z_ff_k_dc`, the
   ordinary single-zone model gain.
2. **`zone_coupling_matrix_provenance_ok()`** (`zone_coupling_solve.c:259-301`,
   quoted in full below): a *gate*, not a data source. If any member of the
   candidate coupling system has a measured, nonzero off-diagonal cell, the whole
   matrix is refused (`COUPLING_SOLVE_FALLBACK_MIXED_PROVENANCE`, caller gets the
   plain uncoupled per-zone feedforward) **unless** the flag is true *and* every
   member's `coupling_diag_k_dc` is populated:

```c
bool zone_coupling_matrix_provenance_ok(const uint8_t *members, uint8_t n, bool use_measured_diag_k_dc)
{
    bool any_measured_off_diagonal = false;
    for (uint8_t row = 0; row < n && !any_measured_off_diagonal; row++) {
        float coupling_row[MAX31856_CHANNEL_COUNT];
        if (!zones_config_get_coupling(members[row], coupling_row)) continue;
        for (uint8_t col = 0; col < n; col++) {
            if (members[col] == members[row]) continue;
            float c = coupling_row[members[col]];
            if (isfinite(c) && c != 0.0f) {
                any_measured_off_diagonal = true;
                break;
            }
        }
    }
    if (!any_measured_off_diagonal) return true;

    if (!use_measured_diag_k_dc) return false;
    for (uint8_t row = 0; row < n; row++) {
        float measured = 0.0f;
        if (!(zones_config_get_coupling_diag_k_dc(members[row], &measured) && isfinite(measured) &&
              measured > 0.0f)) {
            return false;
        }
    }
    return true;
}
```
(`zone_coupling_solve.c:275-301`)

**Since 2026-09-10 the flag is no longer "does the mixed matrix run with a
foreign diagonal" — that mixture is now structurally impossible.** With the
flag true and `coupling_diag_k_dc` populated for every member, the n>1 coupled
solve runs with its own self-consistent diagonal. With the flag false (or a
member's `coupling_diag_k_dc` unpopulated), the coupled matrix is refused
outright and every zone gets its uncoupled 1×1 feedforward using `ff_k_dc`
(the ordinary single-zone model gain) as its own diagonal. There is no
remaining code path that solves an n>1 matrix using a mix of measured
off-diagonals and an imported (non-measured) diagonal — that mixture was the
2026-09-09 defect (`docs/audits/dc_gain_factor_of_ten_2026-09-09.md`) and the
provenance gate above, added same-day, is what closed it (§6 of that audit).

**Important caveat found during this audit: the live MCP tool text is stale.**
`kiln_call(name="control_get_zones")` on this board today prints, per zone:

> `z0=42.7310 (measured, but firmware's s_coupling_use_measured_diag_k_dc is
> compiled false -- not currently used even though present)`

This string is hardcoded in
`tools/PcTools/src/kilnctrl/mcp_server_control.py:120-122`
(`_describe_coupling_matrix()`), written before the 2026-09-10 move, and never
updated afterward. It describes pre-2026-09-10 semantics (a private constant,
compiled false) that no longer match the firmware source (a function, compiled
true, and no longer the load-bearing gate by itself). This is a genuine,
separate small defect in the MCP tooling — the message will mislead the next
person who reads it — but it does not affect firmware behavior, only a
diagnostic string. Not fixed here per the "audit only" scope; flagged for a
follow-up patch to `mcp_server_control.py`.

## 2. What diagonals the firmware uses today, and where they come from

Live board read, `GET http://192.168.1.156/api/zones` (also cross-checked
against `kiln_call(name="control_get_zones")`'s `coupling_diag_k_dc` line):

| zone | `model_k_dc` (= `ff_k_dc`, the 1×1 fallback diagonal) | `coupling_diag_k_dc` (measured, coupling-run diagonal) | `model_tau_s` | `model_dead_time_s` |
|---|---|---|---|---|
| 0 | 42.7310 | 42.7310 | 255.6 | 40.3 |
| 1 | 32.3969 | 32.3969 | 258.9 | 31.3 |
| 2 | 33.8493 | 33.8493 | 247.1 | 26.0 |

**Finding: on this board today, `model_k_dc` (the single-zone FOPDT fit, used
whenever the coupled matrix is *not* engaged) is numerically identical, to the
digit, to `coupling_diag_k_dc` (the coupling run's own measured diagonal), and
both equal the `b64fe09d` capture's diagonal exactly** (see §3 — these are the
same 42.731/32.397/33.849 figures quoted in the task). This is not what the
2026-09-09 mixture defect described (imported single-zone diagonal ≠ coupling
run's diagonal, 17–63% apart, see `dc_gain_factor_of_ten_2026-09-09.md` §4) —
that mismatch existed on an *earlier* board configuration. At some point
between then and now, `model_k_dc` was overwritten to the `b64fe09d` joint
values (almost certainly via `control_set_zone_model()` / a preset apply,
not via `autotune_engine_guard.c`'s normal single-zone-fit write path — see
the provenance note below), so the two numbers that the 2026-09-09 audit
found disagreeing now agree by construction, board-wide.

**Consequence for this specific board: the flag is currently a no-op twice
over.** (a) Per §1, if the coupled matrix engages, it uses
`coupling_diag_k_dc` regardless of the flag's stale name once
`coupling_diag_k_dc` is populated and provenance passes — true today, all
three zones populated and off-diagonals measured (see the coupling matrix
below). (b) Even in the 1×1 fallback path (matrix refused, or `n==1`), the
diagonal used is `ff_k_dc` = `model_k_dc`, which is now numerically identical
to `coupling_diag_k_dc` on this board. Flipping between the two sources
changes nothing for zones 0–2 as configured right now, because there is
nothing left for them to disagree about.

**Provenance caveat:** `model_fit_temp_c` / `model_fit_ambient_c`
(`5d3bc854`'s new fields, meant to record the operating point of whatever
produced `model_k_dc`) read the sentinel `-273.15` (`ZONE_MODEL_FIT_TEMP_UNKNOWN`)
on all three zones — i.e. unset. That sentinel is populated only by
`zones_config_set_model_fit_context()`, called from `autotune_engine_guard.c`
right after a normal single-zone autotune fit. Its absence here confirms
`model_k_dc` was written by some other path (a direct `control_set_zone_model`
call or preset apply carrying the `b64fe09d` numbers) that does not record
provenance — consistent with, but not proof beyond doubt of, "someone hand-
copied the joint capture's diagonal into the single-zone model field."

Off-diagonals and full coupling matrix, live (`kiln_call(name="control_get_zones")`):

```
z0: [0,     25.42, 24.52]
z1: [12.44, 0,     28.69]
z2: [8.08,  10.81, 0]
```

All nonzero — `zone_coupling_matrix_provenance_ok()` sees
`any_measured_off_diagonal = true` and, since `coupling_diag_k_dc` is
populated and finite/positive for all three zones and the flag is `true`,
the gate **passes** on this board today (contrast with the `cplval75` board
state audited 2026-09-10, where `coupling_diag_k_dc` was `0.0` for all three
zones and the gate refused the matrix outright — see
`docs/audits/cplval75_coupling_verdict_2026-09-10.md` §1). Whether the n>1
coupled solve actually *executes* on a given tick depends on further runtime
qualification (hold/climb membership, `z_qualifies`, temperature range) not
audited here — this section establishes only that the *provenance* gate no
longer blocks it on this board's current config, unlike the board state the
most recent field audit examined.

## 3. Comparison against the `b64fe09d` measured constants

Task-supplied measured live plant constants (`b64fe09d`):

| zone | k (measured) | tau (measured) | dead (measured) |
|---|---|---|---|
| z0 | 42.731 | 255.6 | 40.3 |
| z1 | 32.397 | 258.9 | 31.3 |
| z2 | 33.849 | 247.1 | 26.0 |

Firmware's live `model_k_dc`/`model_tau_s`/`model_dead_time_s` (§2 table) match
these to 3-4 significant figures on every zone, every field — differences are
rounding only (`32.3969` vs `32.397`, `33.8493` vs `33.849`, well under 0.01
in relative terms). **Per-zone diagonal difference between live firmware and
the `b64fe09d` measured constants: ≈0 for all three zones** — they are the
same numbers. This is the opposite of what the 2026-09-09 audit found on
whatever board/config it examined (17-63% diagonal mismatch there); this
board's `model_k_dc` has since been synchronized to the joint capture.

## 4. Would flipping the flag help, hurt, or is it unknown?

**Unknown in general architecture, but a confirmed no-op on this board's
current configuration specifically (§2-3) — there is no live discrepancy left
for the flag to arbitrate on zones 0-2 today.**

Splitting what each piece of evidence actually bears on, per the task's
instruction not to over-extend the 2026-09-10/11 refutation:

- **The 2026-09-10/11 joint-identification refutation
  (`coupling_joint_identification_capture_2026-09-10.md`,
  `cplval75_coupling_verdict_2026-09-10.md`, commits `5844a3e8`/`947709a8`)
  is about the OFF-DIAGONAL superposition assumption**: holding multiple
  zones together and predicting each zone's rise as a linear sum of
  single-zone-identified couplings under-predicted the settled rise on all
  three zones by 11-30%, refuting the *matrix's ability to combine
  independently-measured off-diagonal terms*. That evidence says nothing
  about whether a zone's own diagonal (single-zone gain, no cross-terms) is
  correct — the `cplval75` audit's own finding (§2 there) is explicit that
  the residual is "a gain deficit... essentially flat... not a nonlinearity,"
  concentrated by the inverse solve onto z2's row precisely because z2's
  duty carries most of its own rise — i.e. it is diagnosed as a modeling
  problem with the coupled solve's structure (superposition), not with any
  single zone's diagonal in isolation.
- **The flag's live effect today (§1-§3) is to decide, when a coupled matrix
  IS attempted, whether the diagonal comes from the coupling run's own fit or
  from the single-zone fit** — and on this board those two fits have already
  been made to agree, so the flag cannot currently improve or regress
  anything by itself. Flipping it to `false` today would force every zone
  onto the uncoupled 1×1 fallback (per the provenance gate, since
  `use_measured_diag_k_dc=false` refuses any matrix with a measured
  off-diagonal) using `ff_k_dc`, which is now the *same number* as
  `coupling_diag_k_dc` — so even that would only remove cross-zone terms
  from the solve, not change any zone's own gain.
- **What is genuinely unresolved, and is a regression risk if the coupled
  matrix DOES engage on this board (it can, per §2):** the coupled solve
  still assumes off-diagonal superposition, which cplval75 refuted by
  11-30% depending on zone. Enabling the coupled path (which the flag being
  `true` plus a populated `coupling_diag_k_dc` already permits) risks
  reproducing the `ff_hold`-infeasible-above-~62°C failure mode the
  `dc_gain_factor_of_ten` and `cplval75` audits traced to this same solve,
  *regardless of whether the diagonal is self-consistent* — because the
  refuted part is the off-diagonal combination, which is unconditionally
  present whenever `n>1` membership forms.
- **Evidence that would settle it:** a settled multi-zone hold (≥8τ, all
  three zones) with the coupled solve engaged (verify via
  `ff_hold_used_matrix=True` in the poll capture, not inferred), comparing
  solved duty against observed duty the way `cplval75_coupling_verdict`
  did — this measures the *matrix's* prediction quality, which is gated by
  the refuted superposition assumption, not by the diagonal-source question
  this flag controls. Isolating the diagonal question alone would need a
  single-zone (n==1) hold compared against a joint hold with the SAME zone's
  diagonal held fixed — not attempted in any audit on file.

**Verdict: not an improvement or a regression by itself on this board today —
it is a no-op, because the single-zone and coupling-run diagonals have
already been unified outside of what this flag controls.** Whether enabling
the coupled matrix path in general (which this flag partly gates) helps or
hurts remains unknown and is bounded by the still-unresolved off-diagonal
superposition refutation, not by anything this flag decides.

## 5. Does the `zone_model_at()`/`coupling_at()` seam (`5d3bc854`) change this?

**No, on both counts.**

`zone_model_at(zi, T)` / `coupling_at(zi, T)`
(`firmware/KilnFW/App/drivers/control/zones_config_accessors.c`, per `5d3bc854`'s
own commit message) are **exact passthroughs** to
`zones_config_get_model()`/`zones_config_get_coupling()` today — `T` is
unused. The commit is explicitly "no behavior change": it adds
`model_fit_temp_c`/`model_fit_ambient_c` bookkeeping fields
(`ZONES_CFG_VERSION` 23→24) and the accessor seam a *future* gain schedule
would hook into, but no schedule exists yet and only one operating point per
zone is ever recorded (confirmed live in §2: even that one recording is
absent/sentinel on this board's current `model_k_dc`, since it wasn't written
through the normal autotune path).

So:
- It does not change the picture in §1-4 — there is still exactly one
  diagonal value per zone in each of the two sources (`model_k_dc` and
  `coupling_diag_k_dc`), not a scheduled family, and the flag still picks
  between those same two single values.
- **The flag is not redundant with the seam.** They answer different
  questions: the seam is about *which operating point* a model/coupling
  value was fit at (a temperature-schedule axis, currently inert — always
  returns the same one value regardless of `T`); the flag is about *which of
  two already-fit sources* (single-zone vs. coupling-run) supplies the
  diagonal at whatever operating point is active. Populating a real gain
  schedule behind `zone_model_at()`/`coupling_at()` later would still need
  this flag (or its successor) to decide, at each scheduled point, whose
  diagonal to trust — the seam is orthogonal to, not a replacement for, that
  decision.

## Sources

- `firmware/KilnFW/App/drivers/control/zone_coupling_solve.c` (flag definition,
  provenance gate, lines 259-312)
- `firmware/KilnFW/App/drivers/control/zone_coupling_solve.h` (declaration and
  doc comments, lines 270-330)
- `firmware/KilnFW/App/drivers/control/profile_executor_feedforward.c` (call
  sites, lines 206, 231)
- `firmware/KilnFW/App/drivers/control/zones_config_accessors.c`,
  commit `5d3bc854` (schedule seam)
- `tools/PcTools/src/kilnctrl/mcp_server_control.py:86-125` (stale diagnostic
  string, `_describe_coupling_matrix()`)
- Live board reads, 2026-09-11: `kiln_call(name="control_get_zones")`,
  `kiln_call(name="wifi_get_status")`, `GET http://192.168.1.156/api/zones`
  (board IP from the wifi read; read-only, no board state changed)
- `docs/audits/dc_gain_factor_of_ten_2026-09-09.md` (original mixture defect
  and its same-day fix)
- `docs/audits/cplval75_coupling_verdict_2026-09-10.md` (provenance-gate
  confirmation and off-diagonal superposition refutation)
- `docs/audits/coupling_joint_identification_capture_2026-09-10.md` (referenced
  by the task as the source of the `b64fe09d` capture and the superposition
  refutation)
- Task-supplied `b64fe09d` measured constants (quoted verbatim in §3)
