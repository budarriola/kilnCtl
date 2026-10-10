# Review: fwbatch13 (2026-10-10)

Review only. No code changed. Commits reviewed on origin/dev:

| SHA | Subject (short) |
|-----|-----------------|
| 0cb07df6e | backup import: whole-document JSON validation, not-array checks, duplicate top-level keys, parse-only pass 1 |
| 0daf13619 | HTTP parsers: refuse invalid percent escapes, plus-signed/whitespace ids, ERANGE ids (F7, F8f, F9) |
| 2f8625f12 | readiness refusal buffers: `READINESS_GATE_MSG_CAP` 192 |
| 56979d766 | zones `nvs_load()`: `zones_config_cfg_fs_resolve()` under `zcfg_save_lock` |
| 8fdfee2bd | kiln_nvs writers fenced after factory reset (hal_kv write-refuse hook), `check_reset_fence_hooks.ps1` |
| 5c36d6414, 912a2229e | docs only |

Worktree base for verification: origin/dev `097aef73c`.

## Verification run

- Full `firmware\KilnFW\App\test\build_host_tests.ps1` at `097aef73c` (conflict markers resolved by
  `ecfd6bbb4`, `$totalExpected = 82`): **82/82 built and passed**.
- `tools\check_reset_fence_hooks.ps1`: PASS.
- httpd stack: `check_httpd_task_stack_budget.py` against a checkbuild ELF that contains 2f8625f12
  (`C:\wt\checkbuild_e77db94fc2`, source 4c1aebf1f, ELF linked after the source change):
  OK, worst `profile_exec_start_post_handler` 4448 B (= ceiling, unchanged),
  `autotune_start_post_handler` **4352 B** (was 4288 B before 2f8625f12). No regression past the ceiling.
- Negative tests (tools\negtest.ps1): see the table at the end.

## Findings

### LOW-1: kiln_configs save keeps the lax optional-id parse (F7/F9 fix incomplete)

`kiln_cfg_http.c` `save_post_handler()` (POST /api/kiln_configs/save, `name=<str> [id=<int>]`) still parses
its optional `id` with bare `strtol` and treats `http_form_find_field()`'s `-2` as "absent":

- `id=%zz` (now `-2` after 0daf13619), or an over-long id, falls through as "no id" and the request saves
  a **new** slot instead of refusing with 400.
- `id=+5` and `id= 5` are accepted (strtol skips whitespace and takes a sign).
- On the ESP32, `long` is 32 bits, so `id=99999999999` hits ERANGE and saturates to `LONG_MAX == INT32_MAX`, which
  passes the `v > INT32_MAX` check.

0daf13619 hardened only `parse_required_id()` (apply/rename/delete). The save path is the same class (F7/F9)
and should use `val[0] != '+' && http_form_parse_long(...)` and refuse `-2`.

### LOW-2: readiness refusal text is still truncated on the profile-start paths

2f8625f12 sized the autotune start paths for the longest readiness refusal (167 chars + NUL). The profile-start
callers of `profile_executor_run()` -> `readiness_gate_evaluate(err_msg, err_cap)` were not covered:

- `uart_bridge_ext_control.c:566` `err_msg[96]`
- `dashboard_exec_http.c:820` `err_msg[128]`. This only matters on the race path, because the handler's own
  pre-check uses `recovery_err[192]`.
- `ui_page_home_actions.c` and `ui_page_profile_detail.c` `err_msg[160]`, which cuts the longest message by 8 chars.

All of these truncate safely (snprintf). The remedy text can be cut off, which is the exact symptom the commit
fixed for autotune.

### LOW-3: `autotune_send_run_failure()` escape buffer smaller than worst-case escape

The same commit changed `err_escaped[257]` to `[256]` for a 192 B input. Any escaped character expands, so
256 B no longer holds a worst-case escape of 191 chars. `kiln_json_escape_ctl()` truncates inside its cap, so
there is no overflow. The readiness texts are plain ASCII prose with no characters that need escaping, so this
is theoretical. Either size it `READINESS_GATE_MSG_CAP * 2` or note why the shortfall is accepted. The noinline
callee frame grew by about 127 B; that growth is included in the 4352 B measurement above.

### LOW-4: the fence refuses every kiln_nvs writer for the whole reset window, any scope

The reset mark (`relay_authority_reset_in_flight_begin()`) is set for every scope, wifi-only included, and stays
set until the reboot (about 500 ms after the erase, `reboot_task`). The new hook therefore refuses every kiln_nvs
`hal_kv_set_*`/`erase_key`/`commit` on any other task in that window, even when the scope does not erase
kiln_nvs. Examples: `relay_cycles` wear counters, a login's `web_auth_store` lockout counter, and `boot_guard`'s
mark_healthy (which retries and is harmless).

There are no shutdown handlers (`esp_register_shutdown_handler` is unused), so a refused write is simply lost.
For a wifi-only reset that is a small, bounded loss of wear-counter or lockout updates, likely acceptable.
It is not documented in `hal_kv.h` or main.c. A multi-key kiln_nvs writer that is interrupted mid-sequence in that
window keeps its torn state through the reboot when the scope does not erase kiln_nvs. Recommend
documenting this, or gating the predicate on "scope erases kiln_nvs".

### INFO-1: fence coverage is complete for kiln_nvs writes, except partition-level calls

Every kiln_nvs user found by grep goes through `hal_kv`. 28 files reference `"kiln_nvs"`, and none uses raw
`nvs_set_*`/`nvs_open_from_partition` on it. So main.c's comment "every kiln_nvs writer" holds for key writes.
`hal_kv_erase_partition()`/`hal_kv_init_partition()` are not fenced. Their callers are boot-time
`nvs_partition_init()` corrupt-recovery and the reset job itself (exempt), so there is no live gap. Reads stay allowed.

### INFO-2: duplicate-key detection is top level only, by raw key bytes

`bj_no_dup_top_level_key()` compares raw bytes at depth 1 only. This matches F6's stated scope. Nested
duplicates inside profile/zone objects still resolve first-wins in the field scanners. An escaped spelling
(`"pro\u0066iles"`) is not caught, but the scanners compare raw bytes too, so they ignore it as an unknown key
and do not read it as `profiles`. The check is O(keys² × skip_value). Top-level key count is small, so this is not a concern.

### INFO-3: `http_form_url_decode()` strictness is global; no regression found

A `'%'` not followed by two hex digits now makes every `http_form_find_field()` return `-2`. Checked:

- The 5 callers that test `>= 0`/`> 0` on a confirm/commit flag fail closed (not confirmed).
- `diagnostics_http.c` `allow_lower` treats `-2` as absent, which defaults to the safe mask 0.
- `wifi_provision_http.c` `mode` treats `-2` as absent (mode unchanged).
- `safety_cfg_http.c`'s direct decode for `commit` fails closed.
- No web page posts a text field (ssid/password/name) without `encodeURIComponent`. A grep of `drivers/http/*.html|*.js`
  found only numeric fields built by concatenation, and those cannot carry a `'%'`.

The only behavioral gap found is LOW-1.

### INFO-4: zones `nvs_load()` lock: no deadlock

`nvs_load()` has one non-test caller, `zones_http_start()` at boot. `ct_verify_store.c`'s `nvs_load` is a
separate static function. Inside the new `zcfg_save_lock()` section, `zones_config_cfg_fs_resolve()` only calls
`zones_config_cfg_fs_save()`, which takes no zones save lock. The later
`zones_config_persist_migrated_blob_verified()` -> `nvs_save()` takes `zcfg_save_lock` again only **after** the
unlock, so the calls are sequential, not nested. One pre-existing detail: resolve writes `s_zones.cfg` under the save lock but not
`zones_cfg_lock`. This is boot-only, before any reader task.

### INFO-5: backup import pass 1 writes nothing before refusing

`backup_import_apply()` runs `backup_json_validate_document()` and the four not-array checks before anything
else. `apply_body()` then runs `backup_import_parse_only()` (a full `backup_import_apply_two_pass(...,
parse_only=true)` on heap candidates) before the dry_run return and before any commit. A read of
`backup_import_apply_two_pass()` up to its `parse_only` return found only parsing and validation. The real
pass 1 reruns after the kiln_configs/aux phase-1 commits. It can only refuse there if state changed in between,
and that window is the pre-existing restore-in-flight gate's concern. The validator is an iterative state
machine with a depth cap of 32, so its stack is bounded. It rejects trailing data, control bytes in strings,
NUL after a backslash (no overread), and non-RFC numbers. It is lenient on escape letters on purpose.

### INFO-6: hal_kv impl struct and partition copy

`struct hal_kv_esp_impl` grew by `bool + char[16]`, which the `_Static_assert` against the handle storage
covers. A partition label of 16 chars would be truncated to 15 and miss the `"kiln_nvs"` compare. All labels in
`partitions.csv` are shorter, and `"kiln_nvs"` is 8.

## Negative tests (tools\negtest.ps1, base 097aef73c)

| # | Mutation | Preset | Verdict | Failing assertion |
|---|----------|--------|---------|-------------------|
| 1 | `check_reset_fence_hooks.ps1`: hook install line removed from `factory_reset.c` | check | CAUGHT | check reports missing install |
| 2 | `check_reset_fence_hooks.ps1`: refuse predicate forced to `== 0 && false;` | check | CAUGHT | check reports predicate not refusing |
| 3 | fake_kv fence disabled | custom (`test_host_fakes.ps1`) | CAUGHT | fake refuse-hook tests fail |
| 4 | zones `nvs_load` resolve moved outside `zcfg_save_lock` | kilnfw-host | CAUGHT | `test_zones_config_cfg_fs.c:1530` (.bad write inside save section) |
| 5 | backup `parse_only` pass 1 dropped | kilnfw-host | CAUGHT | `test_backup_import.c:5289/7320/7321` (refusal is not a partial write) |
| 6 | backup `validate_document` dropped | kilnfw-host | CAUGHT | `test_backup_import.c:3411` (truncated-prefix fuzz refused) |
| 7 | top-level duplicate-key check dropped | kilnfw-host | CAUGHT | `test_backup_import.c:3411` (F6 duplicate kind/version refused) |
| 8 | `http_form.h` `%` escape validation removed | kilnfw-host | CAUGHT | `test_http_form.c:149-152` (lone/truncated/non-hex escapes refused) |
| 9 | aux `field_long` accepts leading `+` | kilnfw-host | CAUGHT | `test_aux_outputs_http.c:169-192` |
| 10 | `kiln_cfg` required id accepts leading `+` | kilnfw-host | CAUGHT | `test_kiln_cfg_http.c:508-510` (`id=%2B1` refused) |
| 11 | `READINESS_GATE_MSG_CAP` 192 -> 128 | kilnfw-host | CAUGHT | `test_readiness_gate.c:506` (message not truncated) |

All 11 mutations caught; the kilnfw-host baseline passed (308.6 s). The parallel
run's overall verdict printed ERROR only because `real_tree_unchanged` was false:
this review document was being written in the worktree during the run. Both the
worktree and the shared tree were inspected afterwards; the only change is this
document. No test-vacuity finding. Note that LOW-1 (the optional `id` path in
`save_post_handler`) has no test, so no mutation was possible there.

## Fix status (pooled firmware LOW batch)

- LOW-1: FIXED in the pooled firmware LOW batch commit ("Pooled firmware LOW batch: strict kiln_cfg id, ..."). `save_post_handler` parses `id` with the same strict parse as `parse_required_id()` (bad escape, leading `+`, ERANGE, out of range all 400); new test in `test_kiln_cfg_http.c`.
- LOW-2: FIXED in the same commit. The profile-start refusal buffers are sized from `READINESS_GATE_MSG_CAP`; the two handler buffers that would have grown the httpd/uart-bridge stacks are `static` (single worker task), pinned by `test_readiness_gate.c`.
- LOW-4: FIXED in the same commit. The kiln_nvs writer fence is armed only when the reset scope erases `kiln_nvs` (`relay_authority_reset_set_erases_kiln_nvs()`); tests in `test_link_watchdog.c` and `test_ota_http.c`.
