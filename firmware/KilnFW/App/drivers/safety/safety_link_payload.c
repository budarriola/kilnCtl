// PC-facing payload serializers for the safety link (ESP32-S3 <-> RP2040
// safety processor): safety_link_build_status_payload()/build_stats_
// payload()/build_diag_payload()/build_trip_event_payload()/build_fw_
// version_payload(), all answered purely from this driver's own cache
// (safety_link_get_status()/get_stats()/get_peer_version_status()/
// get_peer_build_status()), never by talking to the Pico. Split out of
// safety_link.c (4183 lines) as its own seam, distinct from everything
// that decides what goes INTO the cache (safety_link_frames.c/safety_link_
// poll.c) or asks the Pico for something (safety_link_commands.c).
//
// VERBATIM relocation: every function body below is byte-for-byte the same
// code that used to live in safety_link.c, with no behavior change. None of
// the functions in this file were `static` before the split (they are all
// part of safety_link.h's public API already) and none needed a linkage
// change to move here -- the one internal helper they touch (safety_lock/
// safety_unlock, used only by safety_link_build_fw_version_payload() to
// read pico_boot_id) is declared in safety_link_internal.h.
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



/* ------------------------------------------------------------------------ */
/* PC-facing payload builders (layout defined in uart_task_ids.h)           */
/* ------------------------------------------------------------------------ */

size_t safety_link_build_status_payload(SafetyLinkClass *link, uint8_t *out)
{
    safety_link_status_t status;
    if (!out || safety_link_get_status(link, &status) != ESP_OK) {
        return 0;
    }

    uint8_t flags = status.flags; /* bits 2..5 come from the Pico */
    if (status.link_up) {
        flags |= SAFETY_FLAG_LINK_UP;
    }
    if (status.fault_asserted) {
        flags |= SAFETY_FLAG_FAULT;
    }

    out[0] = SAFETY_CMD_GET_STATUS;
    out[1] = flags;
    safety_put_f32_le(&out[2], status.tc_temp_c);
    safety_put_f32_le(&out[6], status.cj_temp_c);
    out[10] = status.tc_fault;
    safety_put_f32_le(&out[11], status.current_a[0]);
    safety_put_f32_le(&out[15], status.current_a[1]);
    safety_put_f32_le(&out[19], status.current_a[2]);
    safety_put_u16_le(&out[23], status.age_ms);
    /* V2 (2026-08-23): tx_dropped_sat + its own known-bit -- see
     * SAFETY_LINK_STATUS_PAYLOAD_LEN's doc comment for why this is always
     * emitted (V1 retired here, unlike Frame A on the Pico-facing side)
     * rather than version-gated. tx_dropped_sat itself is meaningless
     * (left at whatever safety_link_status_t's zero-init/last-V1-frame
     * left it at) whenever status.tx_dropped_known is false -- the PC tool
     * must read byte26 bit0 before trusting byte25, never infer "no drops"
     * from an absent/zero byte. */
    out[25] = status.tx_dropped_sat;
    out[26] = status.tx_dropped_known ? SAFETY_LINK_STATUS_EXTRA_FLAG_TX_DROPPED_KNOWN : 0u;
    return SAFETY_LINK_STATUS_PAYLOAD_LEN;
}

size_t safety_link_build_stats_payload(SafetyLinkClass *link, uint8_t *out)
{
    safety_link_stats_t stats;
    if (!out || safety_link_get_stats(link, &stats) != ESP_OK) {
        return 0;
    }

    out[0] = SAFETY_CMD_GET_LINK_STATS;
    safety_put_u32_le(&out[1], stats.frames_sent);
    safety_put_u32_le(&out[5], stats.frames_received);
    safety_put_u32_le(&out[9], stats.frame_errors);
    safety_put_u32_le(&out[13], stats.timeouts);
    safety_put_u16_le(&out[17], stats.poll_period_ms);
    safety_put_u32_le(&out[19], stats.broadcast_dropped);
    safety_put_u32_le(&out[23], stats.diag_applied);
    safety_put_u32_le(&out[27], stats.power_applied);
    safety_put_u32_le(&out[31], stats.frames_deframed);
    safety_put_u32_le(&out[35], stats.frames_routed_nowhere);
    safety_put_u32_le(&out[39], stats.frame_length_mismatch);
    safety_put_u32_le(&out[43], stats.frame_crc_mismatch);
    safety_put_u32_le(&out[47], stats.frame_resync);
    safety_put_u32_le(&out[51], stats.dequeued_total);
    safety_put_u32_le(&out[55], stats.unmatched_cmd_count);
    out[59] = stats.last_unmatched_cmd_byte;
    safety_put_u32_le(&out[60], stats.cmd_status_count);
    safety_put_u32_le(&out[64], stats.cmd_fw_version_count);
    safety_put_u32_le(&out[68], stats.cmd_update_status_count);
    safety_put_u32_le(&out[72], stats.cmd_power_count);
    safety_put_u32_le(&out[76], stats.cmd_diag_count);
    safety_put_u32_le(&out[80], stats.cmd_trip_event_count);
    safety_put_u32_le(&out[84], stats.cmd_ct_cal_count);
    safety_put_u32_le(&out[88], stats.cmd_config_page_count);
    safety_put_u32_le(&out[92], stats.cmd_commit_config_rejected_count);
    return SAFETY_LINK_STATS_PAYLOAD_LEN;
}

/* LINK_PROTOCOL.md sec 7: "Mirror all of it on the PC-link SAFETY task as
 * well" -- answered from the cache only, never by talking to the Pico, same
 * as safety_link_build_status_payload()/safety_link_build_stats_payload()
 * above. Layout in uart_task_ids.h's SAFETY_CMD_GET_DIAG doc comment. */
size_t safety_link_build_diag_payload(SafetyLinkClass *link, uint8_t *out)
{
    safety_link_status_t status;
    if (!out || safety_link_get_status(link, &status) != ESP_OK) {
        return 0;
    }

    out[0] = SAFETY_CMD_GET_DIAG;
    out[1] = status.diag_ever_received ? 0x01u : 0x00u;
    out[2] = status.diag_trip_reason;
    safety_put_u16_le(&out[3], status.diag_warn_mask);
    safety_put_u16_le(&out[5], status.diag_trip_mask);
    safety_put_u32_le(&out[7], status.diag_uptime_ms);
    out[11] = status.diag_boot_reason;
    out[12] = status.diag_context_age_100ms;
    safety_put_u32_le(&out[13], status.diag_context_frames_ok);
    safety_put_u32_le(&out[17], status.diag_context_frames_bad);
    safety_put_u32_le(&out[21], status.diag_tx_frames_dropped);
    out[25] = status.diag_state;
    out[26] = status.diag_flags;
    return SAFETY_LINK_DIAG_PAYLOAD_LEN;
}

/* Same cache-only contract as safety_link_build_diag_payload() above. Layout
 * in uart_task_ids.h's SAFETY_CMD_GET_DIAG doc comment (GET_TRIP_EVENT
 * section). */
size_t safety_link_build_trip_event_payload(SafetyLinkClass *link, uint8_t *out)
{
    safety_link_status_t status;
    if (!out || safety_link_get_status(link, &status) != ESP_OK) {
        return 0;
    }

    out[0] = SAFETY_CMD_GET_TRIP_EVENT;
    out[1] = status.trip_event_ever_received ? 0x01u : 0x00u;
    out[2] = status.trip_last_seq;
    out[3] = status.trip_reason;
    safety_put_u32_le(&out[4], status.trip_uptime_ms);
    safety_put_f32_le(&out[8], status.trip_safety_tc_c);
    safety_put_f32_le(&out[12], status.trip_deciding_threshold);
    safety_put_f32_le(&out[16], status.trip_current_a[0]);
    safety_put_f32_le(&out[20], status.trip_current_a[1]);
    safety_put_f32_le(&out[24], status.trip_current_a[2]);
    out[28] = status.trip_relay_recent_mask;
    out[29] = status.trip_context_age_100ms;
    safety_put_u32_le(&out[30], status.trip_event_age_ms);
    return SAFETY_LINK_TRIP_EVENT_PAYLOAD_LEN;
}

/* Same cache-only contract as the two builders above: answered entirely from
 * what the last FW_VERSION (Frame C) frame from the Pico left in this struct,
 * never by sending anything. LINK_PROTOCOL.md sec 7's "mirror it onto the PC
 * link so pc_tools/MCP see it without Wi-Fi" — the web dashboard already
 * shows this data, and this is the same data over the wired link.
 *
 * Wire layout deliberately mirrors the Pico's own Frame C byte for byte
 * (link_frame_pack_fw_version() in SaftyFW/src/tasks/link_frame.c), because
 * pc_tools parses one layout for both paths:
 *
 *   [0]      cmd (SAFETY_CMD_FW_VERSION, 0x0B -- shared id, see uart_bridge.c)
 *   [1..2]   peer protocol_version   u16 LE   <- version fields FIRST, so a
 *   [3..4]   peer min_compatible     u16 LE      version-incompatible peer's
 *                                                frame still parses far
 *                                                enough to say WHY
 *   [5]      dirty                   u8
 *   [6]      commit_len N1           u8
 *   [7..]    commit                  N1 bytes ASCII, NOT null-terminated
 *   [+1]     datetime_len N2         u8
 *   [+N2]    datetime                N2 bytes ASCII, NOT null-terminated
 *   [+1]     boot_id                 u8
 *   [+1]     config_version          u8
 *   [+2]     config_crc              u16 LE
 *
 * "Not known yet" is reported as a real state, never as a plausible-looking
 * zero: until the Pico has pushed (or answered with) one FW_VERSION frame
 * that parsed all the way through config_crc, peer_build_known is false and
 * this returns a frame with both string lengths 0 and every numeric field 0.
 * A PC-side reader distinguishes that from a commissioned board by
 * config_crc != 0, exactly as safety_page.html does. Fabricating a
 * placeholder commit here would put a confident wrong build identity on a
 * safety diagnostics surface, which is worse than an explicit "unknown".
 *
 * Worst-case length is 9 + 64 + 32 = 105 bytes, inside
 * UART_PROTO_MAX_PAYLOAD (253); the caller's buffer is BRIDGE_REPLY_MAX,
 * which is that same constant. */
size_t safety_link_build_fw_version_payload(SafetyLinkClass *link, uint8_t *out)
{
    if (!out) {
        return 0;
    }

    bool     known = false;
    bool     compatible = false;
    uint16_t peer_protocol = 0;
    uint16_t peer_min_compatible = 0;
    if (safety_link_get_peer_version_status(link, &known, &compatible, &peer_protocol,
                                            &peer_min_compatible) != ESP_OK) {
        return 0;
    }

    bool     build_known = false;
    bool     dirty = false;
    uint8_t  commit[64];
    uint8_t  commit_len = 0;
    uint8_t  datetime[32];
    uint8_t  datetime_len = 0;
    uint8_t  config_version = 0;
    uint16_t config_crc = 0;
    if (safety_link_get_peer_build_status(link, &build_known, &dirty, commit, &commit_len,
                                          datetime, &datetime_len, &config_version,
                                          &config_crc) != ESP_OK) {
        return 0;
    }

    /* Nothing parsed through config_crc yet -- emit the well-formed
     * "unknown" shape rather than half-populated fields off an older or
     * truncated frame. peer_build_known is precisely the flag that gates
     * "every other peer_build_* field is meaningless" (see its declaration
     * in safety_link.h), so honouring it here is what keeps this from
     * reporting a stale commit as current. */
    if (!build_known) {
        commit_len = 0;
        datetime_len = 0;
        dirty = false;
        config_version = 0;
        config_crc = 0;
    }
    /* Defensive clamp: the accessor documents these as capped at the wire
     * limits it copied into our buffers, but this function indexes `out`
     * off them, so a bad length here would be a buffer overrun rather than
     * a wrong number. Cheap to re-assert, and the failure it prevents is
     * not a cosmetic one. */
    if (commit_len > sizeof(commit)) {
        commit_len = (uint8_t)sizeof(commit);
    }
    if (datetime_len > sizeof(datetime)) {
        datetime_len = (uint8_t)sizeof(datetime);
    }

    uint8_t peer_boot_id = 0;
    if (safety_lock(link)) {
        peer_boot_id = link->pico_boot_id_known ? link->pico_boot_id : 0u;
        safety_unlock(link);
    }

    size_t i = 0;
    out[i++] = SAFETY_CMD_FW_VERSION;
    safety_put_u16_le(&out[i], known ? peer_protocol : 0u);
    i += 2;
    safety_put_u16_le(&out[i], known ? peer_min_compatible : 0u);
    i += 2;
    out[i++] = dirty ? 1u : 0u;
    out[i++] = commit_len;
    if (commit_len > 0) {
        memcpy(&out[i], commit, commit_len);
        i += commit_len;
    }
    out[i++] = datetime_len;
    if (datetime_len > 0) {
        memcpy(&out[i], datetime, datetime_len);
        i += datetime_len;
    }
    out[i++] = peer_boot_id;
    out[i++] = config_version;
    safety_put_u16_le(&out[i], config_crc);
    i += 2;
    return i;
}
