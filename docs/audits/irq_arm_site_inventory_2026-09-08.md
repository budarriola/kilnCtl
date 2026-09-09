# Interrupt arm-site inventory — 2026-09-08

Follow-on to `4344c32f` (KilnFW touch/LCD side) and
`safety_tc_drdy_stall_2026-09-08.md` (the DRDY stall that motivated this
sweep). Covers every interrupt/edge-arm site found across SaftyFW (RP2040)
and the rest of KilnFW (ESP32-S3) + `hwAbstraction`.

Classification:
- **SAFE** — level-driven, purely polled, or armed before the event is
  physically possible.
- **SELF-HEALING** — a missed event is recovered by later polling or a
  different code path; name the recovering path.
- **LATCHING** — a missed event is permanent (the DRDY class). Urgent.

## SaftyFW (RP2040)

| Site | Mechanism | Event | Armed relative to event | Class | Notes |
|---|---|---|---|---|---|
| `thermo_task.c:371` `gpio_set_irq_enabled_with_callback(SAFTYFW_PIN_THERMO_DRDY, GPIO_IRQ_EDGE_FALL, ...)` | GPIO edge IRQ | Safety MAX31856 conversion-done (`~DRDY` falling) | Armed in `thermo_task_fn()`, AFTER `max31856_configure()` (pre-scheduler, in `main.c`) starts CMODE free-running — the original bug | **Was LATCHING, now FIXED** | Fix already in tree (`docs/audits/safety_tc_drdy_stall_2026-09-08.md`): every loop iteration after a notify timeout does a level check on the pin and reads the chip if still asserted, releasing DRDY for the next edge regardless of cause. Off-limits file for this pass — not touched further. |
| `current_sense.c` / `current_task.c` `hal_adc_select` + `hal_adc_read_raw()` | Polled single-shot ADC read | CT sample on GPIO28 (and 2 other channels) | N/A — no interrupt, no free-running/round-robin FIFO | SAFE | File header explicitly documents this choice ("use `adc_set_round_robin()`... avoided on purpose... single-shot `adc_read()` polling"). |
| `discrete_task.c:89-91` `hal_gpio_get(SAFTYFW_PIN_ESTOP)` / `SAFTYFW_PIN_MAIN_FAULT` | Polled GPIO read | E-stop assert, main-contactor fault feedback | N/A — polled every task cycle | SAFE | No `gpio_set_irq_enabled` anywhere in this file. |
| `max31856.c:318` `hal_gpio_get(s_fault_gpio)` | Polled GPIO read | `~FAULT` from the safety MAX31856 | N/A — polled | SAFE | Mirrors KilnFW's MAX31856 `~FAULT` handling (also polled, per `4344c32f`). |
| `link_task.c` UART RX/TX via `uart_owner` (see hwAbstraction row below) | see below | | | | link_task itself does not touch IRQ registers directly, only the ring buffer API. |
| `main.c:232` `hw_clear_bits(&timer_hw->dbgpause, ...)` | Register write, not an interrupt | Debug-halt behavior of the RP2040 hardware timer | N/A | N/A | Not an interrupt arm site; included here only because it matched the search pattern (`timer_hw`). |

No other `gpio_set_irq_enabled`, `irq_set_exclusive_handler`, `adc_irq_*`, `dma_channel_set_irq`, `uart_set_irq_enables`, or `hardware_alarm_set_callback` call sites exist in `firmware/SaftyFW/src` outside the two rows above and the shared `hwAbstraction/pico` UART driver below. SaftyFW has exactly one true edge-triggered IRQ of its own (DRDY), already fixed.

## hwAbstraction (shared between SaftyFW and, in principle, any Pico-hosted build)

| Site | Mechanism | Event | Armed relative to event | Class | Notes |
|---|---|---|---|---|---|
| `hwAbstraction/pico/uart/uart_owner.c:274-278` `irq_set_exclusive_handler(UART1_IRQ, ...)`, `uart_set_irq_enables(..., true, false)` | UART1 RX interrupt (RXIM, level-triggered on FIFO-above-threshold) | Byte(s) arriving from the ESP over the safety link | Armed inside `uart_owner_init()`, immediately after `uart_init()`/`gpio_set_function()` set up the peripheral — before the far end has any reason to be sending yet | SAFE | RXIM is level-sensitive, not edge: even if a byte physically arrived in the tiny window between `uart_init()` and the interrupt enable, the condition (FIFO non-empty) remains asserted until read, so enabling afterward still fires. Cannot self-latch the way an edge IRQ can. |
| Same file, TX interrupt (TXIM, *transition*-triggered on FIFO-level-falls-through-threshold) | Priming/re-arming TX after enqueuing a frame | | | **Already found and fixed 2026-08-23** (see extensive in-file comments, `uart_owner_tx_policy.h`, `s_tx_self_start_failures`) | Not a new finding — a real "armed at the wrong moment relative to a transition-only signal" bug of the same family as DRDY, but for TX starvation rather than a permanent stall (recovered by the next `uart_owner_send()` call, so it was SELF-HEALING in effect, just slow — not left in this table as urgent since it predates this audit and is already resolved). |
| `hwAbstraction/esp/gpio/hal_gpio_esp.c` | `gpio_config_t.intr_type = GPIO_INTR_DISABLE` (both configured pins) | — | — | SAFE | The ESP GPIO abstraction never enables an interrupt itself; the file comment states interrupt wiring (`gpio_isr_handler_add`) is deliberately kept outside this interface. |
| `hwAbstraction/esp/uart/uart_owner.c` | No `uart_isr_register`/`uart_intr_config`/ISR of any kind | Safety-link RX/TX on the ESP side | N/A — pure polling wrapper | SAFE | Matches `safety_link_poll.c`'s naming; confirms the ESP side of the safety link is polled end to end. |

## KilnFW (ESP32-S3), beyond `4344c32f`'s touch/LCD sweep

| Site | Mechanism | Event | Armed relative to event | Class | Notes |
|---|---|---|---|---|---|
| `bridge/uart_bridge_io.c:633-641` `gpio_set_intr_type(..., GPIO_INTR_NEGEDGE)` + `gpio_isr_handler_add()` | SX1509 I/O-expander `~INT` (falling edge) | Any configured SX1509 input change (relay feedback etc.) | Armed in `uart_bridge_start_io_task()`, a boot phase AFTER `kiln_io_init()` already enables the expander's interrupt sources | SELF-HEALING (decision below) | Same shape as DRDY (arm-after-enable race) but every `kiln_io_read()` call clears the SX1509's interrupt latch and re-reads current state, so a missed edge only delays/drops the ISR-driven **auto-report push**, never leaves stale state cached. |
| `hw/MAX31856.c` `GPIO_INTR_DISABLE` on `~FAULT` | Disabled, polled | Main-board MAX31856 fault | N/A | SAFE | Confirmed by `4344c32f`; unchanged. |
| `hw/SX1509.c` `GPIO_INTR_DISABLE` on `~RESET`/`~INT` config | Disabled, polled | SX1509 reset/interrupt lines | N/A | SAFE | Confirmed by `4344c32f`; unchanged. Actual `~INT` wiring lives in `uart_bridge_io.c` (row above), which is where the real ISR is. |
| `net/wifi_prov.c` `esp_timer_create` (AP-fallback timer, periodic rescan timer) | Software timer callback (esp_timer task), not an edge/level GPIO IRQ | Scheduled timeout, not an external signal that can occur "early" | Created and started well before any dependent state exists | SAFE | Timers fire on a schedule; there is no "event became possible before arming" race for a periodic software timer. |
| `ui/lvgl_port.c` `esp_timer_create` (1 ms `lv_tick` callback) | Software timer callback | LVGL tick | Started once at LVGL init | SAFE | Same reasoning as above; also already covered structurally by `4344c32f`'s LVGL-flush-path review. |
| `safety_link_poll.c` and the rest of `safety/*` | Calls only into `uart_owner` (polled, see above) | Safety-link frames | N/A | SAFE | No direct register/ISR access in this directory; confirmed by grep (`isr`/`irq`/`IRQ` absent). |

No `adc_continuous`/`adc_digi`, DMA-completion ISR, or hardware input-capture timer sites exist anywhere in `firmware/KilnFW/App` or `firmware/hwAbstraction/esp` — the on-board ADC and SPI paths used for thermocouple reads are blocking/polled, matching `4344c32f`'s finding for the touch/LCD side.

## LATCHING sites found beyond DRDY

None. The DRDY site (already fixed, out of scope for edits this pass per
task boundaries) is the only true LATCHING interrupt found in either
firmware. Every other edge/level IRQ in the system is either SAFE by
construction (level-triggered, polled, or armed before the event is
reachable) or SELF-HEALING (the SX1509 `~INT` auto-report push, and the
already-resolved 2026-08-23 TX-priming issue).

## SX1509 `~INT` — decision: leave as is

The SX1509 `~INT` handler (`uart_bridge_io.c:633-641`) has the same
arm-after-enable race as DRDY had, but is not urgent and should **not** be
changed right now:

- Every `kiln_io_read()` call re-reads the expander's actual input state and
  clears its interrupt latch as a side effect. A missed edge therefore never
  produces stale *state* — any subsequent read (which happens continuously
  from the dashboard/status paths, independent of the ISR) recovers the true
  value.
- The only thing a missed edge can do is delay or entirely drop one
  ISR-triggered **auto-report push** (a proactive notification), not corrupt
  or freeze a reading the way DRDY did. DRDY was different because the *only*
  reader of the safety thermocouple was gated behind the notification itself
  — there was no independent polling path to fall back on. Here there is.
- Fixing it for real would mean restructuring
  `uart_bridge_start_io_task()`/`kiln_io_init()` boot ordering, which touches
  relay-safety-adjacent boot sequencing on the main I/O expander used for
  relay control feedback — exactly the kind of change that needs bench
  verification on real hardware, not a good candidate for a same-session
  drive-by fix.

This rationale is being added as a code comment at the `gpio_set_intr_type`
call site in `uart_bridge_io.c` so the next person does not have to
re-derive it (see that file's diff in this commit).

## Mechanical check: considered and rejected

Considered: flag any `gpio_set_intr_type`/`gpio_isr_handler_add`/
`gpio_set_irq_enabled_with_callback` call not immediately preceded, in the
same function, by a level check on the same pin.

Rejected as too noisy to be honest. Of the handful of real edge-IRQ arm
sites in this codebase (DRDY, SX1509 `~INT`, the RP2040 UART RXIM enable),
only DRDY needed the level-check pattern, and only because it is a genuinely
edge-only condition with no other reader. RXIM is level-triggered and
correctly does *not* need a same-function level check — flagging it would
be a false positive on the very case that proves the check's premise wrong.
SX1509 `~INT` is intentionally left as SELF-HEALING (see above) rather than
given a level check, which a mechanical rule would also flag incorrectly. A
rule general enough to skip both of those but still catch a real DRDY-style
regression would need to know *semantically* whether the watched condition
has an independent recovery path — which is exactly the judgment call this
document exists to record by hand, the same reasoning CLAUDE.md's
"reset-one-side" bug class gives for not mechanizing that class either. No
check added.
