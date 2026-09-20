#ifndef UART_LOG_BRIDGE_H
#define UART_LOG_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "../../../../hwAbstraction/esp/uart/uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Redirects every ESP_LOGx call in the firmware onto the reliable UART link
 * (UART_TASK_ID_LOG, see uart_task_ids.h) instead of the USB-Serial-JTAG
 * console, so log output is visible to whatever's already connected over
 * uart_protocol (the GUI's Device Console, an MCP client, etc.) without a
 * second cable or an active debugger session.
 *
 * Call uart_log_bridge_early_init() as the very first thing in app_main,
 * before any other driver bring-up -- it installs the vprintf hook and
 * starts buffering log lines immediately, so even failures during e.g.
 * SX1509_start() (which runs before the uart_protocol_t exists) are
 * captured and get flushed out once uart_log_bridge_start() runs.
 *
 * Call uart_log_bridge_start() once proto is initialized (after
 * uart_protocol_init()) to actually start draining the queue over the wire;
 * anything buffered before that point is sent first, oldest first. */
void uart_log_bridge_early_init(void);
esp_err_t uart_log_bridge_start(uart_protocol_t *proto);

/* Relays one already-formatted SAFETY (RP2040) log line onto the SAME queue
 * and sender task this bridge already uses for the ESP's own lines --
 * CommonFW/docs/LINK_PROTOCOL.md sec 6 "Frame F": the Pico's log_task sends
 * ordinary BROADCAST frames shaped exactly like this board's own LOG
 * payload (level byte + ASCII "tag: message", not null-terminated), so no
 * new wire format or task id is needed, only a relay leg on this side.
 *
 * `text`/`text_len` is that payload's tag+message bytes (payload[1..]),
 * verbatim from the wire -- untrusted input from another processor across
 * an isolated link (CommonFW/README.md rule 6), so this function itself
 * bounds-checks and truncates rather than trusting `text_len`.
 *
 * Tags the relayed line with a "SAFETY " prefix (space, not '/', to read
 * naturally next to this board's own untagged native lines) so the PC side
 * can tell the two sources apart even before it inspects which link/frame
 * the line arrived on -- see uart_log_bridge_set_safety_relay_level()'s
 * comment for the independent per-peer severity filter applied BEFORE this
 * is ever called.
 *
 * Same non-blocking, drop-on-full contract as every other producer into
 * this queue (uart_log_vprintf() in the .c file): never blocks the caller
 * (this is called from safety_poll_task while draining the isolated link's
 * log inbox, and LINK_PROTOCOL.md's "never blocking" rule for Frame F
 * extends all the way to the PC side, not just the RP2040 side). Returns
 * false if the line was dropped (queue full) -- counted in the same
 * dropped-lines total the ESP's own native lines already report, so a
 * relayed SAFETY line and a native ESP line compete for the same finite
 * queue honestly, with one shared drop count rather than two the operator
 * has to add up themselves. Returns true if queued (queuing, not delivery
 * -- delivery itself is best-effort over the wire, same as every other line
 * this bridge sends). */
bool uart_log_bridge_relay_safety(uint8_t level, const char *text, uint8_t text_len);

/* Runtime, independently-settable severity floor applied ONLY to relayed
 * SAFETY lines (uart_log_bridge_relay_safety() above) -- separate from both
 * the RP2040's OWN runtime filter (log_task_set_level(), reached over the
 * link via SAFETY_CMD_SET_LOG_LEVEL / POST /api/safety/log_level) and this
 * board's own native ESP_LOG level (sdkconfig, not runtime-settable at all).
 * "Per-peer" filter: the ESP and the safety processor are the two peers on
 * this link, and each now has its own independently adjustable log
 * threshold as seen on the PC side.
 *
 * Defaults to UART_LOG_LEVEL_WARN -- matching log_task.h's own documented
 * default policy ("errors and warnings only") -- so a factory-default board
 * behaves identically whether this knob is touched or not; it exists to let
 * a bench session ask for MORE relayed detail (e.g. VERBOSE while chasing a
 * link issue) without also having to loosen the Pico's own filter (which
 * would also make it log more internally, spending its own CPU/queue
 * budget), or to QUIET the PC-side transcript (e.g. during a firing)
 * without touching the RP2040's own verbosity at all.
 *
 * A line at a level less severe (numerically greater) than this floor is
 * dropped before ever reaching uart_log_bridge_relay_safety()'s queue --
 * not counted as a "dropped" line, same "filtered is not the same as
 * dropped" distinction log_task.h's own runtime filter draws on the other
 * end of this same link. Backed by a single volatile read/write, same
 * pattern as every other cross-task flag in this codebase
 * (log_task_set_level(), discrete_task.h, relay_owner.h). */
void uart_log_bridge_set_safety_relay_level(uint8_t level);
uint8_t uart_log_bridge_get_safety_relay_level(void);

#ifdef __cplusplus
}
#endif

#endif // UART_LOG_BRIDGE_H
