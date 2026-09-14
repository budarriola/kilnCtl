# `autotune_baseline_k_dc` was write-only from the API's perspective (2026-09-13)

## Summary

Confirmed the reported gap: `GET /api/zones` (`firmware/KilnFW/App/drivers/http/zones_http_get.c`,
`zones_get_handler()`) never emitted `zone_cfg_t::autotune_baseline_k_dc`, even though:

- it is parsed and preserved on `POST /api/zones`
  (`firmware/KilnFW/App/drivers/http/zones_http_post_parse.c:466-478` — preserved-only, no
  `z%u_` wire key of its own; `adaptive_tune.c` is the field's only writer via
  `zones_config_set_autotune_baseline_k_dc()`), and
- it is persisted since `ZONES_CFG_VERSION` 26
  (`firmware/KilnFW/App/drivers/persist/zones_config_json.h:957`,
  `zones_config_accessors.h:185-212` and `:690-714`).

This field is the ratchet-prevention anchor added by
`97288659` ("Fix adaptive_tune K_dc ratchet: anchor plausibility to the original autotune
baseline, not the live adapted value") and protected against a whole-page zeroing bug by
`36f88d62` ("Adversarial review of 9728865: a whole-page zones save silently zeroed the new
adaptive_tune ratchet anchor"). Because it had no JSON key, neither of those fixes' actual
runtime behavior was observable from outside the firmware — an operator or tool reading
`/api/zones` had no way to see what the anchor currently held, or to notice a regression of the
`36f88d62` class recurring.

## Class check: every `zone_cfg_t` field that is POST-accepted or persisted, cross-checked against GET emission

Walked `zone_cfg_t` (`zones_config_json.h`) field-by-field against
`zones_http_post_parse.c`'s `parse_zone_fields()` and `zones_http_get.c`'s per-zone `APPEND`
calls.

- **`autotune_baseline_k_dc`** — POST-preserved (no wire key), persisted, **NOT emitted on
  GET**. Fixed by this pass (below).
- **`adaptive_tune_enabled`** — same shape as `autotune_baseline_k_dc` (POST-preserved only, no
  `z%u_` key, sole writer is `adaptive_tune.c` via
  `zones_config_set_adaptive_tune_enabled()`), and also **not emitted by `zones_http_get.c`**.
  This is NOT the same gap, though: it already has a dedicated observability path —
  `GET /api/adaptive_tune` (`firmware/KilnFW/App/drivers/http/adaptive_tune_http.c:225`,
  `status_get_handler`) reports `adaptive_tune_zones[zi].enabled`, which is read straight from
  `zones_config_get_adaptive_tune_enabled()` (`adaptive_tune.c:819`). Reported here for
  completeness, left alone: adding it to `/api/zones` too would be redundant with an endpoint
  that already exists, and duplicating one piece of state into two JSON documents is its own
  minor hazard (two readers, one source of truth, easy to let them drift). Judgment call —
  flagging as the one ambiguous case rather than silently skipping it.
- Every other field checked (`coil_power_w`, `model_fit_temp_c`/`model_fit_ambient_c`,
  `tuning_*`, `coupling_diag_k_dc`, `ease_off_window_mult`, `approach_rate_cap_c_per_hr`,
  `error_band_c`/`rate_band_c_per_s`, `progress_band_c`, `hyst_c`/`min_on_s`/`min_off_s`/
  `zone_type`/`failsafe_state`, `settings_source[]`, `relay_type`, `fuzzy_strength_pct`,
  `coupling_coeff[]`/`coupling_tau_s[]`/`coupling_dead_time_s[]`, and the ordinary
  operator-facing fields) — all already emitted. No other gap found.

No ambiguous cases beyond `adaptive_tune_enabled` above.

## Fix

Added one more always-emitted key to the per-zone object in `zones_get_handler()`
(`firmware/KilnFW/App/drivers/http/zones_http_get.c`), immediately after `model_fit_ambient_c`:

```c
APPEND("\"autotune_baseline_k_dc\":%.4f,", (double)z->autotune_baseline_k_dc);
```

Conventions matched to sibling fields, deliberately not inventing anything new:

- **Precision**: `%.4f`, same as `model_k_dc` — same unit (a plant gain), same small-gain-zone
  concern about fractional values losing precision at `%.2f`.
- **Sentinel handling**: `0` means "no baseline recorded yet"
  (`zones_config_accessors.h:185-212`). Per the house convention this file already uses for
  every other legal-0-sentinel field (`hyst_c`, `coil_power_w`, `ease_off_window_mult`,
  `approach_rate_cap_c_per_hr`, `error_band_c`/`rate_band_c_per_s`, `progress_band_c`) — emit
  the raw stored value verbatim and let the client interpret 0 as "not recorded", never resolve
  or substitute it server-side. Followed that convention exactly rather than inventing a new
  encoding (e.g. a separate `autotune_baseline_recorded` boolean) for this one field.
- **Always emitted**, unconditionally, for every zone regardless of `thermo_count` — same
  read-back-and-repost round-trip reasoning as `model_k_dc`/`tuning_*`/`model_fit_temp_c` above
  it: an absent key and a stored 0 must never mean different things to a client that reads this
  back and reposts it untouched.

## Buffer headroom

`zones_get_handler()`'s heap response buffer (`json_cap`, currently 7360 bytes) already has a
test that drives the real handler with every zone/profile/relay field pinned at its documented
MAX bound and measures the actual rendered size
(`test_zones_get_handler_max_width_response_fits_json_cap()`,
`firmware/KilnFW/App/test/test_zones_http.c`). Added
`z->autotune_baseline_k_dc = ZONE_AUTOTUNE_K_DC_MAX;` (`ZONE_AUTOTUNE_K_DC_MAX` is
`ZONE_MAX_TEMP_C_MAX` = 2500.0, `zones_config_accessors.h:212`) to that test's max-width fixture
and re-ran it against the real handler:

```
GET /api/zones max-width render: 7199 bytes, against json_cap=7360 -- measured headroom = 161 bytes
```

**161 bytes of headroom remain** after this addition (was measured at 895 bytes as of the
`progress_band_c` pass, `zones_http_get.c`'s own `json_cap` comment chain — the five field
additions since then, `model_fit_temp_c`/`model_fit_ambient_c`, `coil_power_w`,
`zone_type`/`failsafe_state`/`hyst_c`/`min_on_s`/`min_off_s`, and now `autotune_baseline_k_dc`,
consumed most of that margin without any of them individually bumping `json_cap`). This addition
fits without enlarging the buffer, but headroom is now tight — the NEXT per-zone field addition
should re-run this test before assuming it fits, not hand-estimate. Not enlarging the buffer in
this pass since it isn't yet necessary and the buffer is documented elsewhere as something that
must not be casually enlarged.

## Tests

Added `test_get_emits_autotune_baseline_k_dc()`
(`firmware/KilnFW/App/test/test_zones_http.c`, registered in the test list immediately after
`test_post_then_get_round_trips_new_fields()`), driving the real `zones_get_handler()` (and, for
the round-trip leg, the real `zones_post_handler()`/`parse_zone_fields()` via
`run_zones_post()`) directly:

1. A stored non-zero value (12.5) is reported back exactly by GET.
2. The sentinel case (0, "not recorded") is reported RAW, not substituted.
3. End-to-end: an ordinary whole-page POST carrying no `autotune_baseline_k_dc` key at all
   (there is none) still leaves a pre-existing stored value (33.75) intact, and GET after that
   POST reports it preserved — the actual externally-observable proof that `36f88d62`'s
   zeroing fix holds all the way through the real HTTP path, not just at the
   `parse_zone_fields()` unit level `test_post_omitting_new_fields_preserves_stored_values()`
   already covered.

Also updated `test_zones_get_handler_max_width_response_fits_json_cap()`'s fixture (see Buffer
headroom above).

### Negative test (production code broken by hand, then restored)

Removed the new `APPEND(...)` line from `zones_http_get.c` (replaced with a comment), leaving
everything else — including the test — untouched:

```
FAIL test_zones_http.c:4198: GET reports the stored autotune_baseline_k_dc exactly --
previously this key never appeared in the response at all
```

The new test failed as expected, proving it actually exercises the production code path rather
than being vacuous. Restored `zones_http_get.c` by hand (not via `git checkout`/`restore`) back
to the exact fixed version; confirmed byte-identical via `diff` against a saved copy of the
fixed file. Then removed the stale compiled objects for `zones_http_get.c`/`test_zones_http.c`
and forced a full rebuild (per this session's standing instruction: an empty `git diff` proves
source is restored but says nothing about build artifacts) before re-measuring. Post-restore,
post-rebuild run: **1840/1840 checks passed** in the `zones` host-test binary, including the new
test and the un-touched `test_zones_get_handler_max_width_response_fits_json_cap()` reporting
161 bytes headroom as above.

## Follow-up (not done here, out of scope)

The PC-side config renderer (owned by another agent this session) currently reports
`autotune_baseline_k_dc` as "NOT exposed". With this fix landed, it can now read the field from
`GET /api/zones` like every other always-emitted zone field. Updating that renderer is
explicitly left to its owning agent/session — not touched here.

**Immediate consequence observed**: `tools/run_all_checks.ps1` now fails one PC-side check --
`tools/PcTools`'s zones-field-table cross-check
(`tools/PcTools/src/kilnctrl/zones_http_client.py`'s `_ZONE_FIELD_FORM_KEY`, enforced by
`ZonesHttpUnknownFieldError`) compares its own per-zone field table against what the real
firmware GET handler emits, and now reports:

```
FAIL zones per-zone GET keys: client dicts match firmware (missing from client -- would raise
ZonesHttpUnknownFieldError on every zones save: ['autotune_baseline_k_dc'], extra in client: [])
```

This is the expected, intended effect of closing the gap -- the firmware now emits a key the
PC-side client's field table does not yet know about. Per this task's explicit instruction not
to edit `zones_http_client.py` (or `mcp_server_control.py`, also concurrently owned), this is
left for that file's owning agent to pick up: add `autotune_baseline_k_dc` to
`_ZONE_FIELD_FORM_KEY` (as a GET-only / preserved-on-POST field, matching `adaptive_tune_enabled`
and `tuning_*`'s existing treatment there) so `run_all_checks.ps1` goes green again and
`mcp_server_control.py`'s "NOT exposed" note can be updated to reflect this fix.

## Check tally

- `firmware/KilnFW/App/test/test_zones_http.c` (host test binary `kilnctl_host_tests_zones`):
  **1840/1840 checks passed**, including the new test.
- Negative test: confirmed the new assertion fails when the production emission is removed;
  restored by hand, rebuilt from clean objects, re-confirmed green.
- `tools/check_test_c_files_wired.ps1` / `tools/check_no_orphaned_checks.ps1`: the new test
  function is registered in the existing test-list block inside `test_zones_http.c`'s `main()`
  wiring, the same mechanism every other test in this file already uses — no new file, no new
  wiring pattern introduced. Both PASS.
- `tools/check_doc_hash_citations.ps1`: PASS (this doc's cited commit hashes resolve).
- KilnFW target build (`idf.py -C firmware/KilnFW build`, PowerShell): succeeds,
  `KilnCtrl.bin` 0x227790 bytes, 28% partition free.
- `tools/run_all_checks.ps1`: the PC-side field-table check block ended with
  `FAILED (1): zones per-zone GET keys ...` and a summary line of
  `91 passed, 0 skipped, 3 failed` for that section, the 1 named failure being the
  `autotune_baseline_k_dc` field-table gap addressed in Follow-up above — expected and out of
  scope per this task's instructions (the fix belongs in the PC-side `zones_http_client.py`,
  owned by another agent). Did not attribute the section's other 2 counted failures to this
  change without verifying them independently; they were not investigated as part of this
  task since they are outside `zones_http_get.c`'s scope.
