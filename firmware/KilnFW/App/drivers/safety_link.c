#include "safety_link.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"
#include "settings.h"
#include "uart_task_ids.h"

#include "kilnlink/kilnlink_announce.h"
#include "kilnlink/kilnlink_announce_reboot.h"
#include "kilnlink/kilnlink_clear_trip.h"
#include "kilnlink/kilnlink_context.h"
#include "kilnlink/kilnlink_rollback.h"
#include "kilnlink/kilnlink_set_config.h"
#include "kilnlink/kilnlink_version.h"

/* ROADMAP.md M5 -- SAFETY_CMD_PUSH_CONTEXT's live-state sources. safety_link.h
 * only forward-declares these as void* (kiln_io_t is an anonymous-struct
 * typedef, MAX31856BusClass a named one) to keep that header dependency-free;
 * this .c file is where the frame is actually built, so it needs the real
 * types. */
#include "MAX31856.h"
#include "kiln_io.h"
#include "profile_executor.h"
#include "thermo_owner.h"

/* Real build identity (git commit/dirty/build timestamp), generated fresh
 * every build by gen_build_info.cmake into this component's binary dir --
 * see uart_bridge.c's build_fw_version_reply() for the PC-link twin of the
 * payload builder below. Unlike SaftyFW (TODO.md Phase 8: "build_info.h
 * generated on every build... not built this pass"), KilnFW already has
 * this, so the ANNOUNCE_VERSION frame reports real values, not a stub. */
#include "build_info.h"

static const char *TAG = "safety_link";

/* The safety link permanently owns UART1 (settings.h: SAFETY_UART_PORT_NUM).
 * If the PC link is also configured onto UART1 the second uart_driver_install
 * simply fails at runtime, which is a confusing way to discover a
 * menuconfig mistake -- so say it at compile time instead. */
#if CONFIG_KILNCTL_UART_PORT_1
#error "The PC link and the safety link cannot share UART1: set KILNCTL_UART_PORT_0 (see App/drivers/Kconfig)."
#endif

/* Inbox depth. The far side is expected to answer one request at a time; the
 * extra slots absorb an unsolicited push landing next to a poll reply. */
#define SAFETY_INBOX_LEN 4

#define SAFETY_POLL_TASK_STACK    4096
#define SAFETY_POLL_TASK_PRIORITY 5

/* Every fault source this driver recognizes. A caller may only set or clear
 * bits that appear here: an unknown bit set would latch the fault line with no
 * named owner able to release it, and -- far worse -- a wildcard clear such as
 * ~0u would release every *other* source's assertion as a side effect. The
 * whole point of the source mask is that no reason can drop another reason's
 * fault, so the mask itself has to be closed. */
#define SAFETY_FAULT_SRC_ALL                                                       \
    ((uint32_t)(SAFETY_FAULT_SRC_MANUAL | SAFETY_FAULT_SRC_PC_LINK |               \
                SAFETY_FAULT_SRC_THERMO | SAFETY_FAULT_SRC_SAFETY_LINK |           \
                SAFETY_FAULT_SRC_APP))

/* Ceiling on how long a caller waits to start its own request/reply exchange.
 * One exchange is bounded by the ACK timeout times uart_protocol's retries plus
 * the reply timeout (~750 ms with a dead peer), so anything past a few of those
 * means the holder is wedged rather than merely unlucky. Waiting forever here
 * would let a stuck safety link take out whatever task asked it a question --
 * including the bridge task that answers the PC. */
#define SAFETY_XACT_LOCK_TIMEOUT_MS 5000u

/* ANNOUNCE_VERSION is sent BROADCAST (fire-and-forget, per LINK_PROTOCOL.md
 * sec 1/2), so "retry" here means "send it more than once", not "retry an
 * ACK". LINK_PROTOCOL.md sec 4: "repeated a few times against loss" -- same
 * spirit as Frame D (TRIP_EVENT) on the Pico side, "repeated a few times
 * over the next second in case the first copy is lost" (sec 6). Four sends,
 * three 250ms gaps between them, spans ~750ms. */
#define SAFETY_ANNOUNCE_VERSION_REPEATS 4u
#define SAFETY_ANNOUNCE_VERSION_REPEAT_GAP_MS 250u

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

/* The ESP32-S3 is little-endian and so is every multi-byte field in this
 * protocol, so these are memcpy rather than byte assembly -- but they stay
 * functions so the wire layout is still stated once, in one place. */
static float safety_read_f32_le(const uint8_t *bytes)
{
    float value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static uint32_t safety_read_u32_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static uint16_t safety_read_u16_le(const uint8_t *bytes)
{
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static void safety_put_f32_le(uint8_t *out, float value)
{
    memcpy(out, &value, sizeof(value));
}

static void safety_put_u16_le(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void safety_put_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
    out[2] = (uint8_t)((value >> 16) & 0xFFu);
    out[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t safety_elapsed_ms(TickType_t since)
{
    /* Unsigned tick subtraction, so the 32-bit tick counter's eventual wrap
     * (~497 days at the default 100 Hz) produces the right delta rather than
     * a nonsense one. */
    TickType_t delta = xTaskGetTickCount() - since;
    return (uint32_t)delta * portTICK_PERIOD_MS;
}

static inline bool safety_lock(SafetyLinkClass *link)
{
    return xSemaphoreTake(link->state_lock, portMAX_DELAY) == pdTRUE;
}

static inline void safety_unlock(SafetyLinkClass *link)
{
    xSemaphoreGive(link->state_lock);
}

/* ------------------------------------------------------------------------ */
/* Cache / staleness                                                        */
/* ------------------------------------------------------------------------ */

/* Age of the cached status in ms, saturating just below the reserved
 * "never received" value so the two can never be confused. */
static uint16_t safety_age_ms_locked(const SafetyLinkClass *link)
{
    if (!link->ever_received) {
        return SAFETY_LINK_AGE_NEVER;
    }
    uint32_t elapsed = safety_elapsed_ms(link->cached_tick);
    if (elapsed >= SAFETY_LINK_AGE_NEVER) {
        return (uint16_t)(SAFETY_LINK_AGE_NEVER - 1u);
    }
    return (uint16_t)elapsed;
}

/* link_up = a valid reply within SAFETY_LINK_UP_PERIODS poll periods. With
 * polling switched off there is no period to measure against, so the
 * configured default is used -- otherwise turning polling off would make the
 * link look permanently up (nothing ever goes stale) or permanently down
 * (window of zero), and both are lies. */
static bool safety_link_up_locked(const SafetyLinkClass *link)
{
    uint16_t age = safety_age_ms_locked(link);
    if (age == SAFETY_LINK_AGE_NEVER) {
        return false;
    }
    uint32_t base = link->poll_period_ms;
    if (base == 0u) {
        base = (SAFETY_POLL_PERIOD_MS > 0) ? (uint32_t)SAFETY_POLL_PERIOD_MS : 500u;
    }
    return (uint32_t)age <= base * SAFETY_LINK_UP_PERIODS;
}

/* Drives GPIO6 from the current source mask. High = fault asserted = U1's LED
 * lit = the Pico's mainFault input pulled low (docs/HARDWARE.md). Called with
 * state_lock held so the pin and the mask can't disagree. */
static void safety_apply_fault_locked(SafetyLinkClass *link)
{
    gpio_set_level((gpio_num_t)link->fault_io, link->fault_sources != 0u ? 1 : 0);
}

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
static bool safety_link_versions_compatible(uint16_t self_protocol, uint16_t self_min_compatible,
                                             uint16_t peer_protocol, uint16_t peer_min_compatible)
{
    return (peer_protocol >= self_min_compatible) && (self_protocol >= peer_min_compatible);
}

/* commit/datetime must each fit the shared codec's fixed caps (kilnlink_announce.h:
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
static void safety_link_send_announce_version_burst(SafetyLinkClass *link)
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
static bool safety_parse_fw_version(const uint8_t *p, uint8_t len, uint16_t *out_protocol,
                                     uint16_t *out_min_compatible, uint8_t *out_boot_id,
                                     bool *out_have_boot_id)
{
    *out_have_boot_id = false;
    if (len < 5u) {
        return false;
    }
    *out_protocol = (uint16_t)(p[1] | ((uint16_t)p[2] << 8));
    *out_min_compatible = (uint16_t)(p[3] | ((uint16_t)p[4] << 8));

    if (len < 7u) {
        return true; /* no dirty/commit_len byte to even start the tail */
    }
    size_t i = 6; /* byte5 = dirty, not needed here */
    uint8_t commit_len = p[i++];
    if ((size_t)commit_len + i > (size_t)len) {
        return true; /* truncated commit -- the two fields we need are already set */
    }
    i += commit_len;
    if (i >= (size_t)len) {
        return true;
    }
    uint8_t datetime_len = p[i++];
    if ((size_t)datetime_len + i > (size_t)len) {
        return true;
    }
    i += datetime_len;
    if (i >= (size_t)len) {
        return true;
    }
    *out_boot_id = p[i];
    *out_have_boot_id = true;
    return true;
}

/* Applies one Pico FW_VERSION frame: updates the tracked peer-compatibility
 * verdict and boot_id, and re-announces ourselves (a burst, not just one
 * frame -- same loss-tolerance reasoning as the boot push) if the boot_id
 * changed, since a Pico that just rebooted has forgotten who it was talking
 * to (LINK_PROTOCOL.md sec 4). Never touches the isolated fault line
 * directly -- see safety_update_health(), the one place that reads
 * peer_version_known/peer_version_compatible and decides what to do about a
 * mismatch (Phase 7b.5). */
static void safety_apply_fw_version(SafetyLinkClass *link, const uart_proto_message_t *msg)
{
    uint16_t peer_protocol = 0;
    uint16_t peer_min_compatible = 0;
    uint8_t peer_boot_id = 0;
    bool have_boot_id = false;

    if (!safety_parse_fw_version(msg->payload, msg->length, &peer_protocol, &peer_min_compatible,
                                  &peer_boot_id, &have_boot_id)) {
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
    }
    safety_unlock(link);

    if (boot_id_changed) {
        safety_link_send_announce_version_burst(link);
    }
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
static void safety_build_and_send_context(SafetyLinkClass *link)
{
    kiln_io_t *io = (kiln_io_t *)link->context_io;
    MAX31856BusClass *thermo_bus = (MAX31856BusClass *)link->context_thermo_bus;

    uint8_t relay_now_mask = io ? kiln_io_get_relay_shadow(io) : 0u;
    uint8_t relay_recent_mask = safety_context_update_relay_recent(link, relay_now_mask);

    profile_exec_status_t pstat;
    profile_executor_get_status(&pstat);

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

    bool any_zone_faulted = (pstat.state == PROFILE_EXEC_FAULTED);
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

        bool zone_active = pstat.zones[i].active;
        bool zone_relay_on = pstat.zones[i].relay_commanded_on;
        bool zone_faulted = pstat.zones[i].faulted;
        /* profile_executor.c: "each active zone runs its own independent
         * PID/guard/relay against one shared setpoint" -- there is no
         * per-zone setpoint to report, so every currently-active zone
         * reports the one shared target; an inactive zone has no setpoint,
         * same NaN-for-invalid convention as measured_c above. */
        z->setpoint_c = zone_active ? pstat.target_c : NAN;

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

    if (pstat.state == PROFILE_EXEC_RUNNING) {
        ctx.flags |= KILNLINK_CONTEXT_FLAG_PROFILE_RUNNING;
    }
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
static bool safety_apply_update_status(SafetyLinkClass *link, const uart_proto_message_t *msg)
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
static bool safety_apply_status(SafetyLinkClass *link, const uart_proto_message_t *msg)
{
    /* Length before payload[0]: a zero-length frame has no subcommand byte to
     * read, and the || below short-circuits in the right order only if the
     * length test comes first. */
    if (msg->length != SAFETY_LINK_STATUS_FRAME_LEN || msg->payload[0] != SAFETY_CMD_GET_STATUS) {
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
    link->cached.flags = (uint8_t)(p[1] & ~(SAFETY_FLAG_LINK_UP | SAFETY_FLAG_FAULT));
    link->cached.tc_temp_c = safety_read_f32_le(&p[2]);
    link->cached.cj_temp_c = safety_read_f32_le(&p[6]);
    /* The contract says the Pico sends NaN when SAFETY_FLAG_TEMP_VALID is
     * clear, but a peer that sends 0.0 instead -- or a firmware that forgets --
     * must not have it forwarded to the PC as a real reading of a stone-cold
     * kiln. The flag is the authority; enforce it here rather than trusting
     * the far side to have been careful. */
    if (!(link->cached.flags & SAFETY_FLAG_TEMP_VALID)) {
        link->cached.tc_temp_c = NAN;
        link->cached.cj_temp_c = NAN;
    }
    link->cached.tc_fault = p[10];
    link->cached.current_a[0] = safety_read_f32_le(&p[11]);
    link->cached.current_a[1] = safety_read_f32_le(&p[15]);
    link->cached.current_a[2] = safety_read_f32_le(&p[19]);
    link->cached_tick = xTaskGetTickCount();
    link->ever_received = true;
    link->stats.frames_received++;
    safety_unlock(link);
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
 * Mirrors kilnlink_power_decode() in firmware/CommonFW/src/kilnlink_power.c
 * byte-for-byte; see uart_task_ids.h's SAFETY_CMD_POWER comment for why this
 * driver hand-parses rather than linking that codec. */
static bool safety_apply_power(SafetyLinkClass *link, const uart_proto_message_t *msg)
{
    if (msg->length != SAFETY_LINK_POWER_FRAME_LEN || msg->payload[0] != SAFETY_CMD_POWER) {
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
    link->cached.power_ever_received = true;
    safety_unlock(link);
    return true;
}

/* Accepts one SAFETY_CMD_DIAG (Frame B) frame from the Pico and replaces the
 * cached diagnostic snapshot with it. Same discard-rather-than-partially-
 * apply contract as safety_apply_status()/safety_apply_power(): a malformed
 * frame is dropped and counted as a frame error rather than half-applied.
 * Byte layout (26 bytes total, LINK_PROTOCOL.md sec 6):
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
 * Mirrors kilnlink_diag_decode() in firmware/CommonFW/src/kilnlink_diag.c
 * byte-for-byte; see uart_task_ids.h's SAFETY_CMD_DIAG comment for why this
 * driver hand-parses rather than linking that codec. */
static bool safety_apply_diag(SafetyLinkClass *link, const uart_proto_message_t *msg)
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
    link->cached.diag_ever_received = true;
    safety_unlock(link);
    return true;
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
static bool safety_apply_trip_event(SafetyLinkClass *link, const uart_proto_message_t *msg)
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
    bool is_new_event =
        !link->cached.trip_event_ever_received || link->cached.trip_last_seq != trip_seq;

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
 * GET_STATUS payload still is (inside safety_apply_status() itself). */
static bool safety_drain_inbox(SafetyLinkClass *link, uint32_t wait_ms)
{
    uart_proto_message_t msg;
    bool got_status = false;
    TickType_t wait = pdMS_TO_TICKS(wait_ms);

    while (uart_protocol_receive(link->inbox, &msg, wait) == ESP_OK) {
        if (msg.length >= 1) {
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
            default:
                break;
            }
        }
        wait = 0; /* only the first receive is allowed to block */
    }
    return got_status;
}

/* One complete request/reply exchange, serialized against every other one on
 * this link (see SafetyLinkClass::xact_lock).
 *
 * Note what is *not* here: no bespoke framing, retry or CRC logic. Retries,
 * de-duplication and the CRC all belong to uart_protocol, which is the same
 * code the PC link runs -- this function is only the request/reply pairing on
 * top of it. */
static esp_err_t safety_exchange(SafetyLinkClass *link, const uint8_t *request, size_t length,
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

    esp_err_t err = uart_protocol_send(&link->proto, UART_PROTO_DEVICE_SAFETY, UART_TASK_ID_SAFETY,
                                        UART_TASK_ID_SAFETY, request, length,
                                        SAFETY_LINK_ACK_TIMEOUT_MS);
    if (err != ESP_OK) {
        if (safety_lock(link)) {
            link->stats.timeouts++;
            safety_unlock(link);
        }
        /* NOT logged here: with no Pico attached this fires on every single
         * poll. The poll task logs the *state* (link down) at most once per
         * SAFETY_LINK_DOWN_LOG_PERIOD_MS instead -- see safety_update_health. */
        xSemaphoreGive(link->xact_lock);
        return err;
    }

    if (expect_status) {
        if (!safety_drain_inbox(link, SAFETY_LINK_REPLY_TIMEOUT_MS)) {
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

/* ------------------------------------------------------------------------ */
/* Poll task                                                                */
/* ------------------------------------------------------------------------ */

/* Rate-limited link-state logging plus the one fault source this driver
 * raises on its own. Only ever called from the poll task, which is why
 * down_logged/version_mismatch_logged/down_log_tick need no locking.
 *
 * Phase 7b.5 (LINK_PROTOCOL.md sec 4, "What each side does about a
 * mismatch"): "The ESP: treats it exactly like a dead link --
 * SAFETY_FAULT_SRC_SAFETY_LINK asserts, every heater-on is blocked, a
 * running firing aborts." Rather than inventing a second fault source, a
 * known incompatible peer is folded into the same assert-if-any-reason
 * calculation as link staleness, and both are gated by the *same*
 * fault_on_link_loss policy switch -- "exactly like a dead link" reads as
 * "governed the same way a dead link is," including the bench override
 * (safety_link_fault_on_link_loss(link, false)) that already exists for
 * boards with no Pico fitted. */
static void safety_update_health(SafetyLinkClass *link)
{
    bool up = false;
    bool policy = false;
    bool version_mismatch = false;
    bool update_in_progress = false;
    uint16_t age = SAFETY_LINK_AGE_NEVER;

    if (safety_lock(link)) {
        up = safety_link_up_locked(link);
        age = safety_age_ms_locked(link);
        policy = link->fault_on_link_loss;
        version_mismatch = link->peer_version_known && !link->peer_version_compatible;
        update_in_progress = link->update_in_progress_quiet;
        safety_unlock(link);
    }

    /* LINK_PROTOCOL.md sec 8 / ROADMAP.md M6: the fault is "no telemetry
     * within 1.5 s", a fixed ceiling -- OR it into `up` rather than replacing
     * it, so a reconfigured poll period can only make the fault fire
     * *sooner* than the period-relative check, never later. At the default
     * 500 ms period the two agree and this is a no-op. */
    if (safety_link_is_stale(age, SAFETY_LINK_STALE_MS)) {
        up = false;
    }

    if (up) {
        if (link->down_logged) {
            ESP_LOGI(TAG, "safety processor link is up again");
            link->down_logged = false;
        }
    } else if (!link->down_logged ||
               safety_elapsed_ms(link->down_log_tick) >= SAFETY_LINK_DOWN_LOG_PERIOD_MS) {
        /* TODO.md 9.6: text only -- the fault bit below still asserts
         * unconditionally on `!up`, exactly as it always has. */
        if (update_in_progress) {
            ESP_LOGI(TAG, "safety processor link quiet -- Pico update relaying (expected)");
        } else if (age == SAFETY_LINK_AGE_NEVER) {
            ESP_LOGW(TAG, "no reply from the safety processor (never seen one). Expected while "
                          "the RP2040 firmware does not exist; status reports link_up=0.");
        } else {
            ESP_LOGW(TAG, "safety processor link is down (last status %u ms ago)", age);
        }
        link->down_logged = true;
        link->down_log_tick = xTaskGetTickCount();
    }

    if (version_mismatch != link->version_mismatch_logged) {
        if (version_mismatch) {
            ESP_LOGE(TAG, "safety processor protocol version incompatible -- treating link as "
                          "down (Phase 7b.5, LINK_PROTOCOL.md sec 4)");
        } else {
            ESP_LOGI(TAG, "safety processor protocol version now compatible");
        }
        link->version_mismatch_logged = version_mismatch;
    }

    if (policy) {
        safety_link_set_fault_source(link, SAFETY_FAULT_SRC_SAFETY_LINK, !up || version_mismatch);
    }
}

static void safety_poll_task(void *arg)
{
    SafetyLinkClass *link = (SafetyLinkClass *)arg;
    const uint8_t request[] = { SAFETY_CMD_GET_STATUS };
    /* ROADMAP.md M6 "Boot-time version request with retry" -- distinct from
     * ANNOUNCE_VERSION above (that's the ESP telling the Pico who it is,
     * unprompted). This is the ESP asking the Pico who IT is. A Pico that
     * was already running before this boot has no reason to volunteer
     * FW_VERSION again on its own (LINK_PROTOCOL.md sec 4: unsolicited only
     * "at Pico boot and on request") -- without an explicit request, a
     * same-boot-cycle Pico's version would never be learned at all. */
    const uint8_t fw_version_request[] = { SAFETY_CMD_FW_VERSION };

    /* Boot push, unsolicited, before entering the steady loop -- mirrors
     * SaftyFW's link_task_fn's own FW_VERSION boot push (Phase 7b.2,
     * LINK_PROTOCOL.md sec 4: "Sent as BROADCAST at ESP boot, repeated a few
     * times against loss"). This is what lets a Pico that boots *after* the
     * ESP still learn our version promptly instead of waiting for its own
     * boot_id to first appear on a FW_VERSION frame we have to receive. */
    safety_link_send_announce_version_burst(link);

    while (true) {
        uint16_t period = 0;
        if (safety_lock(link)) {
            period = link->poll_period_ms;
            safety_unlock(link);
        }

        if (period == 0u) {
            /* Polling off: still drain anything the peer pushes unsolicited,
             * so an enabled-but-unpolled link isn't blind. */
            (void)safety_drain_inbox(link, SAFETY_LINK_IDLE_TICK_MS);
            continue;
        }

        TickType_t started = xTaskGetTickCount();
        esp_err_t poll_err = safety_exchange(link, request, sizeof(request), true);

        /* SAFETY_LINK_BACKOFF_MAX_STREAK's comment: streak resets to 0 the
         * instant any exchange succeeds -- a Pico that comes online later is
         * back to full normal cadence on its very first reply, no reboot
         * needed. */
        if (poll_err == ESP_OK) {
            link->no_reply_streak = 0;
        } else if (link->no_reply_streak < SAFETY_LINK_BACKOFF_MAX_STREAK) {
            link->no_reply_streak++;
        }

        bool peer_version_known = false;
        if (safety_lock(link)) {
            peer_version_known = link->peer_version_known;
            safety_unlock(link);
        }
        if (!peer_version_known) {
            /* expect_status=false, same as the REQUEST_ENABLE-style calls
             * this parameter already exists for (safety_exchange's own doc
             * comment): folds whatever reply lands within
             * SAFETY_LINK_ACK_TIMEOUT_MS into the cache via safety_drain_inbox
             * -> safety_apply_fw_version, without the GET_STATUS-specific
             * got_status bookkeeping mislabeling a successful FW_VERSION
             * reply as a timeout. A silent Pico (not yet built/attached, per
             * this repo's current bench state) means this simply repeats
             * every poll period for as long as the version stays unknown --
             * that repetition at a bounded, already-existing cadence IS the
             * "retry" this roadmap item asks for, not a separate backoff
             * scheme. */
            (void)safety_exchange(link, fw_version_request, sizeof(fw_version_request), false);
        }

        safety_update_health(link);
        /* ROADMAP.md M5: SAFETY_CMD_PUSH_CONTEXT, same cadence as the
         * GET_STATUS poll above -- LINK_PROTOCOL.md sec 4's "every
         * CONFIG_KILNCTL_SAFETY_POLL_PERIOD_MS". Independent of whether the
         * exchange above got a reply: this is a broadcast, not part of that
         * request/reply pairing. */
        safety_build_and_send_context(link);

        /* Measure the sleep from the start of the attempt, so the poll rate
         * stays at the requested period rather than period + however long a
         * dead peer took to time out. Floored so a period shorter than the
         * exchange itself still yields. */
        uint32_t spent = safety_elapsed_ms(started);
        uint32_t sleep_ms = (spent >= period) ? SAFETY_LINK_MIN_POLL_GAP_MS : (period - spent);
        if (sleep_ms < SAFETY_LINK_MIN_POLL_GAP_MS) {
            sleep_ms = SAFETY_LINK_MIN_POLL_GAP_MS;
        }
        /* SAFETY_LINK_BACKOFF_MAX_STREAK's comment: extra sleep on top of the
         * requested period while nothing has answered recently, capped and
         * reset on the next reply -- poll_period_ms itself (what
         * GET_LINK_STATS reports) is untouched, only how long this
         * particular iteration sleeps. */
        if (link->no_reply_streak > 0u) {
            uint32_t backoff_extra = (uint32_t)period * ((1u << (link->no_reply_streak - 1u)) - 1u);
            if (backoff_extra > SAFETY_LINK_BACKOFF_MAX_EXTRA_MS) {
                backoff_extra = SAFETY_LINK_BACKOFF_MAX_EXTRA_MS;
            }
            sleep_ms += backoff_extra;
        }
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}

/* ------------------------------------------------------------------------ */
/* Bring-up                                                                 */
/* ------------------------------------------------------------------------ */

esp_err_t safety_link_start(SafetyLinkClass *link)
{
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (link->initialized) {
        return ESP_OK;
    }

    /* Validated before it is shifted into pin_bit_mask below: a bad Kconfig
     * value would otherwise be a shift past the width of the type, and the
     * fault line is the last thing on this board that should be driven by
     * accident. */
    if (!GPIO_IS_VALID_OUTPUT_GPIO(SAFETY_FAULT_IO)) {
        ESP_LOGE(TAG, "SAFETY_FAULT_IO (%d) is not a usable output", (int)SAFETY_FAULT_IO);
        return ESP_ERR_INVALID_ARG;
    }

    memset(link, 0, sizeof(*link));
    link->fault_io = SAFETY_FAULT_IO;
    link->poll_period_ms = (uint16_t)SAFETY_POLL_PERIOD_MS;
    link->fault_on_link_loss = true; /* fail-safe; see safety_link.h */
    /* Diagnostic identity only (Phase 7b.2), same spirit as SaftyFW's own
     * s_boot_id (link_task.c: "not a security or safety value, so true
     * entropy is not required") -- but the ESP has a real hardware RNG
     * (esp_random(), backed by the SAR ADC/RF noise per esp_random.h), so
     * there is no reason to fall back to a time-derived pseudo-random value
     * the way the Pico does. Lets the Pico notice "the ESP just rebooted"
     * from ANNOUNCE_VERSION alone, without polling for it. */
    link->esp_boot_id = (uint8_t)esp_random();
    /* No reading has ever arrived, and NaN is the only honest value for that.
     * Zero would read as a stone-cold kiln, which is exactly the wrong
     * direction to be wrong in. */
    link->cached.tc_temp_c = NAN;
    link->cached.cj_temp_c = NAN;
    link->cached.current_a[0] = NAN;
    link->cached.current_a[1] = NAN;
    link->cached.current_a[2] = NAN;
    /* SAFETY_CMD_POWER (Frame E) -- same "NaN, not 0, until a real reading
     * arrives" reasoning as the temperatures above. */
    link->cached.power_total_w = NAN;
    link->cached.power_mains_voltage_v = NAN;
    for (unsigned ch = 0; ch < SAFETY_LINK_POWER_CHANNELS; ch++) {
        link->cached.power_channel_w[ch] = NAN;
        link->cached.power_channel_i_conducting_a[ch] = NAN;
        link->cached.power_channel_conduction_fraction[ch] = NAN;
    }
    /* SAFETY_CMD_TRIP_EVENT (Frame D) -- same NaN-until-real-reading
     * reasoning; gated behind trip_event_ever_received either way, but a
     * caller that forgets to check it sees NaN, not a plausible-looking 0. */
    link->cached.trip_safety_tc_c = NAN;
    link->cached.trip_deciding_threshold = NAN;
    for (unsigned ch = 0; ch < SAFETY_LINK_TRIP_EVENT_CHANNELS; ch++) {
        link->cached.trip_current_a[ch] = NAN;
    }
    link->cached.diag_context_age_100ms = (uint8_t)SAFETY_LINK_DIAG_CONTEXT_AGE_NEVER;

    /* Declared up here, not at first use: every failure below lands on the
     * shared cleanup labels, which report it. */
    esp_err_t err = ESP_ERR_NO_MEM;

    link->state_lock = xSemaphoreCreateMutex();
    link->xact_lock = xSemaphoreCreateMutex();
    if (!link->state_lock || !link->xact_lock) {
        ESP_LOGE(TAG, "failed to create link mutexes");
        goto fail_locks;
    }

    /* The fault line first, and de-asserted, before anything else can fail:
     * the pin powers up as a floating input, and a floating gate on U1 is an
     * undefined fault state at the safety processor. Driving it low (LED off,
     * Pico's mainFault released) is the known-good starting point, and it is
     * the poll task's job -- not bring-up's -- to raise it. */
    gpio_config_t fault_cfg = {
        .pin_bit_mask = 1ULL << (uint32_t)link->fault_io,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&fault_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fault line gpio_config(%d) failed: %s", link->fault_io, esp_err_to_name(err));
        goto fail_locks;
    }
    gpio_set_level((gpio_num_t)link->fault_io, 0);

    err = uart_owner_init(&link->owner, SAFETY_UART_PORT_NUM, SAFETY_TX_IO, SAFETY_RX_IO,
                           SAFETY_UART_BAUD_RATE, UART_OWNER_QUEUE_LEN, UART_OWNER_TASK_PRIORITY,
                           UART_OWNER_STACK_SIZE, tskNO_AFFINITY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_owner_init(uart%d) failed: %s", SAFETY_UART_PORT_NUM, esp_err_to_name(err));
        goto fail_locks;
    }

    /* The one thing that makes this link different from the PC link. Each
     * TCMT1109 inverts: the driver's high lights the LED, which pulls the
     * receiver's collector low, so an idle-high UART line arrives idle-low in
     * both directions. Inverting both signals in the UART peripheral puts the
     * bits back the right way up for free; doing it in software would mean
     * hand-decoding the line. Must be after uart_param_config (inside
     * uart_owner_init), which rewrites the same register block.
     *
     * This also makes the barrier transparent to the far end: TXD_INV and U2
     * are two inversions in series, so the Pico's RX sees ordinary polarity,
     * and U3's inversion of the Pico's ordinary TX is undone by RXD_INV. The
     * RP2040 therefore needs no PIO UART and no external inverter -- exactly
     * one end inverts, and it is this one. */
    err = uart_set_line_inverse(SAFETY_UART_PORT_NUM, UART_SIGNAL_TXD_INV | UART_SIGNAL_RXD_INV);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_line_inverse failed: %s", esp_err_to_name(err));
        goto fail_owner;
    }

    /* GPIO5 is the bare collector of U3, on net DataFromSafty -- R15's 1k
     * pull-up to 3.3V_Main is already fitted on this net, and nothing else
     * sits on it. With the phototransistor off the pin would float, so the
     * internal pull-up is what
     * defines the LED-off level (high at the pad = low after RXD_INV = the
     * space/break level). uart_set_pin already asks for this, but it is
     * restated because it is load-bearing rather than incidental: without it
     * the link doesn't merely get noisy, it has no defined idle at all. */
    err = gpio_set_pull_mode((gpio_num_t)SAFETY_RX_IO, GPIO_PULLUP_ONLY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rx pull-up on gpio%d failed: %s", SAFETY_RX_IO, esp_err_to_name(err));
        goto fail_owner;
    }

    err = uart_protocol_init(&link->proto, &link->owner, UART_PROTO_DEVICE_ESP,
                              UART_PROTOCOL_TASK_PRIORITY, UART_PROTOCOL_STACK_SIZE, tskNO_AFFINITY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_protocol_init(uart%d) failed: %s", SAFETY_UART_PORT_NUM,
                 esp_err_to_name(err));
        goto fail_owner;
    }

    /* Registered on *this* protocol instance (the isolated link), not on the
     * PC link's -- same task_id, two separate address spaces. This is the
     * inbox the Pico's replies land in. */
    err = uart_protocol_register_task(&link->proto, UART_TASK_ID_SAFETY, SAFETY_INBOX_LEN,
                                       &link->inbox);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register safety task failed: %s", esp_err_to_name(err));
        goto fail_proto;
    }

    /* Set before the task exists, not after: the poll task calls back into the
     * public API (safety_link_set_fault_source), which refuses to touch an
     * uninitialized link -- so the flag has to be true by the time it runs its
     * first iteration, not merely by the time start() returns. */
    link->initialized = true;
    if (xTaskCreatePinnedToCore(safety_poll_task, "safety_poll", SAFETY_POLL_TASK_STACK, link,
                                 SAFETY_POLL_TASK_PRIORITY, &link->poll_task,
                                 tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "failed to create safety poll task");
        link->initialized = false;
        err = ESP_ERR_NO_MEM;
        goto fail_task;
    }

    ESP_LOGI(TAG, "safety link up on uart%d (tx=%d rx=%d, inverted), fault out=gpio%d, poll=%ums",
             SAFETY_UART_PORT_NUM, SAFETY_TX_IO, SAFETY_RX_IO, link->fault_io,
             link->poll_period_ms);
    return ESP_OK;

fail_task:
    uart_protocol_unregister_task(&link->proto, UART_TASK_ID_SAFETY);
fail_proto:
    uart_protocol_deinit(&link->proto);
fail_owner:
    uart_owner_deinit(&link->owner);
fail_locks:
    if (link->state_lock) {
        vSemaphoreDelete(link->state_lock);
        link->state_lock = NULL;
    }
    if (link->xact_lock) {
        vSemaphoreDelete(link->xact_lock);
        link->xact_lock = NULL;
    }
    return (err != ESP_OK) ? err : ESP_ERR_NO_MEM;
}

esp_err_t safety_link_stop(SafetyLinkClass *link)
{
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Take BOTH locks before deleting the poll task, in the documented order
     * (xact outside state), so it can only be killed between exchanges *and*
     * outside safety_update_health's state-lock section. Taking only xact_lock
     * left the other window open: the task could be deleted while holding
     * state_lock, and the vSemaphoreDelete below would then destroy a mutex
     * that is still held -- after which every remaining caller blocks forever.
     * `initialized` is cleared under the locks so late callers bounce off the
     * state check instead of racing the teardown. */
    xSemaphoreTake(link->xact_lock, portMAX_DELAY);
    xSemaphoreTake(link->state_lock, portMAX_DELAY);
    link->initialized = false;
    if (link->poll_task) {
        vTaskDelete(link->poll_task);
        link->poll_task = NULL;
    }
    xSemaphoreGive(link->state_lock);
    xSemaphoreGive(link->xact_lock);

    uart_protocol_unregister_task(&link->proto, UART_TASK_ID_SAFETY);
    uart_protocol_deinit(&link->proto);
    uart_owner_deinit(&link->owner);

    /* The fault line is deliberately left in whatever state it was in. Tearing
     * the link down is not evidence that the controller is healthy, and
     * releasing the safety processor's fault input on the way out would be the
     * opposite of fail-safe. */
    vSemaphoreDelete(link->state_lock);
    vSemaphoreDelete(link->xact_lock);
    link->state_lock = NULL;
    link->xact_lock = NULL;
    link->inbox = NULL;
    link->initialized = false;
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

esp_err_t safety_link_get_status(SafetyLinkClass *link, safety_link_status_t *out)
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
    *out = link->cached;
    out->age_ms = safety_age_ms_locked(link);
    out->link_up = safety_link_up_locked(link);
    out->fault_asserted = (link->fault_sources != 0u);
    /* trip_event_age_ms, same "age computed on read" contract as age_ms
     * above -- meaningless (and left at whatever safety_elapsed_ms(0) works
     * out to) until trip_event_ever_received is true. */
    out->trip_event_age_ms =
        out->trip_event_ever_received ? safety_elapsed_ms(link->trip_event_tick) : 0u;
    safety_unlock(link);
    return ESP_OK;
}

esp_err_t safety_link_request_enable(SafetyLinkClass *link, bool enable)
{
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t request[] = { SAFETY_CMD_REQUEST_ENABLE, (uint8_t)(enable ? 1u : 0u) };
    return safety_exchange(link, request, sizeof(request), false);
}

esp_err_t safety_link_ping(SafetyLinkClass *link)
{
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t request[] = { SAFETY_CMD_GET_STATUS };
    return safety_exchange(link, request, sizeof(request), true);
}

esp_err_t safety_link_set_poll_period(SafetyLinkClass *link, uint16_t period_ms)
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
    link->poll_period_ms = period_ms;
    safety_unlock(link);
    ESP_LOGI(TAG, "poll period set to %u ms%s", period_ms, period_ms == 0 ? " (polling off)" : "");
    return ESP_OK;
}

esp_err_t safety_link_get_stats(SafetyLinkClass *link, safety_link_stats_t *out)
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
    *out = link->stats;
    out->poll_period_ms = link->poll_period_ms;
    safety_unlock(link);
    /* uart_owner counts the physical line errors (break/parity/frame and ring
     * overflows) for this port; they are the same class of problem as a
     * payload this driver had to reject, so the PC sees one number. Read
     * outside the lock -- it's a plain volatile counter owned by the UART
     * event task. */
    out->frame_errors += uart_owner_get_rx_error_count(&link->owner);
    return ESP_OK;
}

esp_err_t safety_link_get_peer_version_status(SafetyLinkClass *link, bool *out_known,
                                               bool *out_compatible,
                                               uint16_t *out_peer_protocol,
                                               uint16_t *out_peer_min_compatible)
{
    if (!link || !out_known || !out_compatible) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!safety_lock(link)) {
        return ESP_FAIL;
    }
    *out_known = link->peer_version_known;
    *out_compatible = link->peer_version_compatible;
    if (out_peer_protocol) {
        *out_peer_protocol = link->peer_protocol_version;
    }
    if (out_peer_min_compatible) {
        *out_peer_min_compatible = link->peer_min_compatible;
    }
    safety_unlock(link);
    return ESP_OK;
}

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

    kilnlink_clear_trip_t msg = { .trip_mask = trip_mask };
    uint8_t payload[KILNLINK_CLEAR_TRIP_LEN];
    kilnlink_clear_trip_status_t status = KILNLINK_CLEAR_TRIP_OK;
    size_t len = kilnlink_clear_trip_encode(&msg, payload, sizeof(payload), &status);
    if (len == 0) {
        ESP_LOGE(TAG, "clear_trip: encode failed (status=%d)", (int)status);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "clear_trip: sending, trip_mask=0x%04X", trip_mask);
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

esp_err_t safety_link_set_fault_source(SafetyLinkClass *link, uint32_t source_mask,
                                        bool assert_fault)
{
    if (!link || source_mask == 0u || (source_mask & ~SAFETY_FAULT_SRC_ALL) != 0u) {
        /* An unrecognized bit is refused outright rather than masked down to
         * the known ones: silently narrowing a clear request would release
         * sources the caller never named. */
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!safety_lock(link)) {
        return ESP_FAIL;
    }
    uint32_t before = link->fault_sources;
    if (assert_fault) {
        link->fault_sources |= source_mask;
    } else {
        link->fault_sources &= ~source_mask;
    }
    uint32_t after = link->fault_sources;
    safety_apply_fault_locked(link);
    safety_unlock(link);

    if (after != before) {
        /* Logged on change only: this is called from the poll task on every
         * iteration once the link-loss policy is active, so logging every call
         * would be one line per poll. */
        ESP_LOGW(TAG, "isolated fault line %s (sources 0x%02X -> 0x%02X)",
                 (after != 0u) ? "ASSERTED" : "released", (unsigned)before, (unsigned)after);
    }
    return ESP_OK;
}

esp_err_t safety_link_set_fault(SafetyLinkClass *link, bool assert_fault)
{
    return safety_link_set_fault_source(link, SAFETY_FAULT_SRC_MANUAL, assert_fault);
}

bool safety_link_get_fault(SafetyLinkClass *link)
{
    if (!link || !link->initialized) {
        return false;
    }
    if (!safety_lock(link)) {
        return false;
    }
    bool asserted = (link->fault_sources != 0u);
    safety_unlock(link);
    return asserted;
}

uint32_t safety_link_get_fault_sources(SafetyLinkClass *link)
{
    if (!link || !link->initialized) {
        return 0u;
    }
    if (!safety_lock(link)) {
        return 0u;
    }
    uint32_t sources = link->fault_sources;
    safety_unlock(link);
    return sources;
}

esp_err_t safety_link_fault_on_link_loss(SafetyLinkClass *link, bool enable)
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
    link->fault_on_link_loss = enable;
    safety_unlock(link);

    if (!enable) {
        /* Drop the source this policy owns; leaving it latched would make
         * "stop asserting on link loss" a no-op until something else happened
         * to clear it. Other sources are untouched. */
        safety_link_set_fault_source(link, SAFETY_FAULT_SRC_SAFETY_LINK, false);
        ESP_LOGW(TAG, "fault-on-link-loss DISABLED: a dead safety link will no longer assert "
                      "the isolated fault line");
    } else {
        ESP_LOGI(TAG, "fault-on-link-loss enabled (default, fail-safe)");
    }
    return ESP_OK;
}

esp_err_t safety_link_set_update_in_progress(SafetyLinkClass *link, bool in_progress)
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
    link->update_in_progress_quiet = in_progress;
    safety_unlock(link);
    return ESP_OK;
}

bool safety_link_get_fault_on_link_loss(SafetyLinkClass *link)
{
    if (!link || !link->initialized) {
        return false;
    }
    if (!safety_lock(link)) {
        return false;
    }
    bool enabled = link->fault_on_link_loss;
    safety_unlock(link);
    return enabled;
}

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
