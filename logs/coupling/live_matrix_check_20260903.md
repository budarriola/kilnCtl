# Live coupling matrix check — 2026-09-03

**Question:** which coupling matrix is actually live on the bench board, `coupling_matrix_20260831`
("new") or `coupling_matrix_pre20260902` ("old")?

**Method:** `GET /api/zones` against 192.168.1.156 directly (the kiln_call MCP layer does not surface
`coupling_c*` fields — `control_get_zones`/`get_board_state` omit them entirely, which is what made
the field-shape lookup fail earlier today). Board confirmed idle first: `profiles_get_exec_status`
state=0, `io_read` R1..R4=0. Re-confirmed idle after the check (no write was made).

**Schema, confirmed from both preset files' `_comment` blocks and matched against the raw HTTP
response:** persisted storage is per-zone scalar keys `coupling_c0`..`coupling_c{N-1}` (not a nested
list, not a `coupling_coeff` row on the wire) — `zones_http_handlers.c` emits these directly from the
firmware's `coupling_coeff[affected][stepped]` array, one row per zone, diagonal force-ranged to
`[0,0]` regardless of what is posted. The preset JSON's `coupling_coeff: [...]` list is that same row,
just expressed as a list instead of `c0/c1/c2` keys — `zones_http_client.py`'s
`_PRESET_ZONE_COUPLING_FIELD` mapping does the list-to-`c*`-keys expansion on write.

**Result — side by side, all three zones (`[affected][stepped]`, diagonal 0 in all three):**

| zone | live board | coupling_matrix_20260831 ("new") | coupling_matrix_pre20260902 ("old") |
|---|---|---|---|
| z0 | `[0, 27.32, 21.72]` | `[0, 27.32, 21.72]` | `[0, 12.0586, 6.0039]` |
| z1 | `[14.30, 0, 22.15]` | `[14.30, 0, 22.15]` | `[5.7656, 0, 6.7734]` |
| z2 | `[8.33, 12.42, 0]` | `[8.33, 12.42, 0]` | `[2.4062, 4.1094, 0]` |

The live board matches `coupling_matrix_20260831` exactly on all nine cells (three diagonal-zero,
six off-diagonal) and matches `coupling_matrix_pre20260902` on none.

**Conclusion:** the NEW matrix — the one today's re-analysed A/B (`dae34cc`,
`logs/coupling/ab_campaign_report.md`) favors on z0/z1 whole-run normalized IAE — is already live.
No preset was applied and no write was made to the board. `coupling_diag_k_dc = 0.0` and
`coupling_c{n}=0` on the diagonal are expected (force-ranged, matches `s_coupling_use_measured_diag_k_dc
= false`), not evidence of a stale or partial load. PID gains (`pid_kp`/`pid_ki`/`pid_kd` per zone) and
`fuzzy_strength_pct = 0.00` read back unchanged from the hand-restored values, confirming nothing else
was disturbed by this read-only check.
