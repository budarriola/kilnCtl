# HTTP parser host-test campaign findings, 2026-10-09

Scope: host-only fuzz of `http_form.h` helpers, `zones_http_parse_zone_fields()` and
`zones_post_handler()`. New passing tests: `test_fuzz_inputs` in `test_http_form.c`,
`test_fuzz_zone_fields_hostile_values` / `test_fuzz_zones_post_toplevel_hostile` in
`test_zones_http.c`. Inputs covered: empty body, missing/duplicate keys, key-prefix
collisions, bad/truncated/non-hex percent escapes, `%00`, over-long values at the buffer
boundary, NaN/inf/overflow/hex/exponent numbers, out-of-range masks/modes/indices.
Result: zones POST and per-zone fields refuse all of them with 400 and no OK; no crash.

No mechanism exists to commit an expected-failing host test, so the failing probes are
recorded here, not committed.

Part 2 (same day) added `test_fuzz_hostile_backup_shapes` (`test_backup_import.c`),
`test_fuzz_hostile_bodies` (`test_aux_outputs_http.c`), `test_fuzz_profile_post_hostile`
(`test_profiles_http.c`) and `test_apply_hostile_bodies_never_reach_swap` (`test_kiln_cfg_http.c`).
Covered: every proper prefix of a valid backup, wrong JSON types, 5000-entry zone array, 100000-deep
nesting (refused, no crash), duplicate zone index, ct_mask range/type, aux set/manual hostile forms,
profile POST hostile fields, Content-Length longer than the received body / 0 / negative / 100 MB,
kiln_cfg apply id forms. A source scan of every `httpd_req_recv` loop (aux, kiln_cfg, profiles edit/live,
auth_totp forgot/reset) found each treats `ret <= 0` as failure and bounds `content_len`.

Part 3 (2026-10-09, same findings file, no new defect found) covered: `POST /api/auth/login`
(`test_web_auth_login_http.c`), live-edit `decide`/`accept` (`test_profiles_live_http.c`), profile
delete/favorite/builtin hide/builtin restore (`test_profiles_http.c`) and OPEN-tier `POST
/api/auth/forgot` and `/api/auth/reset` (new `test_auth_totp_http_fuzz.c`, own executable). Inputs:
every truncation, recv error/EOF at every offset, Content-Length 0/-1/over-cap/INT64_MAX/longer than
the body, bad and NUL escapes, duplicate and prefix-colliding keys, sign/whitespace/overflow numbers,
8 KB values, over-long names. Asserted per refusal: password record, TOTP state, reset-token store
and session table unchanged, setter never reached, nothing erased/hidden/favorited/decided.

Not covered: there is no `run_queue` body handler and no retarget body handler under
`drivers/http` (retarget is internal to `profiles_http.c`/`zone_aux_convert_http.c`, covered by the
existing `test_retarget_*`); `bootstrap_password` and `security_http.c` POST (their cores are tested,
but no recv-stub harness exists; `security_backend_web_auth.c` pulls the full backend and was not
built this round); `live edit` (`profile_live_edit` field editor) and `zones`-style per-field fuzz.
Design note, not a defect: `/api/auth/reset` consumes the one-time token before the backend's
strength check, so a weak `new_password` burns the token and the caller must redo `forgot`.

## F1 (Low) `http_form_url_decode` writes `out[0]` when `out_cap == 0` and `src_len == 0` [FIXED 2026-10-09 in 20e1263f]
- `firmware/KilnFW/App/drivers/common/http_form.h:32-60`. The `o + 1 >= out_cap` guard sits
  inside the loop, so an empty source skips it and the final `out[o] = '\0'` runs.
- Input: `http_form_url_decode("", 0, buf, 0)`. Observed: `buf[0]` overwritten (verified by probe).
  Expected: no write, return -1.
- Latent: every current caller passes a non-zero `sizeof`.

## F2 (Low) `http_form_parse_float` accepts C99 hex floats [FIXED 2026-10-09 in 20e1263f]
- `http_form.h` `http_form_parse_float`, via `strtof`. Input `0x1p3` returns 8.0 (verified).
  Expected: refused like `0x10` is for `http_form_parse_long`. Value is still finite and
  range-checked by callers, so no unsafe value results.

## F3 (Low) whole-page zones POST tolerates leading whitespace in numeric fields [FIXED 2026-10-09 in 20e1263f]
- `drivers/persist/zones_config_json.c:766-774` (`zones_config_json_parse_float_field`, plain
  `strtof`). Input `z0_kp=%201`, same for `z0_cal/ki/kd/ramp/maxtemp/mintemp/sanity`:
  accepted (verified, 8 probes). `http_form_parse_float` refuses the same value.
  Inconsistent strictness only; NaN, inf, overflow, `%00` and garbage are all refused.

## F4 (Medium) backup import accepts and applies truncated JSON
- `firmware/KilnFW/App/drivers/http/backup_import.c` (field scanners, no whole-document validation
  before pass 1). Probe: every proper prefix of a valid v2 body. Accepted (apply returns ok) at
  cut points 36-48 (`{"kind":"kilnctl_backup","version":2` ... through `"profiles":` , nothing to write),
  142-156 (cut inside the first profile segment: the profile is SAVED with the truncated value, e.g.
  `dwell_min` 3 instead of 30), and 200-202 (zones array/object closers missing; the zone IS applied).
- Expected: 400 and nothing written, as for prefixes 0-35, 49-141, 157-199.
- Reach: needs a client that sends a self-consistent truncated body (a half-saved backup file);
  a transport cut is already a 400 via Content-Length. Result is a silently wrong restore.
- Test coverage skips exactly those cut points (marked F4 in `test_fuzz_hostile_backup_shapes`).

## F5 (Low) backup import ignores a wrong-typed `profiles` / `zones` value
- Same file. Bodies `"profiles":{}` and `"zones":"x"` are accepted with ok and nothing written
  for that section; the operator sees a successful restore that restored nothing.
- Expected: 400 naming the key.

## F6 (Low) duplicate top-level keys: first occurrence wins
- `{"kind":"kilnctl_backup","kind":"x",...}` and `"version":2,"version":9999` are accepted (the
  first value is used). Ambiguous documents should be refused. No unsafe write results.

## F7 (Low) aux/kiln_cfg numeric fields accept a leading space ("relay=+1")
- `aux_outputs_http_core.c` `field_long` (`strtol` skips leading whitespace; `+` decodes to a
  space). `relay=+1&enabled=1` -> 200 for relay 1. `http_form_parse_long` refuses the same input.
  Same shape in `kiln_cfg_http.c` `parse_required_id`. Inconsistent strictness only.

## F8 (Low) invalid percent escapes in names are kept literally
- `http_form_url_decode` passes `%`, `%zz`, `%0` through as literal characters, so POST
  /api/profile `name=%zz` stores the name "%zz" (numeric fields refuse because they fail to parse).
  Expected by strict decoders: 400. Harmless for storage; recorded as a leniency.

## F9 (Low) `parse_required_id` saturates instead of rejecting overflow
- `kiln_cfg_http.c` `parse_required_id`: `strtol` returns LONG_MAX (INT32_MAX on both Xtensa and
  MSVC) for `id=99999999999`, `v > INT32_MAX` is then false, so the id aliases 2147483647 with no
  ERANGE check. The id then fails the existence lookup (404), so nothing is written; verified in the
  test with a stub where every id exists (apply reached the swap worker).
