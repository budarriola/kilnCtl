// CONTROL (task 8) + PROFILES (task 9) bridge tasks -- split out of
// uart_bridge_ext.c 2026-09-04 (ROADMAP.md M15, 1500-line rule). See
// uart_bridge_ext_internal.h for the full file map and the shared
// infrastructure (flash-safe executor, little-endian/reply helpers) this
// file uses via uart_bridge_ext_* names. MOVE-ONLY: no logic, ordering, or
// visibility change beyond the widening rename the split required.
#include "uart_bridge.h"
#include "uart_bridge_ext_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "MAX31856.h"
#include "kiln_io.h"
#include "profile_executor.h"
#include "profiles_builtin.h"
#include "profiles_http.h"
#include "relay_authority.h" /* relay_authority_heat_run_active() -- see the system_mode_gate check below */
#include "run_state.h"
#include "system_mode_gate.h" /* SYS_ACTION_WRITE_ZONES_CONFIG -- owner decision Q2, 2026-09-25;
                                 * SET_ZONE_PID/MODEL were a real gap (no gate at all) before this */
#include "uart_task_ids.h"
#include "unit_pref.h"
#include "zones_config_accessors.h"

/* ==========================================================================
 * CONTROL (task 8)
 * ======================================================================== */

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
} control_ctx_t;

static size_t control_build_get_zones(uint8_t *out)
{
    uint8_t thermo_count = zones_config_get_thermo_count();
    if (thermo_count > MAX31856_CHANNEL_COUNT) {
        thermo_count = MAX31856_CHANNEL_COUNT; /* defensive -- see zones_http.c's own bound */
    }

    size_t o = 0;
    out[o++] = CONTROL_CMD_GET_ZONES;
    out[o++] = thermo_count;
    out[o++] = KILN_IO_RELAY_COUNT;
    out[o++] = thermo_count; /* count field -- see uart_task_ids.h */

    for (uint8_t zi = 0; zi < thermo_count; zi++) {
        uint8_t relay_mask = 0;
        zones_config_get_relay_mask(zi, &relay_mask);
        zone_control_mode_t mode = ZONE_CONTROL_MODE_OFF;
        zones_config_get_control_mode(zi, &mode);
        /* No direct getter for the raw cal offset exists (only
         * zones_config_apply_cal(), which adds it to a caller-supplied raw
         * reading) -- recovering it via apply_cal(zi, 0.0f) is exact and
         * avoids adding a new getter just for this. */
        float cal_offset_c = zones_config_apply_cal(zi, 0.0f);
        float kp = 0.0f, ki = 0.0f, kd = 0.0f;
        zones_config_get_pid(zi, &kp, &ki, &kd);
        float max_ramp = 0.0f;
        zones_config_get_max_ramp(zi, &max_ramp);
        float max_temp = 0.0f, min_temp = 0.0f;
        zones_config_get_temp_limits(zi, &max_temp, &min_temp);

        out[o++] = zi;
        out[o++] = relay_mask;
        out[o++] = (uint8_t)mode;
        uart_bridge_ext_put_f32_le(&out[o], cal_offset_c); o += 4;
        uart_bridge_ext_put_f32_le(&out[o], kp); o += 4;
        uart_bridge_ext_put_f32_le(&out[o], ki); o += 4;
        uart_bridge_ext_put_f32_le(&out[o], kd); o += 4;
        uart_bridge_ext_put_f32_le(&out[o], max_ramp); o += 4;
        uart_bridge_ext_put_f32_le(&out[o], max_temp); o += 4;
        uart_bridge_ext_put_f32_le(&out[o], min_temp); o += 4;
    }
    return o;
}

/* Runs on the flash-safe worker (uart_bridge_ext_run_on_flash_worker()),
 * never on control_task itself: CONTROL_CMD_SET_ZONE_PID/MODEL both end in
 * zones_http.c's nvs_save(). `reply` is a local here on purpose -- that is
 * what moves the BRIDGE_REPLY_MAX buffer onto the worker's internal stack. */
/* Owner decision Q2 (docs/SYSTEM_MODE_GATE.md, 2026-09-25,
 * gate-slices-2/4/5 spec): refuse ALL zone/relay/guard config writes -- not
 * scoped to which field changed -- while a firing or autotune run is active,
 * PAUSED included. CONTROL_CMD_SET_ZONE_PID/SET_ZONE_MODEL were a real gap
 * before this pass: the UART bridge could rewrite a zone's PID gains or
 * thermal model mid-firing with no gate at all, while the exact same write
 * from the HTTP page (zones_http_post.c) was already refused. Returns true
 * (refused) with `reason` filled -- same shape as system_mode_gate_check()
 * itself -- so the caller can reply with the same reason string a refused
 * HTTP request gets, never a bare "no". */
static bool control_zones_write_refused(char *reason, size_t reason_cap)
{
    sys_mode_snapshot_t snap = { 0 };
    relay_authority_heat_run_active(&snap.profile_running, &snap.autotune_running);
    return system_mode_gate_check(SYS_ACTION_WRITE_ZONES_CONFIG, &snap, reason, reason_cap);
}

static void control_handle_message(void *vargs)
{
    bx_handler_args_t          *args = (bx_handler_args_t *)vargs;
    control_ctx_t              *ctx  = (control_ctx_t *)args->ctx;
    const uart_proto_message_t  msg  = *args->msg;
    uint8_t reply[BRIDGE_REPLY_MAX];

    uint8_t subcmd = msg.payload[0];

    {
        switch (subcmd) {
            case CONTROL_CMD_GET_ZONES: {
                size_t len = control_build_get_zones(reply);
                uart_bridge_ext_reply(ctx->proto, &msg, UART_TASK_ID_CONTROL, reply, len);
                break;
            }
            case CONTROL_CMD_SET_ZONE_PID: {
                if (!uart_bridge_ext_args_ok("control", &msg, 14)) {
                    uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_CONTROL, subcmd, false, "truncated");
                    break;
                }
                char mode_reason[SYSTEM_MODE_GATE_REASON_MAX];
                mode_reason[0] = '\0';
                if (control_zones_write_refused(mode_reason, sizeof(mode_reason))) {
                    uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_CONTROL, subcmd, false, mode_reason);
                    break;
                }
                uint8_t zi = msg.payload[1];
                float kp = uart_bridge_ext_f32_le(&msg.payload[2]);
                float ki = uart_bridge_ext_f32_le(&msg.payload[6]);
                float kd = uart_bridge_ext_f32_le(&msg.payload[10]);
                zones_set_result_t r = zones_config_set_pid_checked(zi, kp, ki, kd);
                uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_CONTROL, subcmd, r == ZONES_SET_OK,
                                             r == ZONES_SET_BUSY_RUNNING ? "run active" : NULL);
                break;
            }
            case CONTROL_CMD_SET_ZONE_MODEL: {
                if (!uart_bridge_ext_args_ok("control", &msg, 14)) {
                    uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_CONTROL, subcmd, false, "truncated");
                    break;
                }
                char mode_reason[SYSTEM_MODE_GATE_REASON_MAX];
                mode_reason[0] = '\0';
                if (control_zones_write_refused(mode_reason, sizeof(mode_reason))) {
                    uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_CONTROL, subcmd, false, mode_reason);
                    break;
                }
                uint8_t zi = msg.payload[1];
                float k_dc = uart_bridge_ext_f32_le(&msg.payload[2]);
                float tau_s = uart_bridge_ext_f32_le(&msg.payload[6]);
                float dead_time_s = uart_bridge_ext_f32_le(&msg.payload[10]);
                zones_set_result_t r = zones_config_set_model_checked(zi, k_dc, tau_s, dead_time_s);
                uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_CONTROL, subcmd, r == ZONES_SET_OK,
                                             r == ZONES_SET_BUSY_RUNNING ? "run active" : NULL);
                break;
            }
            case CONTROL_CMD_GET_UNIT_PREF: {
                /* 2026-08-21 (ROADMAP.md shared unit preference): additive
                 * QUERY-style subcommand, see uart_task_ids.h's doc comment
                 * for why this needs no protocol version bump. */
                uint8_t reply2[2];
                reply2[0] = CONTROL_CMD_GET_UNIT_PREF;
                reply2[1] = (uint8_t)unit_pref_get();
                uart_bridge_ext_reply(ctx->proto, &msg, UART_TASK_ID_CONTROL, reply2, sizeof(reply2));
                break;
            }
            case CONTROL_CMD_SET_UNIT_PREF: {
                if (!uart_bridge_ext_args_ok("control", &msg, 2)) {
                    uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_CONTROL, subcmd, false, "truncated");
                    break;
                }
                uint8_t raw = msg.payload[1];
                bool adopted = false;
                bool ok = (raw == (uint8_t)UNIT_PREF_CELSIUS || raw == (uint8_t)UNIT_PREF_FAHRENHEIT) &&
                          unit_pref_set_ex((unit_pref_t)raw, &adopted) == ESP_OK;
                uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_CONTROL, subcmd, ok,
                                             ok ? NULL : (adopted ? "save not verified, live value now matches file" : NULL));
                break;
            }
            default:
                ESP_LOGW(UART_BRIDGE_EXT_TAG, "control: unknown subcmd 0x%02X -- rejected", subcmd);
                /* Every other reply path here uses uart_bridge_ext_reply_ok_err,
                 * but an unrecognized subcmd was falling through unanswered --
                 * the transport ACK (uart_protocol.c, before this switch ever
                 * runs) already told the host "delivered", and with no reply
                 * that looked identical to "executed". Same gap uart_bridge.c
                 * closes with bridge_reply_unsupported(); see its comment. */
                uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_CONTROL, subcmd, false,
                                             "unknown subcommand");
                break;
        }
    }
}

static void control_task(void *arg)
{
    control_ctx_t *ctx = (control_ctx_t *)arg;
    uart_proto_message_t msg;

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            ESP_LOGW(UART_BRIDGE_EXT_TAG, "control: empty payload -- rejected");
            continue;
        }
        bx_handler_args_t args = { .ctx = ctx, .msg = &msg };
        /* Not reachable on-worker today: control_task() is the top-level
         * consumer of this task's own inbox queue, never itself invoked as
         * a job callback on bx_flash_worker, so THIS dispatch site cannot
         * be the re-entrant case. (A downstream re-entrancy hazard DOES
         * exist below control_handle_message() -- CONTROL_CMD_SET_UNIT_PREF
         * reaches unit_pref_set() -> pref_cfg_fs_save() -> the cfg_fs
         * device write_fn, which dispatches onto this same worker a SECOND
         * time -- but that is a property of the write_fn's own dispatch
         * call, flagged separately by flash_worker_lint.py at its own call
         * site, not of this one.) */
        uart_bridge_ext_run_on_flash_worker(control_handle_message, &args);
    }
}

esp_err_t uart_bridge_start_control_task(uart_protocol_t *proto)
{
    if (!proto) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Must succeed before the bridge task exists: without the worker,
     * control_handle_message() would never run at all, and silently falling
     * back to running it on this task's PSRAM stack is the exact bug being
     * fixed. Fail the start instead. */
    if (!uart_bridge_ext_worker_ensure_started()) {
        return ESP_ERR_NO_MEM;
    }
    static control_ctx_t ctx;
    ctx.proto = proto;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_CONTROL, BRIDGE_INBOX_LEN, &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }
    BaseType_t created = uart_bridge_ext_retry_task_create_pinned(control_task, "control_uart_bridge", 4096, &ctx, 5);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_CONTROL);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ==========================================================================
 * PROFILES (task 9)
 * ======================================================================== */

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
} profiles_ctx_t;

/* Appends one summary record if it fits. Returns false when the frame is
 * full, which is the signal to stop the page here. */
static bool profiles_list_append(uint8_t *out, size_t *io, uint8_t id, const profile_t *p)
{
    size_t name_len = strlen(p->name);
    /* Bail before overrunning the frame rather than truncate a profile
     * entry mid-record -- a short page is still useful (the client asks for
     * the next one), a mis-parsed record is not. */
    if (*io + 1 + 1 + name_len + 1 + 1 > BRIDGE_REPLY_MAX) {
        return false;
    }
    out[(*io)++] = id;
    *io = uart_bridge_ext_put_lstring(out, BRIDGE_REPLY_MAX, *io, p->name);
    out[(*io)++] = p->zone_mask;
    out[(*io)++] = p->segment_count;
    return true;
}

/* LIST is PAGED, because the catalogue does not fit in one frame.
 *
 * A summary record is 1 (id) + 1 (name len) + <=15 (name) + 1 (zone_mask) +
 * 1 (segment_count) = up to 19 bytes, and BRIDGE_REPLY_MAX is 253 with 2
 * bytes of header, so ~13 records per frame against 8 user slots plus 28
 * shipped schedules. Rather than invent a second command, LIST takes an
 * optional `start_id` byte and enumerates every EXISTING profile with
 * id >= start_id in ascending id order -- user slots 0..7 first, then the
 * builtin catalogue at PROFILE_BUILTIN_ID_BASE.. -- stopping when the frame
 * fills. The client pages by re-asking with (last id + 1) until a reply
 * comes back with count == 0. The reply layout is unchanged, so an old
 * client that sends no argument still parses this fine; it simply sees the
 * first page (start_id 0) instead of "all of them", which was already the
 * documented behaviour of the break above.
 *
 * Hidden builtins are skipped here, matching GET /api/profiles. They stay
 * reachable by direct GET, same as on the HTTP side. */
static size_t profiles_build_list(uint8_t *out, uint8_t start_id)
{
    size_t o = 0;
    out[o++] = PROFILES_CMD_LIST;
    size_t count_pos = o++;
    uint8_t count = 0;

    for (uint16_t id = start_id; id < PROFILES_MAX_COUNT; id++) {
        profile_t p;
        if (!profiles_http_get((uint8_t)id, &p)) {
            continue;
        }
        if (!profiles_list_append(out, &o, (uint8_t)id, &p)) {
            goto done;
        }
        count++;
    }

    for (size_t i = 0; i < g_builtin_profile_count; i++) {
        uint8_t id = (uint8_t)(PROFILE_BUILTIN_ID_BASE + i);
        if (id < start_id || profiles_builtin_is_hidden(id)) {
            continue;
        }
        profile_t p;
        if (!profiles_http_get(id, &p)) {
            continue;
        }
        if (!profiles_list_append(out, &o, id, &p)) {
            goto done;
        }
        count++;
    }

done:
    out[count_pos] = count;
    return o;
}

/* GET serves a user slot and a builtin catalogue id alike -- profiles_http_get()
 * resolves both (and fills a builtin's zone_mask from the configured zones), so
 * there is deliberately no id-range test here.
 *
 * Reply budget, worst case: 1 (subcmd) + 1 (ok) + 1 (id) + 1 (name len) + 15
 * (PROFILE_NAME_MAX_LEN) + 1 (zone_mask) + 1 (segment_count) + 12 segments x
 * 12 bytes = 165 bytes, against BRIDGE_REPLY_MAX = UART_PROTO_MAX_PAYLOAD =
 * 253. A full 12-segment builtin fits with 88 bytes to spare; the clamp below
 * never binds today. */
static size_t profiles_build_get(uint8_t *out, uint8_t id)
{
    size_t o = 0;
    out[o++] = PROFILES_CMD_GET;
    profile_t p;
    if (!profiles_http_get(id, &p)) {
        out[o++] = 0; /* ok = 0 */
        return o;
    }
    out[o++] = 1; /* ok */
    out[o++] = id;
    o = uart_bridge_ext_put_lstring(out, BRIDGE_REPLY_MAX, o, p.name);
    out[o++] = p.zone_mask;
    uint8_t seg_count = p.segment_count;
    /* Clamp to what's left of the frame -- 12 bytes/segment, so this can
     * only bind in a build where PROFILE_MAX_SEGMENTS grows well past 12,
     * which it hasn't; kept as a hard backstop rather than an assumption. */
    size_t max_segs_that_fit = (BRIDGE_REPLY_MAX - o - 1) / 12u;
    if (seg_count > max_segs_that_fit) {
        seg_count = (uint8_t)max_segs_that_fit;
    }
    out[o++] = seg_count;
    for (uint8_t i = 0; i < seg_count; i++) {
        uart_bridge_ext_put_f32_le(&out[o], p.segments[i].target_c); o += 4;
        uart_bridge_ext_put_f32_le(&out[o], p.segments[i].ramp_c_per_hr); o += 4;
        uart_bridge_ext_put_u32_le(&out[o], p.segments[i].dwell_min); o += 4;
    }
    return o;
}

/* GET_EXEC_STATUS mirrors /api/control's per-zone shape (control_mode/
 * actual/duty/PID-adjacent fields), not /api/profile_exec's fuller one --
 * see uart_task_ids.h for why. */
static size_t profiles_build_exec_status(uint8_t *out)
{
    /* profile_exec_status_t is 1464 B; this runs on bx_flash_worker, whose
     * stack ceiling profile_executor_get_active_id()'s own doc comment
     * measures at 3792 B (zero headroom on clean main) -- a stack-local
     * instance here would be the same class of regression that function
     * was added to avoid, just for a caller that (unlike that one) needs
     * every field, not only state+id. Heap it instead, same pattern as
     * safety_cfg_http.c's profile_exec_status_t reads. */
    profile_exec_status_t *st = heap_caps_malloc(sizeof(*st), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!st) {
        return 0; /* out of memory -- caller sees an empty reply, same as a malformed frame */
    }
    profile_executor_get_status(st);

    size_t o = 0;
    out[o++] = PROFILES_CMD_GET_EXEC_STATUS;
    out[o++] = (uint8_t)st->state;
    out[o++] = st->profile_id;
    o = uart_bridge_ext_put_lstring(out, BRIDGE_REPLY_MAX, o, st->profile_name);
    out[o++] = st->zone_mask;
    out[o++] = st->segment_index;
    out[o++] = st->segment_count;
    out[o++] = st->dwelling ? 1 : 0;
    uart_bridge_ext_put_f32_le(&out[o], st->target_c); o += 4;
    uart_bridge_ext_put_u32_le(&out[o], st->segment_elapsed_s); o += 4;
    uart_bridge_ext_put_u32_le(&out[o], st->dwell_remaining_s); o += 4;
    out[o++] = st->ramp_lock_held ? 1 : 0;
    out[o++] = st->ramp_lock_lagging_mask;
    out[o++] = st->fault_guard;

    size_t zone_count_pos = o++;
    uint8_t zone_count = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        const profile_exec_zone_status_t *z = &st->zones[zi];
        if (!z->active) {
            continue;
        }
        if (o + 14 > BRIDGE_REPLY_MAX) {
            break; /* frame full -- report what fit rather than overrun */
        }
        out[o++] = zi;
        out[o++] = z->control_mode;
        uart_bridge_ext_put_f32_le(&out[o], z->actual_c); o += 4;
        out[o++] = z->actual_valid ? 1 : 0;
        uart_bridge_ext_put_f32_le(&out[o], z->duty); o += 4;
        out[o++] = z->relay_commanded_on ? 1 : 0;
        out[o++] = z->faulted ? 1 : 0;
        out[o++] = z->fault_guard;
        zone_count++;
    }
    out[zone_count_pos] = zone_count;
    free(st);
    return o;
}

/* Runs on the flash-safe worker. This is the handler whose old in-task form
 * produced the captured coredump: SAVE -> profiles_http_save() -> nvs_save_slot()
 * -> esp_flash_write() -> cache disable -> assert. DELETE, START, STOP, PAUSE
 * and ACK_LAST_RUN reach flash too (run_state / profiles_http). */
static void profiles_handle_message(void *vargs)
{
    bx_handler_args_t          *args = (bx_handler_args_t *)vargs;
    profiles_ctx_t             *ctx  = (profiles_ctx_t *)args->ctx;
    const uart_proto_message_t  msg  = *args->msg;
    uint8_t reply[BRIDGE_REPLY_MAX];

    uint8_t subcmd = msg.payload[0];

    {
        switch (subcmd) {
            case PROFILES_CMD_LIST: {
                /* Optional start_id byte -- absent means "from the top". */
                uint8_t start_id = (msg.length >= 2) ? msg.payload[1] : 0u;
                size_t len = profiles_build_list(reply, start_id);
                uart_bridge_ext_reply(ctx->proto, &msg, UART_TASK_ID_PROFILES, reply, len);
                break;
            }
            case PROFILES_CMD_GET: {
                if (!uart_bridge_ext_args_ok("profiles", &msg, 2)) {
                    uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, false, "truncated");
                    break;
                }
                size_t len = profiles_build_get(reply, msg.payload[1]);
                uart_bridge_ext_reply(ctx->proto, &msg, UART_TASK_ID_PROFILES, reply, len);
                break;
            }
            case PROFILES_CMD_SAVE: {
                if (!uart_bridge_ext_args_ok("profiles", &msg, 5)) {
                    uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, false, "truncated");
                    break;
                }
                uint8_t requested_id = msg.payload[1];
                uint8_t name_len = msg.payload[2];
                if (!uart_bridge_ext_args_ok("profiles", &msg, (size_t)3 + name_len + 2)) {
                    uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, false, "truncated");
                    break;
                }
                if (name_len > PROFILE_NAME_MAX_LEN) {
                    uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, false,
                                                 "name too long");
                    break;
                }
                size_t p = 3;
                profile_t candidate;
                memset(&candidate, 0, sizeof(candidate));
                memcpy(candidate.name, &msg.payload[p], name_len);
                candidate.name[name_len] = '\0';
                p += name_len;
                candidate.zone_mask = msg.payload[p++];
                uint8_t seg_count = msg.payload[p++];
                if (seg_count < 1 || seg_count > PROFILE_MAX_SEGMENTS ||
                    !uart_bridge_ext_args_ok("profiles", &msg, p + (size_t)seg_count * 12)) {
                    uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, false,
                                                 "seg_count missing or out of range");
                    break;
                }
                candidate.segment_count = seg_count;
                for (uint8_t i = 0; i < seg_count; i++) {
                    candidate.segments[i].target_c = uart_bridge_ext_f32_le(&msg.payload[p]); p += 4;
                    candidate.segments[i].ramp_c_per_hr = uart_bridge_ext_f32_le(&msg.payload[p]); p += 4;
                    candidate.segments[i].dwell_min = uart_bridge_ext_u32_le(&msg.payload[p]); p += 4;
                }

                uint8_t out_id = 0, warn_count = 0;
                char err_msg[96] = "";
                bool ok = profiles_http_save(requested_id, &candidate, &out_id, &warn_count, err_msg,
                                             sizeof(err_msg));
                uint8_t rep[BRIDGE_REPLY_MAX];
                size_t o = 0;
                rep[o++] = subcmd;
                rep[o++] = ok ? 1 : 0;
                if (ok) {
                    rep[o++] = out_id;
                    rep[o++] = warn_count;
                } else {
                    o = uart_bridge_ext_put_lstring(rep, sizeof(rep), o, err_msg);
                }
                uart_bridge_ext_reply(ctx->proto, &msg, UART_TASK_ID_PROFILES, rep, o);
                break;
            }
            case PROFILES_CMD_DELETE: {
                if (!uart_bridge_ext_args_ok("profiles", &msg, 2)) {
                    uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, false, "truncated");
                    break;
                }
                uint8_t del_id = msg.payload[1];
                /* Same refusal profile_delete_post_handler() gives: a builtin
                 * is a const table in flash and cannot be erased. Say so and
                 * point at hide, rather than at "no such profile" -- which
                 * would be a lie about an id GET and START both accept. */
                if (profiles_builtin_id_valid(del_id)) {
                    uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, false,
                                                 "built-in schedule is read-only; hide it instead "
                                                 "(POST /api/profile/builtin/hide)");
                    break;
                }
                bool ok = profiles_http_delete(del_id);
                if (!ok) {
                    /* Review fold-in (PROFILE_SLOTS_100.md section 7):
                     * profiles_http_delete() returns false both for "no such
                     * profile" and "that profile is currently running/paused
                     * and refused" -- reporting the latter as "no such
                     * profile" is a lie to a caller that just listed this id
                     * as running. Query the executor first (the same check
                     * profiles_http_delete() makes internally) so the reply
                     * names the real reason. */
                    uint8_t active_id = 0;
                    bool is_running_this_id = profile_executor_get_active_id(&active_id) && active_id == del_id;
                    uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, false,
                                                 is_running_this_id
                                                     ? "profile is currently running -- stop it before deleting"
                                                     : "no such profile");
                } else {
                    uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, true, NULL);
                }
                break;
            }
            /* GET_EXEC_STATUS is handled inline in profiles_task(), before this
             * message ever reaches the worker -- see that function's comment.
             * Kept out of this switch entirely (not just unreachable) so
             * there is exactly one place that builds this reply. */
            case PROFILES_CMD_START: {
                if (!uart_bridge_ext_args_ok("profiles", &msg, 2)) {
                    uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, false, "truncated");
                    break;
                }
                char err_msg[96] = "";
                bool ok = profile_executor_run(msg.payload[1], err_msg, sizeof(err_msg));
                uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, ok, err_msg);
                break;
            }
            case PROFILES_CMD_STOP: {
                profile_executor_halt();
                uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, true, NULL);
                break;
            }
            case PROFILES_CMD_PAUSE: {
                bool ok = profile_executor_pause();
                uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, ok,
                                             ok ? NULL : "nothing running to pause");
                break;
            }
            case PROFILES_CMD_RESUME: {
                bool ok = profile_executor_resume();
                uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, ok,
                                             ok ? NULL : "nothing paused to resume");
                break;
            }
            case PROFILES_CMD_ACK_LAST_RUN: {
                bool ok = run_state_acknowledge();
                uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, ok,
                                             ok ? NULL : "no previous-run record to acknowledge");
                break;
            }
            default:
                ESP_LOGW(UART_BRIDGE_EXT_TAG, "profiles: unknown subcmd 0x%02X -- rejected", subcmd);
                uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_PROFILES, subcmd, false,
                                             "unknown subcommand");
                break;
        }
    }
}

static void profiles_task(void *arg)
{
    profiles_ctx_t *ctx = (profiles_ctx_t *)arg;
    uart_proto_message_t msg;

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            ESP_LOGW(UART_BRIDGE_EXT_TAG, "profiles: empty payload -- rejected");
            continue;
        }

        /* GET_EXEC_STATUS is a pure read (profile_executor_get_status() only
         * takes s_exec.lock and memcpy's a snapshot -- no NVS/flash access
         * anywhere in it), so it does NOT need this file's internal-SRAM-
         * stack worker, which exists solely for the PSRAM-stack-vs-flash-
         * cache hazard documented at the top of uart_bridge_ext.c. Routing it
         * through uart_bridge_ext_run_on_flash_worker() anyway used to
         * serialize it behind that worker's single in-flight job -- shared
         * with every CONTROL/PROFILES/AUTOTUNE mutating (flash-writing)
         * command AND safety_cfg_store's deferred NVS flush -- and
         * additionally behind profile_executor's own s_exec.lock, which the
         * executor task holds for the length of an entire 1 Hz tick
         * (PID/feedforward/guard math for every active zone;
         * relay_cycles_maybe_persist()'s NVS write runs after the lock). A
         * caller polling GET_EXEC_STATUS at a fixed cadence during a
         * multi-zone firing could queue behind either of those and miss a
         * 3 s reply deadline -- the frame was already ACKed by the transport
         * before any of this ran, so the request looked delivered right up
         * until the reply arrived late (or the caller had already given
         * up). thermo_read has no such indirection: its bridge task answers
         * straight from its own task, which is why it never showed the same
         * symptom under the same load. Answering here, directly on
         * profiles_task's own stack, removes both queuing points for this
         * one read-only subcommand without touching the flash-writing
         * subcommands' worker dispatch below. */
        if (msg.payload[0] == PROFILES_CMD_GET_EXEC_STATUS) {
            uint8_t reply[BRIDGE_REPLY_MAX];
            size_t len = profiles_build_exec_status(reply);
            uart_bridge_ext_reply(ctx->proto, &msg, UART_TASK_ID_PROFILES, reply, len);
            continue;
        }

        bx_handler_args_t args = { .ctx = ctx, .msg = &msg };
        /* Not reachable on-worker today: profiles_task() is the top-level
         * consumer of this task's own inbox queue (GET_EXEC_STATUS above is
         * the only subcommand answered off this dispatch), never itself
         * invoked as a job callback on bx_flash_worker, so this dispatch
         * cannot be the re-entrant case flash_worker_lint.py's
         * scan_reentrancy() checks for. */
        uart_bridge_ext_run_on_flash_worker(profiles_handle_message, &args);
    }
}

esp_err_t uart_bridge_start_profiles_task(uart_protocol_t *proto)
{
    if (!proto) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!uart_bridge_ext_worker_ensure_started()) {
        return ESP_ERR_NO_MEM; /* see uart_bridge_start_control_task() */
    }
    static profiles_ctx_t ctx;
    ctx.proto = proto;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_PROFILES, BRIDGE_INBOX_LEN, &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }
    BaseType_t created = uart_bridge_ext_retry_task_create_pinned(profiles_task, "profiles_uart_bridge", 4096, &ctx,
                                                                   5);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_PROFILES);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
