# cfg_convert test fixtures

**SYNTHESIZED, not captured from real hardware.** No real `GET
/api/backup/export` capture exists anywhere in this tree as of 2026-09-16 (a
tree-wide search for `*backup*.json` / `kilnctl_backup` found nothing). These
fixtures were written by hand to match the field shapes documented in
`backup_export.c`/`backup_import.c`, which is close to the vacuous-test shape
CLAUDE.md warns about ("a step tested only against a blob written by the same
person who wrote the step") — flagged here rather than left implicit.

Per the owner's standing release-step requirement (capture a real config
blob before each version bump), a real capture belongs in this same
directory once one exists — e.g. `v4_real_<board-id>_<date>.json` — with the
board identity and date in the filename so a fixture's provenance is visible
without opening it. **Never commit a real capture containing Wi-Fi
credentials** (the real export format never includes them, so a genuine
capture is already safe on this point) or anything under the `kiln_auth`
namespace.

Files here:
- `v1_synthesized.json` -- BACKUP_FORMAT_VERSION 1 shape: bare pid/model/tc_type
  zone entries only, two zones, one profile.
- `v3_synthesized.json` -- BACKUP_FORMAT_VERSION 3 shape: adds the v2 block
  plus fuzzy_strength_pct and a single-neighbor coupling_coeff/
  coupling_neighbor_zone pair.
- `v4_synthesized.json` -- BACKUP_FORMAT_VERSION 4 shape: full coupling_c<N>
  row, coupling_tau_c<N>/coupling_dead_time_c<N>, settings_source_g<N>, and
  one zone carrying a calibrated `normal_current_a` (the field this tool
  must never fabricate on any other zone).
