# HTTP input-parsing audit, 2026-10-09

Scope: every HTTP handler under `firmware/KilnFW/App/drivers/**` and in the recovery
image (`firmware/KilnFW_recovery/main/`) that reads a request body (`httpd_req_recv`,
`req->content_len`) or a query string (`httpd_req_get_url_query_str`,
`httpd_query_key_value`, `http_form_find_field`). The audit started at `origin/dev`
`48e1ba8a`. Every M finding was then re-checked against `origin/dev` `8ddb7a88`.
L line numbers are from the audit pass and are marked `~` where they may have drifted.

Checked for each handler: content_len cap against buffer size; the `httpd_req_recv`
loop (short reads, timeouts, 0 returns); NUL termination; form and JSON bounds
(integer overflow, negative values, NaN/inf from `strtof`, out-of-range zone, relay
or slot indexes, truncation reported as success); stack against the 8192 B httpd
stack (`wifi_provision_http.c:1224`, `recovery_http.c:1083`); error paths that leak a
buffer or a lock; and partial writes to RAM or a store before validation finishes.

Stack rule: `docs/agent_rules/IMPLEMENTER.md:68-69` (no large locals on the 8 KB httpd
stack). This audit flags more than about 1 KB of locals in one frame, or more than
2 KB across a call chain. **Doc gap:** `CLAUDE.md:191` points at an "httpd stack blob
class" note "below", but CLAUDE.md no longer contains that note. Either restore it or
point the reference at IMPLEMENTER.md.

No defect was fixed in this pass. This document is the report only.

Status values:
- **fix in progress**: a fix agent was working on it when this doc was written.
- **open**: no fix assigned yet. The finding carries a recommendation.
- **fixed-by &lt;sha&gt;**: verified fixed on the tip.

## Summary

| Severity | Count | Fixed | Fix in progress | Open |
|---|---|---|---|---|
| H | 0 | 0 | 0 | 0 |
| M | 7 | 0 | 6 | 1 |
| L | 44 | 2 | 31 | 11 |

No H finding. None of the input paths has an out-of-bounds write, an unterminated
buffer, or a leaked lock. Every body reader caps `content_len` below its buffer,
fails on `recv <= 0` and NUL-terminates. Two problem classes recur:

- **Parsing.** `strtol`/`strtof` called with a NULL end pointer. An empty or
  over-long (`-2`) `http_form_find_field` result treated as "absent". An embedded
  `%00` accepted mid-value.
- **Validation order.** A few validation passes are weaker than the setter applied
  later, so a write fails half-way through, after earlier sections were already
  committed.

## M findings

| ID | Status | Location | Finding | Malformed request | Test coverage |
|---|---|---|---|---|---|
| M1 | fixed-by 02ddc944 | `http/backup_import.c:1346-1355` | Pass 1b checks zone PID gains only for `>= 0` before the `(float)` cast. `ZONE_PID_GAIN_MAX` is never checked. The pass-2 setter `zones_config_set_pid_no_save` (`persist/zones_config_accessors.c:549-566`) refuses non-finite or over-max gains, but by then kiln_configs and aux phase 1 have already committed. The result is a 500 after a partial import. | `POST /api/backup/import` with a zone entry `"pid_kp":1e300` (becomes inf as a float) or `"pid_kp":1e6` | Not covered. `test_backup_import.c` stubs the setters (~1919-1928). |
| M2 | fixed-by 02ddc944 | `http/backup_import.c:1967-1985` | `tuning_method` and `tuning_rule` are cast straight to `uint8_t` with no range check (256 wraps to 0; out-of-range values are UB). The setter (`zones_config_accessors.c` ~2305-2320) needs method <= 1 and rule <= 3. `tuning_baseline_c`, `tuning_step_ambient_c`, `tuning_raw_rise_c`, `tuning_rise_inf_c`, `model_fit_temp_c` and `model_fit_ambient_c` are unbounded, so a huge value becomes inf. The setter refuses at commit time, which is a partial write. | Import with `"tuning_valid":1,"tuning_method":7` or `"tuning_baseline_c":1e300` | Not covered. |
| M3 | fix in progress (E2 #1) | `http/security_backend_web_auth.c:463-468` | `POST /api/auth/bootstrap_password` (open while no admin exists) makes one `httpd_req_recv` call with no loop to `content_len`. A short read stores a truncated password and returns 200. | The client sends `username=admin&password=CorrectHorseBattery` in two TCP segments, split after `CorrectHor`. The stored password is `CorrectHor`. | Not covered. |
| M4 | fix in progress (E2 #2) | `http/dashboard_autotune_http.c:296-298` | `zone` is parsed with `strtol(zone_val, NULL, 10)`, so a non-numeric value becomes 0 and passes the range check. `step_duty` uses `strtof` with no end check. | `POST /api/autotune/start` with `zone=abc&step_duty=0.5` runs a step test on zone 0. `zone=1x` runs it on zone 1. `step_duty=0.5abc` is accepted. | Not covered. |
| M5 | fix in progress (D1) | `http/dashboard_exec_http.c:770` | Profile start parses `id` with `strtol(id_val, NULL, 10)`, no end check. | `POST /api/profile_exec/start` with `id=abc` starts profile slot 0. `id=1abc` starts slot 1. | Not covered. |
| M6 | fix in progress (E1 #1) | `http/zones_http_post.c:201-203` (gate), `:692` (commit) | The system-mode gate and the OTA interlock run only on entry. The slow `safety_ceiling_sync_guard_raise` (a Pico write plus confirm) runs after the gate. The commit re-checks only `s_config_generation`, not whether a firing or autotune has started. A profile started during the Pico-confirm window can have relay_mask, guards or gains swapped underneath it. | `POST /api/zones` raising a zone max, with `POST /api/profile_exec/start` sent while the ceiling raise is waiting for Pico confirmation | `test_zones_http.c` (~1786) covers a generation bump, not a firing start. |
| M7 | fixed in 647e7d9d | `http/diagnostics_http.c:1847-1915` (`cfgfs_file_post_handler`) | `POST /api/cfgfs/file?name=<file>` writes the raw body (up to `CFGFS_FILE_BODY_MAX`) to the cfg partition without checking the content. Junk persists, and the loaders fall back to defaults or NVS at boot. The route is ADMIN-tier and is a deliberate restore primitive. | `POST /api/cfgfs/file?name=zones.json` with body `garbage` | `test_cfg_fs.c` covers the write path, not the handler. |

**M7 recommendation:** before the write, run the same parse/validate step the
loader uses for known file names (`zones.json`, profiles, prefs), and refuse with 400
on failure. Otherwise, make the route opt-in with an explicit `raw=1` field.

## L findings

| ID | Status | Location | Finding | Malformed request | Test coverage |
|---|---|---|---|---|---|
| L1 | fixed-by cdbc6fe5 | `http/backup_import.c` ~4306 (near `:4292`) | The `X-Kiln-Config-Dry-Run` header is read into `char[8]`. A value of 8 or more characters comes back as TRUNC, so `dry_run` is false and the **real** import runs. This fails open. | Header `X-Kiln-Config-Dry-Run: 1       ` (padded) | No |
| L2 | fixed-by cdbc6fe5 | `http/backup_import.c` ~4326, ~3149 | The `Ack-Delete` value goes through `strtol` with trailing garbage allowed. A truncated `mode_val[8]` falls back to merge. `backup_prefs_bool` uses `strncmp`, so `"trueXYZ"` reads as true. `backup_json_field_str` truncates silently. | Header `X-Kiln-Config-Ack-Delete: 2junk` | No |
| L3 | fixed-by a517d292 | `persist/zones_config_json.c` ~743-780 via `http/zones_http_pid.c` ~136-162 | An embedded `%00` is accepted. Harmless, because the value is range-checked and `set_pid` revalidates. `zones_http_pid.c` ~140 reads `thermo_count` without the lock. | `zone=1%002&kp=1` | No |
| L4 | fixed-by 595bd701 | `sim/sim_backend.c` ~285 (SIM_PLANT builds only) | `sscanf("%d,%d")` ignores trailing text. A `-2` from `find_field` is treated as absent. | `POST` sim with `pair=1,2junk` | No |
| L5 | open (skipped 2026-10-09: file/area owned by another in-flight change) | `http/backup_import.c` apply frame | The apply frame is about 4.6 KB, plus about 2 KB of kiln_configs locals, both on the 8192 B `http_async_job` task. The combined depth has never been measured. | Any large `POST /api/backup/import` | Stack-margin registration exists. Depth not measured. |
| L6 | fixed-by d85e9e98 | `http/diagnostics_http.c` ~1912 | The cfgfs file `name` is `snprintf`'d into the JSON reply without escaping. | `POST /api/cfgfs/file?name=a%22b` (if the name filter allows it) | `test_cfgfs_file_validate_scan.py` (source scan) |
| L7 | fixed-by 595bd701 | `http/diagnostics_http.c:901` (`relay_cycles_restore`), `:540` (`coredump_chunk`) | `strtoul` accepts `-1` (it wraps) and an empty string. Downstream checks refuse: `relay_cycles_restore_all`, and the 64-bit range check at `hal_sysinfo_esp.c:253`. | `c0=-1`; `offset=&len=` | `test_relay_cycles.c` (downstream) |
| L8 | fix in progress (B L-4) | `http/diagnostics_http.c` watchdog_cfg, ramp_assist, danger_enable; danger_relay | These check only `val[0]=='1'`, so `10` or `1xyz` reads as on. `danger_relay` uses `strtol` with no end pointer. | `enable=1xyz`; `relay=1abc` | Partial: `test_watchdog_cfg.c`, `test_ramp_assist_cfg.c`. None for danger_*. |
| L9 | fix in progress (B L-5) | `http_form_find_field` (shared) | Decodes `%00` into an embedded NUL. `strtol`/`strtof` then see a truncated string. | `ch=1%00junk` reads as 1 | No |
| L10 | fix in progress (B L-6) | `http/safety_cfg_http.c` ~2303-2310 (`rate_guard_auto_post_locked`) | A body of 32 B or more, or an unreadable body, is silently treated as `confirm=false` and left unread. It should be a 400. The unread body can confuse keep-alive. | `POST` rate_guard auto with a 40-byte body | No |
| L11 | fixed-by 697d1027 | `http/diagnostics_http.c` json_escape | Control characters were not escaped. Now uses the shared `kiln_json_escape_ctl`. | n/a | n/a |
| L12 | FIXED in 3ff45386 (`unit_pref_set` persists first, publishes RAM only on success; handler comment updated) | `http/dashboard_settings_http.c` (`unit_pref_post_handler`) | RAM is updated before the persist. On a persist error the handler returns an error, but the live value has already changed (documented in a comment). | `POST /api/unit_pref` while the store is failing | No |
| L13 | fix in progress (B stack) | `http/diagnostics_http.c` (`crash_report_get_handler`) | About 1.3-1.5 KB of locals in one frame. | `GET /api/crash_report` | n/a |
| L14 | fixed-by 697d1027 | `http/wifi_provision_http.c:226-228` | `json_escape` escaped only `"` and `\`, so control characters went raw into /status, /scan and /networks JSON. Now delegates to `kiln_json_escape_ctl`. | n/a | n/a |
| L15 | fixed-by 595bd701 | `http/wifi_provision_http.c` provision/forget; `wifi_prov_api.c` ~131-145, ~205-216 | A `%00` in the SSID or password becomes an embedded NUL, and the stored value is truncated. Bounded. | `POST /provision` with `ssid=home%00x&password=...` | No |
| L16 | fix in progress (C3) | `http/wifi_provision_http.c` scan_get (~418), status_get (~370) | `json[20*64+16]`, about 1.3 KB, and about 1.2 KB of locals respectively. | `GET /scan` | n/a |
| L17 | fix in progress (C4) | `update/update_fetch.c` ~1043-1085 (status_get) | About 1.64 KB of stack locals. | `GET /api/update/fetch` | n/a |
| L18 | fix in progress (C5) | `update/update_http.c` ~385 vs `:415` | A recv failure returns `ESP_FAIL` before `update_stage_set_gate(&s_stage, NULL, NULL)`, which leaves `gate_ctx` pointing at a dead stack frame. Latent, because the next begin resets it (`update_stage.c` ~169-170). | Drop the connection mid-body of `POST /api/update/stage` | No |
| L19 | fix in progress (C6) | `KilnFW_recovery/main/recovery_upload.c:33-50` (`read_exact`); `recovery_http.c` ~838 | A trickling client resets the timeout counter. There is no total-duration cap, so one client can hold the single httpd task indefinitely. | Send 1 byte every 4 s to the recovery upload | No |
| L20 | fix in progress (D2) | `http/profiles_edit_http.c:288-291` | `rule%u_temp_c` goes through `strtof` with no `isfinite` check or end check. `profiles_validate.c` ~265 range-checks it only when `temp_cmp != NONE`, so NaN or inf can persist. | `rule0_temp_cmp=0&rule0_temp_c=nan` | Not for NaN |
| L21 | fix in progress (D3) | `http/profiles_edit_http.c` profile_post | `name=%00` gives an embedded NUL, so the stored name is empty. | `name=%00abc` | No |
| L22 | fix in progress (D4) | `http/profiles_edit_http.c` profile_post | An over-long `id` (`-2`) is treated as "create new". `id=abc` parses as 0 and overwrites slot 0. `1abc` is accepted. | `POST /api/profiles` with `id=abc&name=x&...` | No |
| L23 | open (skipped 2026-10-09: file/area owned by another in-flight change) | `http/profiles_edit_http.c` profile_delete_post_handler | The "is it running" check is unlocked, so a TOCTOU window remains against a profile start. Re-check the ordering against the dev reorder. | Delete slot N while `POST /api/profile_exec/start id=N` races it | No |
| L24 | fix in progress (D6) | `http/profiles_edit_http.c` profile_favorite_post_handler | An empty slot can be favorited. | `POST /api/profiles/favorite` with `id=<empty slot>` | No |
| L25 | fix in progress (D7) | `http/ota_http_esp.c` (`ota_esp_do_transfer`), `http/ota_http_pico.c` (`ota_pico_do_stage`) | Each recv has a 30 s timeout, but there is no overall deadline. A slow drip holds the update claim and the httpd task. | OTA upload that sends 1 KB every 25 s | No |
| L26 | fixed-by 595bd701 | `http/ota_http_pico.c`, `net/pico_img_stage.c` | Begin erases `pico_img` before the body is validated, which destroys the previously staged image. The manifest is not cleared on a failed upload: `pico_image_manifest_clear` (`persist/pico_image_manifest.c:176`) has no HTTP caller. Mitigated by the CRC recheck in `pico_image_source.c`. | A truncated `POST /api/ota/pico` | No |
| L27 | fix in progress (D9) | `http/kiln_cfg_http.c` list_get_handler ~149 | About 1.4 KB of stack (`json[1056]` plus `rows[280]`). | `GET /api/kiln_configs` | n/a |
| L28 | fix in progress (E1 #2) | `http/zones_http_post.c` ~287-330, ~486, ~524; `zones_http_post_parse.c` (90, 148, 207, 292, 308, 316, 354, 370, 387, 672, 686, ...) | The optional-field probes use `find_field() > 0`. An empty (0) or over-long (`-2`) value is silently treated as omitted and the request returns 200. | `max_simultaneous_relays=&z0_zonetype=1111111111111111111111` | No |
| L29 | fix in progress (E1 #3) | `http/zones_http_post.c` ~245 | `%00` is accepted. | `z0_kp=1%00junk` reads as 1 | No |
| L30 | fix in progress (E1 #4) | `http/web_auth_login_http.c:333`, `:411` | `LOGIN_BODY_MAX` is 256, but a password may be 128 characters and percent-encodes to up to 3x. A legitimate password heavy in symbols (about 80 or more) gets a 400 before verification. | Login with an 85-character `!@#...` password | No max-length test |
| L31 | fix in progress (E1 #5) | `http/web_auth_login_http.c` ~423-469 | `body[256]` holds the plaintext password and is never wiped; only `password[]` is cleared. | Any login | No |
| L32 | fix in progress (E1 #6) | `http/web_auth_login_http.c` ~468-487 | If taking `s_login_lock` (50 ms) fails, the failed-attempt record is skipped. Lockout fails open under contention. | Parallel bad-password logins | No |
| L33 | fix in progress (E1 #7) | `http/profiles_catalog_http.c` ~369-382, ~243 | `query[32]`/`id_str[8]`: a long query (for example a cache-buster) truncates, giving a 400 "id missing". `strtol` has no end check. The builtin list ignores truncation of `query[48]`. | `GET ...?id=3&_=1696871234567890` | No |
| L34 | fix in progress (E1 #8) | `http/http_auth_http.c:526` | `s_route_count++` happens before registration succeeds, so a failed registration consumes a slot (startup only). `ctx->uri` is truncated at 79 characters. | n/a (startup) | No |
| L35 | fixed-by 595bd701 | `http/http_origin_check.h` ~136 | An empty `Origin:` header is treated as absent, and then a missing Referer is allowed. Theoretical. | State-changing POST with `Origin:` (empty) and no Referer | `test_http_auth_enforce.c` does not cover the empty value |
| L36 | fix in progress (E1 #10) | `http/zones_http_post.c` ~253, `zones_http_post_parse.c` ~1013 | The combined stack in `POST /api/zones` is about 2.5-3 KB: a temporary `zones_cfg_t` (about 1.1 KB), a relay_names temporary, and a probe `zone_cfg_t[3]` (about 1 KB). That exceeds the 2 KB combined-depth threshold. | Any `POST /api/zones` | n/a |
| L37 | TOCTOU fixed-by ffcea431 (reset-in-flight mark + late mode-gate re-check; OTA interlock not re-checked late); no confirm field, unchanged | `http/factory_reset.c` ~545-590 | There is no confirm field: `scope=all` from any admin session wipes and reboots. The gate and interlock run before the erase, but `execute_scope` does not re-check them (TOCTOU against a firing start). | `POST /api/factory_reset` with `scope=all` | No |
| L38 | fix in progress (E2 #3) | `http/dashboard_autotune_http.c` ~293-340; `security_http.c` ~411-418; `setup_progress_http.c` ~196-198 | An over-long field (`-2`) is treated as absent. In autotune, `step_duty` falls back to 0.5, relay_d/h to 0, and `method` to step test despite the comment saying a typo is refused. In `set_policy`, a missing or over-long `web_enabled` reads as 0, which turns login off (the server's transition checks still apply). In setup progress, an over-long note can clear the existing note. | `method=relay_feedbackXXXXXXXX`; `web_enabled=` | No |
| L39 | fix in progress (E2 #4) | `http/adaptive_tune_http.c:160,231`; `iter_tune_http.c:123`; `settings_http.c:206,212` | `atoi`/`strtol` with no end check. `zone=abc` enables or reverts zone 0. `restore_commissioned?zone=abc` writes PID to zone 0. `brightness=abc` sets brightness 0 (dark), and `timeout=abc` sets timeout 0. | `POST /api/display_power` with `brightness=abc` | Gate test only |
| L40 | fix in progress (E2 #5) | `http/profiles_export_http.c` ~356-394 | The import on_off_rules loop uses `if (opt_num(...) && has_v)`, which silently coerces an out-of-range value to 0 instead of refusing it, contrary to the code comment. | Import a rule with `"temp_cmp":999` | No |
| L41 | fix in progress (E2 #6) | `http/profiles_export_http.c` ~252-257 | RELAY_IO segment `target_c`/`ramp_c_per_hr` are unbounded, so 1e300 becomes inf when cast to float. The export then prints `inf`, which is invalid JSON. | Import a segment with `"target_c":1e300` | No |
| L42 | fix in progress (E2 #7) | `http_form_find_field` callers: set_web_password, bootstrap, reset, TOTP code | A password of `ab%00cd` behaves as `ab`. `backup_json_field_str` does not decode `\uXXXX`. An empty value returns -1, the same as absent. | `password=ab%00cd` | No |
| L43 | fixed-by 595bd701 | `http/setup_progress_http.c` ~103-123 | Returns `ESP_FAIL` after `send_err`, which closes the socket. Benign. | Malformed setup_progress POST | No |
| L44 | fixed-by 595bd701 | `http/auth_totp_http.c` forgot/reset | The 400 paths for a malformed field do not call `totp_backoff_record`, so malformed attempts are not rate-limited. | `POST /api/auth/reset` with `code=` (empty) | No |

### Recommendations for the open L items

- **L4:** use `%d,%d%n` and require the end of the string.
- **L5:** take a stack-margin reading of `http_async_job` after a maximal import, and move the kiln_configs locals to the heap if headroom is under 1 KB.
- **L6:** escape `name` with `kiln_json_escape_ctl`.
- **L7:** refuse a leading `-` and an empty value before `strtoul`.
- **L12:** persist first, then publish to RAM.
- **L15:** refuse `strlen(v) != len`, as `update_settings_http.c` ~97 already does.
- **L23:** take the executor lock across the running check and the erase.
- **L26:** erase on the first validated chunk, and call `pico_image_manifest_clear` on a failed upload.
- **L35:** treat a present-but-empty Origin as a refusal.
- **L43:** return `ESP_OK` after `send_err`.
- **L44:** record backoff on every refusal.

## Handlers checked and found clean

Clean except for the findings listed above.

- **aux_outputs_http.c**: `read_body` (`:104-118`), aux get/set/manual. Tested by `test_aux_outputs_http.c`.
- **safety_cfg_http.c**: `read_body`, commissioning get/post, relay_type, ct_cal, ct_trim, ct_auto_zero, bench_preset, rate_guard_auto_get. Tested by `test_safety_cfg_http.c`, `test_readiness_commissioning.c`.
- **dashboard_settings_http.c**: safety_log_level.
- **cfg_fs_format_http.c**: format_pending, format_confirm. Tested by `test_cfg_fs_format_gate.c`.
- **diagnostics_http.c**: all remaining handlers. `cfgfs_status_get_handler` was only skimmed.
- **update/update_settings_http.c**: settings_post (mode gate after drain).
- **zones_http_pid.c** and **sim/sim_backend.c**: get/post, apart from L3 and L4.
- **backup_import.c**: `backup_import_post_handler` and the job body read. The pass-1 bounds are clean for every section except M1, M2, L1 and L2.
- **backup_http_internal.h**.
- **wifi_provision_http.c**: status, networks, forget, ip_config, provision.
- **wifi_prov_api.c**: add, forget.
- **dashboard_status_http.c**: `/api/status`.
- **update_http.c**: stage_upload (apart from L18), clear, status.
- **update_stage**: begin, write, finish, abort.
- **update_fetch.c**: check, download, cancel, `send_error_json`.
- **Recovery image**:
  - `recovery_http.c`: `parse_pico_query`, pico_upload, pico_status, pico_abort, apply_staged, apply_status, ota_esp, sw_reset (8192 B stack, 10 of 16 route slots, static assert).
  - `recovery_upload_stream`.
  - `recovery_image_check.c`: `ric_validate_first_chunk`, `ric_boot_guard_decode`.
  - Tests: `test_recovery_image_check.c`, `test_recovery_upload.c`.
- **dashboard_exec_http.c**: plan, history, stop, pause, resume, ack, clear_trip, and the start body read.
- **profiles_edit_http.c**: the post recv loop, `read_small_body`, builtin_hide/restore, segment `target_c`/`ramp` (end pointer checked, `:172`, `:184`). Tested by `test_profiles_http.c`.
- **OTA**:
  - `ota_http.c`: refusal_drain.
  - `ota_http_util.c`/`.h`.
  - `ota_http_esp.c`: post and transfer, apart from L25.
  - `ota_http_pico.c`: post and stage, apart from L25 and L26.
  - `pico_img_stage.c`: begin, write, finish.
  - Test: `test_ota_http.c`.
- **kiln_cfg_http.c**: all handlers. Tested by `test_kiln_cfg_http.c`.
- **profiles_live_http.c**: `read_small_body`, live post/get/decide/fork. Tested by `test_profiles_live_http.c`.
- **zones_http_post.c**: `zones_post_body`.
- **zones_http_post_parse.c**: numeric parsing (`strtol` end pointer, isnan, ranges, coupling diagonal, masks). Tested by `test_zones_http.c`.
- **profiles_catalog_http.c**: builtin_list/detail bounds.
- **web_auth_login_http.c**: recv loop, `-2` gives 400, password wipe, uniform refusal, logout. Tested by `test_web_auth_login_http.c`.
- **http_auth_http.c**: cookie extraction (heap, capped at 4096), prehandler (origin check before the body read).
- **http_origin_check.h**: parse (userinfo tricks refused).
- **http_auth_enforce.c**: tested by `test_http_auth_enforce.c`.
- **factory_reset.c**: body and scope parse.
- **security_http.c**: `security_post_handler` (about 1.1 KB of locals, under the threshold).
- **auth_totp_http.c**: enroll, confirm, disable, forgot, reset, totp_status.
- **dashboard_autotune_http.c**: body read and accept.
- **profiles_export_http.c**: export get, import plumbing, segment loop.
- **settings_http.c**: tz.
- **setup_progress_http.c**: post.
- **iter_tune_http.c**: query read.
- **adaptive_tune_http.c**: body read.

## Follow-up 2026-10-09 (Opus review LOWs of e51f9402/4115bc19)

- **L1 fixed**: the upload-deadline status is 503 (`OTA_HTTP_UPLOAD_SLOW_CODE`, `ota_http_util.h`), not 408, on the stage upload, `/api/ota/esp` and `/api/ota/pico`; browsers silently resend a 408 on a reused keep-alive socket. Body error stays `upload_too_slow`. Pinned by `test_ota_http.c`.
- **L2 fixed**: `ota_page.html` maps `upload_too_slow` to a friendly message on the stage card and the Pico card (`uploadTooSlowText`); `ota_http_client._push_image` reports a broken pipe or reset mid-send as "server closed the connection (possibly upload_too_slow or another refusal)", not "unreachable" (pytest added). The response itself cannot be read after urllib's send fails.
- **L3 fixed**: zones cycle-probe copy, `wifi_provision_http.c` scan_get and `kiln_cfg_http.c` list_get use `persist_scratch_alloc()`; the three files are now in `check_persist_scratch_malloc_caps.ps1` scope (negtest: a reverted malloc is caught). Zones probe OOM answers 503, not 400.
- **L5 fixed**: recovery `read_exact` / `read_body_exact` report the overall deadline as 504 "upload too slow" (`RECOVERY_UPLOAD_TOO_SLOW`), distinct from a lost connection (400). 504 is not in `mcp_server_recovery._PRE_ERASE_STATUSES`, so a mid-image timeout keeps its erase warning.
- **L6 fixed**: the missing-give wedge in `update_fetch.c` `wr_call` logs whether a finished WR_FINISH may have left an installable stage.
- **L7 fixed**: the stage-upload recv-failure path uses `update_stage_upload_abort_owned(..., STAGE_SOURCE_UPLOAD)`.
- Also: `update_fetch` host test now links `ota_http_util.c` plus a `hal_time_now_us` stub (it was not building on dev since e51f9402).
