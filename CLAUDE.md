# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**kilnCtl** is a KiCad-based electronics design project for a kiln controller. It includes a hierarchical schematic design, PCB layout, component library, and Python tools for programmatic access to project data.

## Where to start

Fresh clone: run `tools/setup.ps1`, then open `kilnCtl.code-workspace` (not the
folder). `docs/SETUP.md` explains what it generates and why those files are
gitignored — **edit `templates/`, never the generated output.**


**ROADMAP.md** at the repo root is the top-level plan spanning both firmwares
(`KilnFW` on the ESP32-S3, `SaftyFW` on the RP2040). Start tasks from there; it
links to the per-area plans that own the detail.

## Tooling: always go through the MCP facade

**Anything involving the boards or the KiCad project starts
with one of these calls.** Do not conclude a capability is missing because you
cannot see a tool for it — each server publishes six or seven tools and keeps
the rest behind a search facade (195 tools for `kilnctrl`, 86 for `kicad`, both
per `kiln_help()`/`kicad_help()` as of 2026-09-28, when `control_set_zone_type`
was added -- a narrow writer for one zone's `zone_type` field only
(GET-merge-POST `/api/zones`, no PID/control_mode/limits collateral, modeled
directly on `control_set_zone_limits`), fixing the gap where the only
existing `zone_type` writer, `load_config_preset()`, is a whole-page write
forbidden for a single-field fix -- an unverified preset restore once left
bench zone 2 at `zone_type=1` (on/off) this way, which silently made a
3-zone firing never close that zone's relay (HP-02). `control_get_zones` now
also surfaces `zone_type` per zone (it previously showed only `mode`, i.e.
`control_mode`). The one before it was `backup_export`/
`backup_import`, 2026-09-27 -- wrapping `GET /api/backup/export` (READ-ONLY;
saves the JSON under gitignored `logs/backup_export/`) and the synchronous
`POST /api/backup/import` (`confirm=True` exactly, readiness read before and
after, a 500 partial write reported as a failure). The one before it was
`control_set_zone_limits`, 2026-09-25 -- a narrow writer for one zone's `max_temp_c`/`min_temp_c` only
(GET-merge-POST `/api/zones`, no PID/control_mode collateral; the Pico's
`abs_max_temp_c` follows the zone maxima via firmware, so it reports that
ceiling rather than gating on it). The one before it was
`safety_get_unset_commissioning_params`, same day -- a READ-ONLY tool naming the specific safety_cfg_store params
behind `GET /api/readiness`'s "safety_commissioned" count ("N of M applicable
safety parameters still have no value"), which counts but never names them.
No route needed changing: `GET /api/safety/commissioning` already reports
every param's id/name/set, so this tool is a pure re-derivation
(`safety_cfg_http_client.unset_applicable_commissioning_params()`) of
`readiness_http.h`'s `readiness_param_required_for_commissioning()` exclusion
rule (ct_channel_map[0..2] applicable only when ct_installed!=0 and
ct_topology is per_zone; i_normal_a[0..2] applicable only when
ct_installed!=0; everything else always applicable), run against the same
GET response `safety_get_commissioning()` already fetches. The one before it was
`totp_enroll_status`/`totp_reset_password`, 2026-09-24 -- `docs/TOTP_PASSWORD_RESET_PLAN.md` WT-C,
wrapping the OPEN-tier `POST /api/auth/forgot`/`/api/auth/reset` routes and
the ADMIN-tier `GET /api/auth/totp_status` read (wire contract in that plan's
section 6a, for WT-A's firmware to follow). `totp_enroll_status` is
READ-ONLY and never reports a secret. `totp_reset_password(confirm=True)`
reads the code from `KILNCTL_TOTP_CODE` and the new password from
`KILNCTL_WEB_PASSWORD_NEW` (never call parameters, never echoed), refuses
unless `confirm is True` exactly, reports status codes/booleans only, and
re-verifies a reported success with a real login. Enrollment stays
settings-page-only by owner decision. `tools/PcTools/src/kilnctrl/totp.py`
is an unwired RFC 6238 reference (Appendix B-tested) for WT-D's host tests.
The one before it was
`estop_verify`, 2026-09-23 -- an MCP tool wrapping `POST /api/estop/verify`
(`diagnostics_http.c`'s `estop_verify_post_handler()`, ROUTE_TIER_ADMIN),
recording that a HUMAN has physically verified the E-stop interlock per
`firmware/SaftyFW/README.md`'s bench procedure -- never inferred from a GPIO
read, and never to be called from an automated suite (see
`judge_estop_verify()` in `tools/PcTools/src/kilnctrl/bench_test/judgments.py`,
which documents the same rule for its own read-only smoke case and never
calls this route itself). It reads `GET /api/readiness` first, reports the
`estop_verified` item's state, refuses unconditionally if `safety_trip` is
not `ok` (never a way to paper over a latched trip), refuses unless
`confirm is True` exactly, and re-reads readiness afterward, failing loud if
`estop_verified` still doesn't read `ok`. The one before it was
`ota_matrix_run` -- ROADMAP.md M8's scripted, `run_pctools_tests`-style regression for
suite `ota` (`docs/BENCH_TEST_SYSTEM_PLAN.md` section 3.4: OT-B01, OT-E01..12,
OT-P01..05), a thin front door onto the same `BenchTestRunner` engine
`bench_test_run()` already drives, adding a hard `confirm is True` gate
(exactly `True`, not merely truthy -- this matrix can flash both processors,
roll back a slot, and reset the safety link) and a `dry_run=True` mode that
lists the case matrix and its preconditions with zero board access. Before
`BenchTestRunner` is even constructed it runs its own fail-closed run-level
gate reusing `coordinated_gpio_test`'s ARMED/link-up/executor-not-running-or-paused/
OTA-interlock probe (unreadable counts as a refusal, never a pass) plus a
`capability_preflight` read -- `BenchTestRunner.preflight()` alone folds every
probe exception into "could not determine" and never checks ARMED or the
interlock itself, so this run-level gate is additive, not a replacement.
Every mutating case still gates itself on the executor being idle and the
OTA interlock reading ok (`cases_ota.py`) immediately before it acts, with
one deliberate exception: OT-E07/OT-E08 intentionally push during a firing/
autotune run to prove the push is refused, and only run at all when
`allow_heat=True` is passed (default `False`). OT-B01 (the dual-reflash trip)
checks executor-idle but not the OTA interlock itself -- covered by this
tool's run-level gate. **Without an `ota_*` image path/commit parameter set,
only OT-B01 actually executes** -- every other case reads its own image path
out of context and SKIPs for lack of one, same as calling
`bench_test_run(suite="ota")` with none set; passing the relevant image
parameters lets more of the matrix run. Unit-tested with a fake board only --
never run against real hardware. The one before it was
`safety_get_param`,
same day -- a READ-ONLY GET_PARAM (0x23) wrapper that reports a Pico refusal as a
refusal, never as "not found". The one before it was `coordinated_gpio_test`,
same day -- registers `tools/PcTools/scripts/coordinated_gpio_test.py`
(HARDWARE.md section 1 Steps A/B, the ESP4/5<->Pico4/5 crossing test) through
the facade for the first time, with preconditions (safety not ARMED, no
profile running, OTA interlock idle, link up, `confirm=True`) a bare script
invocation never had. The one before it was `check_task_liveness`,
same day -- a READ-ONLY MCP tool cross-checking a live `GET_STACK_MARGIN`
reading (same path as `get_stack_margin`) against the `$requiredNames` list
`tools/check_stack_margin_registration.ps1` enforces at the source level:
that check only proves every required task HAS a `stack_margin_register()`
call site, never that `xTaskCreate*()` actually succeeded for it on a given
boot, and every task-creation failure is log-only (`ESP_LOGE`, non-fatal,
nothing HTTP-visible), so a board that silently failed to start a required
task looks perfectly healthy everywhere else. It reports expected-and-alive,
expected-but-DEAD (registered, not running), expected-but-ABSENT (never
registered), and extra (alive, unexpected -- informational), parsing the
required-task list live out of the `.ps1` script rather than a second
hand-copied one (`tools/PcTools/src/kilnctrl/task_liveness.py`, unit-tested
with no board). `capability_preflight` now also runs this cross-check and
refuses a run with a dead or absent required task, same as an
unacknowledged crash report, unless `allow_missing_tasks=True`.
**2026-09-23 follow-up:** the first live run reported six false positives
that are all by design (`pico_auto_update` self-deletes after boot;
`gpio_probe`/`i2c_owner_ns2009` are config/hardware-conditional;
`ota_pico_rollback`/`recovery_exit`/`ota_rollback_reboot` are on-demand HTTP
handler tasks) -- each `$requiredNames` entry in the `.ps1` script now
carries a `# liveness: <tag>` comment (`always` default, `config`,
`on-demand`, `boot-once`), and only an `always`/untagged DEAD-or-ABSENT or a
`boot-once` ABSENT still refuses; the other combinations report
informationally. The one before it was `boot_guard_get`, 2026-09-22 -- a READ-ONLY MCP tool wrapping
`GET /api/boot_guard`
(`ota_http_recovery.c`'s `ota_boot_guard_status_get_handler()`), reusing
`ota_http_client.get_boot_guard_status()` (already called internally by
`flash_firmware()`'s post-flash reset step) so the recovery-mode counter can
be checked without a flash in flight -- a bench agent had found no tool
exposed it that day. The one before it was `web_auth_logout`, same day --
an MCP tool wrapping `POST /api/auth/logout` via `http_auth.logout()`,
the PC-side session seam every other client already logs in through on a 401
but that nothing previously called to give the session back; reports `[bool]`
only (whether a remembered session existed and the logout POST was sent),
refuses nothing (ending a session is not destructive), and never prints a
credential. Also removed a stale zero-caller-sweep allowlist entry for
`binary_provenance.check_binary_fresh`, which now has a real caller via
`assert_binary_fresh`. The one before it was `crash_report_clear`, same day --
an MCP tool wrapping `POST /api/crash_report/clear`
(`diagnostics_http.c`'s `crash_report_clear_post_handler()`, ROUTE_TIER_ADMIN),
which acknowledges the board's last-crash record AND erases the coredump
image so an old coredump is never re-captured after a reflash -- a strictly
more destructive action than `crash_report_ack`, which never touches the
coredump. It fetches `GET /api/crash_report` first and reports the pending
record's summary before doing anything, refuses to clear a present-but-
unacknowledged record unless `allow_unacknowledged=True` is also passed,
refuses to POST unless `confirm=True`, and fails loud if a read-back after
the POST still shows the record present. The one before it was
`kiln_config_apply`, same day -- an MCP tool wrapping `POST /api/kiln_configs/apply` plus a status
poll (`tools/PcTools/src/kilnctrl/kiln_configs_apply_http_client.py`),
refusing to POST unless `confirm=True` and distinguishing the interlock 428
from the hardware-differs 428 by response header name. The one before it was
`nvs_list_keys`, 2026-09-21 -- an MCP tool wrapping the new `GET /api/nvs/keys` route
(`diagnostics_http.c`'s `nvs_keys_get_handler()`, ROUTE_TIER_ADMIN) that
lists NVS key NAMES AND TYPES ONLY (never a value, never a blob body) for
one partition/namespace via `nvs_entry_find()`/`nvs_entry_info()`, refusing
the `kiln_auth` namespace outright with 403 -- built to verify on the bench
that `esp_wifi_restore()` really empties the driver's own `nvs.net80211`
namespace in the default `nvs` partition after `factory_reset(scope=wifi)`
(`docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md`), previously
only checkable with a JTAG memory read. The route's own JSON is streamed in
small fixed chunks, never a task-stack buffer, per this file's own "httpd
stack blob class" note below. `config.max_uri_handlers` was bumped
160 -> 165 in the same change (`wifi_provision_http.c`) for the usual
~9-slot headroom. The one before it was `cfgfs_format`, same day -- an MCP
tool wrapping the new `POST /api/cfgfs/format_confirm` route
(`cfg_fs_format_http.c`, ROUTE_TIER_ADMIN) that lets an operator confirm
erasing and reformatting the `cfg` LittleFS partition once `cfg_fs_mount.c`'s
auto-format gate defers to that confirmation rather than silently discarding
data. It signs its request with `OTA_HTTP_CONTEXT_FACTORY_RESET`'s
`"factory-reset"` context string -- deliberately reused from
`POST /api/factory_reset`, not a bug -- which this same change also added to
`ota_http_client.py`'s `derive_mac()` allow-list; that allow-list had omitted
it even though firmware already used it for both routes, a tooling gap found
on the bench. It reads `GET /api/cfgfs` before and after, refuses to POST
unless `confirm=True`, and reports before/after file counts rather than
trusting the POST's own plain-text response alone. The one before it was
`kiln_configs_quarantine_clear`, same day -- an MCP tool wrapping the new
`POST /api/kiln_configs/quarantine_clear`
route (`kiln_cfg_http.c`, ROUTE_TIER_ADMIN) that discards a wrong-size-blob
quarantined kiln_configs store and starts a fresh, empty one -- the only
alternative today to a full `factory_reset(scope=KILN)`. It reads
`GET /api/kiln_configs` and a non-mutating status probe on the same route
first, refuses to POST unless `confirm=True`, and re-probes afterward,
failing loud if the store still reads quarantined. The one before it was
`web_auth_setup`, same day -- an MCP tool that bootstraps the board's administrator web credential
and turns web auth on (POST /api/auth/bootstrap_password or
POST /api/auth/security's `set_web_password`/`set_policy`, matched against
security_backend_web_auth.c/security_http.c), reading
`KILNCTL_WEB_USERNAME`/`KILNCTL_WEB_PASSWORD` from the environment, refusing
every write unless `confirm=True`, and verifying by read-back afterward. The
one before it was the profile_live_* quartet, 2026-09-20 -- `profile_live_get`/
`profile_live_fork`/`profile_live_edit`/`profile_live_decide`,
MCP tools wrapping the five-route live profile edit HTTP surface
(`firmware/KilnFW/App/drivers/http/profiles_live_http.c`,
`docs/LIVE_PROFILE_EDIT_PLAN.md` section 10): fork the running profile into a
working copy, read its status/content, edit it (400 on a bound violation, 409
on a window violation), and decide its fate (discard/save_as/overwrite, the
last refused 403 on a builtin origin). All four go through
`tools/PcTools/src/kilnctrl/profile_live_http_client.py`, reuse the same
`http_auth.urlopen()` ADMIN-session seam every other admin-tier tool uses, and
the three write tools (fork/edit/decide) refuse unless `confirm=True`. The one
before it was `get_readiness`, same day -- a READ-ONLY MCP tool wrapping
GET /api/readiness (readiness_http.c), rendering the board's commissioning
checklist (one line per item: status, key, detail, with ok/not_done/other
counts). The one before that was
`crash_report_ack`, same day -- an MCP tool for POST /api/crash_report/ack (diagnostics_http.c,
ROUTE_TIER_ADMIN), which acknowledges the board's last-crash record the same
way the diagnostics page's own "Acknowledge" button does; it fetches
GET /api/crash_report first and reports the pending record's summary before
doing anything, refuses to POST unless `confirm=True`, and fails loud if a
read-back after the POST still shows the record unacknowledged. The one
before it was `convert_config`, same day -- the MCP surface for
`tools/PcTools/src/kilnctrl/config_convert.py`, the PC-side best-effort
config-version converter (file-only, never writes to a board; see
`docs/CONFIG_MIGRATION_CHAIN_PLAN.md`). The one before that was
`safety_set_log_level`, same day -- an HTTP client for POST
/api/safety/log_level, which had no caller anywhere. The one before that was
`saleae_decode_kilnlink`, same day --
decodes a captured kilnlink UART link into a frame timeline,
tools/PcTools/TODO.md capability 2. The three before that were `bench_test_run`/
`bench_test_list`/`bench_test_last`, 2026-09-19 -- docs/BENCH_TEST_SYSTEM_PLAN.md
Wave 0).

```
kiln_help()                      # kilnctrl: main board (ESP32-S3) + RP2040 safety processor
kicad_help()                     # kicad:    the schematic, board and sourcing data
kiln_find(query="read the temperature")
kiln_call(name="thermo_read")
kiln_batch(calls=[{"name":"safety_get_status"},{"name":"safety_get_link_stats"}])
```

`*_batch` is the right form for any sequence of two or more hardware
operations — one round trip, and it stops at the first failure.

Both speak HTTP (`kicad` on 8766, `kilnctrl` on 8767)
and must be running. If a call fails to connect:

```powershell
.\tools\PcTools\scripts\mcp_servers.ps1 status   # or: start | stop | restart
```

The same four actions are status-bar buttons in `kilnCtl.code-workspace`, and
the servers auto-start when the workspace opens.

Both servers are long-running and keep serving whatever code they started
with — restart after editing server source (`kiln_help()`/`kicad_help()` and
`mcp_servers.ps1 status` self-report `fresh`/`stale` off `/health`, which also
carries `changed_files` and `commit`). `flash_firmware()`'s verification fix
(`06ea366`) sat inert for a while this way. Full detail: `docs/MCP_SERVERS.md`
"Stale-server self-announcing".

Firmware builds and host tests are tools too — `build_kilnfw`,
`build_saftyfw_host_tests`, `run_pctools_tests` — so
the toolchain invocations do not have to be rediscovered. Flashing is
`debug_program(peer="pico")` for the Pico. For the ESP32-S3, use `flash_firmware()`
instead — `debug_program(peer="esp")` now **refuses immediately**, before touching
OpenOCD at all, because that generic single-ELF path reliably fails flash-bank
detection/verify on this board (confirmed repeatedly); `flash_firmware()` is the
sanctioned working path, still OpenOCD, never esptool.

`flash_firmware()` accepts an optional `kiln_fw_root` override (absolute path
to a `firmware/KilnFW`-shaped directory whose `build/` already holds the
three binaries) for the sanctioned "build from a clean git worktree at HEAD"
workflow, when the main tree carries another session's foreign WIP that
would otherwise ride along or trip the sensitive-dirty guard below; the
git-provenance record and verification then reflect that worktree, not the
main tree, and `flash_provenance.json` records which path was used.

`flash_firmware()` (2026-09-17 fix) resolves its write target dynamically
from `<kiln_fw_root>/partitions.csv` — the partition named `app` (the ota_0
slot introduced by the single-slot OTA redesign, `docs/OTA_SINGLE_SLOT_PLAN.md`)
— rather than a hardcoded offset. That hardcoded offset used to point at the
old table's `factory` partition; after the table redesign it silently
pointed into the new, much smaller `recovery` partition instead and
overflowed it by roughly 360 KB into `coredump` (the board kept booting its
old app image, and post-flash verification correctly failed loud). A hard
pre-flight size check now also refuses, naming both byte counts, if
`KilnCtrl.bin` is larger than the target partition.

`flash_firmware()` now also hard-requires and flashes `bootloader.bin` and
`partition-table.bin` alongside `KilnCtrl.bin` (2026-09-23), resolving the
partition-table write offset from `CONFIG_PARTITION_TABLE_OFFSET` in
`<kiln_fw_root>/sdkconfig` rather than a hardcoded 0x8000 (`fixture_flash()`
uses the same resolver). Full detail: `docs/MCP_SERVERS.md`'s flash section.

It still does not touch `otadata`, and this is a known, deliberate gap, not
a fixed one: on this table, a blank/erased `otadata` makes the bootloader
boot the factory-subtype partition (`recovery`), not the `app` partition
just written. Writing a correct `otadata` blob by hand could not be
verified against real hardware as part of this fix, and a wrong one risks a
worse, silently-bricked boot than refusing loudly — so `flash_firmware()`'s
own post-flash verification instead fails loud, naming the running
partition, if the board isn't found running `app` afterward. Note that
`ota_rollback_esp()` does **not** fix this scenario: it reverts a board
that is ALREADY booting one OTA image back to a PREVIOUS one over its own
HTTP API, and has no path to set an unset/blank `otadata` after a bare JTAG
flash. A board whose `otadata` already points at `app` (e.g. from a
previously successful OTA) is unaffected; a from-scratch or never-OTA'd
board may need `otadata` resolved by hand before a flash from this tool
will boot.

Resetting both processors close together (a dual reflash) correctly trips
S6a (mainFault) while the ESP's safety-link handshake is still coming up —
expected, not a bug. Confirm link-up via `safety_get_status()` and that
`trip_reason`/`trip_mask` show only `SAFETY_TRIP_MAIN_FAULT` before calling
`safety_clear_trip()`. **`trip_mask` is `1 << (trip_reason - 1)`**
(`link_frame_trip_mask_for_reason()`, `firmware/SaftyFW/src/tasks/link_frame.c:237`)
— derive the expected mask from that formula rather than quoting a fixed
constant, since the enum leaves gaps (S4/S10 are WARN-only and skip a
number) so bit position is NOT the guard number past S3. S6a is
`SAFETY_TRIP_MAIN_FAULT = 6`, so its mask is bit 5, `0x0020` — **not**
`0x0040`, which is bit 6 (`trip_reason 7`, `SAFETY_TRIP_LINK_DEAD`/S6b).
Full procedure and rationale: `docs/MCP_SERVERS.md`'s flash section,
`docs/audits/s6a_startup_grace_revert_2026-09-07.md`, and
`firmware/SaftyFW/docs/ARCHITECTURE.md`'s "Correction, 2026-08-27 audit"
section.

`ota_rollback_esp()` itself has a hazard, 2026-09-04: rolling back past a
`zones_cfg` schema bump (e.g. v22, `ZONES_CFG_VERSION` in
`firmware/KilnFW/App/drivers/persist/zones_config_json.h`) makes the older firmware
refuse the newer-than-it-knows blob and run that boot on **firmware-default
PID gains**, not the tuned ones — flash is left untouched, so reflashing the
newer firmware restores everything, but a firing started right after the
rollback and before reflashing runs on defaults with no separate warning.
Read back `control_get_zones` (or `GET /api/zones`, ROUTE_TIER_ADMIN per
`route_tier_table.h`, so it 401s without an admin session once web auth is on) after any rollback before heating. Full detail: `firmware/CommonFW/docs/UPDATE_PROTOCOL.md`
("`ota_rollback(processor)`" bullet).

Zones config (and profiles, and several preferences) now also dual-write to
a new `cfg` LittleFS partition alongside NVS — NVS stays authoritative and
unconditional, so the rollback hazard above is unchanged. The `cfg`
partition itself is unformatted on the bench board and not yet mounted at
boot, so this is inert today. **Superseded 2026-09-21:** the bench board
(`8ab3b81a`) now has `cfg` mounted and populated with 7 files, confirmed
via `GET /api/cfgfs` on hardware; NVS is still authoritative. Full detail, including a real unfixed
atomicity defect in the RP2040's own config store found by the same
review: `docs/CONFIG_FILESYSTEM.md`.

`flash_firmware()` now checks this **automatically** after every flash (the
`verify` parameter, default `True`): it polls the board's own HTTP API for the
running partition (`/api/partitions`'s RUNNING marker) and for its reported
build timestamp (`fw_build`, compared against the `.bin`'s embedded
`esp_app_desc_t` build time) and FAILS the tool call loud, naming the actual
running partition/build and pointing at `ota_rollback_esp()`, if either
disagrees with what was just flashed. Manually checking the RUNNING marker or
the boot log's `running partition: '<name>'` line is now redundant for a normal
flash — still useful for checking partition state independent of a flash, via
`debug_check_partition_table()`. Pass `verify=False` only for bring-up when the
board's HTTP stack isn't expected to be up yet (e.g. Wi-Fi not provisioned
yet). Verification tries an ordered list of candidate hosts (the address the
board was actually observed answering at moments before the flash, first —
2026-09-04 fixed a bug where it polled only the AP-fallback `192.168.4.1`
immediately post-reset, before Wi-Fi re-associated, while the board was live
on the LAN the whole time) and re-resolves on every poll attempt. A board that
was demonstrably reachable pre-flash and answers at none of the candidates
afterward is a hard verification failure; only a board that was never
observed up (genuine bring-up) gets the soft WARNING instead of a failure,
since that's not the same as confirming the wrong thing landed.

Getting the board's own view of its health is one call: `get_heap_status` now
carries `reset_reason`/`uptime_s` through and also checks
`GET /api/crash_report`, printing a loud `UNACKNOWLEDGED CRASH REPORT` banner
if the board panicked and nobody has reviewed it yet (`2cc90b0` — the previous
version discarded `reset_reason` and never looked at crash_report at all, so a
board that panicked and rebooted clean read as healthy). `capability_preflight`
now refuses to start a run on a board with an unacknowledged crash regardless
of what the run needs.

`flash_firmware()` also records `git status --porcelain`/HEAD at flash time
(reported in the result, persisted to `KilnFW/flash_provenance.json`, a
sibling of `build/` since 2026-09-15 -- not inside it)
and refuses — naming the files — if the dirty set touches config-schema/
migration/safety code (e.g. `zones_config_*`), since this tree is normally
shared across sessions. An ordinary dirty tree is never refused. Override
only after reviewing the named files: `allow_sensitive_dirty=True`.

Also: `debug_reset` does not power-cycle external I2C peripherals. A safety
trip that latches right after an OTA reboot can be the SX1509 I/O expander
failing its post-reset init, not a firmware defect — a second `debug_reset`
clears it.

`tools/PcTools/scripts/capture_lcd.ps1` grabs a frame from the bench webcam
aimed at the board's LCD, cropped to the panel (`-Full` for an uncropped frame
if the camera moved and the crop needs re-measuring) — display rendering can
be checked without a human at the bench. ffmpeg's dshow input exposes no
exposure/gain/focus control on this camera (2026-09-24 check: `-list_options
true` lists only format/resolution/fps), so `-Frames` (default 5,
`-WarmupSeconds` default 1.0) instead captures several frames after an
auto-exposure/auto-focus settling delay and keeps the sharpest by a
Laplacian-sharpness metric, printing the chosen candidate and its score;
`-Frames 1` restores the old single-frame behavior. This fixes blur from a
capture that started before the camera settled, not a genuinely overexposed
scene. Judge colors by **numeric pixel
sampling**, never by eye or by matching theme source constants:
`ffmpeg -i img.jpg -vf "crop=W:H:X:Y,scale=1:1" -f rawvideo -pix_fmt rgb24 - | od -An -tu1`.
Always sample an off-screen bezel region too, as a neutral reference. If it
fails with ffmpeg exit `-5`, check for another process holding the C920
first — the device still enumerates fine (`ffmpeg -list_devices true -f dshow
-i dummy`), it is just busy.

`tools/PcTools/scripts/sample_lcd_region.ps1` wraps that pipeline: given a
capture from `capture_lcd.ps1` and a region (`-X -Y -W -H`), it prints the
region's mean RGB alongside the same for a bezel reference region, so this
recipe doesn't have to be re-derived by hand each time
(`.\sample_lcd_region.ps1 -Image full.jpg -X 500 -Y 650 -W 8 -H 8`).

**Camera aim (2026-09-19):** the owner moved the LCD and put it back, so the
2026-09-10 numbers below are stale by far more than the few-px tolerance --
the panel shifted ~195px right and ~46px down in the 1280x720 frame, and the
old crop clipped away most of the screen. A fresh `-Full` capture and numeric
luminance edge scans against the black-bezel reference (never by eye) put the
panel at left edge `X=298` (top) to `X=323` (bottom), right edge `X=1145-1147`,
top edge `Y=60` (right) to `Y=86` (left), bottom edge `Y=617` (right) to
`Y=635` (left) -- the board now sits rotated a few degrees with its right side
about 25px higher than its left, so it is markedly less square in frame than
before. `capture_lcd.ps1`'s defaults are now the smallest axis-aligned box
containing all four corners, `X=296 Y=58 W=853 H=578`
(`tools/PcTools/scripts/capture_lcd.ps1:57-60`), which includes a few pixels of
bezel on the tighter sides rather than clipping any UI content. (The previous,
2026-09-10 rectangle was `X=102 Y=12 W=907 H=609`.)

**Camera aim (2026-09-24):** LCD-01 found the 2026-09-19 numbers above stale
again. A fresh `-Full` capture and numeric luminance/color edge scans (never
by eye) put the four corners at top-left `(160, 52)`, top-right `(1044, 116)`,
bottom-left `(161, 645)`, bottom-right `(983, 624)` -- the panel moved left
and slightly up, and the geometry is a perspective skew, not a simple
rotation: the right edge measures ~14% shorter than the left and slants
~61px (lower at the top, higher at the bottom) while the left edge runs
near-vertical, and the top edge is ~8% longer than the bottom.
The smallest axis-aligned box containing all four corners is
`X=158 Y=52 W=886 H=593` (`tools/PcTools/scripts/capture_lcd.ps1`'s
`-CropX/-CropY/-CropW/-CropH` defaults),
now the default crop, and `lcd_sampler.py`'s `FRAME_CORNERS` was updated to
match. The 2026-09-19 numbers above are kept only as history.

**Camera aim (2026-09-24, round 3):** the LCD-01 corners above went stale the
same day. A numeric edge scan of
`logs/bench_test/20260924T162517Z_lcd/captures/lcd01_start_pause.jpg` puts
the panel corners at top-left `(180, 69)`, top-right `(1038, 121)`,
bottom-left `(178, 627)`, bottom-right `(985, 628)`; `lcd_sampler.py`'s
`FRAME_CORNERS` now carries these, and it samples four background points
after the homography, downgrading a color FAIL to INCONCLUSIVE ("frame
corners stale or panel dark") whenever any of them reads as bezel. The
`capture_lcd.ps1` crop above still contains all four corners and is unchanged.
Full detail: `docs/COMMISSIONING_LCD_RUNBOOK.md`, "Camera aim (2026-09-24,
round 3)".

**Camera aim (2026-09-24, round 4):** round 3's bottom-right corner
`(985, 628)` was an internal content edge, not the bezel (the lit panel runs
to `y~=660` at `x=950`). A "sustained dark run" edge scan of
`logs/bench_test/20260924T191429Z_lcd/captures/lcd01_start_pause.jpg` puts
the corners at top-left `(179, 68)`, top-right `(1045, 125)`, bottom-left
`(177, 627)`, bottom-right `(981, 662)`; `lcd_sampler.py`'s `FRAME_CORNERS`
carries these. The old crop no longer contained them, so the
`capture_lcd.ps1` default grew to `X=158 Y=52 W=894 H=616`. Full detail:
`docs/COMMISSIONING_LCD_RUNBOOK.md`, "Camera aim (2026-09-24, round 4)".

Full rationale, token measurements, and how to add a tool: **docs/MCP_SERVERS.md**.

Three git-workflow guards live under `tools/`: `worktree_mint.ps1` (mint/remove
a short, uniquely-named worktree at `origin/main` under `C:\wt\`),
`push_verify.ps1` (verify a commit actually landed on `origin/main`, direction-
and `$?`-safe), and `commit_guard.ps1` (refuse a commit whose working copy
differs from `origin/main` until every difference is confirmed as your own).
Full detail: **docs/MCP_SERVERS.md**'s "Git workflow guards" section.

**What is safe to delete during cleanup.** Many parallel sessions build and
run checks in this shared tree and under `C:\wt\`, so cleanup passes recur.
Safe to delete on sight, no owner review needed: untracked build byproducts
in the main tree (loose `.obj` files, host-test scratch directories like
`cfg_fs_test_*/`, `log_store_test_*/`, `build_agent/`, `build_sim_*_obj/`),
and mangled-path files/directories left by a Bash-vs-PowerShell backslash
quoting accident (a literal `C:\wt\...` argument getting eaten mid-path,
producing a stray file or directory named after the mangled remainder — this
is the same class of bug as `worktree_mint.ps1` failing when invoked through
Bash's POSIX shell instead of the PowerShell tool). If a class of these
recurs, add a `.gitignore` pattern for it rather than re-deleting by hand
each time. Under `C:\wt\`, only remove a worktree you can positively confirm
is both unregistered by `git worktree list` (or registered but the branch/
commit shows no pending work) and stale — never one with uncommitted
changes, and never one whose directory shows a file freshly modified in the
last few minutes, since that is very likely a session still using it. Never
delete: anything under `firmware/KilnFW/elf_archive/`, `logs/coupling/*` or
other captured run data, any `.kicad_*` file, or a tracked file with local
modifications — those need owner review, not deletion, and reverting them
to investigate is exactly the mistake to avoid (other sessions' uncommitted
work lives in this same shared tree).

## Project Structure

The tree is split into hardware and software halves. See `docs/REPO_LAYOUT.md`
for the rationale and the move history.

```
hardware/   KiCad projects (mainBoard, ThermocoupleBoard, SaftyThermocoupleBoard,
            UnitTestFixture), shared lib/, datasheets/, simulation/, sourcing/
firmware/   KilnFW (ESP32-S3), SaftyFW (RP2040), CommonFW (shared link code)
tools/      PcTools (GUI + MCP for BOTH processors)
docs/       System-level documents spanning both halves
```

`mykicadMcp/` moved to `tools/mykicadMcp/` 2026-08-28 (its own dedicated pass:
server stopped, submodule remounted at the new `.gitmodules` path, every
`.claude/settings.json` allowlist entry walked one at a time rather than bulk-
edited). `pdfMcp/` moved to `tools/pdfMcp/` the same day — its own running
server process couldn't be moved out from under itself mid-session, so that
directory was copied rather than renamed, and `tools/pdfMcp/.venv`'s shims
still resolved to the root venv's interpreter. The leftover root
`mykicadMcp/` remnant was deleted 2026-09-06. As of 2026-09-06 six live
`pdf-mcp.exe`/`python.exe` processes still held both the root `pdfMcp/`
and `tools/pdfMcp/.venv`, so a self-referential replacement was built
side-by-side at `tools/pdfMcp/.venv_new` instead (`pip install
pdf-mcp==2.0.0`, shebangs confirmed pointing at `.venv_new`). **Swap done**:
`tools/pdfMcp/.venv_new` has since been renamed to `tools/pdfMcp/.venv` and
the root `pdfMcp/` copy deleted — `.mcp.json` already pointed only at
`tools/pdfMcp/.venv/Scripts/pdf-mcp.exe`, so no config change was needed.

All main-board KiCad project files live under **hardware/mainBoard/** (paths below are relative to that
directory unless noted). A second, independent board — the 3-channel thermocouple daughterboard —
lives under **hardware/ThermocoupleBoard/** with its own `.kicad_pro`/`.kicad_pcb`/`.kicad_sch`
(MAX31856 instances U2–U4); its `Thermocouple.kicad_sch` sub-sheet is a copy of the same
single-channel circuit used on the main board's own (currently unused/not-in-hierarchy) copy of
that file, so a fix found in one project's copy often applies to the other's too. A third,
separate MAX31856 lives on the safety processor's own daughterboard,
**hardware/SaftyThermocoupleBoard/**, giving four MAX31856 devices in the system overall: three
feeding the ESP-S3 (KilnFW) and one feeding the RP2040 safety processor (SaftyFW).

### Schematics (hardware/mainBoard/)
- **hardware/mainBoard/kiln.kicad_sch** — Main schematic file; top-level hierarchy
- **hardware/mainBoard/MainControler.kicad_sch** — ESP32-S3-DevKitC main processor
- **hardware/mainBoard/Thermocouple.kicad_sch** — single-channel MAX31856 reference circuit (U6); present on disk but not instantiated by `kiln.kicad_sch` — the main board itself has no populated thermocouple channels, all of which live on the daughterboards above
- **hardware/mainBoard/Regulators.kicad_sch** — Input power conditioning/protection and 5V/3.3V LDO regulators
- **hardware/mainBoard/5V_Regulator.kicad_sch** — Dedicated 5V regulation block
- **hardware/mainBoard/CurrentSense.kicad_sch** — Current monitoring circuitry
- **hardware/mainBoard/SSD.kicad_sch** — Seven-segment display interface
- **hardware/mainBoard/SaftyProcessor.kicad_sch** — Safety monitoring subsystem

### Board & Layout
- **hardware/mainBoard/kiln.kicad_pcb** — PCB layout and routing
- **hardware/mainBoard/kiln.kicad_prl** — KiCad project settings and layers configuration

### Component Data
- **hardware/mainBoard/parts/SamacSys_Parts.pretty/** — Component footprints
- **hardware/mainBoard/parts/SamacSys_Parts.3dshapes/** — 3D models for visualization and export
- **hardware/mainBoard/fp-lib-table** — Footprint library table

### Python Tools
`tools/mykicadMcp/` is a separate git submodule (github.com/budarriola/mykicadMcp) holding the MCP server and its supporting tools:
- **tools/mykicadMcp/kicad_pcb_tool.py** — Lightweight parser for PCB and netlist files; does not require KiCad runtime
- **tools/mykicadMcp/kicad_mouser_tool.py** — Mouser Search API sourcing/stock/pricing lookups
- **tools/mykicadMcp/kicad_ipc_tool.py** — Live-KiCad tools via the IPC API (`kicad-python`); requires a running KiCad session
- **tools/mykicadMcp/kicad_mcp_server.py** — MCP server for the KiCad tools; HTTP on 8766 by default,
  `--transport stdio` still available
- **tools/mykicadMcp/kicad_facade.py** — search taxonomy (groups, keywords, synonyms) for the facade
- **tools/mykicadMcp/mcpkit_registry.py** — vendored copy of `tools/PcTools/src/mcpkit/registry.py`;
  edit the original and re-vendor, never this copy
- **tools/mykicadMcp/requirements-mcp.txt** — Python dependencies (requires `mcp>=1.0.0`)
- **tools/mykicadMcp/README.md** — Full setup guide and tool reference for the MCP server

### MCP Server Tools
The KiCad MCP server holds 86 tools in 12 groups: inspection/netlist, schematic data, Mouser
sourcing, audits, hierarchical sheet groups, layout/placement, PCB groups, net classes & buses,
nets, route templates, live IPC tools, and server control. Like the hardware servers it publishes
a search facade, not the whole set — start with `kicad_help()` or `kicad_find(query=...)` and
reach the rest through `kicad_call` / `kicad_batch`:

```
kicad_help()
kicad_find(query="is this capacitor rated high enough")
kicad_call(name="inspect_kicad_project", args={"project_path":"hardware/mainBoard/kiln.kicad_pro"})
```

`inspect_kicad_project` and `get_kicad_ipc_status` stay directly published. Everything else —
`list_kicad_components`, `get_kicad_component`, `get_kicad_component_connections`,
`list_kicad_nets`, `get_kicad_net` and the rest — is one `kicad_call` away. See
**tools/mykicadMcp/README.md** and `tools/mykicadMcp/docs/mcp-tools/` for the full reference, and
**tools/mykicadMcp/NETCLASS_PLAN.md** for the net-class design doc.

The net classes & buses group supports bus detection, net-class proposal/creation, trace-cost
scoring (with live deviation measurement), bus corridor-area measurement, capacitor voltage
auditing, critical-net classification, connector detection, and `pcb_settings.json` management.

There is **no autorouter**: it was removed (`mykicadMcp` commit "remove autorouter engine, keep
reference-copy/template tools"). What remains is the reference-copy family —
`copy_kicad_component_routing`, `apply_kicad_route_template`, `diff_kicad_route_template` — which
replicates routing already drawn on one instance of a repeated block onto its siblings.

## Development Setup

### Python MCP Server
1. Ensure Python 3 is installed and in `PATH`
2. Activate the virtual environment:
   ```powershell
   tools\mykicadMcp\.venv\Scripts\Activate.ps1
   ```
3. Install dependencies (if needed):
   ```powershell
   pip install -r tools\mykicadMcp\requirements-mcp.txt
   ```
4. Test the MCP server:
   ```powershell
   python tools\mykicadMcp\kicad_mcp_server.py
   ```

### Using MCP with Claude Code
- Configure MCP in your editor using the server path: `python tools\mykicadMcp\kicad_mcp_server.py`
- Example tools: "List the components on the PCB", "Show me component R1 and its connections", "Provide details for net /MainControler/CLK"

### PcTools Dependencies
`tools/PcTools/uv.lock` is tracked in git and should be committed alongside any edits to `pyproject.toml`. The lockfile is kept in sync with declared dependencies and committed deliberately so dependency snapshots are reproducible across sessions.

## Firmware gotchas

Symbolize a crash against the ELF that matches the RUNNING image, not
`build/KilnCtrl.elf` — that path is whatever was built most recently and
produces confident, wrong line numbers once the board is running an older
flash. Use `KilnFW/elf_archive/KilnCtrl-<hash>.elf` (a sibling of `build/`,
since 2026-09-15 -- not inside it, so an `idf.py fullclean` can't wipe it),
matched by embedded build
timestamp against the board's `fw_build` — `find_crash_elf()` does this
lookup for you. As of 2026-09-11 the archive only ever contains ELFs a board
was actually flashed with: `archive_elf.cmake`'s POST_BUILD step used to also
copy every ordinary `idf.py build` output into this same directory, keyed
only by content hash, whether or not it was ever flashed (~1.3 GB / 68 files
found and removed 2026-09-10/11, of which only 2 had ever actually been
flashed) — it now only refreshes a `KilnCtrl-latest.elf` convenience pointer
at the most recently *linked* build (not necessarily flashed; still useful
before a first flash). The real archive is written by
`archive_kiln_elf()`/`archive_safty_elf()` (`tools/PcTools/src/kilnctrl/elf_archive.py`),
called only after a confirmed flash, and retention is provenance-based: an
entry that was actually flashed is kept regardless of age (an older flashed
build can still be the one running, see the OTA/`otadata` hazard above);
anything else is kept only for a short grace window before it becomes
eligible for pruning. `_prune`'s cap (`MAX_ARCHIVED_ELFS`, still 60) says so
loudly, never silently, on the rare occasion actual flash volume alone
exceeds it.

`KILNCTL_TOUCH_CAL_SWAP_XY` is inert on this board's FT6336U capacitive
panel — it only feeds the legacy resistive NS2009 path; the live knob is the
`KILNCTL_TOUCH_CAP_*` family. See `firmware/KilnFW/docs/PROJECT_STATUS.md`
"Hardware present on this bench unit" for the full explanation.

Run `tools/run_all_checks.ps1` with `-ExecutionPolicy Bypass`. Without it the
script fails to load, and the Bash tool still reports exit 0 for the wrapper
— an unbypassed run looks like a pass when nothing ran. Separately, SaftyFW
host-test builds need a short worktree path (e.g. `C:\wt\...`); the default
`.claude/worktrees/...` path overflows the MSVC command line.

As of 2026-09-23 it discovers 141 checks (`-ListOnly -AllowFewerChecks`,
recounted at this commit; the count drifts upward as checks are added, so
re-verify with a fresh `-ListOnly` run rather than trusting this number).
`a42ac369` added `check_lint_pages.ps1` so `lint_pages.js` -- previously only
reachable through `tools/verify.ps1`'s manual "lint" stage -- is now enforced
by the standing suite too;
re-verify with a fresh `-ListOnly` run since this count drifts as checks are
added) and runs them in three phases — phase 1 is the three full target
builds (`check_00_kilnfw_target_build.ps1`, `check_00_saftyfw_target_build.ps1`,
`check_00_kilnfw_recovery_target_build.ps1`) concurrently; phase 2 is
everything else throttled in parallel (`-MaxParallel`, default 8); phase 3
is `check_ui_responsive_sweep.ps1` alone (`-MaxParallel 1`, forced serial
since `eefff2dc` — it drives real headless Chrome over CDP and flaked under
phase 2's concurrent load) — so a full run finishes in under 3 minutes on
this 24-core machine (when no other session holds a build gate slot -- see
"Heavy builds" in `docs/agent_rules/COMMON.md`) instead of exceeding the
600s tool timeout. `-Only
<regex>`/`-Skip <regex>` filter by repo-relative path for iterating on one
check; `-Fast` skips the three phase-1 target builds and, separately,
`check_00_kilnfw_host_tests.ps1` (phase 2) for a caller that already ran the
equivalent multi-minute build/host-test work itself — no other check.
(**2026-09-20 history:** the KilnFW application build briefly gained a
build-order dependency here, once it started `EMBED_FILES`ing both SaftyFW
slot images so the ESP can auto-update the Pico at boot
(`docs/PICO_AUTO_UPDATE_PLAN.md`) — `check_00_kilnfw_target_build.ps1`
originally read those slot bins out of the shared, non-isolated
`firmware/SaftyFW/build/` directory, so phase 1 was briefly split into a
serialized 1a (`check_00_saftyfw_target_build.ps1` alone) / 1b (the two
KilnFW builds) to avoid embedding a stale or in-flight pair. That split was
reverted once `check_00_kilnfw_target_build.ps1` started building the slot
images it embeds inside its own isolated checkbuild worktree instead, which
removed the dependency; the three target builds are independent again and
phase 1 is flat, as above. `build_kilnfw()` — the separate MCP tool in
`tools/PcTools/src/mcpkit/workbench.py`, not this standing check — still
builds SaftyFW before KilnFW, since that tool reads the shared,
non-isolated `firmware/SaftyFW/build/` directory and the real dependency
still applies there.)
`check_embedded_pico_image_fresh.ps1` may **SKIP** whenever
`firmware/SaftyFW/build/SaftyFW_slotA.bin`/`SaftyFW_slotB.bin` haven't been
built yet, including under `-Fast` (which skips
`check_00_saftyfw_target_build.ps1`, the check that produces them) — this is
the same class of expected `-Fast` SKIP as `check_recovery_image_size.ps1`
and `check_web_gzip_parity.ps1`.
**`-Fast` SKIPs are non-fatal (2026-09-23), and this is no longer a caveat
to work around.** `-Fast` sets `KILNCTL_CHECKS_FAST` in the environment; the
three checks above test it and, only for the one SKIP reason that is a
direct consequence of the phase-1 build `-Fast` itself skipped (a missing
`recovery.bin`, missing SaftyFW slot bins, or a missing KilnFW build-output
directory), print `SKIP-FAST: ...` instead of `SKIP: ...`.
`run_all_checks.ps1` files that into its own bucket, reports it separately
in the summary ("N skipped due to -Fast"), and never fails the run over it —
without needing `-AllowSkips`, and without weakening the fact that a SKIP
for any other reason (a genuinely missing toolchain, a 0-byte/in-flight ELF,
etc.) **still fails the overall run by default**; `-AllowSkips` still opts
into treating those as non-fatal on a machine that genuinely and permanently
lacks a prerequisite.

A 2026-09-04 panic (`safety_poll`, `IllegalInstruction`, `exc_addr 0x0`) ran
five hours unnoticed before `get_heap_status` was fixed to surface it (see
the flash/OTA section above). `exc_addr 0x0` was a red herring: the real
cause was a deliberate `abort()` from FreeRTOS's `configASSERT(pxQueue->uxItemSize
== 0)` in `queue.c`, reached via `thermo_owner.c:286`'s `xSemaphoreTake` —
the semaphore handle was non-null but its `StaticSemaphore_t` storage had
been corrupted. Root cause: `s_lvgl_task_stack` (`lvgl_port.c:988`, 8192 B)
sits close behind `thermo_owner.c`'s `s_slots[]` in `.bss`, and the
pre-`51e1ef5` reentrant `lv_obj_invalidate()`-inside-flush-callback path
(see that commit) could run the LVGL task stack deep enough to corrupt it.
Fixed by `51e1ef5` and flashed.

After splitting an oversized firmware file: grep `tools/`, `firmware/*/tools/`
and `tools/PcTools/tests/` for the old filename *and* any renamed identifiers,
then re-run every `check_*.ps1` and lint script — not just the build and host
tests. Nine of twelve 2026-09-04 splits broke a check/test silently this way
(one hardcoded-path guard was `if not path.is_file(): skipTest(...)`, so it
reported green with zero coverage), and separately, prefix-rename every symbol
widened from `static` even when the grep is clean — a same-named global can
collide silently with an unrelated `static` elsewhere. See `b9a5112` for a
worked example of both.

A negative test must end with a **forced full rebuild**, not merely a hand-restore
and an empty `git diff`. `ed854ac5`'s negative test flipped `RULE_TABLE[1][1]`'s
Kp direction in `pid_fuzzy.c`, rebuilt all 37 host-test executables, confirmed
failure, then restored the source by hand and confirmed an empty `git diff` — correct
procedure by the letter, but the poisoned `kilnctl_sim_fuzzy_closedloop.exe` binary
survived the revert sitting in the build directory. Ten minutes later `ba230bca`
read that prebuilt `.exe` instead of rebuilding and measured six numbers from
sabotaged code, which became a committed verdict and reached the project owner
before `docs/audits/review_sim_fuzzy_commits_2026-09-13.md` (`8a12521b`)
root-caused it. An empty `git diff` proves the *source* is restored; it says
nothing about build artifacts. Never measure from a prebuilt binary whose
provenance (what source state actually produced it) isn't established —
rebuild first.

KilnFW's `boot_guard.h` RECOVERY MODE deliberately skips starting subsystems
(`profile_executor`, `autotune_engine`), so a task started unconditionally in
`main_boot_early.c` must gate on `boot_guard_is_recovery_mode()` before calling
into one. Today's accessors are prestart-hardened (they check their lock for
NULL), so the gate is defence in depth rather than the only thing standing
between you and a hang — but the board has been bricked into a permanent
recovery loop THREE times from this area (`e7b8efc`, 2026-08-22, and
2026-09-08 — see `docs/audits/boot_guard_recovery_loop_2026-09-08.md`).

The 2026-09-08 instance was a different failure shape from the first two:
`boot_guard_mark_healthy()`'s NVS write reported `HAL_OK` while the
persisted count never actually reached 0 — confirmed via a live JTAG read
of `s_bg` showing `healthy_marked=true` in RAM on a boot whose successor
still loaded the pre-clear count, on a board that was healthy (NVS/web/OTA
all up, no firing, relays off) for 11+ minutes and, separately, across 5
password-authenticated `POST /api/ota/esp/recovery_exit` calls over 8
minutes — the explicit operator escape hatch was just as affected as the
automatic path, since both call the same function. **`boot_guard_mark_healthy()`
now returns `bool` and only reports success once a read-back
(`verify_persisted_count()`) confirms the clear, with one bounded
erase-then-retry before giving up; `main_ota_rollback_confirm_task()`
(`main_network_http.c`) no longer deletes itself after a single unverified
attempt — it keeps polling (existing 500 ms cadence) until the clear
verifies.** Do not trust a boot_guard NVS write's return code alone anywhere
in this module again — this is the same "logging unchecked success" class
flagged elsewhere in this file, just with an unreliable return code rather
than merely an uninspected one. No HTTP endpoint currently exposes the raw
boot_guard count (`GET /api/ota/esp/status`'s `recovery_mode` is a boolean,
fixed for the boot) — verifying this class of fix without a reflash
required a JTAG memory read of `s_bg`, resolved via `pyelftools` against the
matching ELF; a `/api/boot_guard` diagnostics route is a reasonable follow-up.

A second, separate 2026-09-08 finding
(`docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md`) is why boards get INTO
recovery mode from ordinary development flashing in the first place: `boot_confirm_is_healthy()`
(which gates the automatic counter clear) depends on `nvs_report_capture()`'s one-shot,
never-retried snapshot of three NVS partitions taken once early in
`main_network_http_bringup()`. A boot that samples that snapshot during a transient window —
plausibly moments after `flash_firmware()` resets the chip — never gets a second chance to
confirm healthy that boot, even though `boot_guard`'s own counter keeps incrementing and
persisting correctly (it owns an independent NVS handle, opened earlier in
`main_boot_early.c`). A few ordinary flashes in a row can walk a perfectly healthy board into
recovery mode this way. Fix: `boot_guard_reset_counter()` (`boot_guard.c`/`.h`), a deliberate,
externally-triggered clear that bypasses that flaky snapshot entirely, sharing its actual
verified-clear-with-retry logic with `boot_guard_mark_healthy()` via one common helper so the
`0b5d9dad` write-lies fix covers both paths. It is meant to be called by a TOOL that knows it
just performed a deliberate flash (`flash_firmware()`'s verify step — wired up in `b09294fb`,
2026-09-09: a new authenticated `POST /api/ota/esp/boot_guard_reset` route
(`ota_http_recovery.c`), plus an `ap_password` parameter on `flash_firmware()`
(`tools/PcTools/src/kilnctrl/mcp_server_flash.py`) that calls it ONLY after post-flash
verification confirms full, unambiguous success — never on a raise, a WARNING, or
`verify=False`. **Owner decision 2026-09-19: this is now DEFAULT ON, not opt-in** — a
caller who omits `ap_password` falls back to the `KILNCTL_AP_PASSWORD` environment
variable, the board's **AP Wi-Fi password** — distinct from, and never equal to, the
web admin password (`web_auth_store.c:157`), since the `boot_guard_reset` route verifies
its HMAC keyed on the AP password specifically; a caller with no credential gets the
pre-`b09294fb` behavior plus one line in the result noting the reset was skipped for lack
of credentials. Pass `reset_boot_guard=False` to opt out unconditionally regardless of
credentials. The password is never logged or echoed.
`flash_firmware()`'s result reports the counter's before value, the clear result, and the
verified-or-not after value, never a silent clear. A `GET /api/boot_guard` diagnostics route
also landed in the same commit, exposing `{"boot_count","recovery_mode"}` unauthenticated so
this class of fix no longer needs a JTAG read of `s_bg` to verify), never from inside an
unconditional firmware boot path: a negative
test proved that wiring it into every `boot_guard_init()` call instead defeats the counter
entirely, masking a genuinely failing board. Firmware cannot itself distinguish "a developer
just flashed this" from "this board is quietly reset-looping" — only the tool knows which one
just happened.

Both of the first two times the real fault was a task stack, not the missing gate: a 700-byte
overflow corrupted the heap, and the pool walk then looped inside a critical
section until the interrupt watchdog fired. **Register every new task for
stack-margin reporting and measure it** — `check_stack_margin_registration.ps1`
enforces this. Equally, never hold a module lock across the producer calls a
policy tick makes (`dashboard_get_status()` alone does three MAX31856 SPI reads,
a 200 ms-capable queue wait and four interrupts-disabled heap walks): cache a
snapshot outside the lock instead (`7a8594d`).

**The URI handler cap has essentially no headroom left.** `check_uri_handler_cap.ps1`
(as of 2026-09-23, recounted after the iter_tune step-7-review commit) reports 160 `httpd_uri_t` routes
registered under `firmware/KilnFW/App/drivers/*.c` against
`wifi_provision_http.c`'s `config.max_uri_handlers = 170` — 10 spare slots.
The next route added
anywhere under `drivers/` will need that cap bumped in the same change, or
the check fails; see the check script's own header comment for why this is a
compile-time array size shared by every build configuration (including
`CONFIG_KILNCTL_SIM_PLANT`'s two extra routes) and why past bumps were
allowed to fall behind three times running before this check existed.

**"Reset one side of a pair" bug class — no mechanical check, review by hand.**
Four confirmed instances so far, all silent, all cost real debugging time:
the PC-side `msg_index` restarting per benchproto connection while firmware's
dedup ring/ACK cache lives for the MCU's boot lifetime; SimFW's
`apply_reset()` zeroing `s_ring_next_seq` while `telemetry.c`'s consumer
cursor was only ever initialised at boot (stranded cursor, `drain()` returns
0 forever); `fault_sched.c` reseeding `s_seed` on `SET_SEED` while
`fault_engine_t.rng_state` stayed hardcoded to `0` from
`fault_engine_init(&s_engine, 0)` (every scenario's `seed:` was cosmetic);
and `safety_link_frames.c`'s Pico-reboot handling, where `trip_seq` restarts
at 0 on the Pico but the ESP's dedup `trip_last_seq` did not — fixed in the
2026-08-27 audit (see the comment at `safety_link_frames.c` around the
`boot_id_changed` block, which now explicitly clears `trip_last_seq` and
names this class).

What the four share structurally: two pieces of state, in different modules
(sometimes different processes/processors entirely), joined by a semantic
contract — equality, a monotonic derivation, a shared seed — that is never
expressed as a shared type or a single owning function. A reset/reinit event
is naturally written against only ONE side (the side whose lifecycle event it
is: a new connection, a sim reset, a reseed command, a reboot), and nothing
forces the other side's dependent state to be revisited. Both sides stay
internally consistent afterward — no crash, no assertion — so only the
*relationship* is broken, and it fails silently: health counters on the
stalled side often read perfectly clean (`evt_seq_gap_count: 0`) precisely
*because* nothing downstream of the break ever ran again. (One example fix,
`reset_client_evt_cursors()`, lived in the SimFW/`kilnsim` reference model
before that tree was deleted 2026-08-28 — no replacement reference model
exists today, so this class currently has to be spotted by inspection, not
by diffing against a maintained comparison implementation.)

This was evaluated for a mechanical `check_*` and rejected: the four
instances have no unifying syntactic shape (a per-connection PC/firmware
pair; a struct field skipped by one initialiser but not its sibling; an RNG
seed shadowed by an unrelated hardcoded one; a reboot-driven dedup counter)
and no naming convention connects the two sides of any of them — a regex or
AST rule general enough to catch all four would also flag the large majority
of ordinary, correctly one-sided resets in this codebase (most reset
functions have no paired counterpart at all — see `heater_output_reset()`
zeroing its window state alone, which is correct: nothing outside it derives
from that window). A *narrow* check pinned to one specific pair (e.g. a
mirror-drift check like `approach_rate_cap_mirror_drift_check.py` uses) is
honest only once a concrete pair is nailed down and stable; the current live
candidate for one (`thermal_guard.c`'s window state vs. the PWM-chopping
behaviour under investigation as of 2026-09-04) is mid-edit in another
session's pass, so writing a check against it now would either duplicate
that fix or break under it. **Standing practice instead:** whenever code
resets a counter, window, timestamp, or seed, ask explicitly "who else holds
a copy or a derived expectation of this?" before committing — the four
instances above are the checklist.

## Key Architecture Notes

### Hierarchical Schematic Design
The project uses a hierarchical schematic structure where sub-sheets (ADC, Thermocouple, etc.) are instantiated in the main schematic. This allows modular design and easier debugging of subsystems.

### Multi-Channel Thermocouple Interface
Three MAX31856 converters (U2–U4, on `hardware/ThermocoupleBoard/`) provide the ESP-S3's
independent zone thermocouple monitoring with built-in cold-junction compensation, each on its
own instance of the same schematic sheet. A fourth MAX31856 on `hardware/SaftyThermocoupleBoard/`
feeds the RP2040 safety processor's own, separate sensor. The main board itself (`hardware/mainBoard/`)
has no populated thermocouple channels — see the Schematics list above.

### Power Distribution
Three voltage rails:
- **12V input** (from external supply)
- **5V** (main logic and relay coils)
- **3.3V** (microcontroller I/O and sensors)

Input protection uses TVS diodes (SMAJ24CA) and current-limiting resistors. See hardware/mainBoard/Regulators.kicad_sch.

### Safety Processor
A separate safety processor monitors critical parameters and can disable the main controller if needed. Isolated communication via relay feedback circuits.

### Data Access Pattern
Use Python tools (kicad_pcb_tool.py) when you need to read PCB data, netlist connectivity, or component properties without opening the GUI. The parser reads `.kicad_pcb` and auto-generated netlist files directly.

## Common Tasks

### View the PCB or Schematic
Open with KiCad:
```powershell
kicad hardware\mainBoard\kiln.kicad_pcb
kicad hardware\mainBoard\kiln.kicad_sch
```

### Replicate Routing Across Repeated Blocks
There is no autorouter in this repo any more -- it was removed from `mykicadMcp`, and
`kicad_router_tool.py` no longer exists. Route by hand in KiCad, then replicate that work onto the
sibling instances of a repeated block (the five identical thermocouple channels, for example):

```
kicad_call(name="copy_kicad_component_routing", args={
  "project_path": "hardware/mainBoard/kiln.kicad_pro",
  "template_reference": "U10", "target_reference": "U11"})
kicad_call(name="diff_kicad_route_template", args={...})   # preview before applying
kicad_call(name="apply_kicad_route_template", args={...})
```

Always diff before applying. The same template pattern exists for placement
(`*_layout_template`), reference-designator positions (`*_property_position_template`) and
footprint flips (`*_flip_template`) -- `kicad_find(query="template")` lists them all.

### Query Component or Net Information
Use the MCP server tools or call Python directly:
```powershell
python tools\mykicadMcp\kicad_pcb_tool.py
```

### Update the BOM
No BOM CSV is checked into the repo currently. Regenerate one via KiCad's built-in BOM generator
(part numbers, values, footprints, Mouser links) or query components live through
`kicad_call(name="list_kicad_components", ...)`.

### Check Design Rule Violations
In KiCad: **Tools → Design Rule Checker** or press `Shift+I`. Refer to hardware/mainBoard/JLCPCB.kicad_dru for manufacturing rules if fabricating at JLCPCB.

## Notes for AI Assistants

- The schematic files (`.kicad_sch`) are large text-based files; use Python tools to query data rather than reading raw files.
- Component designators (R1, U1, etc.) in a regenerated BOM match those on the schematic and PCB.
- Footprints are organized in `hardware/mainBoard/parts/SamacSys_Parts.pretty/`; do not modify these directly unless sourcing new parts.
- The MCP server is useful for scripting or integration with other tools; most editing should happen in KiCad GUI.
