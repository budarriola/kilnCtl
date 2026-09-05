// Inbox draining, late-reply stashing, and the request/reply exchange
// primitive for the safety link (ESP32-S3 <-> RP2040 safety processor).
// Split out of safety_link.c (4183 lines) as one of the seams the file's
// own "Frame handling" section banner already separated from the frame-
// decode functions themselves (safety_link_frames.c) and the poll loop
// that drives it (safety_link_poll.c): this file is "how a reply gets
// paired with the request that asked for it, and what happens to a wire
// frame nobody was waiting for."
//
// VERBATIM relocation: every function body below is byte-for-byte the same
// code that used to live in safety_link.c, with no behavior change. The
// only mechanical change is linkage: safety_exchange(), safety_drain_
// inbox(), safety_drain_inbox_for_status(), safety_drain_inbox_ex(),
// safety_take_stashed_config_page(), safety_clear_stashed_commit_rejected(),
// safety_take_stashed_rollback_result() and safety_clear_stashed_rollback_
// result() are now called from other translation units (safety_link_poll.c's
// poll task, safety_link_commands.c's senders, safety_link.c's ping/
// request_enable) and so dropped `static` and gained a declaration in
// safety_link_internal.h; safety_count_cmd_byte() and safety_take_stashed_
// commit_rejected() (the lowercase, non-public one -- distinct from the
// public safety_link_take_stashed_commit_rejected() wrapper, which stays in
// this file since it is only ever a thin wrapper around the stash below)
// are used only inside this file and stay exactly as private as before.
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
#include "esp_random.h"
#include "esp_timer.h"
#include "stack_margin.h"
#include "freertos/idf_additions.h"
#include "settings.h"
#include "uart_task_ids.h"

#include "kilnlink/kilnlink_announce.h"
#include "kilnlink/kilnlink_announce_reboot.h"
#include "kilnlink/kilnlink_clear_trip.h"
#include "kilnlink/kilnlink_commit_config.h"
#include "kilnlink/kilnlink_commit_config_rejected.h"
#include "kilnlink/kilnlink_rollback_result.h"
#include "kilnlink/kilnlink_config_page.h"
#include "kilnlink/kilnlink_context.h"
#include "kilnlink/kilnlink_ct_cal.h"
#include "kilnlink/kilnlink_get_config_page.h"
#include "kilnlink/kilnlink_get_ct_cal.h"
#include "kilnlink/kilnlink_rollback.h"
#include "kilnlink/kilnlink_set_config.h"
#include "kilnlink/kilnlink_set_ct_cal.h"
#include "kilnlink/kilnlink_set_log_level.h"
#include "kilnlink/kilnlink_set_param.h"
#include "kilnlink/kilnlink_version.h"

/* TODO.md owner-report item 3 (2026-08-21): zones_config_get_safety_tc_type()/
 * zones_config_is_valid() for safety_sync_tc_type() below. This is a real,
 * deliberate cross-module dependency (this driver otherwise knows nothing
 * about the zones/thermocouple settings page) -- see safety_sync_tc_type()'s
 * comment for why it lives here instead of being pushed from zones_http.c:
 * that file has no reference to the SafetyLinkClass instance (main.c holds
 * the only one, as a local static, and main.c is off-limits this pass), so
 * the poll task that already runs here and already knows link_up/down
 * transitions is the natural place to pull the desired setting from instead. */
#include "zones_config_accessors.h"

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

/* SAFETY_XACT_LOCK_TIMEOUT_MS is declared in safety_link_internal.h -- it
 * used to live here (this file's own safety_exchange() is its original,
 * still-only caller within this file), but safety_link_commands.c's
 * safety_link_send_rollback_ex() also takes link->xact_lock directly against
 * the same ceiling (the rollback boot-id watch needs to hold it across a
 * longer operation than a plain safety_exchange() call), so it moved to the
 * shared internal header rather than being duplicated in both files. */

/* Drains the inbox for up to wait_ms, applying every status frame found.
 * Returns true if at least one was applied. Waiting on the *first* message
 * only -- once something has arrived the rest of the queue is taken without
 * blocking, so a burst is absorbed in one pass.
 *
 * Dispatches by subcommand byte rather than assuming every frame is a
 * status reply: this inbox now also receives unsolicited BROADCAST pushes
 * (FW_VERSION at Pico boot, per LINK_PROTOCOL.md sec 6's Frame C), which
 * safety_apply_status() would otherwise have logged as an "unexpected
 * frame" wire error. Phase 7b.7 (compatibility floor): ids 0x00-0x0F --
 * GET_STATUS, FW_VERSION and anything else added to this switch in that
 * range -- must stay reachable here regardless of what
 * safety_apply_fw_version() concludes about peer_version_compatible; this
 * dispatch never checks that verdict before routing a frame, on purpose.
 * A subcommand this build doesn't recognise at all falls to the default
 * case and is silently discarded (LINK_PROTOCOL.md's own
 * additive-compatibility principle: "a peer that has never heard of it
 * discards it"), not counted as a frame error the way a genuinely malformed
 * GET_STATUS payload still is (inside safety_apply_status() itself).
 *
 * `out_ct_cal`/`out_got_ct_cal` (both optional, NULL together for every call
 * site except safety_link_get_ct_cal()) let one caller also capture a
 * SAFETY_CMD_CT_CAL (0x1A) reply verbatim while this same pass still applies
 * every other frame the usual way -- CT_CAL is deliberately NOT cached
 * anywhere on this side (safety_link_get_ct_cal()'s own doc comment: every
 * GET_CT_CAL is a live round trip, never answered from a cache), so capturing
 * the raw frame here, rather than adding a case to the switch below, is the
 * only way a caller gets the bytes back at all.
 *
 * `out_config_page`/`out_got_config_page` (both optional, NULL together for
 * every call site except safety_link_get_config_page()) do the identical job
 * for a SAFETY_CMD_CONFIG_PAGE (0x1F) reply -- COMMISSIONING.md sec 3's bulk
 * config read is likewise never cached inside this driver (safety_cfg_store.c
 * owns that cache, one layer up), so every GET_CONFIG_PAGE is a live round
 * trip and this is, again, the only way the caller gets the raw frame back.
 *
 * `out_commit_rejected`/`out_got_commit_rejected` (both optional, NULL
 * together for every call site except safety_link_send_commit_config()) do
 * the same job for a SAFETY_CMD_COMMIT_CONFIG_REJECTED (0x20) reply --
 * ROADMAP.md "no wire codec carries a per-field COMMIT_CONFIG rejection
 * reason back to the ESP" loose end. Never cached: it is meaningful only to
 * the one commit that provoked it, exactly like CT_CAL/CONFIG_PAGE above. */
/* 2026-08-23, the DIAG-frame-went-dark investigation, final measurement:
 * dequeued_total proved this drain is the sole consumer and it IS pulling
 * every deframed message out (dequeued_total tracked frames_deframed almost
 * exactly), and unmatched_cmd_count stayed 0 (nothing hit the switch's
 * default:) -- yet diag_applied/power_applied both stayed 0 while ~80
 * messages over one 30s window were neither GET_STATUS nor logged as
 * unmatched. That is only possible if those ~80 messages ARE matching one of
 * the switch's OTHER real cases (FW_VERSION/UPDATE_STATUS/TRIP_EVENT/CT_CAL/
 * CONFIG_PAGE/COMMIT_CONFIG_REJECTED), which would mean the working
 * assumption that "the non-status traffic is DIAG and POWER" was simply
 * wrong -- supported independently by six SWD samples of the Pico's own
 * s_last_tx_cmd that saw 0x08 (DIAG) zero times out of six. This is the one
 * measurement that settles it: a real per-command histogram, so the answer
 * is read directly rather than inferred from what it is NOT. Called once per
 * dequeued message with a length, right where the existing dequeued_total
 * counter already sits, so every message this drain ever sees is counted
 * exactly once, under exactly the same lock discipline as every other
 * counter in this file (safety_lock()/safety_unlock() around each field
 * write). Bytes that match no case still land in unmatched_cmd_count (see
 * the switch's own default: branch) -- this function does not duplicate
 * that, it only counts the ones that DO match a real case, one field per
 * case, so summing all nine plus unmatched_cmd_count must equal
 * dequeued_total exactly; if it does not, that mismatch is itself a finding
 * worth reporting, not silently absorbed into a stray bucket. */
/* `len` is msg.length, clamped by the caller to uint8_t (the wire's own
 * length field is a byte, so no truncation risk) -- see last_fw_version_len
 * etc.'s own doc comment in safety_link.h for why this is captured
 * per-command rather than just counted. */
static void safety_count_cmd_byte(SafetyLinkClass *link, uint8_t cmd, uint8_t len)
{
    if (!safety_lock(link)) {
        return;
    }
    switch (cmd) {
    case SAFETY_CMD_GET_STATUS:
        link->stats.cmd_status_count++;
        link->stats.last_status_len = len;
        break;
    case SAFETY_CMD_FW_VERSION:
        link->stats.cmd_fw_version_count++;
        link->stats.last_fw_version_len = len;
        break;
    case SAFETY_CMD_UPDATE_STATUS:
        link->stats.cmd_update_status_count++;
        link->stats.last_update_status_len = len;
        break;
    case SAFETY_CMD_POWER:
        link->stats.cmd_power_count++;
        link->stats.last_power_len = len;
        break;
    case SAFETY_CMD_DIAG:
        link->stats.cmd_diag_count++;
        link->stats.last_diag_len = len;
        break;
    case SAFETY_CMD_TRIP_EVENT:
        link->stats.cmd_trip_event_count++;
        link->stats.last_trip_event_len = len;
        break;
    case KILNLINK_CT_CAL_CMD: /* the reply's own id (0x1A) -- since KILNLINK_PROTOCOL_VERSION 7, DIFFERENT from SAFETY_CMD_GET_CT_CAL (0x22, the request's id) */
        link->stats.cmd_ct_cal_count++;
        link->stats.last_ct_cal_len = len;
        break;
    case KILNLINK_CONFIG_PAGE_CMD: /* the reply's own id (0x1F) -- since KILNLINK_PROTOCOL_VERSION 7, DIFFERENT from SAFETY_CMD_GET_CONFIG_PAGE (0x24, the request's id) */
        link->stats.cmd_config_page_count++;
        link->stats.last_config_page_len = len;
        break;
    case KILNLINK_COMMIT_CONFIG_REJECTED_CMD:
        link->stats.cmd_commit_config_rejected_count++;
        link->stats.last_commit_config_rejected_len = len;
        break;
    case KILNLINK_ROLLBACK_RESULT_CMD:
        link->stats.cmd_rollback_result_count++;
        link->stats.last_rollback_result_len = len;
        break;
    default:
        /* Not counted here -- the switch in safety_drain_inbox_ex() below
         * already counts and records this exact case via
         * unmatched_cmd_count/last_unmatched_cmd_byte. Never double-count
         * the same byte in two places. */
        break;
    }
    safety_unlock(link);
}

bool safety_drain_inbox_ex(SafetyLinkClass *link, uint32_t wait_ms, bool want_status,
                                   uart_proto_message_t *out_ct_cal, bool *out_got_ct_cal,
                                   uart_proto_message_t *out_config_page, bool *out_got_config_page,
                                   uart_proto_message_t *out_commit_rejected, bool *out_got_commit_rejected,
                                   uart_proto_message_t *out_rollback_result, bool *out_got_rollback_result)
{
    uart_proto_message_t msg;
    bool got_status = false;
    TickType_t total_wait_ticks = pdMS_TO_TICKS(wait_ms);
    TickType_t wait = total_wait_ticks;
    /* Elapsed-since-start, not a raw deadline tick value: xTaskGetTickCount()
     * wraps (~497 days at the default 100 Hz), and comparing two raw tick
     * snapshots the way `deadline > now` would is not safe across that wrap
     * -- unsigned SUBTRACTION is (same idiom safety_elapsed_ms() above
     * already uses). started is read once and never compared directly
     * against another tick snapshot; only ever subtracted from a later one. */
    TickType_t started = xTaskGetTickCount();

    bool want_ct_cal = out_ct_cal && out_got_ct_cal;
    bool want_config_page = out_config_page && out_got_config_page;
    bool want_commit_rejected = out_commit_rejected && out_got_commit_rejected;
    bool want_rollback_result = out_rollback_result && out_got_rollback_result;

    while (uart_protocol_receive(link->inbox, &msg, wait) == ESP_OK) {
        /* 2026-08-23: this is the ONE registered consumer of link->inbox --
         * confirmed by grepping every uart_protocol_receive() call site in
         * this codebase; every other call site reads a DIFFERENT task's
         * inbox on a DIFFERENT uart_protocol_t instance (uart_bridge.c's own
         * UART_TASK_ID_SAFETY registration is on the PC-link's uart_proto,
         * not link->proto -- two independent instances, two independent
         * task-7 slots, no aliasing). Counted here, before the switch, so it
         * can be compared against frames_deframed - frames_received (the
         * non-status frame arrival rate) to tell "this drain is consuming
         * them" apart from "something else is". */
        if (safety_lock(link)) {
            link->stats.dequeued_total++;
            safety_unlock(link);
        }
        if (msg.length >= 1) {
            safety_count_cmd_byte(link, msg.payload[0], (uint8_t)msg.length); /* per-command histogram + length, see its own doc comment */
            switch (msg.payload[0]) {
            case SAFETY_CMD_GET_STATUS:
                if (safety_apply_status(link, &msg)) {
                    got_status = true;
                }
                break;
            case SAFETY_CMD_FW_VERSION:
                safety_apply_fw_version(link, &msg);
                break;
            case SAFETY_CMD_UPDATE_STATUS:
                safety_apply_update_status(link, &msg);
                break;
            case SAFETY_CMD_POWER:
                safety_apply_power(link, &msg);
                break;
            case SAFETY_CMD_DIAG:
                safety_apply_diag(link, &msg);
                break;
            case SAFETY_CMD_TRIP_EVENT:
                safety_apply_trip_event(link, &msg);
                break;
            case KILNLINK_CT_CAL_CMD: /* the reply's own id (0x1A) -- since KILNLINK_PROTOCOL_VERSION 7, DIFFERENT from SAFETY_CMD_GET_CT_CAL (0x22, the request's id) */
                if (out_ct_cal && out_got_ct_cal && msg.length == KILNLINK_CT_CAL_LEN) {
                    *out_ct_cal = msg;
                    *out_got_ct_cal = true;
                }
                break;
            case KILNLINK_CONFIG_PAGE_CMD: /* the reply's own id (0x1F) -- since KILNLINK_PROTOCOL_VERSION 7, DIFFERENT from SAFETY_CMD_GET_CONFIG_PAGE (0x24, the request's id) */
                /* Length is NOT fixed (KILNLINK_CONFIG_PAGE_HDR_LEN or more,
                 * per page's entry_count) -- unlike CT_CAL's exact-length
                 * check above, any frame carrying this id long enough
                 * to plausibly be a reply is captured; kilnlink_config_page_
                 * decode() (called by safety_link_get_config_page()) is what
                 * actually validates it byte-for-byte. */
                if (msg.length >= KILNLINK_CONFIG_PAGE_HDR_LEN) {
                    if (out_config_page && out_got_config_page) {
                        *out_config_page = msg;
                        *out_got_config_page = true;
                    } else if (safety_lock(link)) {
                        /* Nobody is waiting for this one right now, so stash
                         * it instead of dropping it -- see
                         * SafetyLinkClass::stashed_config_page. This is the
                         * branch that used to silently discard the answer to
                         * a fetch that was still in flight, because the
                         * 500 ms GET_STATUS poll drains this same inbox. */
                        link->stashed_config_page = msg;
                        link->has_stashed_config_page = true;
                        link->stashed_config_page_tick = xTaskGetTickCount();
                        safety_unlock(link);
                    }
                }
                break;
            case KILNLINK_COMMIT_CONFIG_REJECTED_CMD:
                if (msg.length == KILNLINK_COMMIT_CONFIG_REJECTED_LEN) {
                    if (out_commit_rejected && out_got_commit_rejected) {
                        *out_commit_rejected = msg;
                        *out_got_commit_rejected = true;
                    } else if (safety_lock(link)) {
                        /* Nobody is waiting for this one right now -- stash it
                         * instead of dropping it, same reasoning and same
                         * pattern as the CONFIG_PAGE stash above. See
                         * SafetyLinkClass::stashed_commit_rejected. */
                        link->stashed_commit_rejected = msg;
                        link->has_stashed_commit_rejected = true;
                        link->stashed_commit_rejected_tick = xTaskGetTickCount();
                        safety_unlock(link);
                    }
                }
                break;
            case KILNLINK_ROLLBACK_RESULT_CMD: /* SAFETY_CMD_ROLLBACK_RESULT (0x25) -- the missing wire-visible
                                                 * reply for a refused SAFETY_CMD_ROLLBACK, sent ONLY on refusal
                                                 * (see kilnlink_rollback_result.h's own "ASYMMETRIC BY DESIGN"
                                                 * comment). DOES need a stash branch, same reasoning as CT_CAL/
                                                 * CONFIG_PAGE/COMMIT_CONFIG_REJECTED above: safety_link_send_
                                                 * rollback_ex() only waits SAFETY_LINK_REPLY_TIMEOUT_MS in this
                                                 * out-param before moving on to its boot_id-reconnect watch, but a
                                                 * rollback reboot takes far longer than that -- a refusal landing
                                                 * even slightly late used to be silently discarded by the next
                                                 * drain through this inbox (typically the ordinary GET_STATUS
                                                 * poll, which shares this queue). Stashed instead
                                                 * (SafetyLinkClass::stashed_rollback_result), so the boot_id watch
                                                 * can still find it and report REFUSED rather than let the watch
                                                 * time out into UNKNOWN (or worse, a coincidental unrelated
                                                 * reboot's boot_id change being misread as ACCEPTED). */
                if (msg.length == KILNLINK_ROLLBACK_RESULT_LEN) {
                    if (out_rollback_result && out_got_rollback_result) {
                        *out_rollback_result = msg;
                        *out_got_rollback_result = true;
                    } else if (safety_lock(link)) {
                        link->stashed_rollback_result = msg;
                        link->has_stashed_rollback_result = true;
                        link->stashed_rollback_result_tick = xTaskGetTickCount();
                        safety_unlock(link);
                    }
                }
                break;
            default:
                /* Dequeued, CRC-valid, but payload[0] matched no case above
                 * -- recorded, both the count and the actual byte, rather
                 * than inferred: if DIAG/POWER are somehow arriving with an
                 * unexpected first byte, this is what shows what it really
                 * was. */
                if (safety_lock(link)) {
                    link->stats.unmatched_cmd_count++;
                    link->stats.last_unmatched_cmd_byte = msg.payload[0];
                    safety_unlock(link);
                }
                break;
            }
        }

        /* Bug fix (2026-08-23): this used to be an unconditional `wait = 0`
         * after the first receive, which silently starved a caller still
         * waiting on a specific out-of-band reply (CT_CAL/CONFIG_PAGE/
         * COMMIT_CONFIG_REJECTED) whenever an unrelated frame -- GET_STATUS/
         * DIAG/POWER/TRIP_EVENT/FW_VERSION, all of which the Pico also sends
         * on this same inbox -- happened to arrive first. Fix: keep blocking
         * against the REMAINING portion of this call's own declared wait_ms
         * until the wanted reply arrives; degrade to zero-wait only once it
         * has (or if nothing specific was ever wanted). This does not raise
         * the per-call ceiling -- wait never exceeds total_wait_ticks, the
         * same budget this call always had -- it only lets the call actually
         * use that budget instead of abandoning it after one message. See
         * safety_drain_still_waiting()'s doc comment in safety_link.h for
         * the incident this closes, and safety_cfg_store.c's SAFETY_CFG_
         * STORE_REFETCH_BUDGET_MS for the SEPARATE, unrelated fix this one
         * exposed (an NVS write from safety_poll_task's PSRAM stack, not a
         * timing issue in this function). */
        if (safety_drain_still_waiting(want_status, got_status, want_ct_cal,
                                        want_ct_cal && *out_got_ct_cal, want_config_page,
                                        want_config_page && *out_got_config_page, want_commit_rejected,
                                        want_commit_rejected && *out_got_commit_rejected, want_rollback_result,
                                        want_rollback_result && *out_got_rollback_result)) {
            TickType_t elapsed = xTaskGetTickCount() - started; /* wrap-safe unsigned subtraction */
            wait = (elapsed < total_wait_ticks) ? (total_wait_ticks - elapsed) : 0;
        } else {
            wait = 0; /* nothing more this call needs; drain any remainder opportunistically */
        }
    }
    return got_status;
}

/* How long a stashed CONFIG_PAGE stays worth keeping. Comfortably longer than
 * the gap between one refetch attempt's page N and the next attempt's page N,
 * short enough that a page the ESP never asks for again cannot shadow a real
 * reply indefinitely. */
/* Deliberately long -- longer than the config store's retry backoff, so a
 * reply that missed its own wait is still there for the NEXT attempt to adopt.
 * That cross-attempt adoption is the only thing that lets a multi-page fetch
 * make progress at all while the reply latency measured on this bench (below)
 * exceeds SAFETY_LINK_REPLY_TIMEOUT_MS; an earlier, tighter value here (4x the
 * reply timeout) silently removed it and the fetch stopped converging
 * entirely.
 *
 * What keeps a long-lived stash from going stale is not the age but
 * safety_link_clear_stashed_config_page(), which safety_cfg_store.c calls
 * whenever the peer reports a different config CRC -- i.e. whenever a held
 * page could belong to a configuration that no longer exists. The age is only
 * a backstop against a page nobody ever asks for again. */
#define SAFETY_STASHED_PAGE_MAX_AGE_MS 60000u

/* Takes the stashed CONFIG_PAGE frame **only if it is the page being asked
 * for**, and clears the stash so it is handed to exactly one caller. Returns
 * false (leaving *out untouched) when the stash is empty, holds a different
 * page, has aged out, or the state lock could not be taken.
 *
 * Matching here rather than in the caller is the point. The caller used to
 * take unconditionally and then drop a non-matching frame on the floor, which
 * turned a harmless ordering slip into a self-sustaining one on a multi-page
 * fetch: the reply to attempt N's page 1 arrives after that attempt gave up
 * and lands in the stash; attempt N+1 starts at page 0, takes the stash,
 * finds page 1, discards it; page 0's own reply then arrives late and is
 * stashed; the page-1 request takes that, finds page 0, discards it -- and so
 * on, so page 1 times out on every attempt forever while the Pico answers
 * every single request (its own counters showed 868 requests seen, 868
 * handled, 868 broadcast). Observed live as a permanent ~2 Hz refetch storm on
 * the link with the cached safety config never converging.
 *
 * Leaving a non-matching page stashed instead means the page is still there
 * when the fetch loop reaches it, one request later, and is adopted with no
 * wire traffic at all. */
bool safety_take_stashed_config_page(SafetyLinkClass *link, uint8_t want_page_index,
                                             uart_proto_message_t *out)
{
    bool took = false;
    if (safety_lock(link)) {
        if (link->has_stashed_config_page) {
            if (safety_elapsed_ms(link->stashed_config_page_tick) > SAFETY_STASHED_PAGE_MAX_AGE_MS) {
                link->has_stashed_config_page = false; /* too old to be anyone's reply */
            } else {
                kilnlink_config_page_t peek;
                if (kilnlink_config_page_decode(link->stashed_config_page.payload,
                                                 link->stashed_config_page.length,
                                                 &peek) != KILNLINK_CONFIG_PAGE_OK) {
                    link->has_stashed_config_page = false; /* undecodable -- never useful */
                } else if (peek.page_index == want_page_index) {
                    *out = link->stashed_config_page;
                    link->has_stashed_config_page = false;
                    took = true;
                }
                /* else: a different page, deliberately LEFT stashed */
            }
        }
        safety_unlock(link);
    }
    return took;
}

/* Takes the stashed COMMIT_CONFIG_REJECTED frame unconditionally (no page
 * index to match -- see the field's doc comment in safety_link.h). Returns
 * false (leaving *out untouched) when the stash is empty, has aged out, or
 * the state lock could not be taken. */
static bool safety_take_stashed_commit_rejected(SafetyLinkClass *link, uart_proto_message_t *out)
{
    bool took = false;
    if (safety_lock(link)) {
        if (link->has_stashed_commit_rejected) {
            if (safety_elapsed_ms(link->stashed_commit_rejected_tick) > SAFETY_STASHED_PAGE_MAX_AGE_MS) {
                link->has_stashed_commit_rejected = false; /* too old to be anyone's reply */
            } else {
                *out = link->stashed_commit_rejected;
                link->has_stashed_commit_rejected = false;
                took = true;
            }
        }
        safety_unlock(link);
    }
    return took;
}

/* Drops any stashed COMMIT_CONFIG_REJECTED frame. Called at the top of
 * safety_link_send_commit_config() so a rejection left over from a PRIOR
 * commit (already reported to that caller, or abandoned) can never be
 * mistaken for this commit's own outcome. */
void safety_clear_stashed_commit_rejected(SafetyLinkClass *link)
{
    if (safety_lock(link)) {
        link->has_stashed_commit_rejected = false;
        safety_unlock(link);
    }
}

/* Takes the stashed ROLLBACK_RESULT frame unconditionally (no index to
 * match -- same reasoning as COMMIT_CONFIG_REJECTED above: only one
 * rollback request can be in flight at a time, serialized by xact_lock).
 * Returns false (leaving *out untouched) when the stash is empty, has aged
 * out, or the state lock could not be taken. Consumed only by safety_link_
 * send_rollback_ex()'s own boot_id-reconnect watch -- unlike the config-page
 * and commit-rejected stashes, nothing outside this file needs to read a
 * rollback result late, so there is no public safety_link_take_stashed_*()
 * wrapper for this one. */
bool safety_take_stashed_rollback_result(SafetyLinkClass *link, uart_proto_message_t *out)
{
    bool took = false;
    if (safety_lock(link)) {
        if (link->has_stashed_rollback_result) {
            if (safety_elapsed_ms(link->stashed_rollback_result_tick) > SAFETY_STASHED_PAGE_MAX_AGE_MS) {
                link->has_stashed_rollback_result = false; /* too old to be anyone's reply */
            } else {
                *out = link->stashed_rollback_result;
                link->has_stashed_rollback_result = false;
                took = true;
            }
        }
        safety_unlock(link);
    }
    return took;
}

/* Drops any stashed ROLLBACK_RESULT frame. Called at the top of safety_
 * link_send_rollback_ex() so a refusal left over from a PRIOR, already-
 * resolved rollback attempt (reported to that caller, or abandoned once its
 * own boot_id watch gave up) can never be mistaken for THIS attempt's own
 * outcome. */
void safety_clear_stashed_rollback_result(SafetyLinkClass *link)
{
    if (safety_lock(link)) {
        link->has_stashed_rollback_result = false;
        safety_unlock(link);
    }
}

void safety_link_clear_stashed_config_page(SafetyLinkClass *link)
{
    if (!link) {
        return;
    }
    if (safety_lock(link)) {
        link->has_stashed_config_page = false;
        safety_unlock(link);
    }
}

/* Public wrapper for callers OUTSIDE this file (safety_cfg_http.c's
 * apply_pairs()) that want to know whether a COMMIT_CONFIG_REJECTED frame
 * showed up late -- after safety_link_send_commit_config()'s own
 * SAFETY_LINK_REPLY_TIMEOUT_MS window already closed and it reported the
 * commit as accepted by default (see that function's header comment).
 * apply_pairs() calls this AFTER its own live read-back (safety_cfg_store_
 * refetch()) already found the write did not land -- the read-back is what
 * decides pass/fail (COMMISSIONING.md: "a positive read-back is the only
 * version of this that cannot lie"), this is only consulted to attach the
 * Pico's OWN reason to a failure already established some other way. Decodes
 * and consumes whatever is currently stashed; returns false (out-params
 * untouched) if nothing is stashed, it aged out, or it fails to decode. */
bool safety_link_take_stashed_commit_rejected(SafetyLinkClass *link, uint16_t *out_param_id,
                                               uint8_t *out_reason)
{
    if (!link) {
        return false;
    }
    uart_proto_message_t msg;
    if (!safety_take_stashed_commit_rejected(link, &msg)) {
        return false;
    }
    kilnlink_commit_config_rejected_t rejected;
    if (kilnlink_commit_config_rejected_decode(msg.payload, msg.length, &rejected) !=
        KILNLINK_COMMIT_CONFIG_REJECTED_OK) {
        return false;
    }
    if (out_param_id) {
        *out_param_id = rejected.param_id;
    }
    if (out_reason) {
        *out_reason = rejected.reason;
    }
    return true;
}

/* Opportunistic drain: takes whatever has already arrived and does NOT hold
 * the call open waiting for a STATUS. Every caller that passes wait_ms 0, or
 * a short ACK/idle budget, wants exactly this -- see
 * safety_drain_inbox_for_status() below for the one caller that does not. */
bool safety_drain_inbox(SafetyLinkClass *link, uint32_t wait_ms)
{
    return safety_drain_inbox_ex(link, wait_ms, false, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
}

/* The GET_STATUS poll's drain: keeps using its own declared budget until the
 * STATUS it asked for arrives, instead of giving up the moment an unrelated
 * DIAG/POWER push lands first. See safety_drain_still_waiting()'s round-3
 * comment in safety_link.h for the bench measurement behind this. */
bool safety_drain_inbox_for_status(SafetyLinkClass *link, uint32_t wait_ms)
{
    return safety_drain_inbox_ex(link, wait_ms, true, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
}

/* One complete request/reply exchange, serialized against every other one on
 * this link (see SafetyLinkClass::xact_lock).
 *
 * Note what is *not* here: no bespoke framing, retry or CRC logic. Retries,
 * de-duplication and the CRC all belong to uart_protocol, which is the same
 * code the PC link runs -- this function is only the request/reply pairing on
 * top of it. */
esp_err_t safety_exchange(SafetyLinkClass *link, const uint8_t *request, size_t length,
                                  bool expect_status)
{
    if (!request || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(link->xact_lock, pdMS_TO_TICKS(SAFETY_XACT_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "timed out after %ums waiting for the safety link transaction lock",
                 (unsigned)SAFETY_XACT_LOCK_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    /* Anything already queued is a previous reply or an unsolicited push: fold
     * it into the cache now, so the wait below can only see a frame that our
     * own request produced. */
    (void)safety_drain_inbox(link, 0);

    if (safety_lock(link)) {
        link->stats.frames_sent++;
        safety_unlock(link);
    }

    /* LINK_PROTOCOL.md sec "SAFETY_CMD_GET_STATUS ... no longer a poll" and
     * sec 9 item 0.3: the Pico's link_task never runs the ACK'd DATA/ACK/NACK
     * transport at all (firmware/SaftyFW/src/tasks/link_task.c drops anything
     * that isn't a zero-length BROADCAST), so uart_protocol_send() here would
     * never see an ACK and would burn its full UART_PROTO_MAX_RETRIES *
     * SAFETY_LINK_ACK_TIMEOUT_MS budget on *every* call, poll after poll,
     * against a peer that is behaving exactly as designed. Sent as a
     * BROADCAST instead: one shot, no ACK wait, no retry. This also means a
     * failure here can only be "the UART write itself failed"
     * (uart_protocol_send_broadcast's ESP_FAIL/ESP_ERR_INVALID_ARG), never
     * "no ACK" -- so it must NOT be counted as a stats.timeouts the way the
     * old ACK'd send's failure was; that counter now means "asked, and got no
     * reply", which is decided below by whether a reply frame actually
     * showed up, not by whether the send itself succeeded. */
    esp_err_t err = uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY,
                                                  UART_TASK_ID_SAFETY, UART_TASK_ID_SAFETY,
                                                  request, length);
    if (err != ESP_OK) {
        /* Only a local UART write failure, not "peer didn't answer" -- see
         * the comment above. Not counted in stats.timeouts for the same
         * reason; frames_sent above already recorded the attempt. */
        xSemaphoreGive(link->xact_lock);
        return err;
    }

    if (expect_status) {
        if (!safety_drain_inbox_for_status(link, SAFETY_LINK_REPLY_TIMEOUT_MS)) {
            /* ACKed but no answer: the peer's protocol layer is alive and its
             * application layer is not. Counted as a timeout, since the result
             * for the caller is the same -- no fresh data. */
            if (safety_lock(link)) {
                link->stats.timeouts++;
                safety_unlock(link);
            }
            err = ESP_ERR_TIMEOUT;
        }
    } else {
        /* A peer that volunteers a status right after (e.g. after
         * REQUEST_ENABLE) gets it folded into the cache for free. */
        (void)safety_drain_inbox(link, SAFETY_LINK_ACK_TIMEOUT_MS);
    }

    xSemaphoreGive(link->xact_lock);
    return err;
}
