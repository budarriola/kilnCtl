// telemetry_log -- live firing/autotune telemetry on the debug UART.
//
// Rides the SAME wire and the SAME uart_log_bridge.c queue every ESP_LOGx
// call in this firmware already uses (UART_TASK_ID_LOG over the PC-link
// uart_protocol_t instance main.c builds at uart_protocol_init(...,
// UART_PROTO_DEVICE_ESP, ...) -- NOT the isolated safety_link.c UART to the
// RP2040, which is a wholly separate uart_protocol_t/UART peripheral. That
// distinction has bitten this project before ("Name the port in shared
// logs"): this module never touches safety_link.c, never constructs its own
// uart_protocol_t, and has no path to the safety wire at all -- it only ever
// calls ESP_LOGI(), which uart_log_bridge.c's vprintf hook already routes to
// the PC-link instance exclusively.
//
// Riding ESP_LOGI also means telemetry inherits uart_log_bridge.c's already-
// audited non-blocking behaviour for free: uart_log_vprintf() calls
// xQueueSend(s_bridge.queue, &entry, 0) -- a ZERO tick timeout, so a full
// queue never blocks the caller, and a dropped entry increments that file's
// own s_dropped_lines counter (periodically flushed as a "N log line(s)
// dropped (queue full)" WARN once the queue has room again). Telemetry does
// not duplicate that accounting -- there is no separate "did my ESP_LOGI
// survive" signal to read (ESP_LOGx is void), and inventing a second drop
// counter next to an already-correct one is exactly the kind of duplicate
// bookkeeping this codebase's "reset-one-side" bug class comes from. What
// telemetry DOES own is staying far enough under the queue's capacity
// (UART_LOG_BRIDGE_QUEUE_LEN 64, see uart_log_bridge.c's own hazard notes on
// why that number must never be raised) that it is never the reason the
// queue fills -- see telemetry_log.c's rate-budget comment for the
// arithmetic, and TELEMETRY_LOG_FIRING_PERIOD_S/TELEMETRY_LOG_AUTOTUNE_
// PERIOD_S there for the two knobs that keep it there.
//
// The two pure line formatters this task calls (telemetry_format_firing()/
// telemetry_format_autotune()) live in telemetry_format.h/.c, not here --
// see that header's own doc comment for why (host-testability, same
// reasoning as dashboard_json.c's split out of dashboard_http.c).
//
// Default OFF. uart_log_bridge.c's own file banner documents two firmware
// history entries where more volume through this exact queue broke the PC
// link (192 entries) or hung the board at the splash screen (256 entries) --
// both reverted same-session. A brand-new, continuous, opt-in-shaped
// telemetry source defaulting to ON would be adding exactly the kind of load
// that queue has already been shown not to tolerate gracefully, without an
// operator ever having asked for it. telemetry_log_set_enabled(true) turns
// it on for a capture session; nothing in this firmware calls that
// automatically.
#ifndef TELEMETRY_LOG_H
#define TELEMETRY_LOG_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default OFF -- see this header's file banner. Toggling takes effect on the
 * telemetry task's next tick (at most TELEMETRY_LOG_TICK_MS later); no lock
 * is needed to call this from any task, it's a single aligned bool. */
void telemetry_log_set_enabled(bool enabled);
bool telemetry_log_is_enabled(void);

/* Starts the telemetry task. Safe to call even if profile_executor_start()/
 * autotune_engine_start() haven't run yet or failed -- the task only ever
 * calls profile_executor_get_status()/autotune_engine_get_status(), both of
 * which are documented safe from any state (profile_executor.h) including
 * before profile_executor_start(). No hardware pointers, no NVS, no
 * dependency on which drivers came up this boot. Idempotent -- a second
 * call is a no-op if the task is already running. */
esp_err_t telemetry_log_start(void);

#ifdef __cplusplus
}
#endif

#endif // TELEMETRY_LOG_H
