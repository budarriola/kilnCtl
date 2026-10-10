# Dev firmware review 14 (2026-10-09)

This is a read-only Opus review of nine origin/dev commits, taken as one batch. No code was changed and the board was not touched.

| Commit | Subject |
|---|---|
| `fdf8153a` | totp reset/forgot: zero secrets on every early return; scan results to PSRAM |
| `6f4fa7d0` | Pico announce review fixes: MED-1 (zero a stale ESP version on the first context under another boot_id) and LOW-1/2/3 |
| `d06bf784` | crash_report v4: stamp the coredump's own image identity; a foreign dump is not attributed to the running image |
| `2cb20eef` | PcTools: stale-image crash banner, preflight note, `find_crash_elf` by dump ELF sha |
| `3cb3fb94` | backup format v7 carries thermo_count/relay_count; import onto an empty board sets the topology first |
| `994c14d1` | factory reset: the kiln and all scopes clear the crash report and coredump through `crash_report_clear()` |
| `4805274f` | review LOW-1..9 (host rule/linkcov): DHCP hostname, trailing-dot and IPv6 literal checks, stale bound scaled with the poll period, profiles-loading refusal, test hook gated, saves busy during the boot load |
| `65851a47` | esp_netif stub: `esp_netif_set_hostname` |
| `c1b8086c` | XSS audit F4: DHCP hostname note |

The reviewed tree is origin/dev `51933463`. Paths are relative to `firmware/KilnFW/App/drivers/` unless they say otherwise.

Two items were excluded on instruction and are not reported: the format-truncation compile error at `backup_import.c:3590` (fixed since in `2a1d69a8`), and the `check_hal_include_boundary` failure on `esp_netif.h`.

There are no HIGH findings and one MED. MED-1 is an upgrade path in the crash_report version bump: an unacknowledged crash recorded by the previous firmware disappears silently after the OTA. The other findings are narrow races, a save gate that misses one case, and test gaps.

## Findings

### MED-1 (d06bf784): a pending v3 crash record vanishes on upgrade to v4, and the reset-reason gate stops it being recaptured

**Where**
- `safety/crash_report.c:164-166`: `record_valid()` requires `rec->version == CRASH_REPORT_RECORD_VERSION`, which is now 4 (`safety/crash_report.h:71`). There is no v3 migration, so `load()` (`:432`) reports a v3 blob as "no record".
- `safety/crash_report.c:536-555`: in `crash_report_init()`, the dump_id dedup check is followed by the new gate `reset_reason_can_produce_coredump()` (`:245`), which allows only PANIC, INT_WDT, TASK_WDT and WDT.

**What goes wrong**
1. The board panics on firmware N, which writes a v3 record. Nobody acknowledges it.
2. The operator installs firmware N+1 (this commit) by OTA or JTAG. That boot's reset reason is SW, or POWERON/EXT after a JTAG flash.
3. `load()` returns false for the v3 blob, so `refresh_unacked_cache()` clears `s_have_unacked_crash`.
4. The coredump is still in its partition, so the init path continues to the gate. The reset reason is not a panic, so the gate logs "old dump, no crash record captured" and returns.

The result: the `UNACKNOWLEDGED CRASH REPORT` banner in `get_heap_status`, the diagnostics page, and the `capability_preflight` refusal all go quiet for a crash nobody reviewed. Before this commit, step 4 would have recaptured the dump as a fresh v4 record, so the crash stayed visible.

The upgrade that introduces the format is exactly the moment this happens.

**Suggested fix**

Choose one:
- Migrate a v3 blob in `load()`: copy the common prefix, set `image_match = CRASH_REPORT_IMAGE_UNKNOWN` and an empty `dump_elf_sha`, and keep the `acknowledged` bit.
- Or, in `crash_report_init()`, let the gate pass when a blob of an older record version is present and unacknowledged, and stamp the new record UNKNOWN (never MATCH).

Either way, add a host test that seeds a frozen v3 blob and checks that the unacknowledged flag survives init on a SW reset.

### LOW-1 (d06bf784): a failed capture on the panic boot is never retried

**Where**
- `safety/crash_report.c:546` (the gate) and `:588` (`persist()`).

**What goes wrong**
- If the panic boot cannot write the record (NVS init failed, or `persist()` fails), every later boot has a non-panic reset reason, so the gate refuses to capture.
- Before this commit, the next boot retried and the crash surfaced. Now the crash is lost for good, with only a log line on one boot.

**Suggested fix**
- Persist a small "capture pending for dump_id X" marker on the panic boot before anything else that can fail. The gate then also accepts a dump whose id matches that marker.
- Or accept a non-panic boot when the dump's own `app_elf_sha256` prefix matches the running image *and* the dump_id has never been captured. That is the same identity evidence the commit already computes.

### LOW-2 (3cb3fb94): a topology-only v7 restore sets the topology in RAM and never saves it

**Where**
- `http/backup_import.c:2463`: `zones_config_set_topology_no_save()` runs when `topo.apply` is set.
- `http/backup_import.c:2959`: the single batched `zones_config_save_now()` is gated on `timing_profile_candidate_count > 0 || zone_candidate_count > 0`.

**What goes wrong**
- Take a v7 backup that carries `thermo_count`/`relay_count` but no zones entries and no timing-profile entries (a hand-trimmed or minimal backup), imported onto an empty board.
- The topology lands in RAM only. Profiles are then validated and committed against it, and the import reports success.
- On the next reboot the board loads `thermo_count = 0` from NVS. The profiles just restored now refer to zones the board does not have.
- A normal export from a board with zones always carries zone entries, so this needs an unusual backup. That is why this is LOW.

**Suggested fix**
- Add `topo.apply` to the save gate at `:2959`.
- Add a host test: a v7 body with topology and nothing else ends with `thermo_count` persisted.

### LOW-3 (3cb3fb94): the topology is re-read live four times, and POST /api/zones is not fenced against a restore

**Where**
- `backup_import_resolve_topology()` runs in the wrapper (`http/backup_import.c:4108`), in the profiles precheck (`:1041`), in the zone-topology precheck (`:3648`), and in the two-pass body (`:3718`). Each call reads the board's live `thermo_count`.
- POST /api/zones (`http/zones_http_post.c`) does not check `backup_import_restore_in_flight()`. Only the executor, autotune and the current sweep do.

**What goes wrong**
1. A restore onto an empty board starts. The wrapper sees `thermo_count == 0`, so `topo.apply` is set.
2. Meanwhile an admin saves the zones page, which sets `thermo_count`.
3. A later resolve sees a nonzero live count that differs from the backup's, and refuses with a topology mismatch.
4. If that refusal comes from the two-pass body, kiln_configs and aux phase 1 have already committed, so the result is a 500 partial write.

This needs a concurrent admin write during a restore, so it is LOW.

**Suggested fix**
- Resolve once in the wrapper and pass the resolved `backup_topology_t` down to every precheck and the body.
- Refuse POST /api/zones with 409 while `backup_import_restore_in_flight()` is true, as the run starters already do.

### LOW-4 (3cb3fb94): the kiln_cfg topology override is global

**Where**
- `persist/kiln_cfg_store.c:250`: `kiln_cfg_store_restore_topology_override()` stores the override in plain statics.
- It is set and cleared around `backup_import_apply_body()` (`http/backup_import.c:4115-4118`).

**What goes wrong**
- While a restore runs, any other kiln_configs writer (a concurrent POST /api/kiln_configs upload or apply) is validated against the backup's topology instead of the board's.
- The statics are not atomic. A reader on another task can see `active` without the matching counts.

**Suggested fix**

Choose one:
- Pass the topology into the kiln_cfg restore calls as a parameter.
- Or refuse other kiln_configs writes while `backup_import_restore_in_flight()` is true, and publish the three fields under the store's lock.

### LOW-5 (4805274f): the boot-load save gate is check-then-act and misses the retarget path

**Where**
- `http/profiles_http.c:1831`: `profiles_http_save()` checks `s_boot_loading` before `profiles_save_lock()`.
- `profiles_boot_load()` (`:2132-2140`) sets and clears the flag but writes RAM slots without the save lock.
- `profiles_retarget_zone_to_aux_commit()`/`_resume()`/`_revert()` (`:2576-2588`) and `profiles_delete_slot()` do not check the flag.

**What goes wrong**
- POST /api/zones (`move_zone_to_aux`) is registered by `zones_http_start()` (`main_network_http.c:367`) while httpd is already serving, before `profiles_http_start()` (`:423`) runs the boot load.
- A convert request in that window takes the save lock and rewrites profile slots while the boot load writes the same slots without the lock.
- The save gate also has a narrow check-then-act window: a save that read `false` just before the boot load set the flag proceeds unlocked against it. Today no save caller is reachable before `profiles_http_start()`, because the UART bridge, LVGL, backup and live-edit routes all start later. So this part is latent.

**Suggested fix**
- Gate every profile writer (save, delete, retarget commit/resume/revert) on `s_profiles_loaded` rather than `s_boot_loading`, so "not loaded yet" is refused for the whole pre-load window.
- Or have the boot load take `profiles_save_lock()` for its write phase.

### LOW-6 (4805274f): the scaled stale bound makes the stale check redundant, and the LOW-3 comment above it is now wrong

**Where**
- `safety/safety_link.c:306-315`.

**What goes wrong**
- `stale_bound = max(SAFETY_LINK_STALE_MS, poll_period_ms * SAFETY_LINK_UP_PERIODS)`, and `safety_link_up_locked()` already refuses `age > poll_period_ms * SAFETY_LINK_UP_PERIODS`.
  - When `3P >= 1500` the two conditions are the same test.
  - When `3P < 1500` the stale test is strictly weaker than `!up`.
- So the second operand can never change the result. LOW-3's stated purpose, quoted in the comment directly above ("the fixed SAFETY_LINK_STALE_MS bound the rest of the firmware uses must also trigger the clear"), has been silently reverted.
- LOW-5's intent (keep the boot id through one missed reply at a slow poll) is reasonable and its test passes. But a reader of the comment will believe the fixed bound still applies.

**Suggested fix**
- Delete the redundant operand and the LOW-3 comment, leaving the clear on `!safety_link_up_locked()` with a note that it scales with the poll period by design.
- Or, if LOW-3's fixed bound is still wanted, keep `SAFETY_LINK_STALE_MS` and adjust the LOW-5 test. The two review items conflict, and only one can hold.

### LOW-7 (994c14d1): the "all" scope's crash clear is untested, and the crash clear runs against an already-erased partition

**Where**
- `http/factory_reset.c:150,152` (scope table), and `:436` (the `crash_report_clear()` call, after the partition erase loop at `:291-304`).
- `test/test_ota_http.c`: the kiln, wifi and profiles scopes are asserted, but "all" is not.

**What goes wrong**
- Flipping the "all" row's `clear_crash_report` to false is not caught by any test (see the negative tests below).
- At runtime, `hal_kv_erase_partition()` de-initializes `kiln_nvs` (`hwAbstraction/esp/kv/hal_kv_esp.c:392-395`).
  - The `hal_kv_open()` inside `crash_report_clear()` then fails with "not initialized" and logs `could not erase crash record from NVS`. The ack path fails silently.
  - Only the coredump erase does real work. The record itself was already removed by the partition erase.
- The functional result is correct: the record is gone, the coredump is erased, and `refresh_unacked_cache()` reads false. But every kiln/all reset now prints a misleading warning.
- The host fake does not de-initialize on erase, so the tests do not model this.

**Suggested fix**
- Add the "all" assertion, mirroring the kiln one.
- In factory reset, call only the coredump erase plus the cache refresh, or run `crash_report_clear()` before the partition loop.
- Optionally make the fake de-initialize an erased partition, so ordering bugs of this kind show up.

### LOW-8 (2cb20eef): `find_elf_by_sha_prefix`'s input validation is untested

**Where**
- `tools/PcTools/src/kilnctrl/elf_archive.py:1696`: refuses a prefix under 6 characters or a non-hex one.
- `:1700-1702`: compares `min(len(p), len(key))` characters.

**What goes wrong**
- Removing the length check is not caught (see the negative tests below).
- Combined with the `min()` comparison, a 1-5 character prefix would match nearly any archived ELF. An archive file with a short or empty key after the last `-` would match every query on 0-5 characters.
- Archive names are always 12 hex characters today, so this is a test gap rather than a live bug.

**Suggested fix**
- Add pytest cases for "abc" (too short), "zzzzzz" (not hex), and an ambiguous prefix shared by two ELFs.
- Require `len(key) >= len(p)` (or 12) before comparing.

### NIT-1 (d06bf784): stale overflow comment in diagnostics_http.c

`http/diagnostics_http.c:478-479` still says the margin is "~61 B" and "do not enlarge json[]". The buffer is now a 1024 B heap allocation. Update the margin figure and drop the httpd-stack warning, which no longer applies to a heap buffer.

### NIT-2 (3cb3fb94): stale comment above the zone-topology precheck

`http/backup_import.c:3639` says "The backup carries no thermo_count". A v7 backup does carry one. The comment should describe the v7 rule (apply onto an empty board, refuse a mismatch otherwise).

### NIT-3 (4805274f): the hostname "kilnctl" is hard-coded in three places

The same literal is in `net/wifi_prov.c:546` (DHCP hostname), `main_boot_early.c:349` (mDNS hostname) and `http/http_auth_http.c:304` (fallback name for the Host allow-list). One shared constant would stop them drifting apart.

Two kilns on one LAN will also register the same DHCP name. That is not a security problem, because the allow-list matches by prefix, but router DNS will resolve only one of them.

## Checked and found correct

- **fdf8153a, TOTP zeroing.** Every early return in `auth_totp_http.c` now zeroes the request body. The body is zeroed only after the fields are copied out, and is not read afterwards.
- **fdf8153a, scan results.** `ui_page_network_manage.c`'s `s_scan_results` in `EXT_RAM_BSS_ATTR` matches the pattern other UI pages already use. It is filled only by memcpy and touched only by the LVGL task.
- **6f4fa7d0, ESP re-announce.** The unbounded boot-clear re-announce branch in `safety/safety_link_frames.c` is gone, and the budgeted function covers that case. The exhaustion log saturates at MAX+1, so it prints once.
- **6f4fa7d0, SaftyFW staging.** `link_staging_apply_context_session()` zeroes `peer->version` on the first context under a different boot_id (MED-1), and that is caught by the negative test below.
- **d06bf784, ESP-IDF fields.** The v4 record size (228 B), `app_elf_sha256` at the ESP-IDF default 9 hex characters, and `apply_image_match()` dropping this boot's facts on a mismatch are consistent with ESP-IDF v6.0.2's `esp_core_dump_summary_t`.
- **d06bf784, init order.** `crash_report_init()` runs before `boot_guard_init()` (`main_boot_early.c:264` vs `:416`), so a panic boot that also trips the recovery switch still captures first.
- **2cb20eef, `find_crash_elf()`.** With no arguments it prefers the board's `dump_elf_sha`, and treats it as authoritative only when `stale_image` is true. Non-hex archive keys such as "latest" can never match a hex prefix.
- **3cb3fb94, stack depth.** The `backup_import_apply()` wrapper adds one frame. With 9 arguments, some spill to the stack on Xtensa: about 48-80 B, plus about 16 B for `topo` in the two-pass body. The job runs on the `http_async_job` task (8192 B stack, about 4.6 KB measured depth, stack-budget table entry 7552), so roughly 2.2 KB of headroom remains. Not a risk. The new resolver and body are `NOINLINE`, so their frames do not stack up in the caller.
- **3cb3fb94, rollback.** A mid-batch failure after `zones_config_set_topology_no_save()` goes through `zones_config_restore_snapshot_no_save()`, which restores the whole config including `thermo_count`, because the snapshot is taken before the topology set (`:2460`). This is not covered by a test: the test stub's snapshot restore is not asserted to model `thermo_count`.
- **3cb3fb94, aux validation.** It unions every zone slot's relay mask. That is conservative and correct.
- **4805274f, http_origin_check.h.**
  - A bracketed host must be hex digits, `:` and `.` only. That is the right shape for an IPv6 literal and refuses names in brackets.
  - The trailing-dot strip now applies to every non-bracketed form.
  - The new table tests cover the edges (two dots, empty label, empty port, 95/96-byte hosts).
  - Upper-case IPv6 hex is refused, but browsers serialize IPv6 hosts in lower case, so this is fine.
- **4805274f, the rest.**
  - The DHCP hostname is set after the STA netif is created and before `esp_wifi_start()`, so the first DHCP request carries it.
  - The "profiles still loading" message in `profile_executor_run.c` is chosen from `profiles_http_loaded()`.
  - The test hooks are now compiled out of the target (`KILNCTL_PROFILES_LOADED_TEST_HOOK`).

## Negative tests (tools\negtest.ps1)

Each mutation ran in a throwaway worktree copy at `51933463`, with a passing unmutated baseline. KilnFW runs used `build_host_tests.ps1 -Only` with a regex selecting only the executables named in the table: crash_report and profiles_http in one run, then ota_http and main (for `test_http_auth_enforce.c`) in a second. A first attempt for the three ota/auth mutations selected the wrong executables and reported MISSED without running their tests. Those results were discarded and the three were re-run.

| Mutation | Expected to catch | Result |
|---|---|---|
| SaftyFW `link_staging.c`: first-context zeroing `if (...)` -> `if (0)` (MED-1 fix removed) | `test_link_staging.c` | CAUGHT (`test_link_staging.c:300`) |
| `crash_report.c`: `reset_reason_can_produce_coredump()` returns true | `test_crash_report.c` | CAUGHT (`test_crash_report.c:536-537`, poweron/sw reset) |
| `factory_reset.c`: kiln row `clear_crash_report` -> false | `test_ota_http.c` | CAUGHT (`test_ota_http.c:1682,1690`) |
| `factory_reset.c`: all row `clear_crash_report` -> false | `test_ota_http.c` | MISSED (LOW-7) |
| `http_origin_check.h`: bracketed-host character check accepts anything | `test_http_auth_enforce.c` | CAUGHT (`test_http_auth_enforce.c:762`) |
| `profiles_http.c`: `s_boot_loading` save gate -> `if (0)` | `test_profiles_http.c` | CAUGHT (`test_profiles_http.c:5069-5070`) |
| `elf_archive.py`: `len(p) < 6` -> `len(p) < 0` | `test_crash_elf_recovery_archive.py` | MISSED (LOW-8) |

## Fix status (fwbatch14)

Stack-review items (M1, M2, L2, INFO) were fixed separately on origin/dev and are not covered here.

- MED-1, LOW-1, LOW-7, LOW-8: FIXED in b77c0ce94 (plus 19bd98965, test registration).
- LOW-2, LOW-3, LOW-4, LOW-6, NIT-1, NIT-2: FIXED in 283937330.
- LOW-5: FIXED in 38eaf6bcd (save, delete, retarget and revert are refused until the boot load finishes).
- NIT-3: SKIPPED. The hostname constant is shared by three build configurations including the recovery image; consolidating it is out of proportion to a nit.
