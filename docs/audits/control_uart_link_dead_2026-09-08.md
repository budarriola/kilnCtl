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

## RETRACTED: "active panic/reboot loop" root cause (superseded 2026-09-08, later pass)

An earlier version of this document claimed the board was in an active
panic/reboot loop, based on two `GET /api/status` polls that appeared to show
uptime *decreasing* (811 s -> 389 s -> 48 s). **That conclusion is wrong.** A
separate investigation polled `/api/status` every 10 s for 10 minutes and
found uptime rising monotonically on a single boot, with exactly one reboot
at 07:46:37 and none since. The apparent decrease was almost certainly two
different tools/hosts being compared (one of the samples above, `uptime_s=389`,
is actually the *OTA subsystem's* last-recorded value from the pico-update
attempt embedded in `/api/ota/esp/status`'s `last_update` block, not a fresh
`/api/status` poll -- the two were conflated). `reset_reason: panic` describes
the origin of the current, single, long-running boot, not a live loop. This
pass reconfirms that finding independently: `get_heap_status` /
`GET /api/status` against `192.168.1.156` now reports **uptime_s=3454**
(climbing across repeated polls in this session, e.g. 3372 -> 3454), a single
`reset_reason":"panic/exception"` from the boot that already happened, and
`diag_trip_mask:0` / `heat_block_sources:0` -- a stable board, not a
reboot-looping one. The "reset one side of a pair" explanation built on the
reboot-loop premise does not apply; the link fault needs re-localizing from
scratch, below.

## Re-localization (this pass, reboot-loop hypothesis removed)

**Is `pc_link_ready` true?** Inferred **yes**, not directly read. Both
`uart_owner_init()` and `uart_protocol_init()` (`main_network_http.c` lines
674-727) only fail down a path that calls `main_kiln_enter_safe_state()` with
`SAFETY_FAULT_SRC_PC_LINK`, which asserts the isolated fault line via
`safety_link_set_fault_source()`. Live `GET /api/status` shows
`heat_block_sources:0`, `heat_block_sources_words:"none"`, `diag_trip_mask:0`
-- no fault of any kind is currently latched, and specifically not a PC-link
one. If either UART init call had failed on this boot, that fault would be
visible here (it is safety-link-visible, not itself gated by the dead PC
link). This is the strongest evidence available without JTAG: `esp` has no
`nm` tool configured for this MCP server (`debug_list_symbols`/
`debug_read_symbol` both refuse for `peer="esp"`), so `ctx.pc_link_ready`
itself could not be read directly by symbol name without also resolving a
DWARF offset into the ELF by hand -- not attempted, since the fault-line
evidence above is already conclusive enough not to need a core halt.

**Do the bridge tasks exist?** Very likely yes, for the same reason: nothing
in `main_bridges_bringup.c` gates the info/system/thermo/io/safety/control/
profiles/autotune/wifi/gpio_probe bridge-task `xTaskCreate` calls on
`ctx->recovery_mode` -- verified again this pass by re-reading the file
(lines 41, 111, 139 all gate on `ctx->pc_link_ready` only, never
`recovery_mode`). Confirmed independently that `recovery_mode` (which IS true
on this boot -- `GET /api/ota/esp/status` returns `"recovery_mode":true`)
only skips `profile_executor_start()`/`autotune_engine_start()`
(`main_control_bringup.c` lines 160, 187) and the LVGL/UI + UI-test bridge
task (`main_bridges_bringup.c` lines 102, 119) -- nothing on the path that
creates the other nine bridge tasks or the underlying `uart_owner`/
`uart_proto` objects they're registered against.

**Does recovery mode gate anything the bridge depends on transitively?**
Checked the two things it does skip for a hidden dependency: profile executor
and autotune engine are read defensively by their own callers (this
codebase's own documented convention -- NULL-mutex-guarded prestart
accessors), and the bridge tasks that touch them already log a *specific*
`ESP_ERR_NO_MEM`/`ESP_ERR_INVALID_STATE` distinguishable failure rather than
going silent (see the control/profiles/autotune task start block's comments,
`main_bridges_bringup.c` ~140-167) -- that is a deterministic reply, not the
"no reply after all retries" timeout actually observed. **Every** request
type (INFO/THERMO/IO/SAFETY/WIFI/CONTROL/PROFILES/AUTOTUNE) fails identically
with a retry-exhausted timeout, including types with zero dependency on
profile_executor/autotune (INFO, THERMO, IO, WIFI). A recovery-mode-skipped
subsystem would explain a wrong *reply*, not the total absence of one across
every task uniformly. This rules recovery mode back out as the direct cause,
consistent with the retracted doc's original conclusion on this specific
point (which remains correct even though its overall root cause was wrong).

**PC side, rechecked now:** `kiln_help()` reports the `kilnctrl` MCP server
still fresh (started 2026-09-07 13:13:10, commit `c4b4e65d`) -- not restarted,
none needed. `link_status()` reports `connected: True`, `port: COM14`,
`baudrate: 921600`. Opened `COM14` directly with a throwaway
`System.IO.Ports.SerialPort` at 921600 baud from PowerShell while the MCP
server was live: succeeded immediately (`IsOpen=True`), confirming the port
is not held exclusively by a stale handle on the PC side (matches the prior
pass's finding, rechecked fresh rather than inherited). `get_board_state()`
still fails every UART request type with "no reply after all retries - link
or peer is down" -- unchanged from the earlier pass.

**One additional, unexplained observation:** `get_device_log(n=60)` returned
lines timestamped around `I (32322746)` (~32306 s of ESP uptime, ~9 hours),
including `safety_link: safety processor link is down` /
`isolated fault line ASSERTED` -- but the *current* live `uptime_s` from
`GET /api/status`, queried around the same time, is 3372-3454 s (~1 hour).
32306 s of uptime cannot exist on a boot that is currently at 3454 s, so
`get_device_log`'s tool doc ("forwarded over the reliable UART link... this
is also the only place a device-level failure becomes visible") does not
match what was actually returned here: these are stale, PC-side-cached log
lines from a **prior boot**, not live traffic arriving over the CONTROL UART
on this boot. That is itself consistent with (though not proof of) the log
bridge/UART link genuinely not delivering fresh frames this boot -- if it
were, `get_device_log` should have surfaced current-boot lines instead of a
9-hour-old backlog.

**Real cause: undetermined.** Bridge tasks plausibly exist and no app-level
gate explains a uniform, total, retry-exhausted timeout across every request
type. The fault is most likely in the transport/handshake layer underneath
the per-type bridge tasks -- `uart_owner`'s event task or `uart_protocol`'s
RX task wedged, a full queue, a stuck mutex, or the `system_uart_bridge` task
(916 B margin measured earlier the same day) having since overflowed -- but
none of this can be confirmed without either JTAG-halting the ESP core to
walk the FreeRTOS task list/TCBs (a heavier read-only operation than
performed this pass) or a reset, and per this task's constraints a reset
would destroy the coredump that is the only first-hand evidence of the
still-uninvestigated `LoadProhibited` panic. **Stopping here rather than
resetting or reflashing to get a definitive answer.**

## What would settle it

A JTAG read of the ESP's FreeRTOS task list/TCBs (task state, not just
existence) for `uart_owner_evt_task`, `uart_proto_rx`, and
`system_uart_bridge` -- read-only, no reset, but a heavier operation than
performed this pass (needs `esp` symbol resolution, which this MCP server
currently lacks the `nm` tool for; a DWARF offset lookup against the correct
ELF, matched to `fw_build":"Sep  7 2026 22:00:46"`, would be needed instead).
Short of that, the next reboot (whenever the `LoadProhibited` investigation
concludes and a reset/reflash is authorized) is also a natural point to
observe whether the CONTROL link comes up clean from a fresh boot -- if it
does, the fault is in state that accumulates during a long-running boot
(consistent with a wedge) rather than a boot-time initialization defect.

## Link recovered?

**No**, as of this check. Not attempted to force via reflash/reset --
explicitly out of scope this pass (coredump preservation).

Board commit at time of this check: `fc90f682` (dirty=true), from `factory`,
uptime_s=3454, single boot since 07:46:37 (per the corroborating
`boot_guard_recovery_loop_2026-09-08.md`/10-minute-poll investigation).
