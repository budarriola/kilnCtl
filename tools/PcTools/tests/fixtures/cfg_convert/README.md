# cfg_convert test fixtures

Most fixtures below are **SYNTHESIZED, not captured from real hardware** --
written by hand to match the field shapes documented in
`backup_export.c`/`backup_import.c`, which is close to the vacuous-test shape
CLAUDE.md warns about ("a step tested only against a blob written by the same
person who wrote the step") — flagged here rather than left implicit.

A **real capture** now exists: `v4_bench1_2026-09-16.json`, pulled from the
bench board's own `GET /api/backup/export` (protocol_version 12, firmware
commit d459d124) on 2026-09-16, satisfying the owner's standing release-step
requirement to capture a real config blob before each version bump. It was
read byte-for-byte before being committed and confirmed to carry no Wi-Fi
credentials, no `kiln_auth` key, and (bench has no CT fitted) no calibration
key at all -- the real export format never includes credentials by design,
but that was verified in these actual bytes rather than trusted on design
alone. Its field set matched what the synthesized v4 fixture already
assumed; the one notable difference is that the real capture's zones each
carry TWO nonzero coupling neighbors (a genuine measured 3-zone matrix)
rather than the synthesized fixture's at-most-two-cell hand-picked numbers,
which `test_real_capture_downgrade_to_v3_collapses_multi_neighbor_coupling`
in `tests/test_cfg_convert.py` now exercises directly. Per the same standing
requirement, a fresher real capture should join or replace this one at the
next version bump -- name it `v4_<board-id>_<date>.json` the same way, with
the board identity and date in the filename so provenance is visible without
opening it. **Never commit a real capture containing Wi-Fi credentials** or
anything under the `kiln_auth` namespace -- verify the actual bytes, not
just the design intent, every time.

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
- `v4_bench1_2026-09-16.json` -- a REAL capture from the bench board (see
  above): BACKUP_FORMAT_VERSION 4, 3 zones, 8 profiles, full coupling matrix
  per zone, no calibration keys present (bench has no CT fitted).
