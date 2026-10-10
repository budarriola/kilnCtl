// Command senders for the safety link (ESP32-S3 <-> RP2040 safety
// processor): the public safety_link_send_*()/safety_link_get_ct_cal()/
// safety_link_get_config_page()/safety_link_send_update_frame()/safety_
// link_get_update_status() family that ask the Pico to do something or
// fetch something not covered by the ordinary GET_STATUS poll. Split out of
// safety_link.c (4183 lines) as one of the seams the audit named ("command
// handlers"), distinct from the frame decode/build logic underneath some of
// them (safety_link_frames.c) and the request/reply exchange machinery they
// all sit on top of (safety_link_inbox.c).
//
// VERBATIM relocation: every function body below is byte-for-byte the same
// code that used to live in safety_link.c, with no behavior change. None of
// the functions in this file were `static` before the split (they are all
// part of safety_link.h's public API already) and none needed a linkage
// change to move here -- they call safety_lock/safety_unlock, safety_
// elapsed_ms, safety_exchange, safety_drain_inbox[_ex], and the stash
// helpers, all of which are declared in safety_link_internal.h.
#include "safety_link.h"
#include "safety_link_frame.h"
#include "safety_link_internal.h"
#include "safety_trip_decision.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "hal_time.h"
#include "stack_margin.h"
#include "freertos/idf_additions.h"
#include "settings.h"
#include "uart_task_ids.h"

#include "kilnlink/kilnlink_announce.h"
#include "kilnlink/kilnlink_announce_reboot.h"
#include "kilnlink/kilnlink_apply_config_volatile.h" /* SAFETY_CMD_APPLY_CONFIG_VOLATILE (0x2D), item 15 --
                                                       * safety_link_send_apply_config_volatile() */
#include "kilnlink/kilnlink_clear_trip.h"
#include "kilnlink/kilnlink_commit_config.h"
#include "kilnlink/kilnlink_commit_config_rejected.h"
#include "kilnlink/kilnlink_reboot.h"
#include "kilnlink/kilnlink_reboot_result.h"
#include "kilnlink/kilnlink_rollback_result.h"
#include "kilnlink/kilnlink_config_page.h"
#include "kilnlink/kilnlink_context.h"
#include "kilnlink/kilnlink_ct_cal.h"
#include "kilnlink/kilnlink_get_config_page.h"
#include "kilnlink/kilnlink_get_ct_cal.h"
#include "kilnlink/kilnlink_get_param.h"
#include "kilnlink/kilnlink_param.h"
#include "kilnlink/kilnlink_ct_auto_zero_begin.h"
#include "kilnlink/kilnlink_get_ct_auto_zero.h"
#include "kilnlink/kilnlink_ct_auto_zero_status.h"
#include "kilnlink/kilnlink_rollback.h"
#include "kilnlink/kilnlink_set_config.h"
#include "kilnlink/kilnlink_set_ct_cal.h"
#include "kilnlink/kilnlink_set_log_level.h"
#include "kilnlink/kilnlink_set_param.h"
#include "kilnlink/kilnlink_version.h"
#include "kilnlink/kilnlink_stack_margin.h"
#include "kilnlink/kilnlink_get_stack_margin.h"

/* 2026-09-15 (Opus review F3): removed a dead zones_config_accessors.h
 * include here -- leftover from a stale, duplicated comment about
 * safety_sync_tc_type() (removed; see safety_link_poll.c). This file made
 * no accessor call of its own. */

/* TODO owner-report (2026-08-21 follow-up), docs/COMMISSIONING.md sec 3: the
 * ESP-side commissioning cache. Same real, deliberate cross-module dependency
 * as zones_http.h just above (this driver otherwise knows nothing about NVS
 * caching or the commissioning HTTP surface) -- the poll task is where every
 * fresh FW_VERSION frame's config_crc is learned, so it is the natural place
 * to trigger safety_cfg_store_maybe_refetch()'s fetch-on-change check; see
 * safety_sync_cfg_cache() below. */
#include "safety_cfg_store.h"

/* ROADMAP.md M5 -- SAFETY_CMD_PUSH_CONTEXT's live-state sources. safety_link.h
 * only forward-declares these as void* (kiln_io_t is an anonymous-struct
 * typedef, MAX31856BusClass a named one) to keep that header dependency-free;
 * this .c file is where the frame is actually built, so it needs the real
 * types. */
#include "MAX31856.h"
#include "kiln_io.h"
#include "profile_executor_state.h"
#include "thermo_owner.h"

/* Real build identity (git commit/dirty/build timestamp), generated fresh
 * every build by gen_build_info.cmake into this component's binary dir --
 * see uart_bridge.c's build_fw_version_reply() for the PC-link twin of the
 * payload builder below. Unlike SaftyFW (TODO.md Phase 8: "build_info.h
 * generated on every build... not built this pass"), KilnFW already has
 * this, so the ANNOUNCE_VERSION frame reports real values, not a stub. */
#include "build_info.h"


static const char *TAG = "safety_link";

/* CommonFW/docs/LINK_PROTOCOL.md sec 4, SAFETY_CMD_CLEAR_TRIP (0x0A) -- see
 * safety_link.h's doc comment for the full design rationale (staleness
 * bound, why the mask is derived rather than caller-supplied, why a resend
 * after conditions change is safe). This function only does the local
 * fail-closed checks and the encode/send; the actual clear/refuse policy is
 * entirely SaftyFW's (link_task_handle_clear_trip(), read-only reference). */
esp_err_t safety_link_send_clear_trip(SafetyLinkClass *link)
{
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!safety_lock(link)) {
        return ESP_FAIL;
    }
    bool diag_known = link->cached.diag_ever_received;
    uint16_t trip_mask = link->cached.diag_trip_mask;
    /* kilnlink audit 2026-10-09 M4: read under the same lock as the mask so
     * both describe the one DIAG frame. */
    bool trip_seq_known = link->cached.diag_trip_seq_known;
    uint8_t trip_seq = link->cached.diag_trip_seq;
    uint8_t diag_state = link->cached.diag_state;
    uint32_t diag_age_ms = diag_known ? safety_elapsed_ms(link->cached_tick) : 0;
    safety_unlock(link);

    if (!diag_known) {
        ESP_LOGW(TAG, "clear_trip: refused locally, no DIAG frame ever received");
        return ESP_ERR_INVALID_STATE;
    }
    if (diag_age_ms > SAFETY_LINK_STALE_MS) {
        ESP_LOGW(TAG, "clear_trip: refused locally, cached DIAG is %" PRIu32
                       "ms old (stale beyond %ums)",
                 diag_age_ms, SAFETY_LINK_STALE_MS);
        return ESP_ERR_INVALID_STATE;
    }
    if (diag_state != SAFETY_LINK_DIAG_STATE_TRIPPED) {
        ESP_LOGW(TAG, "clear_trip: refused locally, nothing currently latched (diag_state=%u)",
                 diag_state);
        return ESP_ERR_INVALID_STATE;
    }

    kilnlink_clear_trip_t msg = {
        .trip_mask = trip_mask,
        .has_trip_seq = trip_seq_known,
        .trip_seq = trip_seq,
    };
    uint8_t payload[KILNLINK_CLEAR_TRIP_LEN_V2];
    kilnlink_clear_trip_status_t status = KILNLINK_CLEAR_TRIP_OK;
    size_t len = kilnlink_clear_trip_encode(&msg, payload, sizeof(payload), &status);
    if (len == 0) {
        ESP_LOGE(TAG, "clear_trip: encode failed (status=%d)", (int)status);
        return ESP_FAIL;
    }

    if (trip_seq_known) {
        ESP_LOGI(TAG, "clear_trip: sending, trip_mask=0x%04X trip_seq=%u", trip_mask,
                 (unsigned)trip_seq);
    } else {
        ESP_LOGI(TAG, "clear_trip: sending, trip_mask=0x%04X (unbound, DIAG had no trip_seq)",
                 trip_mask);
    }
    /* Same (dst_device, dst_task, src_task) triple as ANNOUNCE_VERSION's own
     * broadcast call site above -- fire-and-forget, no ACK expected
     * (link_task_handle_clear_trip() never replies on the wire). */
    return uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY, UART_TASK_ID_SAFETY,
                                         UART_TASK_ID_SAFETY, payload, len);
}

/* CommonFW/docs/LINK_PROTOCOL.md sec 4, SAFETY_CMD_SET_CONFIG (0x16) -- see
 * safety_link.h's doc comment for the full design rationale (why tc_type is
 * an operator choice rather than derived, why the range check here is
 * looser than SaftyFW's own). This function only does the wire-level range
 * check and the encode/send; the ARMED-refusal and TC-type-recognised
 * policy are entirely SaftyFW's (config_store_decide_write(),
 * link_task_handle_set_config(), read-only reference). */
esp_err_t safety_link_send_set_config(SafetyLinkClass *link, uint8_t tc_type)
{
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (tc_type > 0x0Fu) {
        /* MAX31856 CR1 TC[3:0] is a 4-bit field -- anything above it can
         * never be a legal register value on either side of the link,
         * whatever SaftyFW's own narrower enum eventually decides about it. */
        ESP_LOGW(TAG, "set_config: refused locally, tc_type=%u out of the 0-0x0F wire range",
                 (unsigned)tc_type);
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    kilnlink_set_config_t msg = { .tc_type = tc_type };
    uint8_t payload[KILNLINK_SET_CONFIG_LEN];
    kilnlink_set_config_status_t status = KILNLINK_SET_CONFIG_OK;
    size_t len = kilnlink_set_config_encode(&msg, payload, sizeof(payload), &status);
    if (len == 0) {
        ESP_LOGE(TAG, "set_config: encode failed (status=%d)", (int)status);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "set_config: sending, tc_type=%u", (unsigned)tc_type);
    /* Same (dst_device, dst_task, src_task) triple as CLEAR_TRIP's own
     * broadcast call site above -- fire-and-forget, no ACK expected
     * (link_task_handle_set_config() never replies on the wire). */
    return uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY, UART_TASK_ID_SAFETY,
                                         UART_TASK_ID_SAFETY, payload, len);
}

/* CommonFW/docs/LINK_PROTOCOL.md sec 4, SAFETY_CMD_SET_LOG_LEVEL (0x1B) --
 * see safety_link.h's doc comment for the full design rationale. Same
 * wire-range-check-then-encode-then-broadcast shape as
 * safety_link_send_set_config() above; the only difference is the field
 * (log verbosity, not tc_type) and the range it is checked against
 * (UART_LOG_LEVEL_* rather than the MAX31856 CR1 nibble). */
esp_err_t safety_link_send_set_log_level(SafetyLinkClass *link, uint8_t level)
{
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (level > UART_LOG_LEVEL_VERBOSE) {
        /* Same "refuse locally rather than let a bogus value hit the wire"
         * discipline as safety_link_send_set_config()'s tc_type check --
         * SaftyFW's link_task_handle_set_log_level() checks its own copy of
         * this range again on receipt, but there is no reason to ship a
         * value this driver already knows is illegal. */
        ESP_LOGW(TAG, "set_log_level: refused locally, level=%u out of the 0-%u wire range",
                 (unsigned)level, (unsigned)UART_LOG_LEVEL_VERBOSE);
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    kilnlink_set_log_level_t msg = { .level = level };
    uint8_t payload[KILNLINK_SET_LOG_LEVEL_LEN];
    kilnlink_set_log_level_status_t status = KILNLINK_SET_LOG_LEVEL_OK;
    size_t len = kilnlink_set_log_level_encode(&msg, payload, sizeof(payload), &status);
    if (len == 0) {
        ESP_LOGE(TAG, "set_log_level: encode failed (status=%d)", (int)status);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "set_log_level: sending, level=%u", (unsigned)level);
    /* Same (dst_device, dst_task, src_task) triple as SET_CONFIG's own
     * broadcast call site above -- fire-and-forget, no ACK expected
     * (link_task_handle_set_log_level() never replies on the wire). */
    return uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY, UART_TASK_ID_SAFETY,
                                         UART_TASK_ID_SAFETY, payload, len);
}

/* CommonFW/docs/LINK_PROTOCOL.md sec 4, SAFETY_CMD_ROLLBACK (0x17) --
 * tools/PcTools/TODO.md's `ota_rollback(processor)` line, Pico half (this
 * driver's own ota_http.c owns the ESP half, POST /api/ota/esp/rollback).
 * Same fire-and-forget BROADCAST shape as safety_link_send_clear_trip()/
 * safety_link_send_set_config() above: the Pico's link_task.c never ACKs
 * this on the wire (see link_task_handle_rollback()), so there is no reply
 * to wait for here -- the outcome is observed the same way CLEAR_TRIP's is,
 * by the caller polling GET_STATUS/GET_FW_VERSION afterward (a successful
 * rollback reboots the Pico, which shows up as a link drop-and-recover with
 * a new boot_id), or via the SaftyFW log if a debug probe is attached.
 *
 * No arguments -- unlike SET_CONFIG's tc_type, there is nothing for this
 * driver to validate or supply; every refusal reason (ARMED, or no valid
 * slot to fall back to) is entirely SaftyFW's decision
 * (update_task_request_rollback(), bootloader_decide_rollback()), read-only
 * reference from here. */
esp_err_t safety_link_send_rollback(SafetyLinkClass *link)
{
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    kilnlink_rollback_t msg = {0};
    uint8_t payload[KILNLINK_ROLLBACK_LEN];
    kilnlink_rollback_status_t status = KILNLINK_ROLLBACK_OK;
    size_t len = kilnlink_rollback_encode(&msg, payload, sizeof(payload), &status);
    if (len == 0) {
        ESP_LOGE(TAG, "rollback: encode failed (status=%d)", (int)status);
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "rollback: sending -- requesting the safety processor revert to its "
                  "previous bootloader slot");
    /* Same (dst_device, dst_task, src_task) triple as CLEAR_TRIP/SET_CONFIG's
     * own broadcast call sites above -- fire-and-forget, no ACK expected
     * (link_task_handle_rollback() never replies on the wire). */
    return uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY, UART_TASK_ID_SAFETY,
                                         UART_TASK_ID_SAFETY, payload, len);
}

/* Send-burst sizing for safety_link_send_rollback_ex() -- the request leg is
 * a fire-and-forget BROADCAST (link_task_handle_rollback() never ACKs it),
 * exactly the same shape ANNOUNCE_VERSION already sends over, and the same
 * loss argument applies: a single dropped frame must not be read as "the
 * Pico refused" OR "the Pico accepted" when it may simply never have
 * arrived. Reuses ANNOUNCE_VERSION's own tuning verbatim (4 sends, 250ms
 * apart -- safety_link_send_announce_version_burst()) rather than inventing
 * a second number with no bench data behind it. */
#define SAFETY_LINK_ROLLBACK_SEND_REPEATS 4u
#define SAFETY_LINK_ROLLBACK_SEND_REPEAT_GAP_MS 250u

/* How long to watch the peer's boot_id for a change after the send-burst/
 * reply-window leg above produced no refusal. This is the ONLY window that
 * can turn silence into SAFETY_LINK_ROLLBACK_OUTCOME_ACCEPTED (defect fix:
 * silence alone, or silence plus a version guess, used to be enough -- see
 * safety_link_rollback_outcome_t's doc comment), so it has to be sized
 * against what a rollback reboot actually costs, not against
 * SAFETY_LINK_REPLY_TIMEOUT_MS (~345ms at 230400 baud -- one frame's flight
 * time, not a reboot's). A rollback reboot is: persist the updated
 * bootloader metadata record to flash, watchdog_reboot(), the RP2040
 * bootloader re-validating and jumping to the other slot, SaftyFW's app
 * re-init, and its own boot-time FW_VERSION push reaching this side and
 * being applied by safety_apply_fw_version() -- several hundred ms of flash
 * write and reboot alone before a single byte is back on the wire. 5s gives
 * that sequence more than an order of magnitude of headroom over the single-
 * frame budget while still returning a Pico that is genuinely wedged (or a
 * request that was simply lost) to the caller in bounded time rather than
 * hanging the HTTP handler indefinitely. */
#define SAFETY_LINK_ROLLBACK_BOOT_WATCH_MS 5000u
/* Poll granularity for the boot_id watch -- well under poll_period_ms's
 * default (SAFETY_POLL_PERIOD_MS, typically a few hundred ms), so a boot_id
 * change or a late-arriving stashed refusal (safety_take_stashed_rollback_
 * result()) is noticed promptly rather than sitting until the next coarse
 * tick. Cheap: each iteration is two short state_lock sections, no I/O. */
#define SAFETY_LINK_ROLLBACK_BOOT_WATCH_POLL_MS 200u

/* Closure for rollback_boot_watch_poll() below -- everything the old
 * hand-rolled watch loop closed over as ordinary locals, now passed through
 * safety_link_await_or_unknown()'s void *ctx instead. commit_before/
 * datetime_before are borrowed pointers into safety_link_send_rollback_ex()'s
 * own stack buffers, valid for the lifetime of that call (the only caller). */
typedef struct {
    SafetyLinkClass *link;
    bool had_boot_id;
    uint8_t boot_id_before;
    bool had_build;
    const uint8_t *commit_before;
    uint8_t commit_len_before;
    const uint8_t *datetime_before;
    uint8_t datetime_len_before;
    bool logged_boot_id_without_build; /* dedup for the "looks like an unrelated reboot" log */
    bool stopped_early; /* true once this poll_fn itself has returned a non-PENDING verdict */
    safety_link_rollback_outcome_t *out_outcome;
    uint8_t *out_reason_code;
} rollback_boot_watch_ctx_t;

/* M15 B2: safety_link_await_poll_fn for safety_link_send_rollback_ex()'s
 * boot_id-reconnect watch. Byte-for-byte the same decisions the old inline
 * loop body made each iteration -- see that loop's original comments,
 * preserved below at each branch. Returns ACKED for every path that used to
 * `return ESP_OK` early (a stashed late reply, or a confirmed reboot);
 * PENDING for every path that used to fall through to the next iteration.
 * Never returns UNKNOWN itself -- that verdict belongs to the timeout case,
 * which safety_link_await_or_unknown() reports on its own once poll_fn has
 * had no terminal verdict for the full watch window. */
static safety_link_await_poll_t rollback_boot_watch_poll(void *ctx_v)
{
    rollback_boot_watch_ctx_t *ctx = (rollback_boot_watch_ctx_t *)ctx_v;
    SafetyLinkClass *link = ctx->link;

    uart_proto_message_t stashed;
    if (safety_take_stashed_rollback_result(link, &stashed)) {
        kilnlink_rollback_result_t late_result = {0};
        bool late_ok = (kilnlink_rollback_result_decode(stashed.payload, stashed.length, &late_result) ==
                        KILNLINK_ROLLBACK_RESULT_OK);
        *ctx->out_outcome = safety_link_rollback_infer_outcome(true, late_ok, false);
        if (*ctx->out_outcome == SAFETY_LINK_ROLLBACK_OUTCOME_REFUSED) {
            if (ctx->out_reason_code) {
                *ctx->out_reason_code = late_result.reason;
            }
            ESP_LOGW(TAG, "rollback: a refusal arrived AFTER the reply window closed but was stashed and "
                          "found by the boot_id watch -- REFUSED (reason=%u), not the ACCEPTED this driver "
                          "used to infer from silence", (unsigned)late_result.reason);
        } else {
            ESP_LOGE(TAG, "rollback: a stashed ROLLBACK_RESULT-shaped frame failed to decode");
        }
        ctx->stopped_early = true;
        return SAFETY_LINK_AWAIT_ACKED;
    }

    bool boot_id_changed_now = false;
    bool build_changed_now = false;
    if (safety_lock(link)) {
        boot_id_changed_now = safety_link_rollback_boot_id_changed(ctx->had_boot_id, ctx->boot_id_before,
                                                                    link->pico_boot_id_known, link->pico_boot_id);
        build_changed_now = safety_link_rollback_build_identity_changed(
            ctx->had_build, ctx->commit_len_before, ctx->commit_before, ctx->datetime_len_before,
            ctx->datetime_before, link->peer_build_known, link->peer_build_commit_len, link->peer_build_commit,
            link->peer_build_datetime_len, link->peer_build_datetime);
        safety_unlock(link);
    }
    if (safety_link_rollback_reboot_confirmed(boot_id_changed_now, build_changed_now)) {
        *ctx->out_outcome = safety_link_rollback_infer_outcome(false, false, true);
        ESP_LOGW(TAG, "rollback: no refusal, and the peer's boot_id AND build identity both changed "
                      "within the %ums watch -- ACCEPTED (reboot into a different image observed, not "
                      "inferred from silence or from a boot_id change alone)",
                 (unsigned)SAFETY_LINK_ROLLBACK_BOOT_WATCH_MS);
        ctx->stopped_early = true;
        return SAFETY_LINK_AWAIT_ACKED;
    } else if (boot_id_changed_now && !ctx->logged_boot_id_without_build) {
        /* Evidence this driver deliberately does NOT accept (opus-review
         * finding 2): the boot_id moved but the build identity did not
         * (or could not be compared), which is exactly what an
         * unrelated crash/watchdog/power-glitch reboot inside this
         * watch window looks like. Logged once (this condition is now
         * true for every remaining iteration of the watch, since the
         * boot_id does not un-change), not reported -- the watch keeps
         * running rather than returning here, since a genuine rollback
         * reboot could still complete and its own FW_VERSION (carrying
         * the new build identity) arrive before the window closes. */
        ctx->logged_boot_id_without_build = true;
        ESP_LOGW(TAG, "rollback: the peer's boot_id changed within the watch but its build identity did "
                      "not (or is not yet known) -- NOT reporting ACCEPTED, this looks like an unrelated "
                      "reboot rather than a rollback; continuing to watch");
    }

    return SAFETY_LINK_AWAIT_PENDING;
}

/* See safety_link.h's doc comment on safety_link_send_rollback_ex() and
 * safety_link_rollback_outcome_t for the full design. Structured like
 * safety_link_send_commit_config() above (own xact_lock hold, drain-then-
 * send-then-drain-for-the-reply shape) for the send-burst/reply-window leg,
 * plus a boot_id-reconnect watch AFTER releasing xact_lock -- see that
 * release's own comment below for why the lock must not be held through the
 * watch. */
esp_err_t safety_link_send_rollback_ex(SafetyLinkClass *link, safety_link_rollback_outcome_t *out_outcome,
                                        uint8_t *out_reason_code)
{
    safety_link_rollback_outcome_t local_outcome = SAFETY_LINK_ROLLBACK_OUTCOME_LINK_DOWN;
    if (!out_outcome) {
        out_outcome = &local_outcome;
    }
    *out_outcome = SAFETY_LINK_ROLLBACK_OUTCOME_LINK_DOWN;

    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Checked BEFORE anything is sent, and treated as its own outcome
     * rather than left to fall out of a later timeout -- see safety_link_
     * rollback_outcome_t's own doc comment: this is exactly the distinction
     * that keeps "the link was down the whole time" from ever being
     * misread as "the Pico is thinking about it." */
    safety_link_status_t link_status;
    if (safety_link_get_status(link, &link_status) != ESP_OK || !link_status.link_up) {
        ESP_LOGW(TAG, "rollback: refused locally, the safety link is down -- not sending");
        *out_outcome = SAFETY_LINK_ROLLBACK_OUTCOME_LINK_DOWN;
        return ESP_OK;
    }

    /* Snapshot the peer's boot_id BEFORE anything is sent -- this, not a
     * version guess, is the sole positive evidence this function will ever
     * accept for ACCEPTED (defect fix: the old peer_can_reply gate read
     * THIS side's cached view of the PICO's protocol_version, but whether
     * the Pico SENDS 0x25 is decided by the Pico's own cached view of the
     * ESP's protocol_version -- a value this side cannot observe at all;
     * removed rather than patched, see safety_link_rollback_outcome_t's
     * ACCEPTED doc comment). If had_boot_id comes back false there is no
     * baseline to compare against at all, and safety_link_rollback_boot_id_
     * changed() (safety_link.h) therefore reports "no evidence" for the
     * whole watch rather than treating the first FW_VERSION that happens to
     * arrive as a reboot -- see that function's own comment for why the
     * opposite convention in safety_apply_fw_version() is right there and
     * wrong here. */
    bool had_boot_id = false;
    uint8_t boot_id_before = 0;
    if (safety_lock(link)) {
        had_boot_id = link->pico_boot_id_known;
        boot_id_before = link->pico_boot_id;
        safety_unlock(link);
    }

    /* Snapshot the peer's build identity BEFORE anything is sent too -- see
     * opus-review finding 2, "a reboot is not a rollback": the boot_id watch
     * below on its own cannot tell a rollback reboot apart from an unrelated
     * crash/watchdog/power-glitch reboot inside the same window, since both
     * change the boot_id identically. The commit+datetime pair from the same
     * FW_VERSION frame is independent evidence -- a rollback reboots into
     * the OTHER bootloader slot (a different build), an unrelated reboot
     * comes back running the SAME one. safety_link_get_peer_build_status()
     * copies out the full 64/32-byte buffers regardless of length; the *_len
     * outputs are what safety_link_rollback_build_identity_changed() (safety_
     * link.h) actually compares against. */
    bool had_build = false;
    uint8_t commit_before[64];
    uint8_t commit_len_before = 0;
    uint8_t datetime_before[32];
    uint8_t datetime_len_before = 0;
    (void)safety_link_get_peer_build_status(link, &had_build, NULL, commit_before, &commit_len_before,
                                             datetime_before, &datetime_len_before, NULL, NULL);

    kilnlink_rollback_t msg = {0};
    uint8_t payload[KILNLINK_ROLLBACK_LEN];
    kilnlink_rollback_status_t status = KILNLINK_ROLLBACK_OK;
    size_t len = kilnlink_rollback_encode(&msg, payload, sizeof(payload), &status);
    if (len == 0) {
        ESP_LOGE(TAG, "rollback: encode failed (status=%d)", (int)status);
        *out_outcome = SAFETY_LINK_ROLLBACK_OUTCOME_SEND_FAILED;
        return ESP_OK;
    }

    if (xSemaphoreTake(link->xact_lock, pdMS_TO_TICKS(SAFETY_XACT_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "rollback: timed out waiting for the safety link transaction lock");
        *out_outcome = SAFETY_LINK_ROLLBACK_OUTCOME_SEND_FAILED;
        return ESP_OK;
    }
    (void)safety_drain_inbox(link, 0);
    /* A refusal stashed by a PRIOR, already-resolved rollback attempt must
     * never be read as this attempt's own answer -- see safety_clear_
     * stashed_rollback_result()'s own comment. */
    safety_clear_stashed_rollback_result(link);

    ESP_LOGW(TAG, "rollback: sending -- requesting the safety processor revert to its "
                  "previous bootloader slot (burst of %u, %ums apart)",
             (unsigned)SAFETY_LINK_ROLLBACK_SEND_REPEATS, (unsigned)SAFETY_LINK_ROLLBACK_SEND_REPEAT_GAP_MS);

    uart_proto_message_t result_msg;
    bool got_result = false;
    bool sent_at_least_once = false;
    for (unsigned n = 0; n < SAFETY_LINK_ROLLBACK_SEND_REPEATS; n++) {
        esp_err_t send_err = uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY,
                                                           UART_TASK_ID_SAFETY, UART_TASK_ID_SAFETY, payload, len);
        if (send_err == ESP_OK) {
            sent_at_least_once = true;
        } else if (!sent_at_least_once) {
            /* Never left this board even once -- a local UART fault, not a
             * peer response. Give up now rather than spend the rest of the
             * burst budget on a link that cannot transmit at all. */
            xSemaphoreGive(link->xact_lock);
            ESP_LOGE(TAG, "rollback: send failed on the first attempt: %s", esp_err_to_name(send_err));
            *out_outcome = SAFETY_LINK_ROLLBACK_OUTCOME_SEND_FAILED;
            return ESP_OK;
        } else {
            /* A later repeat failed locally after at least one earlier send
             * already reached the UART -- log and keep listening for a
             * reply to the send(s) that DID go out, rather than discard
             * that progress over one bad repeat. */
            ESP_LOGW(TAG, "rollback: repeat %u/%u send failed: %s (continuing -- an earlier "
                          "send already reached the UART)",
                     n + 1u, (unsigned)SAFETY_LINK_ROLLBACK_SEND_REPEATS, esp_err_to_name(send_err));
        }

        bool last_repeat = (n + 1u == SAFETY_LINK_ROLLBACK_SEND_REPEATS);
        uint32_t wait_ms = last_repeat ? SAFETY_LINK_REPLY_TIMEOUT_MS : SAFETY_LINK_ROLLBACK_SEND_REPEAT_GAP_MS;
        (void)safety_drain_inbox_ex(link, wait_ms, false, NULL, NULL, NULL, NULL, NULL, NULL, &result_msg,
                                     &got_result);
        if (got_result) {
            break;
        }
    }
    /* Released here, BEFORE the boot_id watch below -- holding it through a
     * multi-second watch would block safety_exchange()'s own xact_lock hold
     * (the ordinary GET_STATUS poll), which is exactly the mechanism that
     * keeps link_up current and drains the FW_VERSION pushes the watch is
     * waiting to see. Blocking the poll task here would make the watch wait
     * on a poll it is itself preventing from running. */
    xSemaphoreGive(link->xact_lock);

    if (got_result) {
        kilnlink_rollback_result_t result = {0};
        bool decoded_ok = (kilnlink_rollback_result_decode(result_msg.payload, result_msg.length, &result) ==
                            KILNLINK_ROLLBACK_RESULT_OK);
        if (decoded_ok && result.accepted) {
            // No sender in this codebase ever sets accepted=1 today
            // (kilnlink_rollback_result.h's own comment), but a future one
            // might -- an explicit accepted=1 is stronger evidence than the
            // boot_id watch below, so honor it directly rather than routing
            // it through safety_link_rollback_infer_outcome() (which has no
            // "explicit accept" input, only refusal/boot_id).
            *out_outcome = SAFETY_LINK_ROLLBACK_OUTCOME_ACCEPTED;
            ESP_LOGW(TAG, "rollback: peer reported accepted=1 (unexpected but honored)");
            return ESP_OK;
        }
        *out_outcome = safety_link_rollback_infer_outcome(true, decoded_ok, false);
        if (*out_outcome == SAFETY_LINK_ROLLBACK_OUTCOME_REFUSED) {
            if (out_reason_code) {
                *out_reason_code = result.reason;
            }
            ESP_LOGW(TAG, "rollback: REFUSED by the safety processor (reason=%u)", (unsigned)result.reason);
        } else {
            // Decoded frame matched the id/length gate in safety_drain_
            // inbox_ex() but failed the codec's own checks -- should not
            // happen for a frame this driver's own gate already filtered on
            // length, but do not claim an outcome this driver cannot prove.
            ESP_LOGE(TAG, "rollback: a ROLLBACK_RESULT-shaped frame arrived but failed to decode");
        }
        return ESP_OK;
    }

    // No refusal within the send-burst/reply-window leg. Per kilnlink_
    // rollback_result.h's "ASYMMETRIC BY DESIGN" comment, the only
    // trustworthy positive evidence left is the peer's boot_id changing --
    // watch for that (or a refusal that arrives late enough to be stashed
    // instead) for up to SAFETY_LINK_ROLLBACK_BOOT_WATCH_MS. A timeout here
    // must never be misreported as success (kilnlink_rollback_result.h,
    // CommonFW/docs/LINK_PROTOCOL.md sec 4) -- see safety_link_rollback_
    // infer_outcome()'s fallthrough to UNKNOWN_TIMEOUT below.
    rollback_boot_watch_ctx_t watch_ctx = {
        .link = link,
        .had_boot_id = had_boot_id,
        .boot_id_before = boot_id_before,
        .had_build = had_build,
        .commit_before = commit_before,
        .commit_len_before = commit_len_before,
        .datetime_before = datetime_before,
        .datetime_len_before = datetime_len_before,
        .logged_boot_id_without_build = false,
        .out_outcome = out_outcome,
        .out_reason_code = out_reason_code,
    };
    /* M15 B2: this used to be a hand-rolled poll loop, the ONLY place this
     * driver's "never infer success from silence" discipline existed before
     * safety_link_await_or_unknown() (safety_link.h/.c) extracted it into a
     * shared helper. rollback_boot_watch_poll() below is byte-for-byte the
     * same per-iteration logic that used to live directly in this loop --
     * only where the "keep polling vs. stop" decision is made moved. */
    safety_link_await_result_t watch_result = safety_link_await_or_unknown(
        SAFETY_LINK_ROLLBACK_BOOT_WATCH_MS, SAFETY_LINK_ROLLBACK_BOOT_WATCH_POLL_MS, rollback_boot_watch_poll,
        &watch_ctx);
    if (watch_result == SAFETY_LINK_AWAIT_UNKNOWN && !watch_ctx.stopped_early) {
        /* Only reached via a plain timeout -- rollback_boot_watch_poll()
         * never returns SAFETY_LINK_AWAIT_UNKNOWN itself (every terminal
         * path it takes is ACKED; PENDING just keeps the watch going), so
         * this is exactly the "budget exhausted, no verdict" case the old
         * loop's fallthrough covered. */
        *out_outcome = safety_link_rollback_infer_outcome(false, false, false);
        ESP_LOGW(TAG, "rollback: no refusal and no boot_id change within the %ums send-burst/reply window plus "
                      "%ums boot_id watch -- outcome UNKNOWN, never reported as accepted",
                 (unsigned)SAFETY_LINK_REPLY_TIMEOUT_MS, (unsigned)SAFETY_LINK_ROLLBACK_BOOT_WATCH_MS);
    }
    return ESP_OK;
}

/* CommonFW/docs/LINK_PROTOCOL.md sec 4, SAFETY_CMD_ANNOUNCE_REBOOT (0x18) --
 * see safety_link.h's doc comment for the full design rationale (why this
 * exists, why it grants no heating permission, why it is fire-and-forget).
 * Called from ota_http.c's ota_esp_reboot_task() immediately before
 * esp_restart(), same delayed-reboot-task pattern ota_rollback_reboot_task()
 * already uses for the Pico rollback endpoint. */
esp_err_t safety_link_send_announce_reboot(SafetyLinkClass *link)
{
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    kilnlink_announce_reboot_t msg = {0};
    uint8_t payload[KILNLINK_ANNOUNCE_REBOOT_LEN];
    kilnlink_announce_reboot_status_t status = KILNLINK_ANNOUNCE_REBOOT_OK;
    size_t len = kilnlink_announce_reboot_encode(&msg, payload, sizeof(payload), &status);
    if (len == 0) {
        ESP_LOGE(TAG, "announce_reboot: encode failed (status=%d)", (int)status);
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "announce_reboot: sending -- ESP is about to reboot for a routine "
                  "self-update, suppress S6b's nuisance trip for the grace window");
    /* Same (dst_device, dst_task, src_task) triple as CLEAR_TRIP/ROLLBACK's
     * own broadcast call sites above -- fire-and-forget, no ACK expected
     * (link_task.c's LINK_FRAME_ANNOUNCE_REBOOT_CMD handler never replies on
     * the wire). */
    return uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY, UART_TASK_ID_SAFETY,
                                         UART_TASK_ID_SAFETY, payload, len);
}

/* CommonFW/docs/LINK_PROTOCOL.md sec 4, SAFETY_CMD_REBOOT (0x29) / reply
 * SAFETY_CMD_REBOOT_RESULT (0x2A) -- see safety_link.h's doc comment on
 * this function and on safety_link_reboot_outcome_t for the full contract
 * (and for why this one, unlike the rollback path, never has to infer
 * acceptance from silence). Structured like safety_link_get_ct_auto_zero_
 * status(): one round trip, xact_lock held across it, the reply read from
 * the unconditional stash safety_drain_inbox_ex() fills. */
/* Bounded fallback watch for safety_link_send_reboot()'s NO_REPLY case --
 * see SAFETY_LINK_REBOOT_OUTCOME_CONFIRMED_BY_BOOT_ID's doc comment
 * (safety_link.h) for why this exists (bench evidence, 2026-09-23: the
 * Pico's link task drains its TX ring for only 10ms before rebooting, so a
 * queued REBOOT_RESULT reply is routinely lost to the reset itself even
 * though the Pico genuinely did reboot). Sized as "a few seconds", well
 * under any httpd send timeout, and far shorter than the rollback path's own
 * 5s watch (SAFETY_LINK_ROLLBACK_BOOT_WATCH_MS above) because a reboot-in-
 * place has no flash-metadata write ahead of the Pico's own watchdog reset --
 * only the ANNOUNCE/dispatch/reboot sequence itself, which the rollback
 * comment already documents as "several hundred ms" on top of a slower
 * rollback-specific flash write this path never does. Poll granularity
 * matches the rollback path's (200ms) for the same "cheap, two short
 * state_lock sections, no I/O" reasoning. SAFETY_LINK_REBOOT_BOOT_ID_WATCH_MS/
 * _POLL_MS moved to safety_link.h 2026-09-23 so sw_reset_http.c's own
 * arm-wait can derive from the same constant instead of duplicating it. */

typedef struct {
    SafetyLinkClass *link;
    bool had_boot_id;
    uint8_t boot_id_before;
    uint8_t boot_id_after; /* filled only once ACKED is returned */
} reboot_boot_watch_ctx_t;

static safety_link_await_poll_t reboot_boot_watch_poll(void *ctx_v)
{
    reboot_boot_watch_ctx_t *ctx = (reboot_boot_watch_ctx_t *)ctx_v;
    SafetyLinkClass *link = ctx->link;

    bool known_now = false;
    uint8_t boot_id_now = 0;
    if (safety_lock(link)) {
        known_now = link->pico_boot_id_known;
        boot_id_now = link->pico_boot_id;
        safety_unlock(link);
    }
    bool changed = safety_link_rollback_boot_id_changed(ctx->had_boot_id, ctx->boot_id_before, known_now,
                                                          boot_id_now);
    if (changed) {
        ctx->boot_id_after = boot_id_now;
        return SAFETY_LINK_AWAIT_ACKED;
    }
    return SAFETY_LINK_AWAIT_PENDING;
}

esp_err_t safety_link_send_reboot(SafetyLinkClass *link, safety_link_reboot_outcome_t *out_outcome,
                                   uint8_t *out_reason_code, uint8_t *out_boot_id_before,
                                   uint8_t *out_boot_id_after)
{
    safety_link_reboot_outcome_t local_outcome = SAFETY_LINK_REBOOT_OUTCOME_LINK_DOWN;
    if (!out_outcome) {
        out_outcome = &local_outcome; /* every path below still writes it, so the logs stay honest */
    }
    *out_outcome = SAFETY_LINK_REBOOT_OUTCOME_LINK_DOWN;

    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Refuse locally when the link is already known down, same first check
     * safety_link_send_rollback_ex() makes and for the same reason: sending
     * into a dead link and then reporting the resulting silence as an
     * unknown outcome would hide a fact this board already knows for
     * certain. LINK_DOWN is a distinct outcome from NO_REPLY on purpose. */
    safety_link_status_t link_status;
    if (safety_link_get_status(link, &link_status) != ESP_OK || !link_status.link_up) {
        ESP_LOGW(TAG, "reboot: refused locally, the safety link is down -- not sending");
        *out_outcome = SAFETY_LINK_REBOOT_OUTCOME_LINK_DOWN;
        return ESP_OK;
    }

    /* Snapshot the peer's boot_id BEFORE anything is sent -- the same
     * "baseline first, never manufacture evidence from an unknown-before"
     * discipline safety_link_send_rollback_ex() uses (safety_link_
     * rollback_boot_id_changed()'s own doc comment), reused here for the
     * NO_REPLY fallback watch below. Cheap even on the ordinary ACCEPTED/
     * REFUSED paths that never look at it. */
    bool had_boot_id = false;
    uint8_t boot_id_before = 0;
    if (safety_lock(link)) {
        had_boot_id = link->pico_boot_id_known;
        boot_id_before = link->pico_boot_id;
        safety_unlock(link);
    }

    kilnlink_reboot_t msg = {0};
    uint8_t payload[KILNLINK_REBOOT_LEN];
    kilnlink_reboot_status_t status = KILNLINK_REBOOT_OK;
    size_t len = kilnlink_reboot_encode(&msg, payload, sizeof(payload), &status);
    if (len == 0) {
        ESP_LOGE(TAG, "reboot: encode failed (status=%d)", (int)status);
        *out_outcome = SAFETY_LINK_REBOOT_OUTCOME_SEND_FAILED;
        return ESP_OK;
    }

    if (xSemaphoreTake(link->xact_lock, pdMS_TO_TICKS(SAFETY_XACT_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "reboot: timed out after %ums waiting for the safety link transaction lock",
                 (unsigned)SAFETY_XACT_LOCK_TIMEOUT_MS);
        *out_outcome = SAFETY_LINK_REBOOT_OUTCOME_SEND_FAILED;
        return ESP_OK;
    }

    /* A reply stashed by a PRIOR request must never be read as this one's
     * answer -- same reasoning as safety_clear_stashed_rollback_result()'s
     * own comment, and the same "anything already queued is from before our
     * request" drain safety_link_get_ct_auto_zero_status() does. */
    uart_proto_message_t stale;
    (void)safety_take_stashed_reboot_result(link, &stale);
    (void)safety_drain_inbox(link, 0);

    ESP_LOGW(TAG, "reboot: sending -- asking the safety processor to reboot in place "
                  "(same firmware slot, no configuration touched)");
    esp_err_t err = uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY,
                                                  UART_TASK_ID_SAFETY, UART_TASK_ID_SAFETY,
                                                  payload, len);
    if (err != ESP_OK) {
        if (safety_lock(link)) {
            link->stats.timeouts++;
            safety_unlock(link);
        }
        xSemaphoreGive(link->xact_lock);
        ESP_LOGE(TAG, "reboot: send failed: %s", esp_err_to_name(err));
        *out_outcome = SAFETY_LINK_REBOOT_OUTCOME_SEND_FAILED;
        return ESP_OK;
    }

    (void)safety_drain_inbox(link, SAFETY_LINK_REPLY_TIMEOUT_MS);

    uart_proto_message_t reply;
    bool got_reply = safety_take_stashed_reboot_result(link, &reply);
    xSemaphoreGive(link->xact_lock);

    if (!got_reply) {
        /* Silence within the ordinary reply window. Either a Pico too old to
         * have a dispatch case for 0x29, a lost reply, a link that died
         * mid-request, or (bench evidence, 2026-09-23) a Pico that DID
         * accept and IS rebooting but only drains its TX ring for 10ms
         * before hal_wdt_reboot() (firmware/SaftyFW/src/tasks/link_task.c,
         * LINK_TASK_REBOOT_TX_DRAIN_MS) -- routinely too short for a queued
         * reply to actually leave before the reset. xact_lock was already
         * released above, same as safety_link_send_rollback_ex()'s own
         * release before its boot_id watch, so the GET_STATUS poll that
         * keeps pico_boot_id current keeps running throughout this bounded
         * fallback. A change here is the one and only positive evidence this
         * function accepts for CONFIRMED_BY_BOOT_ID -- see that outcome's
         * doc comment (safety_link.h) for what it does and does not prove.
         * No change within the watch falls through to plain NO_REPLY. */
        reboot_boot_watch_ctx_t watch_ctx = {
            .link = link,
            .had_boot_id = had_boot_id,
            .boot_id_before = boot_id_before,
            .boot_id_after = boot_id_before,
        };
        safety_link_await_result_t watch_result = safety_link_await_or_unknown(
            SAFETY_LINK_REBOOT_BOOT_ID_WATCH_MS, SAFETY_LINK_REBOOT_BOOT_ID_WATCH_POLL_MS,
            reboot_boot_watch_poll, &watch_ctx);
        bool boot_id_confirmed = (watch_result == SAFETY_LINK_AWAIT_ACKED);
        *out_outcome = safety_link_reboot_infer_boot_id_outcome(boot_id_confirmed);
        if (*out_outcome == SAFETY_LINK_REBOOT_OUTCOME_CONFIRMED_BY_BOOT_ID) {
            if (out_boot_id_before) {
                *out_boot_id_before = boot_id_before;
            }
            if (out_boot_id_after) {
                *out_boot_id_after = watch_ctx.boot_id_after;
            }
            ESP_LOGW(TAG, "reboot: no REBOOT_RESULT within %ums, but the peer's boot_id changed "
                          "(%u -> %u) within the %ums fallback watch -- CONFIRMED_BY_BOOT_ID, not "
                          "reported as UNCONFIRMED even though the ACK itself was never seen",
                     (unsigned)SAFETY_LINK_REPLY_TIMEOUT_MS, (unsigned)boot_id_before,
                     (unsigned)watch_ctx.boot_id_after, (unsigned)SAFETY_LINK_REBOOT_BOOT_ID_WATCH_MS);
        } else {
            ESP_LOGW(TAG, "reboot: no REBOOT_RESULT within %ums and no boot_id change within the "
                          "further %ums fallback watch -- outcome NOT confirmed, never reported as "
                          "accepted (a safety processor predating this command answers exactly like "
                          "this)",
                     (unsigned)SAFETY_LINK_REPLY_TIMEOUT_MS, (unsigned)SAFETY_LINK_REBOOT_BOOT_ID_WATCH_MS);
        }
        return ESP_OK;
    }

    kilnlink_reboot_result_t result = {0};
    if (kilnlink_reboot_result_decode(reply.payload, reply.length, &result) != KILNLINK_REBOOT_RESULT_OK) {
        /* Matched the id/length gate in the inbox but failed the codec's own
         * checks -- do not claim an outcome this driver cannot prove. */
        ESP_LOGE(TAG, "reboot: a REBOOT_RESULT-shaped frame arrived but failed to decode");
        *out_outcome = SAFETY_LINK_REBOOT_OUTCOME_NO_REPLY;
        return ESP_OK;
    }

    if (result.accepted) {
        ESP_LOGW(TAG, "reboot: ACCEPTED by the safety processor -- it is about to reset "
                      "(this confirms acceptance, not completion)");
        *out_outcome = SAFETY_LINK_REBOOT_OUTCOME_ACCEPTED;
        return ESP_OK;
    }

    if (out_reason_code) {
        *out_reason_code = result.reason;
    }
    ESP_LOGW(TAG, "reboot: REFUSED by the safety processor (reason=%u)", (unsigned)result.reason);
    *out_outcome = SAFETY_LINK_REBOOT_OUTCOME_REFUSED;
    return ESP_OK;
}

/* SAFETY_CMD_GET_STACK_MARGIN (0x2B) / SAFETY_CMD_STACK_MARGIN (0x2C reply),
 * KILNLINK_PROTOCOL_VERSION 13 -- see safety_link.h's doc comment for the
 * full contract. Structured like safety_link_send_reboot() above (xact_lock,
 * clear-any-stale-stash-first, drain, stashed-reply take), NOT like
 * safety_link_get_ct_cal() (which threads a want/got pair through
 * safety_drain_inbox_ex()) -- see stashed_stack_margin's own comment in
 * safety_link.h for why. */
esp_err_t safety_link_get_stack_margin(SafetyLinkClass *link, kilnlink_stack_margin_t *out)
{
    if (!link || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    kilnlink_get_stack_margin_t req = {0};
    uint8_t request[KILNLINK_GET_STACK_MARGIN_LEN];
    kilnlink_get_stack_margin_status_t req_status = KILNLINK_GET_STACK_MARGIN_OK;
    size_t req_len = kilnlink_get_stack_margin_encode(&req, request, sizeof(request), &req_status);
    if (req_len == 0) {
        ESP_LOGE(TAG, "get_stack_margin: encode failed (status=%d)", (int)req_status);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(link->xact_lock, pdMS_TO_TICKS(SAFETY_XACT_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "get_stack_margin: timed out after %ums waiting for the safety link "
                      "transaction lock", (unsigned)SAFETY_XACT_LOCK_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    /* A reply stashed by a PRIOR request must never be read as this one's
     * answer -- same reasoning as safety_link_send_reboot()'s own stale-take
     * above. */
    uart_proto_message_t stale;
    (void)safety_take_stashed_stack_margin(link, &stale);
    (void)safety_drain_inbox(link, 0);

    if (safety_lock(link)) {
        link->stats.frames_sent++;
        safety_unlock(link);
    }

    esp_err_t err = uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY,
                                                  UART_TASK_ID_SAFETY, UART_TASK_ID_SAFETY,
                                                  request, req_len);
    if (err != ESP_OK) {
        if (safety_lock(link)) {
            link->stats.timeouts++;
            safety_unlock(link);
        }
        xSemaphoreGive(link->xact_lock);
        return err;
    }

    /* Loop the drain across the full budget rather than one wait_ms call:
     * safety_drain_inbox_ex()'s want_ / got_ out-param plumbing (safety_drain_
     * still_waiting() in safety_link.h) has no STACK_MARGIN case, so a single
     * safety_drain_inbox() call degrades to a zero-wait drain the moment ANY
     * other frame is dispatched first -- and the Pico's periodic STATUS/DIAG/
     * POWER pushes share this same inbox. A push landing before our own
     * STACK_MARGIN reply used to end the wait early and report ESP_ERR_TIMEOUT
     * even though the reply was still in flight, the same shape as the
     * CONFIG_PAGE/CT_CAL bug safety_drain_still_waiting()'s header comment
     * documents. Re-checking the stash after every drained frame, until the
     * declared budget (not a frame count) is spent, closes it. */
    uart_proto_message_t reply;
    bool got_reply = false;
    int64_t sm_wait_started_us = (int64_t)hal_time_now_us();
    for (;;) {
        int64_t elapsed_ms = ((int64_t)hal_time_now_us() - sm_wait_started_us) / 1000;
        int32_t remaining_ms = (int32_t)SAFETY_LINK_REPLY_TIMEOUT_MS - (int32_t)elapsed_ms;
        if (remaining_ms < 0) {
            remaining_ms = 0;
        }
        if (remaining_ms < (int32_t)portTICK_PERIOD_MS) {
            remaining_ms = 0;
        }
        (void)safety_drain_inbox(link, (uint32_t)remaining_ms);
        got_reply = safety_take_stashed_stack_margin(link, &reply);
        if (got_reply || remaining_ms == 0) {
            break;
        }
    }
    xSemaphoreGive(link->xact_lock);

    if (!got_reply) {
        /* Silence -- a Pico too old to have a dispatch case for 0x2B, a lost
         * reply, or a dead link. Never fabricate a reading. */
        if (safety_lock(link)) {
            link->stats.timeouts++;
            safety_unlock(link);
        }
        ESP_LOGW(TAG, "get_stack_margin: no STACK_MARGIN reply within %ums (a safety processor "
                      "predating KILNLINK_PROTOCOL_VERSION 13 answers exactly like this)",
                 (unsigned)SAFETY_LINK_REPLY_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    if (kilnlink_stack_margin_decode(reply.payload, reply.length, out) != KILNLINK_STACK_MARGIN_OK) {
        ESP_LOGE(TAG, "get_stack_margin: a STACK_MARGIN-shaped frame arrived but failed to decode");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* SAFETY_CMD_GET_PARAM (0x23) / SAFETY_CMD_PARAM (0x1E reply), KILNLINK_
 * PROTOCOL_VERSION 7 -- see safety_link.h's doc comment for the full
 * contract. Structured like safety_link_get_stack_margin() above (xact_lock,
 * clear-any-stale-stash-first, drain, stashed-reply take), NOT like
 * safety_link_get_ct_cal() -- see stashed_param's own comment in
 * safety_link.h for why. Unlike get_stack_margin, the reply is relayed raw
 * (copied verbatim into `out`), not decoded into a kilnlink_param_t here --
 * same "the ESP relays this frame to the PC unmodified" split safety_link_
 * get_ct_cal() uses, since the PC-facing bridge case (uart_bridge_safety.c)
 * just forwards the bytes on. */
esp_err_t safety_link_get_param(SafetyLinkClass *link, uint16_t param_id, uint8_t *out, size_t out_cap,
                                 size_t *out_len)
{
    if (!link || !out || out_cap < KILNLINK_PARAM_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    kilnlink_get_param_t req = { .param_id = param_id };
    uint8_t request[KILNLINK_GET_PARAM_LEN];
    kilnlink_get_param_status_t req_status = KILNLINK_GET_PARAM_OK;
    size_t req_len = kilnlink_get_param_encode(&req, request, sizeof(request), &req_status);
    if (req_len == 0) {
        ESP_LOGE(TAG, "get_param: encode failed (status=%d)", (int)req_status);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(link->xact_lock, pdMS_TO_TICKS(SAFETY_XACT_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "get_param: timed out after %ums waiting for the safety link "
                      "transaction lock", (unsigned)SAFETY_XACT_LOCK_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    /* A reply stashed by a PRIOR request must never be read as this one's
     * answer -- same reasoning as safety_link_get_stack_margin()'s own
     * stale-take above. */
    uart_proto_message_t stale;
    (void)safety_take_stashed_param(link, &stale);
    (void)safety_drain_inbox(link, 0);

    if (safety_lock(link)) {
        link->stats.frames_sent++;
        safety_unlock(link);
    }

    esp_err_t err = uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY,
                                                  UART_TASK_ID_SAFETY, UART_TASK_ID_SAFETY,
                                                  request, req_len);
    if (err != ESP_OK) {
        if (safety_lock(link)) {
            link->stats.timeouts++;
            safety_unlock(link);
        }
        xSemaphoreGive(link->xact_lock);
        return err;
    }

    /* Loop the drain across the full budget, same fix and same reason as
     * safety_link_get_stack_margin() above: safety_drain_inbox_ex()'s want_ /
     * got_ out-param plumbing has no PARAM case, so a single safety_drain_inbox() call
     * degrades to a zero-wait drain the moment any other frame (a periodic
     * STATUS/DIAG/POWER push) is dispatched first, which can end the wait
     * before our own PARAM reply lands. */
    uart_proto_message_t reply;
    bool got_reply = false;
    int64_t gp_wait_started_us = (int64_t)hal_time_now_us();
    for (;;) {
        int64_t elapsed_ms = ((int64_t)hal_time_now_us() - gp_wait_started_us) / 1000;
        int32_t remaining_ms = (int32_t)SAFETY_LINK_REPLY_TIMEOUT_MS - (int32_t)elapsed_ms;
        if (remaining_ms < 0) {
            remaining_ms = 0;
        }
        if (remaining_ms < (int32_t)portTICK_PERIOD_MS) {
            remaining_ms = 0;
        }
        (void)safety_drain_inbox(link, (uint32_t)remaining_ms);
        got_reply = safety_take_stashed_param(link, &reply);
        if (got_reply || remaining_ms == 0) {
            break;
        }
    }
    xSemaphoreGive(link->xact_lock);

    if (!got_reply) {
        /* Silence -- a Pico too old to have a dispatch case for 0x23, a lost
         * reply, or a dead link. Never fabricate a reading. */
        if (safety_lock(link)) {
            link->stats.timeouts++;
            safety_unlock(link);
        }
        ESP_LOGW(TAG, "get_param: no PARAM reply within %ums (a safety processor predating "
                      "KILNLINK_PROTOCOL_VERSION 7 answers exactly like this)",
                 (unsigned)SAFETY_LINK_REPLY_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    /* The stash's bound check (safety_drain_inbox_ex()'s KILNLINK_PARAM_CMD
     * case) only proved the length is plausible, not that this reply
     * actually answers OUR request -- a very late reply to a prior GET_PARAM
     * call could in principle land here if it arrived in the narrow window
     * between the stale-take above and this request's own send. param_id is
     * bytes 1-2 (LE) of the payload, right after the cmd byte; check it
     * directly rather than fully decoding, since a mismatch must be treated
     * as "no answer for THIS request" regardless of whether the rest of the
     * frame would otherwise decode cleanly. */
    if (reply.length < KILNLINK_PARAM_HDR_LEN) {
        ESP_LOGE(TAG, "get_param: stashed PARAM reply shorter than its own header");
        return ESP_FAIL;
    }
    uint16_t reply_param_id = (uint16_t)reply.payload[1] | ((uint16_t)reply.payload[2] << 8);
    if (reply_param_id != param_id) {
        ESP_LOGW(TAG, "get_param: dropped a PARAM reply for id 0x%04X, requested 0x%04X",
                 (unsigned)reply_param_id, (unsigned)param_id);
        return ESP_FAIL;
    }

    memcpy(out, reply.payload, reply.length);
    if (out_len) {
        *out_len = reply.length;
    }
    return ESP_OK;
}

/* CommonFW/docs/LINK_PROTOCOL.md sec 4, SAFETY_CMD_SET_CT_CAL (0x19) -- see
 * safety_link.h's doc comment for the full design rationale. This function
 * only does the local channel-range check and the encode/send; the
 * ARMED-refusal policy is entirely SaftyFW's (config_store_decide_write(),
 * read-only reference from here), same split safety_link_send_set_config()
 * uses for tc_type. */
esp_err_t safety_link_send_set_ct_cal(SafetyLinkClass *link, uint8_t channel, bool calibrated,
                                       float gain, float offset)
{
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (channel >= KILNLINK_SET_CT_CAL_NUM_CHANNELS) {
        ESP_LOGW(TAG, "set_ct_cal: refused locally, channel=%u out of range 0-%u",
                 (unsigned)channel, (unsigned)KILNLINK_SET_CT_CAL_NUM_CHANNELS - 1u);
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* An uncalibrated channel sends explicit gain=0/offset=0 regardless of
     * what the caller passed -- same "belt and suspenders against stale
     * numbers" choice push_ct_cal.py's encode_set_ct_cal() makes on the PC
     * side (see safety_link.h). */
    kilnlink_set_ct_cal_t msg = {
        .channel = channel,
        .calibrated = calibrated ? 1u : 0u,
        .gain = calibrated ? gain : 0.0f,
        .offset = calibrated ? offset : 0.0f,
    };
    uint8_t payload[KILNLINK_SET_CT_CAL_LEN];
    kilnlink_set_ct_cal_status_t status = KILNLINK_SET_CT_CAL_OK;
    size_t len = kilnlink_set_ct_cal_encode(&msg, payload, sizeof(payload), &status);
    if (len == 0) {
        ESP_LOGE(TAG, "set_ct_cal: encode failed (status=%d)", (int)status);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "set_ct_cal: sending, channel=%u calibrated=%u", (unsigned)channel,
             (unsigned)msg.calibrated);
    /* Same (dst_device, dst_task, src_task) triple as SET_CONFIG's own
     * broadcast call site above -- fire-and-forget, no ACK expected
     * (SaftyFW's link_task.c never replies to SET_CT_CAL on the wire). */
    return uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY, UART_TASK_ID_SAFETY,
                                         UART_TASK_ID_SAFETY, payload, len);
}

/* CommonFW/docs/LINK_PROTOCOL.md sec 4/6, SAFETY_CMD_GET_CT_CAL /
 * SAFETY_CMD_CT_CAL (0x22 request / 0x1A reply -- separate ids since
 * KILNLINK_PROTOCOL_VERSION 7) -- see safety_link.h's doc comment for
 * the full contract. Structured like safety_exchange() (same xact_lock,
 * same "drain anything already queued first" discipline, same
 * SAFETY_LINK_ACK_TIMEOUT_MS/SAFETY_LINK_REPLY_TIMEOUT_MS budget) rather than
 * calling it, because safety_exchange()'s `expect_status` parameter only
 * ever watches for SAFETY_CMD_GET_STATUS -- there is no way to ask it to wait
 * for a CT_CAL reply instead. safety_drain_inbox_ex()'s extra out-params
 * exist for exactly this one caller. */
esp_err_t safety_link_get_ct_cal(SafetyLinkClass *link, uint8_t *out, size_t out_cap,
                                  size_t *out_len)
{
    if (!link || !out || out_cap < KILNLINK_CT_CAL_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    kilnlink_get_ct_cal_t req = {0};
    uint8_t request[KILNLINK_GET_CT_CAL_LEN];
    kilnlink_get_ct_cal_status_t req_status = KILNLINK_GET_CT_CAL_OK;
    size_t req_len = kilnlink_get_ct_cal_encode(&req, request, sizeof(request), &req_status);
    if (req_len == 0) {
        ESP_LOGE(TAG, "get_ct_cal: encode failed (status=%d)", (int)req_status);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(link->xact_lock, pdMS_TO_TICKS(SAFETY_XACT_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "get_ct_cal: timed out after %ums waiting for the safety link "
                      "transaction lock", (unsigned)SAFETY_XACT_LOCK_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    /* Anything already queued is a previous reply or an unsolicited push --
     * fold it into the cache now (any CT_CAL frame in there is stale, from
     * before our own request, so it is deliberately not captured), same as
     * safety_exchange()'s own first drain. */
    (void)safety_drain_inbox(link, 0);

    if (safety_lock(link)) {
        link->stats.frames_sent++;
        safety_unlock(link);
    }

    /* BROADCAST, not the ACK'd DATA transport -- same fix and same reason as
     * safety_exchange()'s GET_STATUS send and safety_link_get_config_page()
     * below. The Pico's link_task_handle_raw_frame() drops every frame that
     * is not a zero-length-header BROADCAST, so an ACK'd DATA request is
     * discarded before it reaches any command dispatch, and the send burns
     * its full UART_PROTO_MAX_RETRIES * SAFETY_LINK_ACK_TIMEOUT_MS budget
     * waiting for an ACK from a peer that never sends one. A send failure
     * here is a local UART fault, not "peer didn't answer".
     *
     * All four of these call sites had the same bug. Anything addressed to
     * UART_PROTO_DEVICE_SAFETY must go out as a broadcast; if you add a
     * fifth, it does too. */
    esp_err_t err = uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY,
                                                  UART_TASK_ID_SAFETY, UART_TASK_ID_SAFETY,
                                                  request, req_len);
    if (err != ESP_OK) {
        if (safety_lock(link)) {
            link->stats.timeouts++;
            safety_unlock(link);
        }
        xSemaphoreGive(link->xact_lock);
        return err;
    }

    uart_proto_message_t ct_cal_msg;
    bool got_ct_cal = false;
    (void)safety_drain_inbox_ex(link, SAFETY_LINK_REPLY_TIMEOUT_MS, false, &ct_cal_msg, &got_ct_cal, NULL, NULL, NULL,
                                 NULL, NULL, NULL);

    if (!got_ct_cal) {
        /* ACKed but no CT_CAL reply: same "protocol layer alive, application
         * layer didn't answer" outcome safety_exchange()'s expect_status path
         * counts as a timeout. */
        if (safety_lock(link)) {
            link->stats.timeouts++;
            safety_unlock(link);
        }
        xSemaphoreGive(link->xact_lock);
        return ESP_ERR_TIMEOUT;
    }

    memcpy(out, ct_cal_msg.payload, KILNLINK_CT_CAL_LEN);
    if (out_len) {
        *out_len = KILNLINK_CT_CAL_LEN;
    }
    xSemaphoreGive(link->xact_lock);
    return ESP_OK;
}

/* CT_COMMISSIONING_PLAN.md step 2, SAFETY_CMD_CT_AUTO_ZERO_BEGIN (0x26).
 * Fire-and-forget, same shape as safety_link_send_set_ct_cal() above: never
 * ACKed on the wire, no reply expected here -- the caller (safety_cfg_
 * http.c's commissioning POST handler) learns whether the arm was accepted
 * by polling safety_link_get_ct_auto_zero_status() afterward. Only ARMS
 * current_task.c's accumulator on the Pico; the actual multi-second
 * measurement happens there, one sample per its own normal period -- see
 * link_frame.h's LINK_FRAME_CT_AUTO_ZERO_BEGIN_CMD comment (SaftyFW) for why
 * a blocking measurement on this link would be unsafe. */
esp_err_t safety_link_send_ct_auto_zero_begin(SafetyLinkClass *link, uint8_t channel)
{
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (channel >= KILNLINK_CT_AUTO_ZERO_NUM_CHANNELS) {
        ESP_LOGW(TAG, "ct_auto_zero_begin: refused locally, channel=%u out of range 0-%u",
                 (unsigned)channel, (unsigned)KILNLINK_CT_AUTO_ZERO_NUM_CHANNELS - 1u);
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    kilnlink_ct_auto_zero_begin_t msg = { .channel = channel };
    uint8_t payload[KILNLINK_CT_AUTO_ZERO_BEGIN_LEN];
    kilnlink_ct_auto_zero_begin_status_t status = KILNLINK_CT_AUTO_ZERO_BEGIN_OK;
    size_t len = kilnlink_ct_auto_zero_begin_encode(&msg, payload, sizeof(payload), &status);
    if (len == 0) {
        ESP_LOGE(TAG, "ct_auto_zero_begin: encode failed (status=%d)", (int)status);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ct_auto_zero_begin: sending, channel=%u", (unsigned)channel);
    return uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY, UART_TASK_ID_SAFETY,
                                         UART_TASK_ID_SAFETY, payload, len);
}

/* CT_COMMISSIONING_PLAN.md step 2, SAFETY_CMD_GET_CT_AUTO_ZERO (0x27) /
 * SAFETY_CMD_CT_AUTO_ZERO_STATUS (0x28 reply). One round trip: send the
 * poll, wait up to SAFETY_LINK_REPLY_TIMEOUT_MS, then check the stash
 * (SafetyLinkClass::stashed_ct_auto_zero_status) -- the reply is ALWAYS
 * stashed by safety_drain_inbox_ex() regardless of who is waiting (see that
 * stash field's own comment), so a plain safety_drain_inbox() wait here is
 * enough; no want-flag/out-param plumbing needed for this one caller. The
 * caller (safety_cfg_http.c) is expected to call this repeatedly, once per
 * HTTP long-poll or busy-wait iteration, until state == DONE -- this
 * function itself does not loop across the full multi-second measurement. */
esp_err_t safety_link_get_ct_auto_zero_status(SafetyLinkClass *link, kilnlink_ct_auto_zero_status_t *out)
{
    if (!link || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    kilnlink_get_ct_auto_zero_t req = {0};
    uint8_t request[KILNLINK_GET_CT_AUTO_ZERO_LEN];
    kilnlink_get_ct_auto_zero_status_t req_status = KILNLINK_GET_CT_AUTO_ZERO_OK;
    size_t req_len = kilnlink_get_ct_auto_zero_encode(&req, request, sizeof(request), &req_status);
    if (req_len == 0) {
        ESP_LOGE(TAG, "get_ct_auto_zero: encode failed (status=%d)", (int)req_status);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(link->xact_lock, pdMS_TO_TICKS(SAFETY_XACT_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "get_ct_auto_zero: timed out after %ums waiting for the safety link "
                      "transaction lock", (unsigned)SAFETY_XACT_LOCK_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    /* Same "fold in anything already queued, but a stale one from before our
     * own request is not ours" reasoning as safety_link_get_ct_cal() above. */
    uart_proto_message_t stale;
    (void)safety_take_stashed_ct_auto_zero_status(link, &stale);
    (void)safety_drain_inbox(link, 0);

    esp_err_t err = uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY,
                                                  UART_TASK_ID_SAFETY, UART_TASK_ID_SAFETY,
                                                  request, req_len);
    if (err != ESP_OK) {
        if (safety_lock(link)) {
            link->stats.timeouts++;
            safety_unlock(link);
        }
        xSemaphoreGive(link->xact_lock);
        return err;
    }

    (void)safety_drain_inbox(link, SAFETY_LINK_REPLY_TIMEOUT_MS);

    uart_proto_message_t status_msg;
    if (!safety_take_stashed_ct_auto_zero_status(link, &status_msg)) {
        if (safety_lock(link)) {
            link->stats.timeouts++;
            safety_unlock(link);
        }
        xSemaphoreGive(link->xact_lock);
        return ESP_ERR_TIMEOUT;
    }

    kilnlink_ct_auto_zero_status_codec_t dstatus =
        kilnlink_ct_auto_zero_status_decode(status_msg.payload, status_msg.length, out);
    xSemaphoreGive(link->xact_lock);
    if (dstatus != KILNLINK_CT_AUTO_ZERO_STATUS_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* docs/COMMISSIONING.md sec 2/3 -- commissioning param staging/commit/     */
/* bulk readback (0x1C/0x1D/0x1F). App/drivers/safety/safety_cfg_store.c and       */
/* safety_cfg_http.c are the only callers.                                  */
/* ------------------------------------------------------------------------ */

/* SAFETY_CMD_SET_PARAM (0x1C) -- stages one CONFIG_REFERENCE.md field on the
 * Pico; nothing reaches its flash until safety_link_send_commit_config()
 * follows. ACK'd unicast (uart_protocol_send, same primitive
 * safety_link_request_enable() uses), NOT a broadcast: COMMISSIONING.md sec 2
 * calls SET_PARAM "retry-safe... a lost frame costs one retry, not the whole
 * commissioning pass" -- that guarantee is uart_protocol's own ACK/retry
 * machinery, which only a unicast send gets. Returns ESP_ERR_TIMEOUT if the
 * Pico never ACKed (no reply frame is expected or waited for beyond the
 * protocol-level ACK -- SET_PARAM has no application-level reply on the wire,
 * per kilnlink_set_param.h's own doc comment). A caller that wants to know
 * whether the id/value was actually ACCEPTED (as opposed to merely
 * delivered) must follow up with safety_link_send_commit_config(). */
esp_err_t safety_link_send_set_param(SafetyLinkClass *link, uint16_t param_id, uint8_t type,
                                      kilnlink_param_value_t value)
{
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    kilnlink_set_param_t msg = { .param_id = param_id, .type = type, .value = value };
    uint8_t payload[KILNLINK_SET_PARAM_MAX_LEN];
    kilnlink_set_param_status_t status = KILNLINK_SET_PARAM_OK;
    size_t len = kilnlink_set_param_encode(&msg, payload, sizeof(payload), &status);
    if (len == 0) {
        ESP_LOGE(TAG, "set_param: encode failed for id 0x%04X (status=%d)", (unsigned)param_id,
                 (int)status);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(link->xact_lock, pdMS_TO_TICKS(SAFETY_XACT_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "set_param: timed out waiting for the safety link transaction lock");
        return ESP_ERR_TIMEOUT;
    }
    (void)safety_drain_inbox(link, 0);
    if (safety_lock(link)) {
        link->stats.frames_sent++;
        safety_unlock(link);
    }
    /* BROADCAST, not the ACK'd DATA transport -- same fix and same reason as
     * safety_exchange()'s GET_STATUS send and safety_link_get_config_page()
     * below. The Pico's link_task_handle_raw_frame() drops every frame that
     * is not a zero-length-header BROADCAST, so an ACK'd DATA request is
     * discarded before it reaches any command dispatch, and the send burns
     * its full UART_PROTO_MAX_RETRIES * SAFETY_LINK_ACK_TIMEOUT_MS budget
     * waiting for an ACK from a peer that never sends one. A send failure
     * here is a local UART fault, not "peer didn't answer".
     *
     * All four of these call sites had the same bug. Anything addressed to
     * UART_PROTO_DEVICE_SAFETY must go out as a broadcast; if you add a
     * fifth, it does too. */
    esp_err_t err = uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY,
                                                  UART_TASK_ID_SAFETY, UART_TASK_ID_SAFETY,
                                                  payload, len);
    if (err != ESP_OK && safety_lock(link)) {
        link->stats.timeouts++;
        safety_unlock(link);
    }
    xSemaphoreGive(link->xact_lock);
    return err;
}

/* SAFETY_CMD_COMMIT_CONFIG (0x1D) -- validates everything staged by
 * safety_link_send_set_param() since the last commit and, if it passes,
 * writes one config_store record on the Pico and bumps config_crc. ACK'd
 * unicast, same reasoning as SET_PARAM above.
 *
 * ROADMAP.md "no wire codec carries a per-field COMMIT_CONFIG rejection
 * reason back to the ESP" loose end -- this used to be exactly the limitation
 * kilnlink_commit_config.h's own doc comment describes (refusal "reported
 * back on the existing diagnostic frame," which had no room for it). It no
 * longer is: SAFETY_CMD_COMMIT_CONFIG_REJECTED (0x20,
 * kilnlink_commit_config_rejected.h) is a real reply now, and this function
 * captures it the same way safety_link_get_ct_cal()/safety_link_get_config_
 * page() capture their own out-of-band replies (safety_drain_inbox_ex()'s
 * out_commit_rejected/out_got_commit_rejected params).
 *
 * `out_param_id`/`out_reason`/`out_rejected` are all optional (pass NULL for
 * any/all when the caller only cares about the protocol-level outcome, e.g.
 * safety_cfg_http.c's bench_preset path). When *out_rejected comes back
 * true, `out_param_id` is either a real COMMISSIONING.md sec 2.1 id or
 * KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID, and `out_reason` is a
 * kilnlink_commit_config_reject_reason_t value -- both only meaningful in
 * that case.
 *
 * Return value is still the PROTOCOL-level outcome, unchanged: ESP_OK means
 * the Pico's link layer ACKed the COMMIT_CONFIG frame (it received and
 * processed the request, cross-field validation included, WHETHER OR NOT
 * that validation then rejected it -- rejection is now visible via
 * *out_rejected, not via this return value), ESP_ERR_TIMEOUT means no ACK
 * arrived at all (dead link). */
esp_err_t safety_link_send_commit_config(SafetyLinkClass *link, uint16_t *out_param_id,
                                          uint8_t *out_reason, bool *out_rejected)
{
    if (out_rejected) {
        *out_rejected = false;
    }
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Discard any rejection stashed by a PRIOR commit (already reported to
     * that caller, or abandoned) so it can never be mistaken for THIS one's
     * outcome -- see safety_clear_stashed_commit_rejected()'s doc comment. */
    safety_clear_stashed_commit_rejected(link);

    kilnlink_commit_config_t msg = {0};
    uint8_t payload[KILNLINK_COMMIT_CONFIG_LEN];
    kilnlink_commit_config_status_t status = KILNLINK_COMMIT_CONFIG_OK;
    size_t len = kilnlink_commit_config_encode(&msg, payload, sizeof(payload), &status);
    if (len == 0) {
        ESP_LOGE(TAG, "commit_config: encode failed (status=%d)", (int)status);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(link->xact_lock, pdMS_TO_TICKS(SAFETY_XACT_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "commit_config: timed out waiting for the safety link transaction lock");
        return ESP_ERR_TIMEOUT;
    }
    (void)safety_drain_inbox(link, 0);
    if (safety_lock(link)) {
        link->stats.frames_sent++;
        safety_unlock(link);
    }
    /* BROADCAST, not the ACK'd DATA transport -- same fix and same reason as
     * safety_exchange()'s GET_STATUS send and safety_link_get_config_page()
     * below. The Pico's link_task_handle_raw_frame() drops every frame that
     * is not a zero-length-header BROADCAST, so an ACK'd DATA request is
     * discarded before it reaches any command dispatch, and the send burns
     * its full UART_PROTO_MAX_RETRIES * SAFETY_LINK_ACK_TIMEOUT_MS budget
     * waiting for an ACK from a peer that never sends one. A send failure
     * here is a local UART fault, not "peer didn't answer".
     *
     * All four of these call sites had the same bug. Anything addressed to
     * UART_PROTO_DEVICE_SAFETY must go out as a broadcast; if you add a
     * fifth, it does too. */
    esp_err_t err = uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY,
                                                  UART_TASK_ID_SAFETY, UART_TASK_ID_SAFETY,
                                                  payload, len);
    if (err != ESP_OK && safety_lock(link)) {
        link->stats.timeouts++;
        safety_unlock(link);
    }

    if (err == ESP_OK) {
        /* Give the Pico's own frame handler a brief window to push
         * COMMIT_CONFIG_REJECTED before this exchange's lock is released --
         * link_task_handle_commit_config() runs to completion synchronously
         * within its own frame dispatch, so any rejection it decides on is
         * already queued (or in flight) by the time the protocol-level ACK
         * above lands. SAFETY_LINK_REPLY_TIMEOUT_MS is the same budget
         * get_ct_cal()/get_config_page() give their own reply; absence of a
         * REJECTED frame in that window is treated as acceptance, same as
         * every accepted commit today (an accepted commit sends nothing). */
        uart_proto_message_t rejected_msg;
        bool got_rejected = false;
        (void)safety_drain_inbox_ex(link, SAFETY_LINK_REPLY_TIMEOUT_MS, false, NULL, NULL, NULL, NULL, &rejected_msg,
                                     &got_rejected, NULL, NULL);
        if (got_rejected) {
            kilnlink_commit_config_rejected_t rejected;
            if (kilnlink_commit_config_rejected_decode(rejected_msg.payload, rejected_msg.length, &rejected) ==
                KILNLINK_COMMIT_CONFIG_REJECTED_OK) {
                if (out_rejected) *out_rejected = true;
                if (out_param_id) *out_param_id = rejected.param_id;
                if (out_reason) *out_reason = rejected.reason;
                ESP_LOGW(TAG, "commit_config: REJECTED by the safety processor (param_id=0x%04X, reason=%u)",
                         (unsigned)rejected.param_id, (unsigned)rejected.reason);
            }
        }
    }

    xSemaphoreGive(link->xact_lock);
    ESP_LOGI(TAG, "commit_config: %s", err == ESP_OK ? "ACKed by the safety processor" : esp_err_to_name(err));
    return err;
}

/* SAFETY_CMD_APPLY_CONFIG_VOLATILE (0x2D, docs/KILN_PROFILES_PLAN.md item 15)
 * -- byte-for-byte the same shape as safety_link_send_commit_config() just
 * above (own xact_lock hold, pre-send drain, BROADCAST send, then a
 * SAFETY_LINK_REPLY_TIMEOUT_MS window for a possible COMMIT_CONFIG_REJECTED
 * reply -- link_task_handle_apply_config_volatile() on the Pico reuses
 * link_task_send_commit_config_rejected() verbatim for its own validation
 * refusal, so this side's rejection decode/stash handling needs no changes
 * of its own). The only difference is the command byte on the wire and what
 * it causes on the Pico: this frame is never refused for ARMED (config_
 * store_write_volatile() never calls config_store_decide_write()) and never
 * reaches flash, so it does not survive a Pico reboot -- see this function's
 * own header comment in safety_link.h for what that means for callers. */
esp_err_t safety_link_send_apply_config_volatile(SafetyLinkClass *link, uint16_t *out_param_id,
                                                  uint8_t *out_reason, bool *out_rejected)
{
    if (out_rejected) {
        *out_rejected = false;
    }
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Same reasoning as safety_link_send_commit_config()'s own call: a
     * rejection stashed by a PRIOR commit/volatile-install must never be
     * mistaken for THIS one's outcome. */
    safety_clear_stashed_commit_rejected(link);

    kilnlink_apply_config_volatile_t msg = {0};
    uint8_t payload[KILNLINK_APPLY_CONFIG_VOLATILE_LEN];
    kilnlink_apply_config_volatile_status_t status = KILNLINK_APPLY_CONFIG_VOLATILE_OK;
    size_t len = kilnlink_apply_config_volatile_encode(&msg, payload, sizeof(payload), &status);
    if (len == 0) {
        ESP_LOGE(TAG, "apply_config_volatile: encode failed (status=%d)", (int)status);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(link->xact_lock, pdMS_TO_TICKS(SAFETY_XACT_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "apply_config_volatile: timed out waiting for the safety link transaction lock");
        return ESP_ERR_TIMEOUT;
    }
    (void)safety_drain_inbox(link, 0);
    if (safety_lock(link)) {
        link->stats.frames_sent++;
        safety_unlock(link);
    }
    /* BROADCAST, not the ACK'd DATA transport -- same fix and same reason as
     * safety_link_send_commit_config()'s own send just above: an ACK'd DATA
     * request is discarded before it reaches command dispatch on the Pico. */
    esp_err_t err = uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY,
                                                  UART_TASK_ID_SAFETY, UART_TASK_ID_SAFETY,
                                                  payload, len);
    if (err != ESP_OK && safety_lock(link)) {
        link->stats.timeouts++;
        safety_unlock(link);
    }

    if (err == ESP_OK) {
        uart_proto_message_t rejected_msg;
        bool got_rejected = false;
        (void)safety_drain_inbox_ex(link, SAFETY_LINK_REPLY_TIMEOUT_MS, false, NULL, NULL, NULL, NULL, &rejected_msg,
                                     &got_rejected, NULL, NULL);
        if (got_rejected) {
            kilnlink_commit_config_rejected_t rejected;
            if (kilnlink_commit_config_rejected_decode(rejected_msg.payload, rejected_msg.length, &rejected) ==
                KILNLINK_COMMIT_CONFIG_REJECTED_OK) {
                if (out_rejected) *out_rejected = true;
                if (out_param_id) *out_param_id = rejected.param_id;
                if (out_reason) *out_reason = rejected.reason;
                ESP_LOGW(TAG, "apply_config_volatile: REJECTED by the safety processor (param_id=0x%04X, reason=%u)",
                         (unsigned)rejected.param_id, (unsigned)rejected.reason);
            }
        }
    }

    xSemaphoreGive(link->xact_lock);
    ESP_LOGI(TAG, "apply_config_volatile: %s",
             err == ESP_OK ? "ACKed by the safety processor" : esp_err_to_name(err));
    return err;
}

/* SAFETY_CMD_GET_CONFIG_PAGE (0x24) / CONFIG_PAGE (0x1F reply -- separate
 * ids since KILNLINK_PROTOCOL_VERSION 7) -- one page of
 * the bulk config readback COMMISSIONING.md sec 2 describes. Structured
 * exactly like safety_link_get_ct_cal() above (own xact_lock hold, own
 * drain-then-send-then-drain-for-the-reply shape) rather than routed through
 * safety_exchange(), for the identical reason: that function's
 * `expect_status` parameter only ever watches for SAFETY_CMD_GET_STATUS.
 * Never cached inside this driver -- safety_cfg_store.c is the one place a
 * fetched page's entries get kept, same "this driver caches no ct_cal state"
 * split GET_CT_CAL uses. */
esp_err_t safety_link_get_config_page(SafetyLinkClass *link, uint8_t page_index,
                                       kilnlink_config_page_t *out)
{
    if (!link || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    kilnlink_get_config_page_t req = { .page_index = page_index };
    uint8_t request[KILNLINK_GET_CONFIG_PAGE_LEN];
    kilnlink_get_config_page_status_t req_status = KILNLINK_GET_CONFIG_PAGE_OK;
    size_t req_len = kilnlink_get_config_page_encode(&req, request, sizeof(request), &req_status);
    if (req_len == 0) {
        ESP_LOGE(TAG, "get_config_page: encode failed for page %u (status=%d)", (unsigned)page_index,
                 (int)req_status);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(link->xact_lock, pdMS_TO_TICKS(SAFETY_XACT_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "get_config_page: timed out waiting for the safety link transaction lock");
        return ESP_ERR_TIMEOUT;
    }

    /* The pre-send drain CAPTURES a CONFIG_PAGE instead of discarding it.
     *
     * Every other call site here can safely clear the inbox before sending,
     * because the reply it waits for is elicited by the request it is about
     * to make. This one cannot: safety_cfg_store_maybe_refetch() re-issues
     * GET_CONFIG_PAGE on every poll for as long as the cached CRC disagrees,
     * so attempt N+1 begins while attempt N's reply is already sitting in the
     * inbox. A blind drain here threw that reply away, then waited
     * SAFETY_LINK_REPLY_TIMEOUT_MS for a fresh one and timed out because the
     * Pico's answer to THIS request lands after the wait -- and was in turn
     * discarded by attempt N+2. One initial slip becomes self-sustaining:
     * measured 2026-08-25 as 91 CONFIG_PAGE frames received against 0
     * successes and ~2 timeouts/s, permanently doubling the link's request
     * load (sent +240 vs received +120 in 60 s). The Pico is innocent: its
     * GET_CONFIG_PAGE handler answers in 578 us worst case (link_task.c's
     * s_page_reply_us_max).
     *
     * A page already in the inbox is a reply to our own immediately-preceding
     * identical request, so adopting it is correct, not stale -- but only if
     * it is for the page index actually being asked for, hence the
     * page_index check below. Anything else in the inbox is still drained
     * (the drain runs to completion either way), and this adds no blocking
     * at all: wait_ms stays 0. That last point is load-bearing -- two
     * previous attempts at this bug extended how long safety_poll may block
     * and put the ESP into a panic-reboot loop (task='safety_poll' at
     * panic_abort), including one that capped a whole refetch at 2000 ms. Do
     * not "fix" a residual timeout here by growing a budget.
     *
     * s_page_adopted_presend counts adoptions so this explanation stays
     * falsifiable: if the count never leaves 0 while page fetches still time
     * out, the mechanism above is NOT what is happening and this comment is
     * wrong. */
    static uint32_t s_page_adopted;
    uart_proto_message_t early_msg;
    bool got_early = false;
    /* 2026-08-28 ROOT CAUSE FIX (live commissioning defect: page 1 timed out
     * on essentially every fetch attempt, forever, while page 0 always
     * eventually succeeded via adoption).
     *
     * This drain passes out_config_page=&early_msg, which -- unlike the
     * NULL passed everywhere else a CONFIG_PAGE might arrive unsolicited --
     * makes safety_drain_inbox_ex() hand back ANY CONFIG_PAGE frame it finds
     * sitting in the live inbox rather than stashing it (its own switch-case
     * for KILNLINK_CONFIG_PAGE_CMD: capture into *out_config_page when a
     * caller supplied one, stash only when nobody did). So when THIS page's
     * own late reply (from a previous attempt that already gave up) happens
     * to be sitting in the inbox when the ESP starts asking for a DIFFERENT
     * page, this drain captures it here -- and the mismatch handling below
     * used to just log it (ESP_LOGD) and let it fall out of scope,
     * discarding the one frame the LATER request for that same page needed
     * to find in the stash. Concretely: attempt N's page 1 reply arrives
     * late; attempt N+1 starts, its own pre-send drain for page 0 happens to
     * run while that late page-1 reply is still sitting in the inbox,
     * captures it here (since it captures indiscriminately), finds it does
     * not match page 0, and used to throw it away -- so page 1 could NEVER
     * be recovered by the stash, no matter how many attempts ran, even
     * though the stash mechanism exists specifically to rescue exactly this
     * kind of late arrival (safety_take_stashed_config_page()'s own doc
     * comment). Verified live: s_page_adopted only ever climbed for page 0
     * ("adopted an already-arrived page 0 (N so far)"); "page 1" never once
     * appeared in that log line across many fetch attempts.
     *
     * Fix: a mismatch here gets the SAME treatment the main wait loop below
     * already gives a mismatch (see its own comment) -- stash it instead of
     * dropping it, so whichever page it actually belongs to can still adopt
     * it later. Overwriting whatever was already stashed is the same
     * trade-off safety_drain_inbox_ex()'s own stash branch already makes
     * (single slot, newest wins) -- unchanged here, just applied on this
     * path too instead of skipped. */
    (void)safety_drain_inbox_ex(link, 0, false, NULL, NULL, &early_msg, &got_early, NULL, NULL, NULL, NULL);
    if (safety_take_stashed_config_page(link, page_index, &early_msg)) {
        got_early = true; /* prefer the stash: it is the older, already-orphaned frame */
    }
    if (got_early) {
        kilnlink_config_page_t early_page;
        if (kilnlink_config_page_decode(early_msg.payload, early_msg.length, &early_page) ==
                KILNLINK_CONFIG_PAGE_OK &&
            early_page.page_index == page_index) {
            s_page_adopted++;
            xSemaphoreGive(link->xact_lock);
            ESP_LOGI(TAG, "get_config_page: adopted an already-arrived page %u (%u so far)",
                     (unsigned)page_index, (unsigned)s_page_adopted);
            *out = early_page;
            return ESP_OK;
        }
        if (safety_lock(link)) {
            link->stashed_config_page = early_msg;
            link->has_stashed_config_page = true;
            link->stashed_config_page_tick = xTaskGetTickCount();
            safety_unlock(link);
        }
        ESP_LOGD(TAG, "get_config_page: stashed a queued CONFIG_PAGE that was not page %u",
                 (unsigned)page_index);
    }

    if (safety_lock(link)) {
        link->stats.frames_sent++;
        safety_unlock(link);
    }

    /* Same fix as safety_exchange()'s GET_STATUS send (see that call site's
     * comment): the Pico's link_task drops every frame that isn't a
     * zero-length-header BROADCAST (firmware/SaftyFW/src/tasks/link_task.c,
     * link_task_handle_raw_frame()'s "the Pico never participates in the
     * ACK'd DATA/ACK/NACK transport" filter). This request went out as an
     * ACK'd DATA frame -- uart_protocol_send() -- so the Pico silently
     * discarded every copy and this call burned its full
     * UART_PROTO_MAX_RETRIES * SAFETY_LINK_ACK_TIMEOUT_MS budget against a
     * peer that would never ACK it, every single time
     * safety_cfg_store_maybe_refetch() found a CRC mismatch worth pursuing
     * (i.e. every poll while the fetch keeps failing) -- observed live
     * 2026-08-22 as a continuous "page 0 failed (ESP_ERR_TIMEOUT)" flood.
     * Sent as a BROADCAST instead: one shot, no ACK wait, no retry, and the
     * reply this elicits (link_task_send_config_page() -> link_task_send_
     * broadcast()) was already a broadcast the ESP was listening for below.
     * As with GET_STATUS, a send failure here is a local UART fault, not
     * "peer didn't answer" -- not counted in stats.timeouts. */
    esp_err_t err = uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY, UART_TASK_ID_SAFETY,
                                                  UART_TASK_ID_SAFETY, request, req_len);
    if (err != ESP_OK) {
        xSemaphoreGive(link->xact_lock);
        return err;
    }

    /* Wait for a CONFIG_PAGE **whose page_index is the one we asked for.**
     *
     * The single-shot wait this replaces accepted whatever CONFIG_PAGE landed
     * first and handed it back as the answer to `page_index`. The pre-send
     * adoption above already knew better -- it checks early_page.page_index --
     * but the main receive path did not, and this endpoint is the one call in
     * the link that routinely has an orphaned reply in flight: the comment
     * above describes attempt N's answer arriving during attempt N+1, and a
     * multi-page fetch turns that into attempt-for-page-0's answer arriving
     * during the wait for page 1.
     *
     * Accepting it is not a missed reply, it is a wrong one:
     * safety_cfg_store_refetch() copies the entries in as though they were the
     * requested page's, and takes the continue/stop decision from the wrong
     * page's `more` flag. The result is a mirror of the safety processor's
     * configuration that is silently shifted by one page and then stamped with
     * the live CRC, i.e. wrong data that reads as freshly verified -- the same
     * "stale reading looks fresh" shape this codebase has hit repeatedly, here
     * landing on the safety config mirror specifically.
     *
     * Discarding and continuing to wait (rather than failing outright) is what
     * actually drains the orphan backlog: each wrong-index frame consumed here
     * is one fewer left to confuse the next request. The deadline is unchanged
     * -- SAFETY_LINK_REPLY_TIMEOUT_MS total, not per frame -- because
     * safety_poll_task's blocking budget is load-bearing (see the pre-send
     * comment: two earlier attempts to fix this by growing a budget put the
     * board into a panic-reboot loop). */
    uart_proto_message_t page_msg;
    bool got_page = false;
    int64_t wait_started_us = (int64_t)hal_time_now_us();
    for (;;) {
        int64_t elapsed_ms = ((int64_t)hal_time_now_us() - wait_started_us) / 1000;
        int32_t remaining_ms = (int32_t)SAFETY_LINK_REPLY_TIMEOUT_MS - (int32_t)elapsed_ms;
        if (remaining_ms < 0) {
            remaining_ms = 0;
        }
        bool got_one = false;
        (void)safety_drain_inbox_ex(link, (uint32_t)remaining_ms, false, NULL, NULL, &page_msg, &got_one,
                                     NULL, NULL, NULL, NULL);
        if (!got_one) {
            break;
        }
        kilnlink_config_page_t peek;
        if (kilnlink_config_page_decode(page_msg.payload, page_msg.length, &peek) ==
                KILNLINK_CONFIG_PAGE_OK &&
            peek.page_index != page_index) {
            /* Stashed, not dropped: on a multi-page fetch the page that shows
             * up here is very often the NEXT one this loop will ask for (the
             * Pico answers every request; a reply that missed its own wait by
             * a few ms is early, not garbage). Keeping it means that request
             * is served from the stash with no wire traffic. */
            if (safety_lock(link)) {
                link->stashed_config_page = page_msg;
                link->has_stashed_config_page = true;
                link->stashed_config_page_tick = xTaskGetTickCount();
                safety_unlock(link);
            }
            ESP_LOGD(TAG, "get_config_page: stashed a reply for page %u while waiting for page %u",
                     (unsigned)peek.page_index, (unsigned)page_index);
            if (remaining_ms == 0) {
                break;
            }
            continue; /* keep waiting for the page actually asked for */
        }
        got_page = true;
        break;
    }

    if (!got_page) {
        /* Last look at the stash before calling it a timeout: this call's own
         * reply can have been pulled off the inbox by a concurrent drain --
         * the 500 ms GET_STATUS poll shares this queue -- in which case it is
         * sitting in the stash rather than lost. Same no-blocking rule as the
         * pre-send check; this only reads a flag. */
        if (safety_take_stashed_config_page(link, page_index, &page_msg)) {
            got_page = true;
            s_page_adopted++;
            ESP_LOGI(TAG, "get_config_page: page %u was taken by another drain, recovered from the stash (%u so far)",
                     (unsigned)page_index, (unsigned)s_page_adopted);
        }
    }
    if (!got_page) {
        if (safety_lock(link)) {
            link->stats.timeouts++;
            safety_unlock(link);
        }
        xSemaphoreGive(link->xact_lock);
        return ESP_ERR_TIMEOUT;
    }

    kilnlink_config_page_status_t decode_status = KILNLINK_CONFIG_PAGE_OK;
    kilnlink_config_page_status_t decode_result =
        kilnlink_config_page_decode(page_msg.payload, page_msg.length, out);
    xSemaphoreGive(link->xact_lock);
    if (decode_result != KILNLINK_CONFIG_PAGE_OK) {
        decode_status = decode_result;
        ESP_LOGW(TAG, "get_config_page: reply for page %u failed to decode (status=%d)",
                 (unsigned)page_index, (int)decode_status);
        if (safety_lock(link)) {
            link->stats.frame_errors++;
            safety_unlock(link);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }
    /* Belt and braces over the wait loop's own check: the stash-recovery path
     * above reaches here too, and this is the single place every successful
     * return passes through. Cheap, and the failure it catches (entries filed
     * under the wrong page) is silent everywhere else. */
    if (out->page_index != page_index) {
        ESP_LOGW(TAG, "get_config_page: reply carried page %u, expected %u -- rejecting",
                 (unsigned)out->page_index, (unsigned)page_index);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t safety_link_send_update_frame(SafetyLinkClass *link, const uint8_t *payload, size_t length)
{
    if (!link || !payload || length == 0 || length > UART_PROTO_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Same (dst_device, dst_task, src_task) triple as ANNOUNCE_VERSION's
     * own broadcast call site above -- see safety_link_send_announce_version_once(). */
    return uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY, UART_TASK_ID_SAFETY,
                                         UART_TASK_ID_SAFETY, payload, length);
}

esp_err_t safety_link_get_update_status(SafetyLinkClass *link, safety_link_update_status_t *out,
                                         uint32_t *out_age_ms)
{
    if (!link || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!safety_lock(link)) {
        return ESP_FAIL;
    }
    if (!link->update_status_ever_received) {
        safety_unlock(link);
        return ESP_ERR_NOT_FOUND;
    }
    *out = link->update_status;
    if (out_age_ms) {
        *out_age_ms = safety_elapsed_ms(link->update_status_tick);
    }
    safety_unlock(link);
    return ESP_OK;
}

