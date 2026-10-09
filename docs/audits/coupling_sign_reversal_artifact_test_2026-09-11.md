# Coupling sign-reversal: artifact test (H2), 2026-09-11

**Status: re-analysis only. No board touched, nothing flashed, no heating run, no `.kicad_*`
file touched.** Tests hypothesis H2 from
`docs/audits/joint_load_model_class_design_2026-09-11.md` §2/§3: that the 62-75°C joint study's
**under**-prediction (opposite sign from the recent low-ΔT joint hold's ~33% **over**-prediction)
is an artifact of comparing against the old, mixed-provenance coupling matrix, rather than a
genuine second physical mechanism.

**Verdict: H2 is REFUTED. H1 stands.** Recomputed against the freshly, correctly,
column-by-column-identified matrix `G` (`docs/audits/coupling_joint_identification_capture_
2026-09-10.md`, all cells same-session, same-provenance), the 62-75°C plateaus still show
under-prediction, same sign as against the old matrix, on all three zones at all three
plateaus, both with and without an ambient-drift correction. The sign reversal between the
low-ΔT regime (over-prediction) and the 62-75°C regime (under-prediction) is not explained by
matrix provenance. It survives the switch to a matrix that is internally consistent and
correctly identified. Either a genuine second, high-ΔT mechanism exists (H1), or some other,
still-unidentified artifact is at play — but "wrong matrix" is ruled out as that artifact.

---

## 1. Which matrix did the 62-75°C study compare against, and what is each cell's provenance?

Per `docs/audits/cplval75_coupling_verdict_2026-09-10.md` §2 ("Finding A"), the matrix the board
actually held during the 62-75°C (`cplval75`) study was:

```
G_old = [[39.2459, 27.32,   21.72  ],
         [14.30,   31.9669, 22.15  ],
         [ 8.33,   12.42,   31.6810]]     cond(G_old) = 5.508
```

Per-cell provenance, as stated in that document and `docs/audits/coupling_joint_identification_
capture_2026-09-10.md`'s own characterization of it:

- **Diagonal (39.2459 / 31.9669 / 31.6810)** — live `model_k_dc` from the board's own per-zone
  FOPDT identification (`GET /api/zones`), a **measured** quantity, but measured in a
  **different experiment** than the one that produced the off-diagonals below.
- **Off-diagonals (27.32/21.72, 14.30/22.15, 8.33/12.42)** — from an earlier coupling run whose
  **own** diagonal was 38.13 / 35.90 / 35.32 (cited verbatim in `cplval75_coupling_verdict_
  2026-09-10.md` §2e), i.e. a column-by-column run — also measured, not assumed or a prior —
  but from a session with a different diagonal than the one actually paired with it on the
  board.
- **Net result**: `G_old` is not a single coherent identification. It is six measured
  off-diagonal cells from one session married to three measured diagonal cells from another
  session — what `cplval75_coupling_verdict_2026-09-10.md` calls "a mixture that is too weak
  overall in exactly the way measured here" (§2e). No cell in `G_old` is a prior/assumption in
  the sense of "never measured" — every cell traces to some real identification run — but the
  matrix as assembled was never validated as a whole, and firmware's own provenance guard
  (`zone_coupling_solve.c:259`, `coupling_matrix_provenance_ok()`) refused to activate it for
  exactly this reason (`any_measured_off_diagonal=true` but `coupling_diag_k_dc` all read 0.0 on
  the live config, forcing `COUPLING_SOLVE_FALLBACK_MIXED_PROVENANCE`).
- **Fresh, single-session, column-by-column matrix** (the one this test re-computes against),
  per `docs/audits/coupling_joint_identification_capture_2026-09-10.md`, all nine cells from one
  continuous same-day session, diagonal and off-diagonal from the same column steps:

```
G_fresh = [[42.731, 25.42, 21.52],
           [12.44,  32.397, 26.08],
           [8.08,   10.81,  33.849]]     cond(G_fresh) = 5.036
```

This is the matrix given in the task brief and used throughout §3 below.

## 2. Raw 62-75°C plateau data — recoverable

The per-5-second raw poll (`logs/coupling/cplval75_20260910.jsonl`) is local-only
(`.gitignore:100` excludes `logs/**/*.jsonl`) and was **not** available to this pass. However,
the **derived, committed** settled-hold summary — `logs/coupling/cplval75_20260910_settled_hold_
points.tsv` (referenced in `docs/audits/cplval75_settled_holds_2026-09-10.md`) — carries the
per-zone, per-plateau mean temperature and mean duty over each plateau's final 600 s window,
confirmed settled by measured temperature std-dev (not by elapsed time alone). This is
measured plateau data, not a reconstruction from summary statistics — it is the same
already-computed settled-window means the original `cplval75_coupling_verdict_2026-09-10.md`
analysis itself used. Values used below, verbatim from that file:

| target °C | zone | mean temp °C | mean duty |
|---|---|---|---|
| 62 | 0 | 62.119 | 0.1680 |
| 62 | 1 | 62.036 | 0.3714 |
| 62 | 2 | 62.123 | 0.5905 |
| 70 | 0 | 69.969 | 0.1764 |
| 70 | 1 | 69.938 | 0.4792 |
| 70 | 2 | 70.003 | 0.7589 |
| 75 | 0 | 75.090 | 0.1756 |
| 75 | 1 | 75.013 | 0.5453 |
| 75 | 2 | 75.023 | 0.8533 |

## 3. Ambient reference used

`cplval75_coupling_verdict_2026-09-10.md` §2 states its ambient convention explicitly: "the
capture's first zone temperatures, 29.19 / 29.07 / 29.03" — i.e. the pre-flight thermocouple
reads at session start (`docs/audits/cplval75_settled_holds_2026-09-10.md`'s pre-flight table:
CH0 29.19°C, CH1 29.07°C, CH2 29.03°C), **measured at the thermocouples**, not backfilled from
an assumed round number. This is the per-zone ambient used in §4 below (labelled "no drift").

That said, the same pre-flight table documents a separate enclosure-temperature sidecar showing
**+2.0°C drift** over the ~2h38m run (29.8°C at 07:55:45 → 31.8°C at 10:33:28), and
`cplval75_coupling_verdict_2026-09-10.md` §2d already applied a linear drift allowance of
**+0.7/+1.2/+1.9°C at the 62/70/75°C plateaus respectively** against the OLD matrix, to bound
how much of the residual could be an uncorrected-ambient artifact rather than a matrix defect.
That allowance is itself an **assumption** (linear interpolation of the enclosure sidecar's
start/end reading onto the three plateau times, not a per-plateau thermocouple re-zero) — it is
carried forward unchanged into §4's drift-corrected row for consistency with the original study,
and labelled as such. No new drift assumption is introduced here.

## 4. Re-computed 62-75°C residuals against the FRESH matrix

`ΔT_obs` = plateau mean temp − ambient (per-zone, 29.19/29.07/29.03). `ΔT_pred` = `G_fresh · u`
using the plateau's observed duty vector. Residual = `ΔT_pred − ΔT_obs` (same sign convention as
`cplval75_coupling_verdict_2026-09-10.md` §2a: negative = plant produced more rise than the
matrix predicts = under-prediction).

**No ambient-drift correction (measured pre-flight ambient only):**

| plateau | z0 residual °C | z1 residual °C | z2 residual °C |
|---|---|---|---|
| 62°C | **−3.60** | **−3.45** | **−7.73** |
| 70°C | **−4.73** | **−3.36** | **−8.68** |
| 75°C | **−6.17** | **−3.84** | **−9.80** |

**Every entry is negative, on all three zones, at all three plateaus — the fresh matrix still
under-predicts.** Same sign as the old matrix's residuals (`cplval75_coupling_verdict_
2026-09-10.md` §2a: −3.36/−5.61/−8.37 at 62°C, growing at 70/75°C). The magnitude changed
somewhat per-row (z1's deficit shrank noticeably, z0's grew slightly, z2's stayed roughly flat)
but **the sign did not flip on a single one of the nine plateau×zone cells.**

Expressed as a relative deficit `(ΔT_obs − ΔT_pred) / ΔT_pred`, for comparison with the old
matrix's own reported per-row multiplicative correction (1.114/1.205/1.339 at 62°C, growing to
1.138/1.183/1.304 at 75°C, `cplval75_coupling_verdict_2026-09-10.md` §2a):

| plateau | z0 | z1 | z2 |
|---|---|---|---|
| 62°C | +12.3% | +11.7% | +30.5% |
| 70°C | +13.1% | +8.9% | +26.9% |
| 75°C | +15.5% | +9.1% | +27.1% |

Comparable in both sign and rough magnitude (9-31%) to the old matrix's 9-34% deficit. A
completely independently, correctly identified matrix reproduces essentially the same
under-prediction the old, refuted matrix showed.

**With the same linear ambient-drift allowance applied (+0.7/+1.2/+1.9°C at 62/70/75°C,
`cplval75_coupling_verdict_2026-09-10.md`'s own assumption, carried forward — labelled as an
assumption, not a new measurement):**

| plateau | z0 residual °C | z1 residual °C | z2 residual °C |
|---|---|---|---|
| 62°C | −2.90 | −2.75 | −7.03 |
| 70°C | −3.53 | −2.16 | −7.48 |
| 75°C | −4.27 | −1.94 | −7.90 |

Still every entry negative. The drift correction shrinks the magnitude (as it did for the old
matrix) but does not touch the sign on any cell.

## 5. Conclusion and uncertainty

**H2 is refuted by this test as stated.** The task's own falsification criterion (§3 of the
brief: "if the sign flips to over-prediction under the fresh matrix, H2 is supported... if it
stays under-predicting, H2 is refuted and H1 stands") is met cleanly: 9 of 9 plateau×zone cells
stay under-predicting, with and without the ambient-drift assumption. The 62-75°C under-
prediction is not an artifact of `G_old`'s mixed provenance — a matrix built entirely from one
coherent, same-session, column-by-column identification (`cond=5.036`, internally consistent,
already independently confirmed reproducible to 4-11% across two z0 runs) shows the same-signed
defect. **The sign reversal between the low-ΔT joint hold (over-prediction, ~33%,
`docs/audits/joint_vs_singlecolumn_matched_dT0_2026-09-11.md`) and the 62-75°C plateaus
(under-prediction, ~9-31% against this same fresh matrix) is real, not a comparison-matrix
artifact.** H1 (a genuine, separate high-ΔT mechanism, or at minimum some other unidentified
explanation) stands; this document does not itself identify what that mechanism is — that
remains open per `joint_load_model_class_design_2026-09-11.md`'s own unresolved §2/§3.

**Magnitude versus coefficient repeatability.** The stated week-over-week single-column
repeatability is ~2.9% (`c02`: 22.15 → 21.52, a 2.9% change). The measured 62-75°C deficits
under the fresh matrix are 9-31% — 3 to over 10 times the repeatability bound — and the
low-ΔT over-prediction is separately reported at ~33% with coefficient noise already bounded
out (±0.3-0.5°C, `947709a8`, per the design doc). **The sign reversal, and the persistence of
under-prediction under a completely different, independently-identified matrix, are both far
larger than anything coefficient-measurement noise (2.9%) could produce.** This is not a
close call decided by noise; it is one clean sign-preserving comparison across two
independently-built matrices.

**What this document does NOT establish**: it does not identify the mechanism behind the
high-ΔT under-prediction (H1's content, unresolved), it does not touch the low-ΔT
over-prediction result (taken as given from `947709a8`/`joint_vs_singlecolumn_matched_
dT0_2026-09-11.md`, not re-derived here), and it does not have access to the raw 5 s poll data
(`cplval75_20260910.jsonl`, gitignored/local-only) — only to the already-computed, committed
settled-hold-window summary. That summary is sufficient to answer the specific sign question
this task asks, but a finer-grained re-analysis (e.g. within-plateau drift, sample-by-sample
residual behavior) would need the raw capture, which this pass did not have and did not
reconstruct.

## 6. Numbers used, for reproducibility

```
G_fresh = [[42.731, 25.42, 21.52],
           [12.44,  32.397, 26.08],
           [8.08,   10.81,  33.849]]

Ambient (no drift):  [29.19, 29.07, 29.03]  (measured, pre-flight TC reads)
Ambient drift allowance (assumption, carried from cplval75_coupling_verdict_2026-09-10.md §2d):
  +0.7C @ 62C, +1.2C @ 70C, +1.9C @ 75C

u(62) = [0.1680, 0.3714, 0.5905]   observed mean temps [62.119, 62.036, 62.123]
u(70) = [0.1764, 0.4792, 0.7589]   observed mean temps [69.969, 69.938, 70.003]
u(75) = [0.1756, 0.5453, 0.8533]   observed mean temps [75.090, 75.013, 75.023]

G_fresh . u(62) = [29.329, 29.520, 25.360]
G_fresh . u(70) = [36.051, 37.512, 32.294]
G_fresh . u(75) = [39.728, 42.106, 36.198]
```

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
