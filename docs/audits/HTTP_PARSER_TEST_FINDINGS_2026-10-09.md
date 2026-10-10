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

Not covered (time): profiles HTTP, backup import, kiln_cfg, aux outputs, auth/TOTP forms.

## F1 (Low) `http_form_url_decode` writes `out[0]` when `out_cap == 0` and `src_len == 0`
- `firmware/KilnFW/App/drivers/common/http_form.h:32-60`. The `o + 1 >= out_cap` guard sits
  inside the loop, so an empty source skips it and the final `out[o] = '\0'` runs.
- Input: `http_form_url_decode("", 0, buf, 0)`. Observed: `buf[0]` overwritten (verified by probe).
  Expected: no write, return -1.
- Latent: every current caller passes a non-zero `sizeof`.

## F2 (Low) `http_form_parse_float` accepts C99 hex floats
- `http_form.h` `http_form_parse_float`, via `strtof`. Input `0x1p3` returns 8.0 (verified).
  Expected: refused like `0x10` is for `http_form_parse_long`. Value is still finite and
  range-checked by callers, so no unsafe value results.

## F3 (Low) whole-page zones POST tolerates leading whitespace in numeric fields
- `drivers/persist/zones_config_json.c:766-774` (`zones_config_json_parse_float_field`, plain
  `strtof`). Input `z0_kp=%201`, same for `z0_cal/ki/kd/ramp/maxtemp/mintemp/sanity`:
  accepted (verified, 8 probes). `http_form_parse_float` refuses the same value.
  Inconsistent strictness only; NaN, inf, overflow, `%00` and garbage are all refused.
