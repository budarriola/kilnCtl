#include "uart_bridge.h"

#include <math.h>
#include <string.h>

#include "build_info.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/idf_additions.h"
#include "factory_reset.h"
#include "watchdog_cfg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "panel_spi.h" /* ILI9488_PANEL_WIDTH/HEIGHT -- TOUCH_CMD_INJECT bounds check */
#include "SX1509.h"
#include "danger_mode.h" /* danger_mode_active() -- link-loss watchdog suppression */
#include "heat_interlock.h" /* HEAT_INTERLOCK_REASON_MAX -- IO_CMD_SET_RELAY[_MASK]'s ERR_UPDATING case */
#include "kiln_io.h"
#include "kiln_io_owner.h"
#include "kilnlink/kilnlink_set_ct_cal.h"
#include "kiln_ui.h"
#include "lvgl_port.h"
#include "ota_http.h" /* ota_http_heat_blocked_by_update() -- same ERR_UPDATING case */
#include "relay_authority.h"
#include "settings.h"
#include "stack_margin.h"
#include "thermo_owner.h"
#include "uart_task_ids.h"
#include "wifi_prov.h"
#include "zones_http.h"
#include "uart_bridge_internal.h"

static const char *TAG = "uart_bridge";

/* --------------------------------------------------------------------------
 * Little-endian payload helpers. Every multi-byte field in this protocol is
 * little-endian on the wire (see uart_task_ids.h); the SX1509's own registers
 * are big-endian pairs, but that conversion lives inside SX1509.c and never
 * leaks up here.
 * ------------------------------------------------------------------------ */

uint16_t bridge_u16_le(const uint8_t *bytes)
{
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

void bridge_put_u16_le(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
}

void bridge_put_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
    out[2] = (uint8_t)((value >> 16) & 0xFFu);
    out[3] = (uint8_t)((value >> 24) & 0xFFu);
}

float bridge_f32_le(const uint8_t *bytes)
{
    float value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

void bridge_put_f32_le(uint8_t *out, float value)
{
    memcpy(out, &value, sizeof(value));
}

/* Appends a length-prefixed ASCII string to `out` at `o`, capping to what's
 * left of `cap`; truncates (never overruns) rather than asserting or
 * dropping the reply. Same shape as uart_bridge_ext.c's bx_put_lstring() --
 * duplicated rather than shared because that one is file-static there and
 * this file has no common header the two could both include without a
 * larger refactor neither needs today. */
size_t bridge_put_lstring(uint8_t *out, size_t cap, size_t o, const char *text)
{
    if (o >= cap) {
        return o;
    }
    size_t room = cap - o - 1; /* -1 for the length byte itself */
    size_t len = text ? strlen(text) : 0;
    if (len > room) {
        len = room;
    }
    if (len > 255) {
        len = 255;
    }
    out[o++] = (uint8_t)len;
    if (len > 0) {
        memcpy(&out[o], text, len);
        o += len;
    }
    return o;
}

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
 * subcommand byte itself.
 *
 * uart_proto_message_t::payload is a fixed 128-byte buffer that the protocol
 * layer only fills to ::length; the bytes past that are whatever the previous
 * frame left in this task's copy. Parsing them would not read out of bounds --
 * it would read stale, attacker-chosen-last-time data and act on it. On this
 * board that is a live hazard rather than a cosmetic one: a SET_RELAY truncated
 * to two bytes would take its on/off flag from the previous frame and could
 * energize a heating element nobody asked for. */
bool bridge_args_ok(const char *who, const uart_proto_message_t *msg, size_t need)
{
    if ((size_t)msg->length >= need) {
        return true;
    }
    ESP_LOGW(TAG, "%s: subcmd 0x%02X truncated (%u byte payload, needs %u) -- rejected", who,
             msg->payload[0], (unsigned)msg->length, (unsigned)need);
    return false;
}

/* Inclusive range gate for the index arguments -- channel 0-2, relay 1-4,
 * io 1-7, expander pin 0-15 and friends.
 *
 * The drivers underneath all range-check their own arguments too, and that
 * stays the last line of defence. This one exists in front of them because a
 * rejection here names the field and the value in the log, where the driver's
 * bare ESP_ERR_INVALID_ARG cannot distinguish "the host asked for relay 9"
 * from "the expander is not responding" -- and on a kiln those two want very
 * different reactions from whoever is reading the log. */
bool bridge_range_ok(const char *who, uint8_t subcmd, const char *what, uint32_t value,
                            uint32_t lo, uint32_t hi)
{
    if (value >= lo && value <= hi) {
        return true;
    }
    ESP_LOGW(TAG, "%s: subcmd 0x%02X %s=%u out of range %u-%u -- rejected", who, subcmd, what,
             (unsigned)value, (unsigned)lo, (unsigned)hi);
    return false;
}

/* Clamp an auto-report period to something a bridge task can actually serve.
 * See BRIDGE_AUTO_REPORT_MIN_MS -- 0 is preserved exactly, because 0 is "off"
 * on the wire and must not become "as fast as possible". */
uint16_t bridge_clamp_auto_period(const char *who, uint16_t period_ms)
{
    if (period_ms != 0 && period_ms < BRIDGE_AUTO_REPORT_MIN_MS) {
        ESP_LOGW(TAG, "%s: auto-report period %ums raised to the %ums floor", who, period_ms,
                 (unsigned)BRIDGE_AUTO_REPORT_MIN_MS);
        return (uint16_t)BRIDGE_AUTO_REPORT_MIN_MS;
    }
    return period_ms;
}

/* --------------------------------------------------------------------------
 * PC link liveness.
 *
 * "The link is up" here means: a frame from the host was accepted, or a frame
 * to the host was ACKed by it, within the last UART_BRIDGE_LINK_TIMEOUT_MS.
 * Both halves matter -- a host that only listens to auto-reports sends no
 * commands, but its ACKs still prove it is there. Recorded from every bridge
 * task; acted on by the watchdog at the bottom of this file.
 *
 * Written from several tasks and read from one; a TickType_t store is atomic
 * on this target and the value is a timestamp whose worst case under a race is
 * being one check period stale, so no lock is taken. Taking one would put a
 * mutex in the path of every frame for no benefit. */
static volatile TickType_t s_link_last_activity;
static volatile bool s_link_ever_seen;

void bridge_note_link_activity(void)
{
    s_link_last_activity = xTaskGetTickCount();
    s_link_ever_seen = true;
}

/* Answers whoever asked, rather than a hardcoded destination: the requester's
 * address is carried in the inbound message, so a bridge never needs to know
 * who is on the other end of the link. */
void bridge_reply(uart_protocol_t *proto, const uart_proto_message_t *msg, uint8_t src_task,
                         const uint8_t *reply, size_t reply_len)
{
    esp_err_t err = uart_protocol_send(proto, msg->device, msg->task_id, src_task, reply, reply_len,
                                       BRIDGE_REPLY_ACK_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "task%u reply (cmd 0x%02X) to dev%u/task%u failed: %s", src_task, reply[0],
                 msg->device, msg->task_id, esp_err_to_name(err));
        return;
    }
    /* An ACK is proof the host is still listening, which is exactly what the
     * link watchdog wants to know -- see bridge_note_link_activity(). */
    bridge_note_link_activity();
}

/* Sent from a bridge task's default: case, or from any guard below that
 * refuses a *recognized* subcommand -- a truncated frame, an out-of-range
 * argument, a relay refused for ownership/safety/OTA reasons, or a driver
 * call that failed outright. uart_protocol.c's handle_raw_frame() ACKs the
 * frame at the transport layer the instant it lands in this task's inbox,
 * *before* the switch statement below ever runs -- that ACK only proves
 * delivery, not that the command did anything. Falling through with no
 * reply left "delivered" and "succeeded" indistinguishable to a host that
 * only checks the transport ACK, which is exactly how SET_CT_CAL (commit
 * 5fb6928) got ACKed and silently discarded: safety_bridge_task() simply had
 * no case for it -- and it is exactly how a relay refused for a safety
 * reason (KILN_IO_OWNER_RELAY_ERR_SAFETY et al.) looked identical to a relay
 * that actually switched, right up until 2026-08-24.
 *
 * Echoes the subcmd byte back with an explicit ok=0 -- the same {subcmd, ok}
 * shape uart_bridge_ext.c's bx_reply_ok_err() already uses for known-but-
 * refused commands there, so a PC client that already understands that
 * convention needs no new parser to recognize this as a rejection. `reason`
 * is optional in the sense that a NULL/empty one still produces a valid
 * 2-byte {subcmd, 0} rejection -- but NOTHING in this file passes NULL any
 * more, and nothing should: that shape collides with the honest empty-success
 * reply of THERMO_CMD_READ_FAULTS and IO_CMD_SX_SCAN, which is why the
 * default: cases now pass "unsupported" (see bridge_reply_unsupported()
 * below). Always give a reason. A non-NULL
 * reason is appended as a length-prefixed ASCII string -- same encoding
 * bx_put_lstring() uses in uart_bridge_ext.c -- truncated rather than
 * overrunning `reply` on the (never expected in practice) chance a caller
 * hands this a reason longer than the payload has room for. A PC client that
 * does not read this reply is unaffected either way: it still only sees the
 * transport ACK, exactly as before this change. See uart_task_ids.h and
 * tools/PcTools' TODO for which callers need updating to actually look for
 * it. */
void bridge_reply_reject(uart_protocol_t *proto, const uart_proto_message_t *msg,
                                uint8_t src_task, uint8_t subcmd, const char *reason)
{
    uint8_t reply[BRIDGE_REPLY_MAX];
    size_t o = 0;
    reply[o++] = subcmd;
    reply[o++] = 0; /* ok = 0 */
    if (reason && reason[0] != '\0') {
        size_t room = sizeof(reply) - o - 1; /* -1 for the length byte itself */
        size_t len = strlen(reason);
        if (len > room) {
            len = room;
        }
        reply[o++] = (uint8_t)len;
        memcpy(&reply[o], reason, len);
        o += len;
    }
    bridge_reply(proto, msg, src_task, reply, o);
}

/* Thin wrapper kept for the default: cases below -- an unrecognized subcmd
 * has no more specific reason to give than "unsupported".
 *
 * Passes a real reason string rather than NULL (TODO.md section 11): two
 * query replies in this file echo their subcmd byte with byte[1] as a count
 * that can legitimately be 0 -- THERMO_CMD_READ_FAULTS and IO_CMD_SX_SCAN --
 * so their honest empty-success reply, {subcmd, 0}, was exactly 2 bytes,
 * byte-identical to this function's old NULL-reason output. A reasoned
 * reply is always longer (subcmd + ok=0 + len + at least one reason byte),
 * so "unsupported" can never again be misread as "found nothing". This
 * changes the *unsupported* reply's length, never the *empty-success*
 * reply's, so every existing decoder for a real subcommand (which checks
 * its own success shape, not this one) is unaffected -- see this commit's
 * message for the survey of tools/PcTools' parsers that was done before
 * making this change. */
void bridge_reply_unsupported(uart_protocol_t *proto, const uart_proto_message_t *msg,
                                     uint8_t src_task, uint8_t subcmd)
{
    bridge_reply_reject(proto, msg, src_task, subcmd, "unsupported");
}

/* An unsolicited push (an auto-report tick). Shorter ACK timeout than a reply:
 * a report that can't be delivered is stale by the time the retries run out,
 * and the next tick carries newer data anyway. */
void bridge_push(uart_protocol_t *proto, uart_proto_device_t dst_device, uint8_t dst_task,
                        uint8_t src_task, const uint8_t *payload, size_t len)
{
    esp_err_t err = uart_protocol_send(proto, dst_device, dst_task, src_task, payload, len, 300);
    if (err != ESP_OK) {
        /* Deliberately not an escalation: the watchdog decides when a run of
         * these adds up to a lost link, and it decides from the absence of
         * successes, not from any single failure. */
        ESP_LOGD(TAG, "task%u auto-report push failed: %s", src_task, esp_err_to_name(err));
        return;
    }
    bridge_note_link_activity();
}

/* --------------------------------------------------------------------------
 * PC link watchdog -- CONFIG_KILNCTL_SX1509_RELAYS_OFF_ON_LINK_LOSS
 *
 * See the long comment in uart_bridge.h for exactly what "the link went away"
 * means in wall-clock terms and what this does about it. Everything below is
 * the mechanism.
 * ------------------------------------------------------------------------ */

typedef struct {
    kiln_io_t *io;             /* NULL if the expander never came up */
    SafetyLinkClass *link;     /* NULL if the safety link failed to start */
} link_watchdog_ctx_t;

static void link_watchdog_task(void *arg)
{
    link_watchdog_ctx_t *ctx = (link_watchdog_ctx_t *)arg;

    /* Starts "up" only so that the first pass through the loop takes the
     * down transition and logs it: at boot no host has spoken, so the honest
     * state is down and the board should already be sitting in it. */
    bool was_up = true;
    /* Sticky until a drop actually succeeds, so a failed I2C transfer is
     * retried on the next tick instead of being assumed done. */
    bool relays_confirmed_off = false;
    /* Impossible as a real mask (only KILN_IO_RELAY_COUNT bits are ever set),
     * so the first pass always takes the "mask changed" branch below. */
    uint8_t last_unowned_mask = 0xFFu;

    const TickType_t timeout_ticks = pdMS_TO_TICKS(UART_BRIDGE_LINK_TIMEOUT_MS);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(UART_BRIDGE_LINK_CHECK_MS));

        /* Unsigned tick subtraction, so this stays correct across the tick
         * counter's wrap (~49 days at 1 kHz) without a special case. */
        const bool up = s_link_ever_seen &&
                        ((TickType_t)(xTaskGetTickCount() - s_link_last_activity) < timeout_ticks);

        if (up) {
            if (!was_up) {
                ESP_LOGW(TAG, "PC link back after %ums of silence"
#if CONFIG_KILNCTL_PC_LINK_LOSS_ASSERTS_FAULT
                              " -- clearing the link fault source;"
#else
                              ";"
#endif
                              " relays stay off until the host commands them",
                         (unsigned)UART_BRIDGE_LINK_TIMEOUT_MS);
#if CONFIG_KILNCTL_PC_LINK_LOSS_ASSERTS_FAULT
                if (ctx->link) {
                    esp_err_t err = safety_link_set_fault_source(ctx->link,
                                                                 SAFETY_FAULT_SRC_PC_LINK, false);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "could not clear the PC-link fault source: %s",
                                 esp_err_to_name(err));
                    }
                }
#endif
                was_up = true;
                relays_confirmed_off = false;
            }
            continue;
        }

        if (was_up) {
            ESP_LOGE(TAG, "PC link lost (no frame or ACK for %ums) -- dropping all relays"
#if CONFIG_KILNCTL_PC_LINK_LOSS_ASSERTS_FAULT
                          " and asserting the isolated fault line"
#else
                          " (the isolated fault line is deliberately NOT asserted -- see "
                          "KILNCTL_PC_LINK_LOSS_ASSERTS_FAULT)"
#endif
                          ,
                     (unsigned)UART_BRIDGE_LINK_TIMEOUT_MS);
            was_up = false;
            relays_confirmed_off = false;
        }

        /* Which relays this watchdog actually has authority over.
         *
         * This watchdog was written when the PC host was the only thing that
         * could energize a relay, so "the host stopped talking" and "nobody is
         * in control" were the same statement and dropping everything was
         * right. That is no longer true: profile_executor and autotune_engine
         * run firings autonomously on the board, and danger mode is driven
         * from the web UI -- none of which involve the serial host at all.
         *
         * Left unqualified, the consequences were severe and are not
         * hypothetical. `up` is gated on s_link_ever_seen, so a board that
         * boots with no PC attached -- the normal standalone deployment --
         * counts as link-lost forever and had all four relays forced off every
         * 250 ms. A firing could not hold a relay on for a single check tick.
         * With a PC attached but idle it is the same story intermittently:
         * observed dropping relays roughly every 5 s through a whole bench
         * session, overriding LCD manual control within seconds each time.
         * And the profile executor is never told, so it goes on computing duty
         * for relays something else keeps opening behind it.
         *
         * So: still a fail-safe for relays under MANUAL/no ownership, which is
         * the case this was built for and where host silence really does mean
         * nobody is watching. Relays claimed by a PROFILE, a RULE or an
         * AUTOTUNE have an on-board owner that the serial link's health says
         * nothing about, and are left alone -- that owner has its own
         * watchdogs (profile_executor's guard 9 and safety-link silence abort)
         * which are the ones that actually apply to it. Danger mode suppresses
         * the drop wholesale for its window, since an operator is deliberately
         * holding relays closed from the browser with no serial traffic at
         * all. */
        uint8_t unowned_mask = 0;
        for (uint8_t relay = 1; relay <= KILN_IO_RELAY_COUNT; relay++) {
            if (!relay_authority_manual_blocked_by_owner(relay)) {
                unowned_mask |= (uint8_t)(1u << (relay - 1u));
            }
        }
        if (danger_mode_active()) {
            unowned_mask = 0;
        }
        /* The confirmation is per-mask, not once per outage. A firing that
         * ends (or a danger-mode window that closes) hands relays back while
         * the link is still down, and those newly unowned relays must then be
         * dropped -- a "done" flag latched on the previous, smaller mask would
         * leave them energized for the rest of the outage, which on this board
         * is forever when no host ever connects. */
        if (unowned_mask != last_unowned_mask) {
            relays_confirmed_off = false;
            last_unowned_mask = unowned_mask;
        }

        /* Relays first, fault line second. The relays are the thing actually
         * carrying mains to the elements; the fault line is a request to a
         * processor that may or may not be listening. Do the one we control. */
        if (ctx->io && unowned_mask == 0) {
            /* Nothing here to drop -- everything is under an on-board owner.
             * Treated as done so this does not spin, and so the next genuine
             * unowned relay still gets dropped (the flag is cleared on every
             * up/down transition above). */
            relays_confirmed_off = true;
        } else if (ctx->io && !relays_confirmed_off) {
            /* Routed through kiln_io_owner (AUTHORIZED producer) rather than
             * calling kiln_io_set_relay_mask() directly, 2026-08-28.
             * kiln_io_owner.h's top comment explains why a direct call is
             * unsafe: kiln_io_set_relay_mask() is a read-modify-write
             * against the SX1509's data register, and SX1509.c's own mutex
             * only serializes each individual I2C transaction -- it does not
             * stop this watchdog's write from racing owner_task's, each
             * computing its new byte from a stale read of the other's most
             * recent change (a lost update reachable on the exact code path
             * that energizes mains contactors). Unlike
             * kiln_io_all_relays_off() (kiln_io_owner.h:75-89's one
             * documented direct-call exception), this write only ever
             * touches `unowned_mask` -- a strict subset of the register --
             * so it cannot claim that call's "unconditional, all relays,
             * only ever OFF" reasoning: a race here can silently revert an
             * owner-controlled relay's bit that owner_task set in the same
             * window, in either direction.
             *
             * Safe to route through the queue: owner_task (kiln_io_owner.c)
             * waits only on its own command queue and the SX1509 I2C mutex,
             * never on uart_protocol_send()/the PC link/any bridge task, so
             * it keeps running when every bridge task is wedged waiting on a
             * departed host -- exactly the condition this watchdog exists
             * for. kiln_io_owner_command_set_relay_mask_authorized() also
             * needs no additional ownership/safety gate here: unowned_mask
             * above already excludes every PROFILE/RULE/AUTOTUNE-owned
             * relay, and a mask paired with value=0 can only ever turn
             * relays off, which relay_authority's "on" gate was never meant
             * to block anyway. See tools/check_relay_writes_through_owner.ps1
             * for the guard that enforces this repo-wide. */
            esp_err_t err = kiln_io_owner_command_set_relay_mask_authorized(unowned_mask, 0u);
            if (err == ESP_OK) {
                relays_confirmed_off = true;
            } else {
                /* Deliberately left un-sticky: this repeats every check tick
                 * for as long as the link is down and the write keeps failing.
                 * A kiln with an element stuck on and a dead I2C bus is worth
                 * a log line every 250 ms. */
                ESP_LOGE(TAG, "link-loss relay drop failed: %s -- retrying",
                         esp_err_to_name(err));
            }
        } else if (!ctx->io && !relays_confirmed_off) {
            /* No expander handle at all: the relays cannot be commanded from
             * here and their state is unknown. Say so once, and leave the
             * fault line asserted below as the only remaining lever. */
            ESP_LOGE(TAG, "link lost and no expander handle -- relay state is UNKNOWN and cannot "
                          "be forced off from this firmware");
            relays_confirmed_off = true; /* nothing more to try; stop repeating */
        }

#if CONFIG_KILNCTL_PC_LINK_LOSS_ASSERTS_FAULT
        /* Re-asserted every tick rather than once on the transition: it is a
         * single GPIO write, and doing it unconditionally means the line is
         * still right even if something else cleared the source in between.
         *
         * Compiled out by default -- see KILNCTL_PC_LINK_LOSS_ASSERTS_FAULT.
         * The PC link is the MCP tooling's debug and control channel, not
         * something the kiln needs in order to run, so its absence is not a
         * hazard and must not trip the safety processor. A board on the bench
         * with nothing plugged in used to assert this five seconds after boot
         * and sit there permanently tripped, because the trip latches on the
         * far side and does not clear when the line goes back low. */
        if (ctx->link) {
            esp_err_t err = safety_link_set_fault_source(ctx->link, SAFETY_FAULT_SRC_PC_LINK, true);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "could not assert the PC-link fault source: %s",
                         esp_err_to_name(err));
            }
        }
#endif
    }
}

esp_err_t uart_bridge_start_link_watchdog(kiln_io_t *io, SafetyLinkClass *link)
{
    if (!io && !link) {
        /* Nothing to act on: neither the relays nor the fault line is
         * reachable, so a watchdog would only burn a task. */
        return ESP_ERR_INVALID_ARG;
    }

#if !CONFIG_KILNCTL_SX1509_RELAYS_OFF_ON_LINK_LOSS
    /* Bench builds only -- see the Kconfig help. The fault line still follows
     * the link, because telling the safety processor the truth about the
     * control link costs nothing and is never the thing you want switched off.
     */
    ESP_LOGW(TAG, "relays-off-on-link-loss is DISABLED in this build: an unattended board can "
                  "hold relays energized after the PC link drops");
    io = NULL;
#endif

    static link_watchdog_ctx_t ctx;
    ctx.io = io;
    ctx.link = link;

    /* Priority 6: above the bridge tasks (5), because this must still run when
     * every one of them is blocked inside a uart_protocol_send that a departed
     * host will never ACK -- which is precisely the situation it exists for. */
    /* PSRAM stack, 2026-08-20, same reason and same API as lvgl_port.c's and
     * uart_bridge_ext.c's: this 3072-byte stack could not be satisfied from
     * an internal heap fragmented to a sub-1KB largest free block, and the
     * failure mode was the worst one on this board -- app_main logged "PC
     * link watchdog did not start (ESP_ERR_NO_MEM) -- RELAYS WILL NOT DROP ON
     * LINK LOSS" and dropped into safe state. A safety watchdog that cannot
     * be created because of heap fragmentation is not an acceptable failure,
     * and PSRAM is sitting 8MB empty.
     *
     * Safe in PSRAM: this task polls timestamps and, on timeout, calls
     * kiln_io/safety_link to drop relays and assert the fault line. It holds
     * no DMA buffers and runs from no ISR. Note the fault line is also
     * asserted by hardware-independent paths, so the safe direction does not
     * depend solely on this task's stack being reachable. */
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(link_watchdog_task, "link_watchdog", 3072,
                                                         &ctx, 6, NULL, tskNO_AFFINITY,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "PC link watchdog up: relays drop and the fault line asserts after %ums with no "
                  "frame or ACK", (unsigned)UART_BRIDGE_LINK_TIMEOUT_MS);
    return ESP_OK;
}
