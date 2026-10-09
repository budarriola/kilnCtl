# Review: crash-report manual relay gate (`61765de7`), 2026-09-15

Scope: the option-B decision (`manual_relay_readiness_gating_options_2026-09-15.md`).
Read-only review. Host tests were run in a clean detached worktree at `61765de7`.

## Verdict

No blocking defects. The gate does what the owner decided. Findings below are LOW or
MEDIUM (MEDIUM means an operator-experience gap, not a safety gap).

## Questions checked

**Does it gate AUTOMATIC paths?** No. profile_executor and autotune send
`CMD_SET_RELAY_MASK_AUTHORIZED` (`kiln_io_owner.c` owner_task), which never calls
`relay_on_blocked()`. So a crash report can't chop relays mid-firing. A fault loop isn't
possible either: the flag only becomes true in `crash_report_init()` at boot, and a new
crash reboots the board anyway. The CT sweep is covered by the gate on purpose: it uses the
MANUAL `kiln_io_owner_command_set_relay_mask` (`zones_current_sweep_engine.c`). The gate
also covers raw `CMD_SX_WRITE_REG` through `sx_write_reg_touches_relay_on()`. That is
consistent with the other gates.

**Is the flag valid when it is read?** `crash_report_init()` runs in `main_boot_early.c`
before boot_guard and before owner start, on both normal and RECOVERY boots. It refreshes
the flag from NVS before the "no coredump" early return, so an old unacknowledged record
still gates.
- Fail-open case: if `nvs_partition_init` fails, init returns before the refresh and the
  flag stays false. The readiness gate reads `crash_report_get()` against the same partition,
  so both fail the same way. **LOW**, accepted.
- A CRC-corrupt record reads as no record in both places. Consistent.

**Can it read true forever (lockout)?**
- `crash_report_clear()` wasn't modified. It relies on its internal `crash_report_acknowledge()`.
  If that acknowledge write fails but the key erase that follows succeeds, the record is gone
  and the flag stays true until the next reboot. The reboot fixes it (the refresh reads no
  record), and danger mode bypasses the gate meanwhile. **LOW.** Fix: after the erase, set
  the flag false or call `refresh_unacked_cache()`.
- If the acknowledge NVS write fails, `/api/crash_report/ack` returns 409 "no crash record to
  acknowledge". That message is wrong in this case and the operator stays locked out with no
  real reason given. **LOW.**

**Lock order / blocking:** the gate only reads a static bool: no I/O, no lock. The bool is
written from httpd and read on owner_task; a single-byte bool is benign here. No findings.

**Can the operator acknowledge?** Only from the web Diagnostics page
(`diagnostics_page.html`, then `POST /api/crash_report/ack` or `/clear`). The LCD has no
acknowledge control, but the LCD refusal text tells the operator to acknowledge. An LCD-only
operator without network access can't clear the gate except through danger mode, which is
also web-only. **MEDIUM (usability).** Options: add an LCD acknowledge, or name the web page
in the LCD message.

**Does the refusal show up correctly?**
- LCD override: distinct message. OK.
- benchproto SET_RELAY / SET_RELAY_MASK: rejected with `"crash_unacked"`. OK.
- Danger route: the gate is bypassed, and the bypass is logged. OK.
- CT sweep: returns `ZONE_SWEEP_ZONE_ENERGIZE_REFUSED` with `safety_sources = 0`, so the
  crash-report reason is lost and shows as "refused, sources 0x00". The OTA refusal already
  has this gap. **LOW.**

**Consumer without a producer?** No. The production producer is `crash_report_init()`, which
does a boot refresh plus a refresh after capture. Acknowledge and clear are the clearing
producers.

**Do the tests exercise the real function?** Yes. `test_kiln_io_owner.c` includes
`kiln_io_owner.c` and calls the real static `relay_on_blocked()`. `test_crash_report.c`
drives the real cache through init, acknowledge and clear. Not covered:
- the result mapping in `handle_set_relay`/`handle_set_relay_mask` (updating beats crash beats safety)
- the danger-mode bypass
- the acknowledge-failure path

All **LOW**.

## Negative test (independent)

Clean worktree at `61765de7`:
1. Baseline build: 45/45 built and passed.
2. Changed line 227 of `kiln_io_owner.c` to `if (0 && crash_report_has_unacknowledged())`.
   Rebuilt into a fresh output directory. Result: RUN FAILURES (1): `kiln_io_owner`.
3. Restored the line by hand with the reverse substitution. `git diff` was 0 bytes.
4. Rebuilt into another fresh output directory: 45/45 passed.

The worktree was then removed. Nothing was flashed.
