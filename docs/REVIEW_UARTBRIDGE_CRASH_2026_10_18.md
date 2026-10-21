# Review: info_uart_bridge crash (2026-10-18)

HIGH 0, MED 0 (root cause already fixed), LOW 1 (fixed), INFO 1.

## Crash

Bench board now on main 179cb0058. It held an unacknowledged crash record from the previous boot,
on the old image (fw build 07:11:45, main 207134862): task `info_uart_bridg`, LoadProhibited at
uptime 13 s. Read read-only through `read_esp_coredump` (archived as
`firmware/KilnFW/coredump_archive/coredump-e641d10fa9de.bin`). The record was not acknowledged.
Symbolized with `find_crash_elf_for_coredump` against `elf_archive/KilnCtrl-d7c0e2ca4759.elf`.

```
exccause 0x1c LoadProhibited, excvaddr 0x1
#0 prvTaskCheckFreeStackSpace (pucStackByte=0x1)       tasks.c:4811
#1 uxTaskGetStackHighWaterMark                          tasks.c:4885
#2 stack_margin_read (index=23)                         stack_margin.c:108
#3 build_stack_margin_reply (start_index=21)            uart_bridge_info.c:185
#4 info_bridge_task                                     uart_bridge_info.c:262
```

## Root cause (UB-01 = BM-01, already fixed)

In 207134862, `main_network_http.c:1054` registered
`stack_margin_register("uart_proto_rx", ctx->uart_proto->rx_task_handle, ...)`. That passes the
handle value where the slot address belongs (BM-01, a regression from RAM-03). `stack_margin_read()`
then loads `*slot`, which is the TCB's first word (`pxTopOfStack`), and uses it as a task handle.
`uxTaskGetStackHighWaterMark()` reads `pxStack` at that fake TCB, which is a word on the task's
stack (value 1), and dereferences it. The fault fires on the first INFO 0x04 `get_stack_margin`
request whose page reaches that row. On a benign run it only hung the reply, as BM-01 recorded.
On this boot it faulted.

054d8dbea fixed this (`&ctx->uart_proto->rx_task_handle`). The fix is not an ancestor of
207134862 but is in origin/dev and in main 179cb0058, where benchmain10 verified that
get_stack_margin replies. The fault cannot recur on the current image.

The coredump does not include `.bss`, so the registry row at index 23 cannot be read back
directly. The attribution rests on three things: the old source, the fault shape (handle -> stack
word -> 0x1), and BM-01's own symptom on the same request.

## Latent hazard found while tracing (UB-02, LOW, fixed)

`stack_margin_read()` loaded the slot and then walked the stack with no lock. Ten registered tasks
delete themselves after a bare `slot = NULL;`: ota_confirm, pico_auto_update, zone_sweep,
http_async_job, update_fetch and update_fetch_wr, ota_rollback_reboot, ota_pico_rollback,
recovery_exit and recovery_boot. A reader on the other core could load the handle just before the
store, lose the race to `vTaskDelete` and the TCB free (immediate with `vTaskDeleteWithCaps`), and
fault in the same frame. ota_confirm and pico_auto_update exit at about 10-15 s of boot, the same
window as this crash.

Fix: a `portMUX` spinlock in stack_margin.c is held across the slot load and
`uxTaskGetStackHighWaterMark()`. A new `stack_margin_task_exit(&slot)` clears the slot under the
same lock, and all ten sites use it. Once it returns, no reader holds the old handle. The critical
section is a short stack scan and never blocks.

## INFO

- Tasks deleted by another task (`vTaskDelete(handle)`) are outside UB-02. None of the registered
  slots is deleted that way today.

## Tests

`test_stack_margin_registry.c` gets two new tests. The functional one checks that
`task_exit` clears the slot and that the read then reports `alive=false`. The source scan checks
the lock order in `stack_margin_read()` and that all ten exit sites call `task_exit` with no bare
store left. `build_host_tests.ps1 -Only "stack_margin|update_fetch|http_async_job"` passes.
negtest kilnfw-host: 4/4 CAUGHT on assertions (lock moved after load, task_exit not clearing,
ota_confirm bare store, zone_sweep bare store). No target build was run.
