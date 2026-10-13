# Bench findings 2026-10-09 (origin/dev 0dd056c6)

Context: `docs/BENCH_TEST_LOG.md` entry of the same date. Suites could not run (estop_verified gate), so these findings come from the build, flash and read-only checks only.

## 1. HIGH: KilnFW does not build at origin/dev 0dd056c6 (-Werror=format-truncation)
- Evidence: `backup_import.c:3590:22: error: ' configured); the backup doe...' directive output truncated writing 126 bytes into a region of size between 87 and 98 [-Werror=format-truncation=]` (`backup_import_zone_topology_precheck`, snprintf into `err_msg`/`err_cap`).
- Also a real defect: the message is longer than the smallest error buffer, so the refusal text is cut off.
- Repro: `build_kilnfw_start(kiln_fw_root=<worktree at origin/dev>)`. Log `kilnfw-build-1791609495.log`.
- Bench workaround (not committed): `#pragma GCC diagnostic ignored "-Wformat-truncation"` at the top of the file.

## 2. HIGH: KilnFW does not build: main.c calls undeclared cfg_fs_set_write_refuse_hook
- Evidence: `App/main.c:228:5: error: implicit declaration of function 'cfg_fs_set_write_refuse_hook'; did you mean 'cfg_fs_mount_set_write_refuse_hook'?` Introduced with 04848cd9 (declaration is in `cfg_fs.h`, `main.c` includes only `cfg_fs_mount.h`).
- Repro: same build, after working around finding 1. Log `kilnfw-build-1791609885.log`.
- Bench workaround (not committed): `#include "cfg_fs.h"` in main.c.
- Both 1 and 2 show origin/dev was never target-built at this tip; host tests and checks do not catch it.

## 3. MEDIUM (blocker): every bench_test suite is unrunnable while estop_verified is not_done
- Evidence: `PREFLIGHT FAILED: readiness gate blocks: estop_verified`, run `logs/bench_test/20261010T053127Z_smoke_benchdev`, 36/36 NOT_RUN. `get_readiness`: `not_done estop_verified: never confirmed`. Same result already logged earlier today for web and OT-G06.
- No bench-only bypass exists, and estop_verify must not be automated, so aux, web, lcd, static, ota, autotune and heat coverage of dev is zero until a human verifies the E-stop (record is erased by factory reset and by any committed estop_active_level param).

## 4. LOW: autotune start refusal text is truncated to 100 characters
- Evidence: `autotune_start(zone=0, method="step", step_duty_or_setpoint_c=0.2)` returned `refused: refused -- the E-STOP INTERLOCK has not been verified on this board. Run the bench procedure, t`. Full text in `readiness_gate.h:241` continues "...then confirm it on the Readiness page."
- Likely the UART autotune reply buffer or the PC tool cap; not located.

## 5. LOW/INFO: update_check ends 404 on the default repo
- Evidence: `update_check` -> `state=failed ... http_status=404 (404 = no release published in this repo)` for `budarriola/kilnCtl`. Tags `v1.0.0-pre.N` are on origin/release but a prerelease-only repo (or tags without GitHub releases) gives 404 from the latest-release endpoint. WP8 gate b (29556 B internal heap figure) could not be measured; TLS handshake did run with internal min_free unchanged at 22607 B.

## 6. INFO: link stats after reflash
- `safety_get_link_stats`: crc/framing errors 1, timeouts 1, length mismatch 2 of 721 frames sent. Probably reflash-time noise; not reproduced.

## Observations (not defects)
- Boot heap: internal min_free 22607 B at 214 s uptime (floor 8192 B). Previous firmware had 14371 B after 4297 s.
- Lowest stack headroom after boot: backlight_pwm 1152/3072, info_uart_bridge 1496/4096, wifi_prov_owner 1536/4096 B free.
