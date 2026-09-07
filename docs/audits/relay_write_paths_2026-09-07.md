# Relay write-path audit, 2026-09-07

Sweep for the "bypassed owner module" bug class (memory:
`project_bypassed_owner_module_bug_class.md`) against `firmware/KilnFW/App`:
every call that can change physical relay state, and whether it goes through
`kiln_io_owner` with an authority or bypasses it.

## Scope

The only functions that reach the SX1509 relay bits are `kiln_io_set_relay()`,
`kiln_io_set_relay_mask()`, and `kiln_io_all_relays_off()` (`drivers/owners/
kiln_io.h`). One layer lower, `SX1509_write_masked/_port/_pin` are called only
from `kiln_io.c` itself (grepped repo-wide) and `SX1509.c`'s own definitions,
so there is no lower-level bypass to chase separately.

## Table

| File:line | Path | Owner-routed | Notes |
|---|---|---|---|
| `drivers/owners/kiln_io.c:262,270,312` | defines `kiln_io_set_relay`/`_mask`/`_all_relays_off` | n/a | the primitives themselves |
| `drivers/owners/kiln_io_owner.c:304,328,357,368` | owner task's own dispatch loop | YES (is the owner) | the one module allowed to call the primitives directly |
| `drivers/bridge/uart_bridge_io.c:201,264,311` | UART `SET_RELAY`/`SET_RELAY_MASK`/`SET_IO` handlers | YES | `kiln_io_owner_command_set_relay[_mask]`/`set_io` (MANUAL, ownership+safety gated) |
| `drivers/bridge/uart_bridge.c:469` | link-loss watchdog, unowned-relay drop | YES | `kiln_io_owner_command_set_relay_mask_authorized()` (2026-08-28 fix; see header doc, watchdog only ever pairs `unowned_mask` with value=0) |
| `drivers/http/dashboard_http.c:556` | HTTP/LCD manual relay toggle (`dashboard_set_relay`) | YES | `kiln_io_owner_command_set_relay()` (MANUAL) |
| `drivers/control/profile_executor_relay_io.c:97,353,402,648,747` | PID/time-proportioning relay + IO writes | YES | `kiln_io_owner_command_set_relay_mask_authorized()`/`set_io()` (AUTHORIZED; caller already holds zone ownership via `relay_authority_zone_blocked()`) |
| `drivers/control/autotune_engine_guard.c:120` | autotune relay drive | YES | `kiln_io_owner_command_set_relay_mask_authorized()` |
| `drivers/control/zones_current_sweep_engine.c:670` | current-sense commissioning sweep | YES | `kiln_io_owner_command_set_relay_mask()` (MANUAL) |
| `drivers/ui/ui_page_temperature.c` | LCD manual relay buttons | YES (indirect) | calls `dashboard_set_relay()`, never `kiln_io`/`relay_authority` directly (file's own header comment) |
| `drivers/control/profile_executor.c:1394,1445,1466` | guard 9 / FAULT / RETRY_RELAYS_OFF in `watchdog_task_entry()` | NO — direct `kiln_io_all_relays_off()` | **Legitimate.** Must force relays off when the control task itself has stopped ticking; routing through the owner queue would make the fail-safe depend on the very task whose liveness is in question. Unconditional, OFF-only, so a race with `owner_task` is benign. |
| `main.c:164` (`main_kiln_enter_safe_state`) | panic/shutdown path | NO — direct `kiln_io_all_relays_off()` | **Legitimate.** Same reasoning: must still run if the owner task is wedged. |

No call site in production code touches `kiln_io_set_relay()`/`kiln_io_set_relay_mask()` (as opposed to `_all_relays_off()`) from outside `kiln_io.c`/`kiln_io_owner.c`.

## Classification

- **Owner-routed (11 call sites across 8 files):** correct, no action.
- **Legitimate bypasses (2 files, `profile_executor.c` watchdog + `main.c` panic path):** both are the documented "must survive a wedged owner task" fail-safe class from `kiln_io_owner.h`'s own top comment, both unconditional-OFF, both already reasoned about by prior audits. No fix needed.
- **Genuine offenders found: none.** Every relay-write call site in `firmware/KilnFW/App` was already either owner-routed or one of the two documented fail-safe exceptions before this pass started. No code fix was required.

## Check

`tools/check_relay_authority_paths.py` covered only the PC-side `IoClient`
wrapper contract (`tools/PcTools/src`, `tools/PcTools/scripts`). It has been
**extended** (not duplicated) to also scan `firmware/KilnFW/App` and flag:

- any direct `kiln_io_set_relay()`/`kiln_io_set_relay_mask()` call outside
  `kiln_io.c`/`kiln_io_owner.c` (zero-tolerance, no allowlist available), and
- any direct `kiln_io_all_relays_off()` call outside `kiln_io.c`/
  `kiln_io_owner.c` and outside a small, named allowlist
  (`main.c:main_kiln_enter_safe_state`, `profile_executor.c:
  watchdog_task_entry`).

`tools/check_relay_authority_paths.ps1` already exists and is discovered by
`run_all_checks.ps1`'s `check_*.ps1` glob, so no new registration was needed.

**Negative test performed by hand:** added
`kiln_io_set_relay(s_dash.io, relay_index, on);` directly inside
`dashboard_http.c`'s `dashboard_set_relay()` (bypassing
`kiln_io_owner_command_set_relay()`). The check failed with:

```
check_relay_authority_paths: relay-write bypass(es) found:
  ...dashboard_http.c:556: direct kiln_io_set_relay() call bypasses kiln_io_owner -- ...
```

(An earlier version of the block-comment stripper collapsed multi-line
comments to zero lines, which shifted the reported line number for every
match after one; fixed by replacing each stripped comment with the same
number of newlines it spanned, so reported line numbers stay aligned with
the source file — caught by this same negative test.)

The injected line was then removed by hand and `git diff` on
`dashboard_http.c` confirmed empty before the check was re-run clean.
