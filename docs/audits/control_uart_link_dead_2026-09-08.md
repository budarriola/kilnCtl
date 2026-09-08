# CONTROL UART link (COM14) dead this session (2026-09-08)

## Is this expected recovery-mode behaviour?

No. `GET /api/ota/esp/status` confirms `"recovery_mode":true`, but
`firmware/KilnFW/App/main_bridges_bringup.c` shows every UART bridge task
(info/system/thermo/io/safety/control/profiles/autotune/wifi/gpio_probe) is
gated only on `ctx->pc_link_ready`, which is set in
`main_network_http.c` purely by whether `uart_owner_init()` /
`uart_protocol_init()` succeeded -- it has no dependency on
`ctx->recovery_mode`. The only things recovery mode skips are the LVGL/LCD UI
and the UI-test bridge task (explicitly commented: "recovery mode is defined
as Wi-Fi and the OTA routes... the panel stays blank"). So a dead CONTROL
link is not an expected side effect of recovery mode per the current source.

## Localization: PC vs USB vs firmware

- **USB/physical**: healthy. `[System.IO.Ports.SerialPort]::GetPortNames()`
  lists COM14; `Get-CimInstance Win32_PnPEntity` identifies it as
  `USB-SERIAL CH340K (COM14)`, Status OK (and COM10, the Pico CMSIS-DAP
  probe, is separately present and OK, so port identity is not confused
  between the two probes). Opening COM14 directly with a throwaway
  `System.IO.Ports.SerialPort` succeeded immediately -- nothing else holds it.
- **PC/MCP server**: not stale. `mcp_servers.ps1 status` reports `kilnctrl`
  UP, **fresh**, started 2026-09-07 13:13:10 at commit `c4b4e65d`, matching
  current code -- no restart needed, and none was performed.
- **Firmware**: this is where the fault is, but it is not the UART bridge
  task itself. All UART request types (INFO/THERMO/IO/SAFETY/WIFI/CONTROL/
  PROFILES/AUTOTUNE) fail identically with "link or peer is down" -- not a
  per-task pattern, the whole link is down. Meanwhile HTTP is fully
  responsive on `192.168.1.156` throughout.

## Root cause found: the board is in an active panic/reboot loop

`GET /api/status` polled twice, seconds apart, shows uptime **decreasing**
between polls (811 s -> 389 s -> 48 s), with `reset_reason: panic/exception`
on every boot and an unacknowledged crash report:
`exc_task='main' exc_cause_str='LoadProhibited'`. The board is rebooting on
the order of every ~1 minute or faster. This is the `LoadProhibited` crash
that is off-limits for this task to root-cause, so it was not investigated
further -- but its *effect* fully explains the dead CONTROL link: no UART
session/handshake between the PC-side MCP server and the ESP can survive a
reboot that happens faster than the retry/backoff window, and every reboot
resets the firmware side's protocol state while the PC side has no reason to
know a reboot occurred mid-request. This is a live instance of the
"reset one side of a pair" bug class already known in this codebase, just
triggered by crash-driven reboots rather than a deliberate reset call.

`system_uart_bridge`'s previously-reported 916 B stack margin was not
directly implicated -- there is no evidence in the device log of that task
overflowing; the whole board is being torn down by the panic before any
single subsystem's health would matter.

## Bridge task state + margin

Could not be read: `get_stack_margin()` itself goes over the dead CONTROL
link and fails the same way as everything else. No HTTP-exposed equivalent
exists (same gap noted in `boot_guard_recovery_loop_2026-09-08.md` for the
boot_guard counter). Given the reboot cadence, a live snapshot would be stale
within seconds regardless.

## What would fix it

Not a server restart (already fresh), not recovery-mode escape (unrelated,
and `ota_recovery_exit_esp()` itself reboots the board -- of no help against
a board already reboot-looping), not a reflash (out of scope: the
`LoadProhibited` crash is explicitly off-limits, and per CLAUDE.md a
`debug_reset` would also destroy the RTC-slow-memory state another session's
recovery fix depends on -- not performed). The real fix belongs to whoever
owns the `LoadProhibited` investigation: once the board stops crash-looping
and holds an uptime longer than a UART handshake needs, this link should
recover on its own with no PC-side action required.

## Link recovered?

**No**, as of this check -- the board was mid-reboot-loop at time of writing
and remained so. Not attempted to force via reflash/reset per the
constraints above.

Board commit at time of check: `fc90f682` (dirty=true), from `factory`.
