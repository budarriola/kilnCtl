// Frame handling for the safety link (ESP32-S3 <-> RP2040 safety processor):
// decodes every frame the Pico sends (GET_STATUS/POWER/DIAG/TRIP_EVENT/
// UPDATE_STATUS/FW_VERSION) into SafetyLinkClass's cache, and builds/sends
// the two outbound broadcasts this driver originates on its own initiative
// (ANNOUNCE_VERSION, PUSH_CONTEXT). Split out of safety_link.c (which grew
// to 4183 lines) as one of the seams the file's own doc-comment structure
// already named ("Frame handling" / "ROADMAP.md M5 -- SAFETY_CMD_PUSH_
// CONTEXT" / "Phase 7b" / "Phase 10 (SaftyFW) / TODO.md 9.5" section
// banners) -- everything here is "what does this driver do with one wire
// frame", as opposed to the inbox-draining/request-reply machinery
// (safety_link_inbox.c), the periodic poll loop (safety_link_poll.c), or
// the command senders (safety_link_commands.c).
//
// VERBATIM relocation: every function body below is byte-for-byte the same
// code that used to live in safety_link.c, with no behavior change. The
// only mechanical change is linkage: safety_link_send_announce_version_
// burst(), safety_build_and_send_context(), safety_apply_status(),
// safety_apply_power(), safety_apply_diag(), safety_apply_trip_event(),
// safety_apply_update_status() and safety_apply_fw_version() are now called
// from other translation units (safety_link_poll.c's poll task,
// safety_link_inbox.c's drain dispatch) and so dropped `static` and gained a
// declaration in safety_link_internal.h; every other function/static here
// (safety_build_announce_version_payload, safety_link_send_announce_
// version_once, safety_context_update_relay_recent, safety_count_cmd_byte's
// sibling s_boot_clean statics, safety_link_mark_boot_clean's own file-local
// state) stays exactly as private as it always was.
#include "safety_link.h"
#include "safety_link_frame.h"
#include "safety_link_internal.h"
#include "safety_trip_decision.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
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
#include "kilnlink/kilnlink_frame_a_offsets.h" /* KILNLINK_FRAME_A_* -- single source of truth for
                                                 * Frame A's byte offsets/lengths, ROADMAP.md M15
                                                 * "Frame A's field layout is hand-duplicated" */
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

/* RELAY_LIFE_BUDGET.md: K4 edge counting off the observed
 * SAFETY_FLAG_RELAY bit in safety_apply_status() below -- another real,
 * deliberate cross-module dependency, same shape as safety_cfg_store.h just
 * above (this driver otherwise knows nothing about relay-life accounting). */
#include "relay_cycles.h"

/* ROADMAP.md M5 -- SAFETY_CMD_PUSH_CONTEXT's live-state sources. safety_link.h
 * only forward-declares these as void* (kiln_io_t is an anonymous-struct
 * typedef, MAX31856BusClass a named one) to keep that header dependency-free;
 * this .c file is where the frame is actually built, so it needs the real
 * types. */
#include "MAX31856.h"
#include "kiln_io.h"
#include "profile_executor_state.h"
#include "thermo_owner.h"
#include "heat_enable.h"
#include "danger_mode.h"
#include "heat_owner_active_decide.h"

/* Real build identity (git commit/dirty/build timestamp), generated fresh
 * every build by gen_build_info.cmake into this component's binary dir --
 * see uart_bridge.c's build_fw_version_reply() for the PC-link twin of the
 * payload builder below. Unlike SaftyFW (TODO.md Phase 8: "build_info.h
 * generated on every build... not built this pass"), KilnFW already has
 * this, so the ANNOUNCE_VERSION frame reports real values, not a stub. */
#include "build_info.h"


static const char *TAG = "safety_link";

/* ANNOUNCE_VERSION is sent BROADCAST (fire-and-forget, per LINK_PROTOCOL.md
 * sec 1/2), so "retry" here means "send it more than once", not "retry an
 * ACK". LINK_PROTOCOL.md sec 4: "repeated a few times against loss" -- same
 * spirit as Frame D (TRIP_EVENT) on the Pico side, "repeated a few times
 * over the next second in case the first copy is lost" (sec 6). Four sends,
 * three 250ms gaps between them, spans ~750ms. */
#define SAFETY_ANNOUNCE_VERSION_REPEATS 4u
#define SAFETY_ANNOUNCE_VERSION_REPEAT_GAP_MS 250u

/* ------------------------------------------------------------------------ */
/* Phase 7b -- mutual version compatibility (ANNOUNCE_VERSION/FW_VERSION)   */
/* ------------------------------------------------------------------------ */

/* Same formula as SaftyFW's link_frame_versions_compatible()
 * (firmware/SaftyFW/src/tasks/link_frame.c) -- ported rather than shared
 * via CommonFW, since kilnlink today carries only the framing/CRC layer
 * (CommonFW/README.md's "Integration" section), not frame-payload logic,
 * and this is one boolean formula, not a codec. CommonFW/docs/LINK_PROTOCOL.md
 * sec 4, "What 'compatible' means": both directions matter, because "I can
 * read you" and "you can read me" are different claims. */
/* safety_link_versions_compatible() moved to safety_link_frame.c/.h (pure
 * two-sided comparison, no locking/hardware -- see that header's own
 * comment). Called the same way from here.
 *
 * commit/datetime must each fit the shared codec's fixed caps (kilnlink_announce.h:
 * KILNLINK_ANNOUNCE_MAX_COMMIT_LEN/MAX_DATETIME_LEN) -- a build identity longer than
 * that isn't a real build stamp (see that header's own comment), so catch it at
 * compile time rather than let kilnlink_announce_encode() silently drop the frame
 * via KILNLINK_ANNOUNCE_ERR_STRING_TOO_LONG. sizeof() includes each string's
 * implicit '\0', so this is intentionally a byte more conservative than the true
 * length. */
#define SAFETY_ANNOUNCE_VERSION_PAYLOAD_MAX KILNLINK_ANNOUNCE_MAX_LEN
_Static_assert(sizeof(FW_GIT_COMMIT) <= KILNLINK_ANNOUNCE_MAX_COMMIT_LEN,
               "ANNOUNCE_VERSION commit hash no longer fits kilnlink_announce_t");
_Static_assert(sizeof(FW_BUILD_DATE " " FW_BUILD_TIME) <= KILNLINK_ANNOUNCE_MAX_DATETIME_LEN,
               "ANNOUNCE_VERSION build datetime no longer fits kilnlink_announce_t");
/* safety_link.h's peer_build_commit/peer_build_datetime buffers are sized by
 * literal (64/32) rather than by #include, since that header stays free of
 * the kilnlink dependency -- pin them equal to the real caps here, where both
 * are already visible. */
_Static_assert(sizeof(((SafetyLinkClass *)0)->peer_build_commit) == KILNLINK_ANNOUNCE_MAX_COMMIT_LEN,
               "safety_link.h's peer_build_commit no longer matches KILNLINK_ANNOUNCE_MAX_COMMIT_LEN");
_Static_assert(sizeof(((SafetyLinkClass *)0)->peer_build_datetime) == KILNLINK_ANNOUNCE_MAX_DATETIME_LEN,
               "safety_link.h's peer_build_datetime no longer matches KILNLINK_ANNOUNCE_MAX_DATETIME_LEN");

/* Builds the ESP's outbound ANNOUNCE_VERSION (0x0F) payload via the shared
 * CommonFW codec (kilnlink_announce_encode) -- same layout as Frame C
 * (SAFETY_CMD_FW_VERSION), truncated at boot_id (LINK_PROTOCOL.md sec 4's
 * table: no config_version/config_crc, those describe the Pico's own active
 * config; kilnlink_announce.h's own comment notes the same truncation). Real
 * build identity from build_info.h, same source uart_bridge.c's
 * build_fw_version_reply() uses for the PC link's INFO_CMD_GET_FW_VERSION --
 * see this file's build_info.h include comment for why this can be real data
 * rather than SaftyFW's current honest stub. Returns bytes written, 0 if
 * out_cap is too small or encoding otherwise fails. */
static size_t safety_build_announce_version_payload(const SafetyLinkClass *link, uint8_t *out,
                                                      size_t out_cap)
{
    static const char commit[] = FW_GIT_COMMIT;
    static const char datetime[] = FW_BUILD_DATE " " FW_BUILD_TIME;
    size_t commit_len = sizeof(commit) - 1u;   /* drop the implicit '\0' */
    size_t datetime_len = sizeof(datetime) - 1u;

    kilnlink_announce_t msg = {0};
    msg.protocol_version = (uint16_t)KILNLINK_PROTOCOL_VERSION;
    msg.min_compatible = (uint16_t)KILNLINK_MIN_COMPATIBLE;
    msg.dirty = FW_GIT_DIRTY ? 1u : 0u;
    msg.commit_len = (uint8_t)commit_len;
    memcpy(msg.commit, commit, commit_len);
    msg.datetime_len = (uint8_t)datetime_len;
    memcpy(msg.datetime, datetime, datetime_len);
    msg.boot_id = link->esp_boot_id;

    kilnlink_announce_status_t status;
    size_t len = kilnlink_announce_encode(&msg, out, out_cap, &status);
    if (len == 0) {
        ESP_LOGE(TAG, "ANNOUNCE_VERSION encode failed (status=%d)", (int)status);
        return 0;
    }
    return len;
}

/* One-shot BROADCAST send -- no ACK, no blocking beyond handing the bytes to
 * the UART (uart_protocol_send_broadcast's own contract). Safe to call from
 * any task that holds a valid, initialized link. */
static void safety_link_send_announce_version_once(SafetyLinkClass *link)
{
    uint8_t payload[SAFETY_ANNOUNCE_VERSION_PAYLOAD_MAX];
    size_t len = safety_build_announce_version_payload(link, payload, sizeof(payload));
    if (len == 0) {
        return;
    }
    (void)uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY, UART_TASK_ID_SAFETY,
                                        UART_TASK_ID_SAFETY, payload, len);
}

/* LINK_PROTOCOL.md sec 4: sent "at ESP boot, repeated a few times against
 * loss, and re-sent whenever the Pico's boot_id changes". This function is
 * both call sites -- link_task_start's boot push (safety_poll_task's own
 * startup, mirroring SaftyFW's link_task_fn boot push) and the boot_id-change
 * path in safety_apply_fw_version() below. */
void safety_link_send_announce_version_burst(SafetyLinkClass *link)
{
    for (unsigned n = 0; n < SAFETY_ANNOUNCE_VERSION_REPEATS; n++) {
        safety_link_send_announce_version_once(link);
        if (n + 1u < SAFETY_ANNOUNCE_VERSION_REPEATS) {
            vTaskDelay(pdMS_TO_TICKS(SAFETY_ANNOUNCE_VERSION_REPEAT_GAP_MS));
        }
    }
}

/* Parses as much of a Pico FW_VERSION (0x0B) frame as is present, per
 * LINK_PROTOCOL.md sec 4's "read bytes 1-4 first" floor rule: protocol/
 * min_compatible are read and returned whenever the frame is at least 5
 * bytes, independent of whether the variable-length commit/datetime/boot_id
 * tail parses cleanly. *out_have_boot_id is only set true if boot_id was
 * actually reachable -- a truncated or old-format frame that stops short of
 * it must not report a stale/zero boot_id as real. Returns false only if
 * bytes 1-4 themselves aren't present (frame too short to say anything). */
/* safety_parse_fw_version() moved to safety_link_frame.c/.h (pure decode, no
 * locking/hardware -- see that header's own comment, including the
 * 2026-08-27 stack-overflow fix's truncation contract). Called the same way
 * from here.
 *
 * Applies one Pico FW_VERSION frame: updates the tracked peer-compatibility
 * verdict and boot_id, and re-announces ourselves (a burst, not just one
 * frame -- same loss-tolerance reasoning as the boot push) if the boot_id
 * changed, since a Pico that just rebooted has forgotten who it was talking
 * to (LINK_PROTOCOL.md sec 4). Never touches the isolated fault line
 * directly -- see safety_update_health(), the one place that reads
 * peer_version_known/peer_version_compatible and decides what to do about a
 * mismatch (Phase 7b.5). */
void safety_apply_fw_version(SafetyLinkClass *link, const uart_proto_message_t *msg)
{
    uint16_t peer_protocol = 0;
    uint16_t peer_min_compatible = 0;
    uint8_t peer_boot_id = 0;
    bool have_boot_id = false;
    bool dirty = false;
    uint8_t commit[64];
    uint8_t commit_len = 0;
    uint8_t datetime[32];
    uint8_t datetime_len = 0;
    uint8_t config_version = 0;
    uint16_t config_crc = 0;
    bool have_build = false;

    if (!safety_parse_fw_version(msg->payload, msg->length, &peer_protocol, &peer_min_compatible,
                                  &peer_boot_id, &have_boot_id, &dirty, commit, &commit_len,
                                  datetime, &datetime_len, &config_version, &config_crc,
                                  &have_build)) {
        return; /* too short to read even bytes 1-4 -- malformed, discard */
    }

    bool compatible = safety_link_versions_compatible((uint16_t)KILNLINK_PROTOCOL_VERSION,
                                                        (uint16_t)KILNLINK_MIN_COMPATIBLE,
                                                        peer_protocol, peer_min_compatible);

    bool boot_id_changed = false;
    if (!safety_lock(link)) {
        return;
    }
    link->peer_version_known = true;
    link->peer_version_compatible = compatible;
    link->peer_protocol_version = peer_protocol;
    link->peer_min_compatible = peer_min_compatible;
    if (have_boot_id) {
        boot_id_changed = (!link->pico_boot_id_known) || (peer_boot_id != link->pico_boot_id);
        link->pico_boot_id = peer_boot_id;
        link->pico_boot_id_known = true;
        if (boot_id_changed) {
            /* The Pico restarted, so its trip_seq counter restarted at 0 too
             * -- forget ours, or the dedup below mistakes the new boot's
             * first trip for one we have already seen (audit 2026-08-27:
             * another instance of this repo's recurring "counter reset on
             * one side of a producer/consumer pair" class). Concretely:
             * Pico trips with seq=1, watchdog-reboots, the same condition
             * trips again with seq=1, and safety_apply_trip_event()'s
             * is_new_event test reads false -- so the "safety processor
             * TRIPPED" log for a genuine second trip is never emitted,
             * exactly in the reboot-loop scenario where that record matters
             * most. The cached trip fields themselves still refresh, so this
             * costs the human-visible record rather than the trip response.
             *
             * Only the dedup bookkeeping is cleared, deliberately NOT the
             * cached trip DATA: a trip reported just before the reboot is
             * still the most recent thing that actually happened, and
             * blanking it would erase evidence rather than refresh it. */
            link->cached.trip_event_ever_received = false;
            link->cached.trip_last_seq = 0u;
            /* RELAY_LIFE_BUDGET.md: the Pico rebooting may have
             * left K4 in either state before it ever comes up -- this ESP's
             * last-observed safety_relay_state predates that reboot and must
             * not be compared against the new boot's first status frame
             * (same reset-one-side-of-a-pair hazard as trip_last_seq just
             * above). Forget it; safety_apply_status() resyncs silently on
             * the next frame, counting zero edges for that resync. */
            link->safety_relay_state_known = false;
            /* Owed to the peer, but NOT sent from here -- see
             * reannounce_pending's own doc comment (safety_link.h) for why
             * this moved off the calling task's stack 2026-09-10. */
            link->reannounce_pending = true;
        }
    }
    /* TODO.md owner-report item 5: only overwrite the cached build/config
     * identity once a frame actually reached that far -- a truncated reply
     * must not clobber a previously-known-good value with a fabricated one. */
    if (have_build) {
        link->peer_build_known = true;
        link->peer_build_dirty = dirty;
        memcpy(link->peer_build_commit, commit, commit_len);
        link->peer_build_commit_len = commit_len;
        memcpy(link->peer_build_datetime, datetime, datetime_len);
        link->peer_build_datetime_len = datetime_len;
        link->peer_config_version = config_version;
        link->peer_config_crc = config_crc;
    }
    safety_unlock(link);

    /* No synchronous send here any more -- reannounce_pending (set above,
     * under the same lock, in the boot_id_changed branch) is what actually
     * schedules the burst; safety_poll_task picks it up on its own stack. */
}

/* ------------------------------------------------------------------------ */
/* ROADMAP.md M5 -- SAFETY_CMD_PUSH_CONTEXT (LINK_PROTOCOL.md sec 4)        */
/* ------------------------------------------------------------------------ */

void safety_link_set_context_sources(SafetyLinkClass *link, void *io_or_null,
                                      void *thermo_bus_or_null)
{
    if (!link) {
        return;
    }
    link->context_io = io_or_null;
    link->context_thermo_bus = thermo_bus_or_null;
}

/* Rolls relay_now_mask into the relay_last_on_tick[] bookkeeping and returns
 * the resulting relay_recent_mask -- "relays commanded on at any point in
 * the last recent_window_s" (LINK_PROTOCOL.md sec 4). Poll-task-only, no
 * locking needed: same reasoning as down_logged above, only the poll task
 * ever touches these fields. */
static uint8_t safety_context_update_relay_recent(SafetyLinkClass *link, uint8_t relay_now_mask)
{
    TickType_t now = xTaskGetTickCount();
    uint8_t recent = 0;
    for (unsigned i = 0; i < 4u; i++) {
        if (relay_now_mask & (1u << i)) {
            link->relay_last_on_tick[i] = now;
            link->relay_last_on_tick_valid[i] = true;
        }
        if (link->relay_last_on_tick_valid[i] &&
            safety_elapsed_ms(link->relay_last_on_tick[i]) <=
                (uint32_t)SAFETY_LINK_CONTEXT_RECENT_WINDOW_S * 1000u) {
            recent |= (uint8_t)(1u << i);
        }
    }
    return recent;
}

/* Builds and sends one SAFETY_CMD_PUSH_CONTEXT broadcast from live KilnFW
 * state -- relay mask from kiln_io (context_io), raw per-channel
 * thermocouple readings + configured tc_type from the MAX31856 bus
 * (context_thermo_bus), and per-zone setpoint/active/relay-on/guard-tripped
 * from profile_executor_get_status() (a free accessor already shared with
 * dashboard_http.c, no pointer needed). Called from the poll task every
 * iteration, same cadence LINK_PROTOCOL.md sec 4 specifies ("every
 * CONFIG_KILNCTL_SAFETY_POLL_PERIOD_MS"); never blocks beyond handing bytes
 * to the UART (uart_protocol_send_broadcast's own contract).
 *
 * Either source pointer may be NULL (that board/bus never came up this
 * boot) -- the frame still goes out with zone_count = 0 and/or
 * relay_now_mask = 0 rather than being skipped: the Pico still learns
 * boot_id/seq/uptime/flags, which is strictly better than silence. */
void safety_build_and_send_context(SafetyLinkClass *link)
{
    kiln_io_t *io = (kiln_io_t *)link->context_io;
    MAX31856BusClass *thermo_bus = (MAX31856BusClass *)link->context_thermo_bus;

    uint8_t relay_now_mask = io ? kiln_io_get_relay_shadow(io) : 0u;
    uint8_t relay_recent_mask = safety_context_update_relay_recent(link, relay_now_mask);

    /* Runs on safety_poll_task every poll iteration (safety_link_poll.c),
     * whose stack is only SAFETY_POLL_TASK_STACK = 8192 B and lives in
     * PSRAM (safety_link.c:1636-1638's own comment on why nothing here can
     * risk a stack overflow with the flash cache disabled) -- heap-allocate
     * rather than materialize a 1384-byte profile_exec_status_t on that
     * stack, same pattern as safety_cfg_http.c/dashboard_exec_http.c. Every
     * field below is read, not just the active-firing bool, so the narrow
     * profile_executor_get_active_id() accessor does not fit here. */
    profile_exec_status_t *pstat = heap_caps_malloc(sizeof(*pstat), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!pstat) {
        ESP_LOGE(TAG, "safety_build_and_send_context: out of memory, skipping this PUSH_CONTEXT broadcast");
        return;
    }
    profile_executor_get_status(pstat);

    kilnlink_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.boot_id = link->esp_boot_id;
    ctx.seq = link->context_seq++;
    ctx.uptime_ms = (uint32_t)(xTaskGetTickCount() * (TickType_t)portTICK_PERIOD_MS);
    ctx.relay_now_mask = relay_now_mask;
    ctx.relay_recent_mask = relay_recent_mask;
    ctx.recent_window_s = (uint8_t)SAFETY_LINK_CONTEXT_RECENT_WINDOW_S;

    /* One burst read of every initialized channel -- MAX31856_read_all skips
     * channels that never came up rather than faking them, so readings[]
     * is indexed by *position among initialized channels*, not by channel
     * number; match each entry back to its channel via ::channel below
     * rather than assuming readings[i] is channel i.
     *
     * 2026-08-19, TODO.md 10.14 Phase 2: goes through
     * thermo_owner_command_read_all() instead of calling MAX31856_read_all()
     * on `thermo_bus` directly -- see thermo_owner.h's top comment for why
     * this is the one MAX31856_read_all() caller migrated this pass (this
     * function already had to be touched to reach MAX31856_get_config()
     * below, which stays a direct call; the other MAX31856_read_all()
     * callers elsewhere in this codebase were out of scope). */
    MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
    size_t reading_count = 0;
    if (thermo_bus) {
        (void)thermo_owner_command_read_all(readings, MAX31856_CHANNEL_COUNT, &reading_count);
    }

    bool any_zone_faulted = (pstat->state == PROFILE_EXEC_FAULTED);
    bool heat_requested = false;

    uint8_t zone_count = thermo_bus ? (uint8_t)MAX31856_CHANNEL_COUNT : 0u;
    if (zone_count > KILNLINK_CONTEXT_MAX_ZONES) {
        zone_count = KILNLINK_CONTEXT_MAX_ZONES; /* defensive; the two constants agree today */
    }
    ctx.zone_count = zone_count;

    for (uint8_t i = 0; i < zone_count; i++) {
        kilnlink_zone_context_t *z = &ctx.zones[i];
        z->zone_index = i;

        const MAX31856Reading *reading = NULL;
        for (size_t r = 0; r < reading_count; r++) {
            if (readings[r].channel == i) {
                reading = &readings[r];
                break;
            }
        }

        MAX31856Config cfg;
        memset(&cfg, 0, sizeof(cfg));
        /* Direct call, deliberately not routed through thermo_owner: per
         * MAX31856_get_config()'s implementation it only reads
         * ch->cr0_shadow/ch->cr1_shadow under ch->lock -- no SPI transfer at
         * all -- so a task hop here would add latency for no correctness
         * benefit (thermo_owner.h's top comment). */
        MAX31856Class *ch = MAX31856_bus_channel(thermo_bus, i);
        if (ch) {
            (void)MAX31856_get_config(ch, &cfg);
        }
        z->tc_type = cfg.tc_type;

        bool measured_valid =
            reading && !reading->spi_failed && !isnan(reading->tc_temperature_c);
        z->measured_c = measured_valid ? reading->tc_temperature_c : NAN;
        z->tc_fault = reading ? reading->fault_status : 0u;
        /* LINK_PROTOCOL.md sec 4: increment only at the point a fresh
         * conversion is actually consumed -- reading->stale is exactly that
         * signal (MAX31856Reading's own "no new conversion since the
         * previous read" flag), never the mere act of building this frame. */
        if (reading && !reading->stale) {
            link->context_sample_counter[i]++;
        }
        z->sample_counter = link->context_sample_counter[i];

        bool zone_active = pstat->zones[i].active;
        bool zone_relay_on = pstat->zones[i].relay_commanded_on;
        bool zone_faulted = pstat->zones[i].faulted;
        /* profile_executor.c: "each active zone runs its own independent
         * PID/guard/relay against one shared setpoint" -- there is no
         * per-zone setpoint to report, so every currently-active zone
         * reports the one shared target; an inactive zone has no setpoint,
         * same NaN-for-invalid convention as measured_c above. */
        z->setpoint_c = zone_active ? pstat->target_c : NAN;

        if (measured_valid) {
            z->flags |= KILNLINK_ZONE_FLAG_MEASURED_VALID;
        }
        if (zone_active) {
            z->flags |= KILNLINK_ZONE_FLAG_ACTIVE;
        }
        if (zone_relay_on) {
            z->flags |= KILNLINK_ZONE_FLAG_RELAY_ON;
            heat_requested = true;
        }
        if (zone_faulted) {
            z->flags |= KILNLINK_ZONE_FLAG_GUARD_TRIPPED;
            any_zone_faulted = true;
        }
    }

    if (pstat->state == PROFILE_EXEC_RUNNING) {
        ctx.flags |= KILNLINK_CONTEXT_FLAG_PROFILE_RUNNING;
    }
    /* 2026-09-15 Opus re-review N1: "no active heat owner" for the Pico's
     * tc-type heat-safety gate, as distinct from HEAT_REQUESTED's literal
     * instantaneous relay state above. RUNNING or PAUSED (a paused firing
     * can resume heat at any moment without a fresh operator commit);
     * either heat_enable claimant held (autotune's claimant also covers CT
     * sweep / relay-identification, see heat_enable.h's own comment on
     * HEAT_ENABLE_CLAIMANT_AUTOTUNE); or danger mode active/requesting --
     * danger_mode_active() alone is folded in too, not just its own
     * heat-requested flag, since danger mode's manual diagnostics bypass
     * calls safety_link_request_enable() directly and can flip K4 on a
     * separate cadence from its own heat_requested bookkeeping. */
    if (heat_owner_active_decide(pstat->state, heat_enable_is_held(HEAT_ENABLE_CLAIMANT_PROFILE),
                                  heat_enable_is_held(HEAT_ENABLE_CLAIMANT_AUTOTUNE),
                                  danger_mode_active())) {
        ctx.flags |= KILNLINK_CONTEXT_FLAG_HEAT_OWNER_ACTIVE;
    }
    free(pstat); /* last read of pstat was just above -- nothing below needs it */
    if (any_zone_faulted) {
        ctx.flags |= KILNLINK_CONTEXT_FLAG_ANY_ZONE_FAULTED;
    }
    if (heat_requested) {
        ctx.flags |= KILNLINK_CONTEXT_FLAG_HEAT_REQUESTED;
    }
    /* This driver always believes its own numbers -- an absent thermo_bus or
     * io just makes the numbers empty (relay_now_mask 0 / zone_count 0), not
     * untrustworthy. */
    ctx.flags |= KILNLINK_CONTEXT_FLAG_CONTEXT_VALID;
#if CONFIG_KILNCTL_SIM_PLANT
    ctx.flags |= KILNLINK_CONTEXT_FLAG_SIM_PLANT;
#endif

    uint8_t payload[KILNLINK_CONTEXT_MAX_LEN];
    kilnlink_context_status_t status = KILNLINK_CONTEXT_OK;
    size_t len = kilnlink_context_encode(&ctx, payload, sizeof(payload), &status);
    if (len == 0) {
        ESP_LOGW(TAG, "PUSH_CONTEXT encode failed: status %d", (int)status);
        return;
    }

    /* Same (dst_device, dst_task, src_task) triple as ANNOUNCE_VERSION's own
     * broadcast call site above -- fire-and-forget, no ACK expected. */
    (void)uart_protocol_send_broadcast(&link->proto, UART_PROTO_DEVICE_SAFETY, UART_TASK_ID_SAFETY,
                                        UART_TASK_ID_SAFETY, payload, len);
}

/* ------------------------------------------------------------------------ */
/* Phase 10 (SaftyFW) / TODO.md 9.5 -- UPDATE_STATUS (0x14)                 */
/* ------------------------------------------------------------------------ */

/* Accepts one UPDATE_STATUS frame and replaces the cached update_status
 * with it -- same shape as safety_apply_status() above (a short/malformed
 * payload is dropped and counted as a frame error, not partially applied).
 * Wire layout: SAFETY_LINK_UPDATE_STATUS_HEADER_LEN (16) bytes of fixed
 * header, then gap_count * 2 bytes of u16 LE gap chunk indices -- see
 * safety_link.h's header comment on safety_link_update_status_t for the
 * mirrored-from-SaftyFW field-by-field layout. */
bool safety_apply_update_status(SafetyLinkClass *link, const uart_proto_message_t *msg)
{
    if (msg->length < SAFETY_LINK_UPDATE_STATUS_HEADER_LEN) {
        if (safety_lock(link)) {
            link->stats.frame_errors++;
            safety_unlock(link);
        }
        ESP_LOGW(TAG, "UPDATE_STATUS from dev%u/task%u: %u bytes, shorter than the %u-byte header",
                 msg->device, msg->task_id, msg->length, SAFETY_LINK_UPDATE_STATUS_HEADER_LEN);
        return false;
    }

    const uint8_t *p = msg->payload;
    uint8_t gap_count = p[15];
    if (gap_count > SAFETY_LINK_UPDATE_STATUS_MAX_GAPS) {
        gap_count = SAFETY_LINK_UPDATE_STATUS_MAX_GAPS; /* defensive clamp -- untrusted wire byte */
    }
    size_t needed = (size_t)SAFETY_LINK_UPDATE_STATUS_HEADER_LEN + (size_t)gap_count * 2u;
    if ((size_t)msg->length < needed) {
        if (safety_lock(link)) {
            link->stats.frame_errors++;
            safety_unlock(link);
        }
        ESP_LOGW(TAG, "UPDATE_STATUS from dev%u/task%u: %u bytes, too short for its own gap_count=%u",
                 msg->device, msg->task_id, msg->length, gap_count);
        return false;
    }

    if (!safety_lock(link)) {
        return false;
    }
    link->update_status.state = p[1];
    link->update_status.last_error = p[2];
    link->update_status.bytes_received = safety_read_u32_le(&p[3]);
    link->update_status.total_chunks = safety_read_u32_le(&p[7]);
    link->update_status.received_chunks = safety_read_u32_le(&p[11]);
    link->update_status.gap_count = gap_count;
    for (uint8_t i = 0; i < gap_count; i++) {
        link->update_status.gap_chunk_indices[i] = safety_read_u16_le(&p[16 + 2u * i]);
    }
    link->update_status_tick = xTaskGetTickCount();
    link->update_status_ever_received = true;
    safety_unlock(link);
    return true;
}

/* ------------------------------------------------------------------------ */
/* Frame handling                                                           */
/* ------------------------------------------------------------------------ */

/* Accepts one status frame from the Pico (see the contract in safety_link.h)
 * and replaces the cache with it. Returns false, and counts a frame error, if
 * the payload isn't a status frame of the right length -- a short or unknown
 * payload is dropped rather than partially applied, because half a temperature
 * reading is worse than none. */
bool safety_apply_status(SafetyLinkClass *link, const uart_proto_message_t *msg)
{
    /* Length before payload[0]: a zero-length frame has no subcommand byte to
     * read, and the || below short-circuits in the right order only if the
     * length test comes first.
     *
     * 2026-08-23: accepts EITHER SAFETY_LINK_STATUS_FRAME_LEN_V1 (23) or _V2
     * (24) -- never just one. An older Pico (23 bytes, no tx_dropped_sat)
     * and a newer one (24 bytes) must both keep working here regardless of
     * which side gets flashed first; rejecting either length would silence
     * Frame A itself, the one channel this whole investigation proved still
     * works when Frame B (DIAG) does not. See safety_link.h's
     * SAFETY_LINK_STATUS_FRAME_LEN comment for the Pico-side half of this
     * same skew-safety argument.
     *
     * 2026-09-03: also accepts _V3 (26, BORROWED status), same reasoning one
     * more step out -- see safety_link.h's SAFETY_LINK_STATUS_FRAME_LEN_V3
     * comment. */
    if ((msg->length != SAFETY_LINK_STATUS_FRAME_LEN_V1 &&
         msg->length != SAFETY_LINK_STATUS_FRAME_LEN_V2 &&
         msg->length != SAFETY_LINK_STATUS_FRAME_LEN_V3) ||
        msg->payload[0] != SAFETY_CMD_GET_STATUS) {
        if (safety_lock(link)) {
            link->stats.frame_errors++;
            safety_unlock(link);
        }
        ESP_LOGW(TAG, "unexpected frame from dev%u/task%u: subcmd 0x%02X, %u bytes",
                 msg->device, msg->task_id, msg->payload[0], msg->length);
        return false;
    }

    const uint8_t *p = msg->payload;
    if (!safety_lock(link)) {
        return false;
    }
    /* Bits 0/1 describe *our* view of the link and *our* fault output; they
     * are filled in at read time, so whatever the peer put there is dropped
     * rather than trusted (see safety_link.h). */
    link->cached.flags = (uint8_t)(p[KILNLINK_FRAME_A_OFF_FLAGS] & ~(SAFETY_FLAG_LINK_UP | SAFETY_FLAG_FAULT));
    link->cached.tc_temp_c = safety_read_f32_le(&p[KILNLINK_FRAME_A_OFF_TC_TEMP_C]);
    link->cached.cj_temp_c = safety_read_f32_le(&p[KILNLINK_FRAME_A_OFF_CJ_TEMP_C]);
    /* The contract says the Pico sends NaN when SAFETY_FLAG_TEMP_VALID is
     * clear, but a peer that sends 0.0 instead -- or a firmware that forgets --
     * must not have it forwarded to the PC as a real reading of a stone-cold
     * kiln. The flag is the authority; enforce it here rather than trusting
     * the far side to have been careful.
     *
     * 2026-09-08: ONLY tc_temp_c is gated on SAFETY_FLAG_TEMP_VALID here.
     * cj_temp_c used to be NaN'd alongside it unconditionally, which is
     * exactly the bug this fix addresses -- the on-chip cold junction is
     * independent of the external thermocouple probe TEMP_VALID describes,
     * so a probe fault must not blank a genuinely good cj_c. cj_temp_c's own
     * gating happens below, once cj_valid[_known] is known from the V3
     * flags2 byte (or falls back to this same flag on an older/unconfirmed
     * peer that cannot report cj_valid separately at all). */
    if (!(link->cached.flags & SAFETY_FLAG_TEMP_VALID)) {
        link->cached.tc_temp_c = NAN;
    }
    link->cached.tc_fault = p[KILNLINK_FRAME_A_OFF_TC_FAULT];
    link->cached.current_a[0] = safety_read_f32_le(&p[KILNLINK_FRAME_A_OFF_AMPS1]);
    link->cached.current_a[1] = safety_read_f32_le(&p[KILNLINK_FRAME_A_OFF_AMPS2]);
    link->cached.current_a[2] = safety_read_f32_le(&p[KILNLINK_FRAME_A_OFF_AMPS3]);
    /* Byte 23 -- only present on a V2-length frame (checked above, msg->length
     * already verified to be exactly V1 or V2 by this point, nothing else).
     * V1 leaves both fields at their prior cached value if not explicitly
     * reset here, so tx_dropped_known is set unconditionally either way
     * rather than only on the true branch -- a peer that regresses from V2
     * to V1 mid-session (e.g. a rollback) must not leave a stale "known"
     * flag pointing at a now-meaningless stale byte. */
    if (msg->length == SAFETY_LINK_STATUS_FRAME_LEN_V2 || msg->length == SAFETY_LINK_STATUS_FRAME_LEN_V3) {
        link->cached.tx_dropped_known = true;
        link->cached.tx_dropped_sat = p[KILNLINK_FRAME_A_OFF_TX_DROPPED_SAT];
    } else {
        link->cached.tx_dropped_known = false;
        link->cached.tx_dropped_sat = 0;
    }
    /* Bytes 24/25 -- only present on a V3-length frame. Same "unconditional
     * else, not only on the true branch" discipline as tx_dropped_known just
     * above: a peer that regresses from V3 to V1/V2 mid-session (a rollback,
     * or simply an ESP that stops confirming V3 support) must not leave a
     * stale borrowed_known=true pointing at a now-meaningless stale byte. */
    if (msg->length == SAFETY_LINK_STATUS_FRAME_LEN_V3) {
        link->cached.borrowed_known = true;
        link->cached.borrowed = (p[KILNLINK_FRAME_A_OFF_FLAGS2] & SAFETY_LINK_STATUS_FLAG2_BORROWED) != 0u;
        link->cached.borrowed_zone_index = p[KILNLINK_FRAME_A_OFF_BORROWED_ZONE_INDEX];
        link->cached.cj_valid_known = true;
        link->cached.cj_valid = (p[KILNLINK_FRAME_A_OFF_FLAGS2] & SAFETY_LINK_STATUS_FLAG2_CJ_VALID) != 0u;
        link->cached.tc_config_reasserted_known = true;
        link->cached.tc_config_reasserted =
            (p[KILNLINK_FRAME_A_OFF_FLAGS2] & SAFETY_LINK_STATUS_FLAG2_TC_CONFIG_REASSERTED) != 0u;
        link->cached.pico_active_slot_known =
            (p[KILNLINK_FRAME_A_OFF_FLAGS2] & SAFETY_LINK_STATUS_FLAG2_ACTIVE_SLOT_KNOWN) != 0u;
        link->cached.pico_active_slot_is_b =
            (p[KILNLINK_FRAME_A_OFF_FLAGS2] & SAFETY_LINK_STATUS_FLAG2_ACTIVE_SLOT_B) != 0u;
    } else {
        link->cached.borrowed_known = false;
        link->cached.borrowed = false;
        link->cached.borrowed_zone_index = SAFETY_LINK_BORROWED_ZONE_UNKNOWN;
        link->cached.cj_valid_known = false;
        link->cached.cj_valid = false;
        link->cached.tc_config_reasserted_known = false;
        link->cached.tc_config_reasserted = false;
        link->cached.pico_active_slot_known = false;
        link->cached.pico_active_slot_is_b = false;
    }
    /* cj_temp_c's own NaN gate -- deliberately separate from tc_temp_c's
     * TEMP_VALID gate above (2026-09-08 fix). A V3 peer told us cj_valid
     * directly; an older/unconfirmed peer never reported it at all, so the
     * only honest fallback is the OLD behaviour (tied to TEMP_VALID) rather
     * than inventing a "known" answer this frame never carried. */
    if (link->cached.cj_valid_known) {
        if (!link->cached.cj_valid) {
            link->cached.cj_temp_c = NAN;
        }
    } else if (!(link->cached.flags & SAFETY_FLAG_TEMP_VALID)) {
        link->cached.cj_temp_c = NAN;
    }
    /* RELAY_LIFE_BUDGET.md: count an observed off->on or on->off
     * transition of K4 (SAFETY_FLAG_RELAY). safety_relay_state_known starts
     * false (this driver's own boot, or a just-applied Pico boot_id change --
     * see safety_apply_fw_version() above) so the FIRST frame after either
     * event only resyncs the tracked state and counts zero edges, never a
     * fabricated one. The edge is computed here, under link->state_lock, but
     * relay_cycles_note_safety_edge() itself is called after safety_unlock()
     * below -- calling it while still holding state_lock would establish a
     * new lock order (link->state_lock -> s_rc.lock) alongside whatever
     * order relay_cycles.c's own callers already use elsewhere, and nothing
     * needs the edge count to be applied inside this critical section. */
    bool relay_now = (link->cached.flags & SAFETY_FLAG_RELAY) != 0;
    bool relay_edge = link->safety_relay_state_known && relay_now != link->safety_relay_state;
    link->safety_relay_state = relay_now;
    link->safety_relay_state_known = true;

    link->cached_tick = xTaskGetTickCount();
    link->ever_received = true;
    link->stats.frames_received++;
    safety_unlock(link);
    if (relay_edge) {
        relay_cycles_note_safety_edge();
    }
    return true;
}

/* Accepts one SAFETY_CMD_POWER (Frame E) frame from the Pico and replaces the
 * cached power reading with it. Same discard-rather-than-partially-apply
 * contract as safety_apply_status(): "no guard reads any of this. It exists
 * to be displayed" (LINK_PROTOCOL.md sec 6), so a malformed frame is simply
 * dropped -- nothing downstream needs it to fail safe. Byte layout (55
 * bytes total):
 *   byte0        cmd (0x0E)
 *   byte1        power_window_s
 *   byte2        flags: bit0 mains_voltage_configured, bit1 any_channel_clipped, bit2 calibrated
 *   bytes3..6    mains_voltage_v, f32 LE (NaN if not configured)
 *   bytes7..30   3 x (i_conducting_a f32 LE, conduction_fraction f32 LE), 8 bytes each
 *   bytes31..42  3 x p_avg_w, f32 LE
 *   bytes43..46  p_total_w, f32 LE
 *   bytes47..54  energy_wh, f64 LE (accumulated since Pico boot -- not cached
 *                here; nothing in this build displays it yet)
 *   bytes55..60  3 x counts_avg, u16 LE (V2 only, 2026-09-06 -- raw ADC
 *                counts per channel, valid only when flags bit3 is set)
 * Mirrors kilnlink_power_decode() in firmware/CommonFW/src/kilnlink_power.c
 * byte-for-byte; see uart_task_ids.h's SAFETY_CMD_POWER comment for why this
 * driver hand-parses rather than linking that codec. Accepts either
 * SAFETY_LINK_POWER_FRAME_LEN_V1 (55, legacy Pico) or _V2 (61, current). */
bool safety_apply_power(SafetyLinkClass *link, const uart_proto_message_t *msg)
{
    if ((msg->length != SAFETY_LINK_POWER_FRAME_LEN_V1 &&
         msg->length != SAFETY_LINK_POWER_FRAME_LEN_V2) ||
        msg->payload[0] != SAFETY_CMD_POWER) {
        if (safety_lock(link)) {
            link->stats.frame_errors++;
            safety_unlock(link);
        }
        ESP_LOGW(TAG, "unexpected POWER frame from dev%u/task%u: subcmd 0x%02X, %u bytes",
                 msg->device, msg->task_id, msg->payload[0], msg->length);
        return false;
    }

    const uint8_t *p = msg->payload;
    if (!safety_lock(link)) {
        return false;
    }
    link->cached.power_window_s = p[1];
    uint8_t flags = p[2];
    link->cached.power_mains_voltage_configured =
        (flags & SAFETY_LINK_POWER_FLAG_MAINS_VOLTAGE_CONFIGURED) != 0u;
    link->cached.power_any_channel_clipped = (flags & SAFETY_LINK_POWER_FLAG_ANY_CHANNEL_CLIPPED) != 0u;
    link->cached.power_calibrated = (flags & SAFETY_LINK_POWER_FLAG_CALIBRATED) != 0u;
    link->cached.power_mains_voltage_v = safety_read_f32_le(&p[3]);
    for (unsigned ch = 0; ch < SAFETY_LINK_POWER_CHANNELS; ch++) {
        size_t base = 7u + (size_t)ch * 8u;
        link->cached.power_channel_i_conducting_a[ch] = safety_read_f32_le(&p[base]);
        link->cached.power_channel_conduction_fraction[ch] = safety_read_f32_le(&p[base + 4u]);
    }
    for (unsigned ch = 0; ch < SAFETY_LINK_POWER_CHANNELS; ch++) {
        link->cached.power_channel_w[ch] = safety_read_f32_le(&p[31u + (size_t)ch * 4u]);
    }
    link->cached.power_total_w = safety_read_f32_le(&p[43]);
    /* bytes47..54 (energy_wh) intentionally not cached -- nothing in this
     * build's dashboard/LCD reads it yet; add a field here when it does. */
    bool counts_valid = (msg->length == SAFETY_LINK_POWER_FRAME_LEN_V2) &&
                         ((flags & SAFETY_LINK_POWER_FLAG_COUNTS_VALID) != 0u);
    link->cached.power_counts_valid = counts_valid;
    for (unsigned ch = 0; ch < SAFETY_LINK_POWER_CHANNELS; ch++) {
        link->cached.power_channel_counts_avg[ch] =
            counts_valid ? safety_read_u16_le(&p[55u + (size_t)ch * 2u]) : 0u;
    }
    link->cached.power_ever_received = true;
    link->stats.power_applied++; /* 2026-08-23: real counter, see its own doc comment (safety_link.h) */
    safety_unlock(link);
    return true;
}

/* Accepts one SAFETY_CMD_DIAG (Frame B) frame from the Pico and replaces the
 * cached diagnostic snapshot with it. Same discard-rather-than-partially-
 * apply contract as safety_apply_status()/safety_apply_power(): a malformed
 * frame is dropped and counted as a frame error rather than half-applied.
 * Byte layout (30 bytes total, LINK_PROTOCOL.md sec 6):
 *   byte0        cmd (0x08)
 *   byte1        trip_reason
 *   bytes2..3    warn_mask, u16 LE
 *   bytes4..5    trip_mask, u16 LE
 *   bytes6..9    uptime_ms, u32 LE
 *   byte10       boot_reason
 *   byte11       context_age_100ms (255 = never received)
 *   bytes12..15  context_frames_ok, u32 LE
 *   bytes16..19  context_frames_bad, u32 LE
 *   bytes20..23  tx_frames_dropped, u32 LE
 *   byte24       state
 *   byte25       flags
 *   bytes26..29  log_frames_dropped, u32 LE -- KILNLINK_PROTOCOL_VERSION 15 -> 16
 * Mirrors kilnlink_diag_decode() in firmware/CommonFW/src/kilnlink_diag.c
 * byte-for-byte; see uart_task_ids.h's SAFETY_CMD_DIAG comment for why this
 * driver hand-parses rather than linking that codec. */
/* SaftyFW src/safety_guards.h SAFETY_TRIP_MAIN_FAULT -- the only trip reason
 * fed by this board's own isolated fault-out line (GPIO6), see
 * safety_link_mark_boot_clean()'s doc comment (safety_link.h). */
#define SAFETY_LINK_TRIP_REASON_MAIN_FAULT 6u

/* Set once by safety_link_mark_boot_clean(), consumed by safety_apply_diag()
 * below -- see both functions' doc comments. Single ESP-side safety link
 * instance in this codebase, same "static app-wide flag" precedent as
 * danger_mode.c's s_dm.
 *
 * Two defects an Opus review found in the original version of this feature
 * (2026-08-27), both fixed here:
 *
 * 1. s_boot_clean never expired, so it stayed true for the ENTIRE boot, not
 *    just its first few seconds. A boot with no link at power-up (no DIAG
 *    yet) followed hours later by a genuine runtime S6a, followed by the
 *    link recovering, would have had that first-ever TRIPPED/MAIN_FAULT
 *    DIAG frame -- describing a REAL, current trip -- silently cleared.
 *    Fixed with s_boot_clean_deadline_ms: only frames arriving within
 *    SAFETY_LINK_BOOT_CLEAN_WINDOW_MS of the mark_boot_clean() call are
 *    ever eligible, and s_boot_clean itself is force-cleared once that
 *    deadline passes (belt-and-suspenders with the deadline check itself).
 *    Also now requires link->fault_sources == 0 at the moment of the
 *    attempt -- this board's OWN fault-source bits (kiln_enter_safe_state(),
 *    profile_executor.c, autotune_engine.c, uart_bridge.c's PC-link-lost
 *    path, ...) can legitimately go non-zero well after boot; the clear
 *    must never fire while this board itself currently has a reason to be
 *    asserting the fault line.
 *
 * 2. s_boot_clear_attempted was latched BEFORE calling
 *    safety_link_send_clear_trip(), whose return was discarded -- a locally
 *    refused send (stale/never-received DIAG age, see that function's own
 *    doc comment) burned the one-shot with nothing actually sent on the
 *    wire, leaving a real stale S6a latched for the rest of the boot. Fixed
 *    by only latching s_boot_clear_attempted on ESP_OK; a refused attempt
 *    can retry on the next DIAG frame, still bounded by the deadline above. */
#define SAFETY_LINK_BOOT_CLEAN_WINDOW_MS (30u * 1000u)
static bool s_boot_clean = false;
static bool s_boot_clear_attempted = false;
static uint32_t s_boot_clean_deadline_ms = 0;

void safety_link_mark_boot_clean(void)
{
    s_boot_clean = true;
    s_boot_clean_deadline_ms =
        (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) + SAFETY_LINK_BOOT_CLEAN_WINDOW_MS;
}

bool safety_apply_diag(SafetyLinkClass *link, const uart_proto_message_t *msg)
{
    if (msg->length != SAFETY_LINK_DIAG_FRAME_LEN || msg->payload[0] != SAFETY_CMD_DIAG) {
        if (safety_lock(link)) {
            link->stats.frame_errors++;
            safety_unlock(link);
        }
        ESP_LOGW(TAG, "unexpected DIAG frame from dev%u/task%u: subcmd 0x%02X, %u bytes",
                 msg->device, msg->task_id, msg->payload[0], msg->length);
        return false;
    }

    const uint8_t *p = msg->payload;
    if (!safety_lock(link)) {
        return false;
    }
    link->cached.diag_trip_reason = p[1];
    link->cached.diag_warn_mask = safety_read_u16_le(&p[2]);
    link->cached.diag_trip_mask = safety_read_u16_le(&p[4]);
    link->cached.diag_uptime_ms = safety_read_u32_le(&p[6]);
    link->cached.diag_boot_reason = p[10];
    link->cached.diag_context_age_100ms = p[11];
    link->cached.diag_context_frames_ok = safety_read_u32_le(&p[12]);
    link->cached.diag_context_frames_bad = safety_read_u32_le(&p[16]);
    link->cached.diag_tx_frames_dropped = safety_read_u32_le(&p[20]);
    link->cached.diag_state = p[24];
    link->cached.diag_flags = p[25];
    link->cached.diag_log_frames_dropped = safety_read_u32_le(&p[26]);
    link->cached.diag_ever_received = true;
    link->stats.diag_applied++; /* 2026-08-23: real counter, see its own doc comment (safety_link.h) */
    bool want_boot_clear = false;
    if (s_boot_clean) {
        uint32_t now = (uint32_t)(xTaskGetTickCount() * (TickType_t)portTICK_PERIOD_MS);
        if ((int32_t)(s_boot_clean_deadline_ms - now) <= 0) {
            /* Window has passed -- never eligible again this boot, whether
             * or not an attempt ever succeeded. See s_boot_clean's own doc
             * comment above for the "hours later" real-trip scenario this
             * closes. */
            s_boot_clean = false;
        } else if (!s_boot_clear_attempted && link->fault_sources == 0u &&
                   link->cached.diag_state == SAFETY_LINK_DIAG_STATE_TRIPPED &&
                   link->cached.diag_trip_reason == SAFETY_LINK_TRIP_REASON_MAIN_FAULT) {
            want_boot_clear = true;
            /* s_boot_clear_attempted is NOT latched here -- only on ESP_OK
             * from the actual send, in safety_link_service_boot_clear_if_
             * pending() below. A locally refused send (stale diag age, etc.)
             * must be retryable on the next DIAG frame, still bounded by the
             * deadline above. */
        }
    }
    if (want_boot_clear) {
        /* Flagged, not sent from here -- see boot_clear_pending's doc
         * comment (safety_link.h) for why (2026-09-10, docs/audits/
         * profile_executor_panic_2026-09-10_root_cause.md): this function is
         * reached from safety_drain_inbox_ex()'s dispatch on WHATEVER task
         * called it, and safety_link_send_clear_trip() reaches the same deep
         * uart_protocol_send_broadcast() chain as the announce-version
         * burst. safety_poll_task performs the actual send on its own
         * 8192 B stack. */
        link->boot_clear_pending = true;
    }
    safety_unlock(link);
    return true;
}

/* Performs the deferred boot_clear_pending send (see its doc comment,
 * safety_link.h) and latches s_boot_clear_attempted on success -- the exact
 * logic safety_apply_diag() used to run inline. Called only from
 * safety_poll_task (safety_link_poll.c), which has the stack headroom this
 * chain needs; safe to call unconditionally each iteration since it is a
 * no-op whenever nothing is pending. */
void safety_link_service_boot_clear_if_pending(SafetyLinkClass *link)
{
    bool owed = false;
    if (safety_lock(link)) {
        owed = link->boot_clear_pending;
        link->boot_clear_pending = false;
        safety_unlock(link);
    }
    if (!owed) {
        return;
    }
    ESP_LOGW(TAG, "boot was clean but a stale S6a (main-controller-fault) trip is still "
                  "latched from before this boot -- sending clear_trip to release it");
    if (safety_link_send_clear_trip(link) == ESP_OK && safety_lock(link)) {
        s_boot_clear_attempted = true;
        safety_unlock(link);
    }
}

/* Accepts one SAFETY_CMD_TRIP_EVENT (Frame D) frame from the Pico and
 * replaces the cached "most recent trip" snapshot with it -- never cleared
 * by anything else (see safety_link_status_t's trip_event_* field comments
 * in safety_link.h for why). Same discard-rather-than-partially-apply
 * contract as the other Pico->ESP frames. Byte layout (29 bytes total,
 * LINK_PROTOCOL.md sec 6):
 *   byte0        cmd (0x0D)
 *   byte1        trip_seq
 *   byte2        trip_reason
 *   bytes3..6    uptime_ms, u32 LE, at trip
 *   bytes7..10   safety_tc_c, f32 LE, at trip
 *   bytes11..14  deciding_threshold, f32 LE
 *   bytes15..18  current_a[0], f32 LE
 *   bytes19..22  current_a[1], f32 LE
 *   bytes23..26  current_a[2], f32 LE
 *   byte27       relay_recent_mask last received
 *   byte28       context_age_100ms at trip
 * Mirrors kilnlink_trip_decode() in firmware/CommonFW/src/kilnlink_trip.c
 * byte-for-byte; see uart_task_ids.h's SAFETY_CMD_TRIP_EVENT comment for why
 * this driver hand-parses rather than linking that codec.
 *
 * Idempotent per LINK_PROTOCOL.md sec 6 ("the ESP dedups on trip_seq"): the
 * Pico repeats this frame a few times against loss, so every copy is still
 * *applied* (repetition is what makes trip_event_age_ms track the most
 * recent copy actually received), but only a trip_seq that differs from the
 * one already cached is logged as a new event -- otherwise a healthy resend
 * burst would look like three separate trips in the log. */
bool safety_apply_trip_event(SafetyLinkClass *link, const uart_proto_message_t *msg)
{
    if (msg->length != SAFETY_LINK_TRIP_EVENT_FRAME_LEN || msg->payload[0] != SAFETY_CMD_TRIP_EVENT) {
        if (safety_lock(link)) {
            link->stats.frame_errors++;
            safety_unlock(link);
        }
        ESP_LOGW(TAG, "unexpected TRIP_EVENT frame from dev%u/task%u: subcmd 0x%02X, %u bytes",
                 msg->device, msg->task_id, msg->payload[0], msg->length);
        return false;
    }

    const uint8_t *p = msg->payload;
    uint8_t trip_seq = p[1];
    uint8_t trip_reason = p[2];
    float safety_tc_c = safety_read_f32_le(&p[7]);
    float deciding_threshold = safety_read_f32_le(&p[11]);

    if (!safety_lock(link)) {
        return false;
    }
    /* 2026-08-28 audit fix (N3): "new to this boot" is NOT the same claim as
     * "genuinely live" -- an ESP reboot with a trip still latched on the Pico
     * makes the FIRST resend this boot look identical to a real new trip
     * (trip_event_ever_received was false either way), but link->fault_sources
     * at that instant reflects THIS boot's fault lines, not whatever was
     * actually asserted when the trip latched, possibly minutes/boots ago.
     * Only a trip_seq change witnessed while this boot was already tracking a
     * previous one is something this boot actually watched happen live, so
     * only that case may claim the snapshot is trustworthy. See
     * safety_link.h's trip_fault_sources_valid field comment and
     * ui_page_diagnostics.c/dashboard_http.c's "(at trip)" renderers, which
     * must show "not captured" rather than a plausible-looking wrong value
     * when this is false.
     *
     * The actual decision is factored into safety_trip_decision.c (2026-08-28
     * opus review: "no automated test touches this" -- see that file's doc
     * comment for why it lives in its own dependency-free translation unit
     * and firmware/KilnFW/App/test/test_safety_trip_decision.c for the host
     * test that now drives it directly). */
    safety_trip_decision_t trip_decision = safety_trip_decide_event((safety_trip_decision_input_t){
        .trip_event_ever_received_before = link->cached.trip_event_ever_received,
        .cached_trip_last_seq = link->cached.trip_last_seq,
        .incoming_trip_seq = trip_seq,
    });
    bool is_new_event = trip_decision.is_new_event;
    bool is_genuinely_live_event = trip_decision.fault_sources_valid;

    link->cached.trip_last_seq = trip_seq;
    link->cached.trip_reason = trip_reason;
    link->cached.trip_uptime_ms = safety_read_u32_le(&p[3]);
    link->cached.trip_safety_tc_c = safety_tc_c;
    link->cached.trip_deciding_threshold = deciding_threshold;
    link->cached.trip_current_a[0] = safety_read_f32_le(&p[15]);
    link->cached.trip_current_a[1] = safety_read_f32_le(&p[19]);
    link->cached.trip_current_a[2] = safety_read_f32_le(&p[23]);
    link->cached.trip_relay_recent_mask = p[27];
    link->cached.trip_context_age_100ms = p[28];
    /* Snapshot THIS board's own fault_sources at the instant a NEW trip_seq
     * is seen -- see safety_link.h's trip_fault_sources field comment for
     * why SaftyFW cannot supply this itself and why it is only overwritten
     * on a genuinely new event, never on a dedup resend of the same one
     * (that would let heat_block_sources at resend-time silently overwrite
     * what was actually asserted when the trip first latched). */
    if (is_new_event) {
        link->cached.trip_fault_sources = link->fault_sources;
        link->cached.trip_fault_sources_valid = is_genuinely_live_event;
    }
    link->cached.trip_event_ever_received = true;
    link->trip_event_tick = xTaskGetTickCount();
    safety_unlock(link);

    if (is_new_event) {
        ESP_LOGW(TAG, "safety processor TRIPPED: reason 0x%02X, trip_seq %u, safety TC %.1f C "
                      "(threshold %.1f)",
                 (unsigned)trip_reason, (unsigned)trip_seq, (double)safety_tc_c,
                 (double)deciding_threshold);
    }
    return true;
}
