// AUTOTUNE (task 10) bridge task -- split out of uart_bridge_ext.c 2026-09-04
// (ROADMAP.md M15, 1500-line rule). See uart_bridge_ext_internal.h for the
// full file map and the shared infrastructure (flash-safe executor,
// little-endian/reply helpers) this file uses via uart_bridge_ext_* names.
// MOVE-ONLY: no logic, ordering, or visibility change beyond the widening
// rename the split required.
#include "uart_bridge.h"
#include "uart_bridge_ext_internal.h"

#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "autotune_engine.h"
#include "readiness_gate.h"
#include "uart_task_ids.h"

/* ==========================================================================
 * AUTOTUNE (task 10)
 * ======================================================================== */

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
} autotune_ctx_t;

static size_t autotune_build_status(uint8_t *out)
{
    autotune_engine_status_t st;
    autotune_engine_get_status(&st);

    size_t o = 0;
    out[o++] = AUTOTUNE_CMD_GET_STATUS;
    out[o++] = (uint8_t)st.state;
    out[o++] = (uint8_t)st.method;
    out[o++] = st.zone_index;
    uart_bridge_ext_put_u32_le(&out[o], st.elapsed_s); o += 4;
    uart_bridge_ext_put_u16_le(&out[o], st.sample_count); o += 2;
    uart_bridge_ext_put_f32_le(&out[o], st.actual_c); o += 4;
    out[o++] = st.actual_valid ? 1 : 0;
    uart_bridge_ext_put_f32_le(&out[o], st.duty); o += 4;
    out[o++] = st.model.valid ? 1 : 0;
    uart_bridge_ext_put_f32_le(&out[o], st.model.k_gain_c_per_duty); o += 4;
    uart_bridge_ext_put_f32_le(&out[o], st.model.tau_s); o += 4;
    uart_bridge_ext_put_f32_le(&out[o], st.model.dead_time_s); o += 4;
    uart_bridge_ext_put_f32_le(&out[o], st.proposed_gains.kp); o += 4;
    uart_bridge_ext_put_f32_le(&out[o], st.proposed_gains.ki); o += 4;
    uart_bridge_ext_put_f32_le(&out[o], st.proposed_gains.kd); o += 4;
    out[o++] = (uint8_t)st.proposed_gains.rule;
    uart_bridge_ext_put_f32_le(&out[o], st.predicted_max_ramp_c_per_hr); o += 4;
    out[o++] = st.relay.valid ? 1 : 0;
    uart_bridge_ext_put_f32_le(&out[o], st.relay.ku); o += 4;
    uart_bridge_ext_put_f32_le(&out[o], st.relay.tu_s); o += 4;
    uart_bridge_ext_put_f32_le(&out[o], st.relay.amplitude_c); o += 4;
    o = uart_bridge_ext_put_lstring(out, BRIDGE_REPLY_MAX, o,
                                    st.state == AUTOTUNE_ENGINE_ABORTED ? st.abort_reason : "");
    /* 2026-09-02 CRITICAL FIX (round-2 review): this byte used to be
     * inserted BEFORE the lstring above, on the false premise that it was
     * "appended after the fixed-size fields". abort_reason is NOT
     * fixed-size -- it is length-prefixed (uart_bridge_ext_put_lstring), so
     * inserting anything before it moves its own start by however many
     * bytes were inserted. tools/PcTools/src/kilnctrl/devices.py hardcodes
     * the abort_reason length-prefix offset (UART_PROTOCOL_VERSION 8's
     * documented layout); the earlier version of this fix shifted that
     * offset 62->63 without updating devices.py, silently corrupting EVERY
     * abort_reason this frame ever carries (decoded as "" when settled==0,
     * one garbage byte when settled==1) -- exactly while this same pass was
     * adding new abort reasons for callers to read. Genuinely appending
     * AFTER the variable-length lstring, as done here, is the only
     * placement that cannot move any existing offset, including
     * abort_reason's own. devices.py's autotune status decoder now reads
     * this byte at the FIRST offset past the length-prefixed string
     * (abort_len_offset + 1 + abort_len), not a fixed constant -- see that
     * file's own comment. UART_PROTOCOL_VERSION was bumped alongside this
     * (uart_task_ids.h) because this is a layout change to an existing
     * frame, not a purely additive one -- see that header's own version
     * history for why versions 4/5 bumped on far smaller changes. */
    /* uart_bridge_ext_put_lstring() never overruns BRIDGE_REPLY_MAX but CAN
     * legitimately return o == BRIDGE_REPLY_MAX (abort_reason truncated to
     * fill the buffer exactly) -- guard the same way every other bounded
     * write in this file does rather than assume there is always one byte
     * left. On that (pathological, abort_reason near the wire's max)
     * truncation the settled/converged/tau_consistent bits are simply
     * dropped; a stale/absent reading is far less harmful than an
     * out-of-bounds write. */
    if (o < BRIDGE_REPLY_MAX) {
        out[o++] = st.model.settled ? 1 : 0;
    }
    /* 2026-09-02 round-3 follow-up: two more trailing bytes, genuinely
     * APPENDED after model.settled (itself already after the length-
     * prefixed abort_reason lstring) -- same append-only discipline the
     * settled byte's own fix established (see its comment above and
     * UART_PROTOCOL_VERSION's "Version 9" history in uart_task_ids.h for
     * the corruption that discipline exists to prevent). Surfaces
     * fopdt_model_t::extrapolation_converged/::tau_consistent_with_gain
     * distinctly, not collapsed into the settled bit, so a PC-side caller
     * can tell an operator WHICH of the three ack_unsettled-gated
     * conditions (autotune_engine_accept()'s own comment) is unmet, not
     * just that one is. UART_PROTOCOL_VERSION bumped again (9->10) --
     * this is a second layout change to the same frame. */
    if (o < BRIDGE_REPLY_MAX) {
        out[o++] = st.model.extrapolation_converged ? 1 : 0;
    }
    if (o < BRIDGE_REPLY_MAX) {
        out[o++] = st.model.tau_consistent_with_gain ? 1 : 0;
    }
    return o;
}

/* Runs on the flash-safe worker: AUTOTUNE_CMD_ACCEPT writes the tuned gains
 * through to NVS via autotune_engine_accept(). */
static void autotune_handle_message(void *vargs)
{
    bx_handler_args_t          *args = (bx_handler_args_t *)vargs;
    autotune_ctx_t             *ctx  = (autotune_ctx_t *)args->ctx;
    const uart_proto_message_t  msg  = *args->msg;
    uint8_t reply[BRIDGE_REPLY_MAX];

    uint8_t subcmd = msg.payload[0];

    {
        switch (subcmd) {
            case AUTOTUNE_CMD_GET_STATUS: {
                size_t len = autotune_build_status(reply);
                uart_bridge_ext_reply(ctx->proto, &msg, UART_TASK_ID_AUTOTUNE, reply, len);
                break;
            }
            case AUTOTUNE_CMD_START: {
                if (!uart_bridge_ext_args_ok("autotune", &msg, 16)) break;
                uint8_t zone = msg.payload[1];
                uint8_t method = msg.payload[2];
                float arg_a = uart_bridge_ext_f32_le(&msg.payload[3]);   /* step_duty or setpoint_c */
                float relay_d = uart_bridge_ext_f32_le(&msg.payload[7]);
                float relay_h = uart_bridge_ext_f32_le(&msg.payload[11]);
                uint8_t rule_byte = msg.payload[15];

                char err_msg[READINESS_GATE_MSG_CAP] = "";
                bool ok;
                if (method == AUTOTUNE_METHOD_WIRE_RELAY) {
                    autotune_rule_t rule = (rule_byte == AUTOTUNE_RULE_WIRE_ZN) ? AUTOTUNE_RULE_ZIEGLER_NICHOLS
                                                                                : AUTOTUNE_RULE_TYREUS_LUYBEN;
                    ok = autotune_engine_run_relay(zone, arg_a, relay_d, relay_h, rule, err_msg, sizeof(err_msg));
                } else if (method == AUTOTUNE_METHOD_WIRE_STEP) {
                    /* Wire protocol has no step-rule byte of its own yet -- SIMC only,
                     * same default the HTTP path uses when no rule is given. */
                    ok = autotune_engine_run(zone, arg_a, AUTOTUNE_RULE_SIMC, err_msg, sizeof(err_msg));
                } else {
                    ok = false;
                    snprintf(err_msg, sizeof(err_msg), "method must be 0 (step) or 1 (relay)");
                }
                uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_AUTOTUNE, subcmd, ok, err_msg);
                break;
            }
            case AUTOTUNE_CMD_ABORT: {
                autotune_engine_abort("aborted from UART GUI");
                uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_AUTOTUNE, subcmd, true, NULL);
                break;
            }
            case AUTOTUNE_CMD_ACCEPT: {
                /* payload[1] is the optional ack_unsettled byte -- see
                 * autotune_engine_accept()'s own comment (autotune_engine.h)
                 * for what it gates. A short/absent payload (an older
                 * PC-tools client that has never heard of this byte)
                 * defaults to false, i.e. exactly the refuse-a-low-
                 * confidence-fit behavior that comment documents -- an old
                 * client cannot accidentally force-accept a fit it doesn't
                 * know is low-confidence. */
                /* uart_bridge_ext_args_ok() is a REJECTION helper -- it logs
                 * a warning every time it returns false, which is wrong
                 * here: a missing ack_unsettled byte is an ordinary,
                 * expected, common case (every client before this field
                 * existed, and every accept of an already-settled fit), not
                 * a truncated/malformed frame worth logging about. Probe the
                 * length directly instead -- see this arg's own doc comment
                 * (autotune_engine.h) for why a short payload correctly
                 * defaults to false. */
                bool ack_unsettled = (msg.length >= 2) && (msg.payload[1] != 0);
                autotune_accept_opts_t accept_opts = {.ack_unsettled = ack_unsettled, .adopt_ceiling = false};
                autotune_accept_result_t accept_result = {0};
                bool ok = autotune_engine_accept(&accept_opts, &accept_result);
                uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_AUTOTUNE, subcmd, ok,
                                             ok ? NULL
                                                : accept_result.refused_by_mode_gate
                                                    ? accept_result.mode_reason
                                                    : "no completed autotune result to accept, or it never settled "
                                                         "and needs the ack_unsettled byte set to accept anyway");
                break;
            }
            default:
                ESP_LOGW(UART_BRIDGE_EXT_TAG, "autotune: unknown subcmd 0x%02X -- rejected", subcmd);
                uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_AUTOTUNE, subcmd, false,
                                             "unknown subcommand");
                break;
        }
    }
}

static void autotune_task(void *arg)
{
    autotune_ctx_t *ctx = (autotune_ctx_t *)arg;
    uart_proto_message_t msg;

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            ESP_LOGW(UART_BRIDGE_EXT_TAG, "autotune: empty payload -- rejected");
            continue;
        }
        bx_handler_args_t args = { .ctx = ctx, .msg = &msg };
        /* Not reachable on-worker today: autotune_task() is the top-level
         * consumer of this task's own inbox queue, never itself invoked as
         * a job callback on bx_flash_worker, so this dispatch can never be
         * the re-entrant case flash_worker_lint.py's scan_reentrancy()
         * checks for -- unlike a generic write-function callback (e.g.
         * cfg_fs's *_set_write_fn() indirection), nothing else calls
         * autotune_task() or autotune_handle_message() from any other
         * context. */
        uart_bridge_ext_run_on_flash_worker(autotune_handle_message, &args);
    }
}

esp_err_t uart_bridge_start_autotune_task(uart_protocol_t *proto)
{
    if (!proto) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!uart_bridge_ext_worker_ensure_started()) {
        return ESP_ERR_NO_MEM; /* see uart_bridge_start_control_task() */
    }
    static autotune_ctx_t ctx;
    ctx.proto = proto;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_AUTOTUNE, BRIDGE_INBOX_LEN, &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }
    /* Restored to 4096 on 2026-08-20, having been shrunk to 3072 earlier the
     * same day. The shrink was a workaround for internal-SRAM exhaustion at
     * task-creation time; that shortfall is fixed at its source now (LVGL's
     * allocator and the Wi-Fi/lwIP pools moved to PSRAM), and more to the
     * point THIS STACK IS NOT IN INTERNAL SRAM AT ALL -- uart_bridge_ext_
     * retry_task_create_pinned() allocates it from PSRAM, of which ~8MB is
     * free. Shrinking it therefore bought nothing and cost margin.
     *
     * Not hypothetical: the identical shrink applied to wifi_uart_bridge
     * (uart_bridge_ext_wifi.c) overflowed its stack and rebooted the board.
     * See that call. */
    BaseType_t created = uart_bridge_ext_retry_task_create_pinned(autotune_task, "autotune_uart_bridge", 4096, &ctx,
                                                                   5);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_AUTOTUNE);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
