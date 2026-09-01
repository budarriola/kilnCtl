#ifndef UART_BRIDGE_INTERNAL_H
#define UART_BRIDGE_INTERNAL_H

/* Shared plumbing for uart_bridge.c's per-task translation units
 * (uart_bridge_thermo.c, uart_bridge_io.c, uart_bridge_touch.c,
 * uart_bridge_ui_test.c, uart_bridge_safety.c, uart_bridge_system.c,
 * uart_bridge_info.c) plus uart_bridge.c itself (shared helpers and the PC
 * link watchdog).
 *
 * uart_bridge.c used to be one 2755-line file; this header is the seam that
 * split it, mechanically, along per-UART-task boundaries -- see the split's
 * commit message for the rationale. Every declaration below was `static` in
 * that file and is now shared across the resulting translation units, so it
 * has to be non-static with a declaration reachable from all of them. Nothing
 * here changes behaviour: same functions, same bodies, only the linkage and
 * the file they live in.
 *
 * Deliberately internal (not installed alongside uart_bridge.h): nothing
 * outside App/drivers's own uart_bridge_*.c files has ever called any of
 * this, and uart_bridge_ext.c keeps its own bx_*() duplicates rather than
 * share these (see uart_bridge.c's bridge_put_lstring() doc comment for why).
 * TAG stays a private `static const char *TAG = "uart_bridge"` in each .c
 * file rather than being centralized here, so every file's log lines keep
 * exactly the same "uart_bridge" tag they always had. */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "espInterfaces/uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BRIDGE_INBOX_LEN 8

/* Every reply this file builds fits one protocol payload; the biggest is the
 * THERMO READ for three channels (2 + 3*12 = 38) and the SAFETY status (25). */
#define BRIDGE_REPLY_MAX UART_PROTO_MAX_PAYLOAD

/* ACK timeout for a reply to a query. uart_protocol_send retries internally up
 * to UART_PROTO_MAX_RETRIES (10) times, so the worst case a bridge task spends
 * inside one send is this value times ten. At the old 1000 ms that was ten
 * seconds during which the task answered nothing else -- no relay command, no
 * ~INT edge, no auto-report tick -- purely because the host stopped listening.
 * A host that has gone away must not be able to stall the tasks that switch
 * mains relays, so the ceiling is 2 s instead. The host sees a reply timeout
 * either way; the difference is only how long we wait to notice. */
#define BRIDGE_REPLY_ACK_TIMEOUT_MS 200u

/* Floor on any auto-report period. The wire lets a host ask for 1 ms, which at
 * the FreeRTOS tick rate becomes "wake immediately, every time" -- a bridge
 * task spinning at full priority hammering the I2C/SPI bus, starving the relay
 * and safety paths that share it. 0 still means "off" (that is the frozen wire
 * semantic); anything non-zero below this is quietly raised to it and logged,
 * which is more useful than rejecting a request that is merely optimistic. */
#define BRIDGE_AUTO_REPORT_MIN_MS 20u

/* --------------------------------------------------------------------------
 * Little-endian payload helpers. Every multi-byte field in this protocol is
 * little-endian on the wire (see uart_task_ids.h); the SX1509's own registers
 * are big-endian pairs, but that conversion lives inside SX1509.c and never
 * leaks up here.
 * ------------------------------------------------------------------------ */

uint16_t bridge_u16_le(const uint8_t *bytes);
void bridge_put_u16_le(uint8_t *out, uint16_t value);
void bridge_put_u32_le(uint8_t *out, uint32_t value);
float bridge_f32_le(const uint8_t *bytes);
void bridge_put_f32_le(uint8_t *out, float value);

/* Appends a length-prefixed ASCII string to `out` at `o`, capping to what's
 * left of `cap`; truncates (never overruns) rather than asserting or
 * dropping the reply. Same shape as uart_bridge_ext.c's bx_put_lstring() --
 * duplicated rather than shared because that one is file-static there and
 * this file has no common header the two could both include without a
 * larger refactor neither needs today. */
size_t bridge_put_lstring(uint8_t *out, size_t cap, size_t o, const char *text);

/* --------------------------------------------------------------------------
 * Untrusted-payload guards.
 *
 * Everything arriving on the PC link is attacker-or-bug-shaped until proven
 * otherwise: a frame can be truncated mid-argument, carry an index no pin or
 * channel has, or name a subcommand this firmware has never heard of. The two
 * helpers below are the only places that judgement is made, so that all ~40
 * subcommand handlers reject the same way -- with a log line naming the exact
 * reason, and *before* any driver call, so a rejected frame is never
 * half-applied.
 * ------------------------------------------------------------------------ */

/* `need` is the total payload length the subcommand requires, counting the
 * subcommand byte itself. See uart_bridge.c for the full doc comment on why
 * a short payload must never be read past ::length. */
bool bridge_args_ok(const char *who, const uart_proto_message_t *msg, size_t need);

/* Inclusive range gate for the index arguments -- channel 0-2, relay 1-4,
 * io 1-7, expander pin 0-15 and friends. See uart_bridge.c for the full doc
 * comment on why this exists in front of the drivers' own range checks. */
bool bridge_range_ok(const char *who, uint8_t subcmd, const char *what, uint32_t value,
                     uint32_t lo, uint32_t hi);

/* Clamp an auto-report period to something a bridge task can actually serve.
 * See BRIDGE_AUTO_REPORT_MIN_MS -- 0 is preserved exactly, because 0 is "off"
 * on the wire and must not become "as fast as possible". */
uint16_t bridge_clamp_auto_period(const char *who, uint16_t period_ms);

/* --------------------------------------------------------------------------
 * PC link liveness / reply plumbing. See uart_bridge.c for the full doc
 * comments -- s_link_last_activity/s_link_ever_seen themselves stay static
 * there (only link_watchdog_task, which lives in the same file, reads them),
 * so only the functions below need to cross a file boundary.
 * ------------------------------------------------------------------------ */

void bridge_note_link_activity(void);

/* Answers whoever asked, rather than a hardcoded destination: the requester's
 * address is carried in the inbound message, so a bridge never needs to know
 * who is on the other end of the link. */
void bridge_reply(uart_protocol_t *proto, const uart_proto_message_t *msg, uint8_t src_task,
                  const uint8_t *reply, size_t reply_len);

/* Sent from a bridge task's default: case, or from any guard that refuses a
 * *recognized* subcommand. See uart_bridge.c for the full doc comment (the
 * SET_CT_CAL/relay-refusal history behind why this exists). */
void bridge_reply_reject(uart_protocol_t *proto, const uart_proto_message_t *msg,
                         uint8_t src_task, uint8_t subcmd, const char *reason);

/* Thin wrapper kept for the default: cases -- an unrecognized subcmd has no
 * more specific reason to give than "unsupported". See uart_bridge.c for the
 * full doc comment on why a real reason string (never NULL) matters here. */
void bridge_reply_unsupported(uart_protocol_t *proto, const uart_proto_message_t *msg,
                              uint8_t src_task, uint8_t subcmd);

/* An unsolicited push (an auto-report tick). Shorter ACK timeout than a reply:
 * a report that can't be delivered is stale by the time the retries run out,
 * and the next tick carries newer data anyway. */
void bridge_push(uart_protocol_t *proto, uart_proto_device_t dst_device, uint8_t dst_task,
                 uint8_t src_task, const uint8_t *payload, size_t len);

#ifdef __cplusplus
}
#endif

#endif // UART_BRIDGE_INTERNAL_H
