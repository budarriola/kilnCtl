#ifndef UART_BRIDGE_EXT_INTERNAL_H
#define UART_BRIDGE_EXT_INTERNAL_H

// Internal seams for the uart_bridge_ext.c split (2026-09-04, ROADMAP.md M15
// "files over 1500 lines should be broken up where it makes sense" --
// uart_bridge_ext.c had grown to 1727 lines). This header is NOT public API
// -- uart_bridge.h stays that -- it exists purely so pieces that used to be
// one translation unit (and could reach each other's `static` state and
// helpers for free) can still do so now that they are four. Same shape as
// ota_http.c's 2026-09-04 split (ota_http_internal.h) and wifi_prov.c's
// 2026-09-04 split (wifi_prov_internal.h): every symbol declared below was
// `static` in the original single file and is widened to file-scope-
// internal linkage ONLY because a sibling .c file in this split now calls
// or reads it directly. Every widened symbol is renamed with a
// `uart_bridge_ext_` prefix -- `TAG` and several of the original short
// helper names (`bx_put_lstring`, `bx_reply_ok_err`, `retry_task_create_
// pinned`) are referenced BY NAME in other drivers' comments (uart_bridge.c,
// gpio_probe.c) and, per this audit's own rule, get the prefix regardless
// of whether a real symbol collision was found -- see ota_http_internal.h's
// own doc comment for the precedent this follows.
//
// THIS IS A MOVE-ONLY REFACTOR: no logic, ordering, naming (beyond the
// widening rename above) or visibility change beyond what moving requires.
//
//   uart_bridge_ext.c          -- file banner, retry_task_create_pinned,
//                                  the flash-safe executor (bx_worker_task,
//                                  worker_ensure_started, uart_bridge_ext_
//                                  start_flash_worker(), bx_run_on_internal_
//                                  stack(), uart_bridge_ext_run_on_flash_
//                                  worker(), uart_bridge_ext_is_on_flash_
//                                  worker()), and the shared little-endian /
//                                  reply-framing helpers
//   uart_bridge_ext_control.c  -- CONTROL (task 8) + PROFILES (task 9)
//   uart_bridge_ext_autotune.c -- AUTOTUNE (task 10)
//   uart_bridge_ext_wifi.c     -- WIFI (task 11)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "uart_bridge.h"

// Shared log tag. Defined (non-static) in uart_bridge_ext.c; every split
// file logs under the same "uart_bridge_ext" tag the single file used to,
// unchanged. NOT named plain `TAG` -- every other driver file in
// App/drivers/ has its own `static const char *TAG`, and a global `TAG`
// here would clash the moment two translation units of this split (or this
// split and any other file) end up in the same link.
extern const char *UART_BRIDGE_EXT_TAG;

#define BRIDGE_INBOX_LEN 4
#define BRIDGE_REPLY_MAX UART_PROTO_MAX_PAYLOAD
#define BRIDGE_REPLY_ACK_TIMEOUT_MS 200u

// Retries task creation up to 5 times with a short backoff -- see
// uart_bridge_ext.c's own definition for the internal-SRAM-vs-PSRAM-stack
// race this guards against. Used by every uart_bridge_start_*_task()
// function across the split.
BaseType_t uart_bridge_ext_retry_task_create_pinned(TaskFunction_t task_fn, const char *name, uint32_t stack_depth,
                                                     void *param, UBaseType_t priority);

// Idempotent flash-safe-worker startup, shared by uart_bridge_ext_start_
// flash_worker() (uart_bridge_ext.c, the normal early-app_main path) and by
// every uart_bridge_start_*_task() function across this split as a
// fallback -- see uart_bridge_ext.c's own comment on why callers MUST fail
// their start rather than fall through to running handlers on a PSRAM
// stack. Returns false if the worker could not be created.
bool uart_bridge_ext_worker_ensure_started(void);

// Shared shape for the three refactored (flash-safe-worker-dispatched)
// handlers: the task's ctx plus the message it just received. Lives on the
// bridge task's stack across the blocking uart_bridge_ext_run_on_flash_
// worker() call.
typedef struct {
    void                        *ctx;
    const uart_proto_message_t  *msg;
} bx_handler_args_t;

// --------------------------------------------------------------------------
// Shared little-endian helpers -- same layout uart_bridge.c uses, duplicated
// here (rather than exported from there) because they're a handful of
// one-liners and not worth widening that file's already-large surface for.
// --------------------------------------------------------------------------
void     uart_bridge_ext_put_u16_le(uint8_t *o, uint16_t v);
uint32_t uart_bridge_ext_u32_le(const uint8_t *b);
void     uart_bridge_ext_put_u32_le(uint8_t *o, uint32_t v);
float    uart_bridge_ext_f32_le(const uint8_t *b);
void     uart_bridge_ext_put_f32_le(uint8_t *o, float v);

// Same discipline as uart_bridge.c's bridge_args_ok(): reject a truncated
// frame before touching anything, rather than parse whatever stale bytes
// are sitting past msg.length in the payload buffer.
bool uart_bridge_ext_args_ok(const char *who, const uart_proto_message_t *msg, size_t need);

// Appends a length-prefixed ASCII string to *out at *o, capping to what's
// left of `cap`; truncates (never overruns) and reports how many bytes of
// `text` actually fit via the return value.
size_t uart_bridge_ext_put_lstring(uint8_t *out, size_t cap, size_t o, const char *text);

void uart_bridge_ext_reply(uart_protocol_t *proto, const uart_proto_message_t *msg, uint8_t src_task,
                            const uint8_t *reply, size_t reply_len);

// Common shape for every "mutating command with an ok/fail reply" handler:
// byte0 = subcmd echoed back, byte1 = ok, [optional] length-prefixed error
// text.
void uart_bridge_ext_reply_ok_err(uart_protocol_t *proto, const uart_proto_message_t *msg, uint8_t src_task,
                                   uint8_t subcmd, bool ok, const char *err_msg);

#endif // UART_BRIDGE_EXT_INTERNAL_H
