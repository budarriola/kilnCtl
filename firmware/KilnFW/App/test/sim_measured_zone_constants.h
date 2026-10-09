// sim_measured_zone_constants.h -- ONE checked-in copy of the bench-measured
// G1 plant constants (model_k_dc/model_tau_s/model_dead_time_s) and the
// coupling cross-gain matrix, shared by every simulation harness that needs
// them (sim_credibility_gate.c, sim_iter_tune.c, sim_wide_temp_sweep.c).
//
// WHY THIS FILE EXISTS (docs/audits/cplval75_coupling_verdict_2026-09-10.md
// sec 4, D2/D3): all three harnesses used to carry their OWN copy of these
// four arrays, hand-transcribed from
// tools/PcTools/config_presets/tuned_baseline_20260831.json -- a PRESET
// FILE, never the live board. That preset's k_dc/tau_s/dead_time_s go stale
// the moment the board's own values move (autotune, hand edit, a fresh
// coupling capture) and nothing caught the drift: it silently invalidated
// docs/audits/dc_gain_factor_of_ten_2026-09-09.md's condition-number/solve
// headline (14.194 / [0.076,-0.118,2.085], preset-only artifacts; the live
// board's own G is 5.508 / [0.119,0.532,1.165]) and produced a "3 of 6
// cross-gains structurally infeasible" conclusion in
// docs/audits/sim_credibility_gate_coupling_fit_2026-09-10.md that flips to
// zero infeasible pairs once k_dc is corrected. Three independent literals
// meant three independent chances to go stale and nothing forcing them back
// into agreement with each other, let alone with the board.
//
// PROVENANCE of the numbers below: read from the live bench board's own
// GET /api/zones this session (2026-09-10), cross-checked against
// tools/PcTools/src/kilnctrl/coupled_ident.py's FF_K_DC_DIAGONAL (dated
// "read back 2026-09-02", identical to six decimal places) -- the board has
// carried these continuously since (tuning_valid=false / tuning_seq=0 on
// all three zones, i.e. no autotune has been accepted over that span). The
// coupling cross-gain matrix (off-diagonal only; diagonal is always 0, see
// zone_coupling_solve.c) is UNCHANGED from the three harnesses' prior
// literal -- it already matched the live `GET /api/zones` coupling_c* cells
// exactly and was never the stale part.
//
// THIS WILL GO STALE AGAIN the next time the board is autotuned or
// re-identified. There is no automatic re-sync from the board into this
// header (host tests build without network/board access) -- re-read
// `GET /api/zones` (or `control_get_zones` via the kilnctrl MCP facade) and
// hand-edit this file when that happens. What this file DOES fix is the
// three-way drift within the repo: there is now exactly one place to update,
// not three, so a future correction cannot land in two files and miss the
// third the way this one did.
//
// zone_coupling_solve.c's coupling_matrix_provenance_ok() is the reason
// coupling_coeff (off-diagonal, measured by column-step autotune runs) and
// k_dc (diagonal, from a separate single-zone FOPDT step) must never be
// silently mixed on the real board without a matching coupling_diag_k_dc --
// see that guard's own header comment. This simulation header carries both
// halves side by side for convenience; it is not itself subject to that
// guard because sim_kiln never round-trips through zones_config_get_coupling*().

#ifndef SIM_MEASURED_ZONE_CONSTANTS_H
#define SIM_MEASURED_ZONE_CONSTANTS_H

#define SIM_MEASURED_NZ 3

// model_k_dc, C/duty. Live GET /api/zones, 2026-09-10.
static const float g_k_dc[SIM_MEASURED_NZ] = { 39.2459f, 31.9669f, 31.6810f };

// model_tau_s, s. Live GET /api/zones, 2026-09-10 (average ~271s, not the
// stale preset's 166.9s -- this drives the 8*tau dwell-length floor used by
// docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md; see that doc's D5 note).
static const float g_tau_s[SIM_MEASURED_NZ] = { 263.8f, 269.8f, 270.9f };

// model_dead_time_s, s. Live GET /api/zones, 2026-09-10.
static const float g_dead_time_s[SIM_MEASURED_NZ] = { 52.8f, 43.5f, 33.9f };

// Coupling cross-gain, C per unit duty of the column (STEPPED) zone, as
// measured by column-step autotune runs. Diagonal is always 0 (self-gain is
// g_k_dc, not a coupling cell). Unchanged from the prior per-file literal --
// this half was never stale, and matches live GET /api/zones coupling_c*
// cells exactly.
static const float g_coupling_coeff[SIM_MEASURED_NZ][SIM_MEASURED_NZ] = {
    { 0.00f, 27.32f, 21.72f },
    { 14.30f, 0.00f, 22.15f },
    { 8.33f, 12.42f, 0.00f },
};

#endif // SIM_MEASURED_ZONE_CONSTANTS_H
