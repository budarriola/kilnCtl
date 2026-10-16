# Firmware gotchas (standing rules)

Split out of CLAUDE.md 2026-10-14; history in docs/CLAUDE_HISTORY.md.

## Crash symbolization

Use `KilnFW/elf_archive/KilnCtrl-<hash>.elf` matched by embedded build timestamp to the board's
`fw_build` (`find_crash_elf()` does it), never `build/KilnCtrl.elf` (whatever was built last).
The archive holds only flashed ELFs (`archive_kiln_elf()`/`archive_safty_elf()`); never delete it.

- **boot_guard:** RECOVERY MODE skips starting subsystems (`profile_executor`, `autotune_engine`),
  so a task started unconditionally in `main_boot_early.c` must gate on
  `boot_guard_is_recovery_mode()`. The board has been bricked into a recovery loop three times
  from this area. Never trust a boot_guard NVS write's return code alone: success needs the
  read-back (`verify_persisted_count()`). `GET /api/boot_guard` reports `boot_count` (fixed per
  boot) and `persisted_count` (live NVS re-read, the field that moves; GET 0 merges "cleared"
  with "never written").
- **Task stacks:** register every new task for stack-margin reporting and measure it
  (`check_stack_margin_registration.ps1`). Too-small stacks may be raised without asking.
- **Never hold a module lock across producer calls** (e.g. `dashboard_get_status()` does SPI reads,
  queue waits and interrupts-disabled heap walks): cache a snapshot outside the lock.
- **URI handler cap:** `check_uri_handler_cap.ps1` compares `httpd_uri_t` routes under
  `firmware/KilnFW/App/drivers/*.c` with `wifi_provision_http.c`'s `config.max_uri_handlers`
  (184 vs 176 registered on 2026-10-09, ~8 spare; also needs `http_auth_http.c`'s
  `KILN_HTTP_MAX_ROUTES` >= both). Any new route needs the cap bumped in the same change.
- **"Reset one side of a pair" bug class** (no mechanical check, review by hand): when code resets a
  counter, window, timestamp or seed, ask "who else holds a copy or derived expectation of
  this?" before committing. Past instances: PC `msg_index` vs firmware dedup ring; SimFW ring
  seq vs telemetry cursor; fault-engine seed vs hardcoded `rng_state`; Pico `trip_seq` restart vs
  ESP `trip_last_seq`.
- **After splitting an oversized firmware file:** grep `tools/`, `firmware/*/tools/` and
  `tools/PcTools/tests/` for the old filename and renamed identifiers, re-run every `check_*.ps1`
  and lint script (not just build/host tests; path-keyed checks break silently, and a
  `skipTest` on a missing path reports green with zero coverage). Prefix-rename any symbol
  widened from `static`.
- `KILNCTL_TOUCH_CAL_SWAP_XY` is inert on the FT6336U capacitive panel; the live knob is the
  `KILNCTL_TOUCH_CAP_*` family (`firmware/KilnFW/docs/PROJECT_STATUS.md`).
