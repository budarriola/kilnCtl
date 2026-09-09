# Touch/LCD interrupt audit — 2026-09-08

Follow-up to `docs/audits/safety_tc_drdy_stall_2026-09-08.md` (safety MAX31856
~DRDY armed after the event it watches, self-latching). Question: does
KilnFW's touch/LCD path have the same shape?

## 1. Touch hardware on this bench unit

FT6336U capacitive controller (I2C, `firmware/KilnFW/App/drivers/hw/FT6336U.c`).
Per CLAUDE.md and confirmed by reading the driver's own header comment: **this
driver is dead code at runtime today** — "nothing in main.c constructs an
FT6336UClass, and FT6336U_start's caller is nobody" (FT6336U.c:13-15).
Neither RST nor INT is wired on this board's harness (FT6336U.c:160-163) — the
driver never attempts to drive or read either pin. `KILNCTL_TOUCH_CAP_*`
(CLAUDE.md) govern calibration/orientation math (`touch_dev.c`), not an
interrupt path — there is no capacitive-touch interrupt line on this hardware
at all to investigate.

The live touch path on this bench unit is the resistive NS2009 (also
polling-only, see below) selected via `lvgl_port.c`'s `touch_dev` field
(comment at line ~861-862: "actually wired up -- NS2009 ... or FT6336U").

## 2/3. Touch INT edge/level, arm timing, polling fallback

**There is no touch interrupt at all.** `lvgl_port.c`'s `touch_read_cb()` is
called every LVGL indev poll tick from `lvgl_port_task`'s own loop
(`lv_timer_handler()`), and it calls `touch_dev_read()` (NS2009 or FT6336U)
synchronously — pure polling, no GPIO ISR, no semaphore-driven wake. This is
structurally immune to the DRDY bug class: there is no "arm" step at all, so
there is nothing to arm too late. A missed sample is impossible by
construction — the next poll (LVGL indev timer, ~ms cadence) reads current
state again. **Verdict: not applicable / safe by construction.**

## 4. LCD side

No interrupt (no flush-complete IRQ, no vsync). `ili9488_flush_cb()` runs
synchronously inside `lv_timer_handler()`'s refresh walk (both the DMA-async
and blocking-write branches call `lv_display_flush_ready()` either inline or
from a completion path — no ISR-context re-entry into LVGL). The 2026-09-04
incident (reentrant `lv_obj_invalidate()` from inside the flush callback
corrupting `s_lvgl_task_stack`, fixed by `51e1ef5`) was reviewed: the fix
(`lvgl_port_service_idle_wake()`, lvgl_port.c:795-834) now performs all
screen-wake `lv_obj_invalidate()` calls from `lvgl_port_task`'s loop *before*
`lv_timer_handler()` runs, never from inside `ili9796_flush_cb`, and the
comment there explicitly warns future edits not to move it back. Confirmed
correct as currently written. **Verdict: safe.**

## 5. KilnFW-wide edge-triggered IRQ arm-site inventory

Grepped `GPIO_INTR_*`, `gpio_isr_handler_add`, `gpio_install_isr_service`
across all of `firmware/KilnFW/App`:

| Site | Line | Edge type | Verdict |
|---|---|---|---|
| `MAX31856.c` ~FAULT pin | 571, 598 | `GPIO_INTR_DISABLE` — polled in `MAX31856_read()`, level sampled and corroborated by SR each read | Safe — polling only, no ISR |
| `SX1509.c` ~RESET pin | 450 | `GPIO_INTR_DISABLE` — plain output | N/A, not an interrupt source |
| `SX1509.c` ~INT pin | 476 | `GPIO_INTR_DISABLE` at the driver layer — deliberately: "whoever owns the expander decides how it wants to hear about an edge" (SX1509.c:468-470) | Deferred to caller, see below |
| `uart_bridge_io.c` SX1509 ~INT | 633-641 | **`GPIO_INTR_NEGEDGE`, real ISR (`io_bridge_isr`)** — the only genuine edge-triggered IRQ found anywhere in KilnFW | **Same shape as DRDY — see below** |

## The one real finding: SX1509 ~INT arm-order gap (latching in theory, self-healing in practice)

`uart_bridge_io.c:629-631` documents it correctly: "~INT is open-drain and
active low, and stays low until the interrupt source is cleared (which
kiln_io_read does), so a falling edge is the event" — this is a **level-held,
not pulsed**, active-low line, exactly DRDY's shape.

Boot order (confirmed by reading `main_boot_early.c` / `main_control_bringup.c`
/ `main_bridges_bringup.c`):

1. `SX1509_start()` (main_boot_early.c:375)
2. `kiln_io_init()` (main_boot_early.c:379) → calls `SX1509_set_interrupt()`
   (kiln_io.c:221), which **arms interrupt sources on the SX1509 chip itself**
   — from this point on, any qualifying input transition pulls ~INT low.
3. `kiln_io_owner_start()` (main_control_bringup.c:107) — later bringup phase.
4. `uart_bridge_start_io_task()` (main_bridges_bringup.c:71) → **only here**
   does the ESP32 side call `gpio_set_intr_type(NEGEDGE)` +
   `gpio_isr_handler_add()` + `gpio_intr_enable()` (uart_bridge_io.c:638-640).

Between step 2 and step 4 (two different bringup phases/files), the SX1509
can assert ~INT. Because the ESP32 GPIO is configured for `NEGEDGE` and the
line is already low by the time it is armed, no falling edge is ever seen on
that occurrence — the same "armed after the event it watches could already
have happened" defect as the safety DRDY bug.

**Why this is materially less serious than DRDY, and does not self-latch a
control-affecting fault:** unlike the safety thermocouple, IO/relay state on
this line is not read *exclusively* through the ISR-driven path. Any ordinary
`kiln_io_read()` call — from `kiln_io_owner_command_read()`, which the
dashboard/HTTP status endpoints and the LCD UI's periodic status refresh all
go through independently of this IRQ — also clears the SX1509's latched
interrupt-source register on that same read. So:

- The **specific loss** from the gap is delayed/skipped delivery of the
  bridge's IRQ-driven "auto report" push (and only while `auto_enabled` is
  true; when it's false — the default — the task blocks `portMAX_DELAY` on
  the queue set with no periodic tick at all, so the push is skipped entirely
  until the next explicit read from *any* consumer or the next genuine edge).
- The **underlying IO/relay state itself does not go stale**: every other
  consumer path re-reads the expander directly and, as a side effect, clears
  the same interrupt-source flags, which also re-arms future edges correctly.
- So this is "latching" only for the push-notification side channel, not for
  the state itself, and only until any other reader happens to poll — which
  in practice is frequent (dashboard/LCD refresh cadence). Not the same
  failure mode as DRDY, where nothing except the (never-firing) ISR ever
  triggered a read of that specific value.

**Not fixed here.** A correct fix (arm the ESP32 GPIO ISR before or
immediately after `SX1509_set_interrupt()`, or do one unconditional
`kiln_io_read()` right after arming to catch/clear any pre-existing
assertion) touches boot-sequencing code shared with relay-safety paths
(`kiln_io_owner_start`, `main_control_bringup.c`) and needs on-hardware
verification of the SX1509 interrupt-source register behavior — out of scope
for a host-testable, negative-testable change under the current
do-not-flash/do-not-reset constraint, and not clearly a live operational bug
given the redundant-poll mitigation above. Recommend as a follow-up: either
move the `gpio_isr_handler_add`/`gpio_intr_enable` call earlier (adjacent to
`SX1509_set_interrupt()` in `kiln_io_init()`, before any input can be live),
or have `uart_bridge_start_io_task()` perform one `io_build_read_payload()`
call immediately after enabling the ISR to flush any pending assertion.

## Board observation

Did not query `touch_log_tap_targets` or reset the board — out of scope per
task instructions (read-only, no reset). No live touch anomaly to report;
analysis above is static-code based.

## Summary

- Touch: no interrupt exists on this hardware (FT6336U dead code, NS2009 pure
  polling) — DRDY bug class does not apply, safe by construction.
- LVGL flush: no interrupt; the earlier reentrant-invalidate defect is fixed
  and confirmed correct in current code.
- One real same-shape arm-order gap found: SX1509 ~INT edge IRQ in
  `uart_bridge_io.c`, armed 1-2 boot phases after the expander's own
  interrupt sources are enabled in `kiln_io_init()`. Self-healing in practice
  via redundant `kiln_io_read()` call sites elsewhere; not fixed pending
  on-hardware verification.
- No other edge-triggered GPIO ISR exists anywhere else in KilnFW; every
  other `GPIO_INTR_*` site is `GPIO_INTR_DISABLE` (deliberately polled).
