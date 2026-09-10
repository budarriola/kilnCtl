// The periodic poll task for the safety link (ESP32-S3 <-> RP2040 safety
// processor) and the link-health bookkeeping it drives: tc_type/config-crc
// sync to the Pico, and the up/down + version-mismatch fault logic. Split
// out of safety_link.c (4183 lines) as one of the seams the file's own
// "Poll task" section banner already named, distinct from the frame decode/
// build functions it calls (safety_link_frames.c) and the request/reply
// exchange machinery underneath it (safety_link_inbox.c).
//
// VERBATIM relocation: every function body below is byte-for-byte the same
// code that used to live in safety_link.c, with no behavior change. The
// only mechanical change is linkage: safety_poll_task() is now referenced
// from safety_link.c's safety_link_start() (xTaskCreatePinnedToCoreWithCaps
// takes its address) and so dropped `static` and gained a declaration in
// safety_link_internal.h. safety_sync_tc_type(), safety_sync_cfg_cache()
// and safety_update_health() are called only from safety_poll_task() in
// this same file and stay exactly as private as they always were.
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
#include "safety_ceiling_sync.h" /* safety_ceiling_sync_reconcile_on_link_up() -- see its call site below */

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
/* TODO.md owner-report item 3 (2026-08-21): keeps the RP2040 safety
 * processor's own, independent MAX31856 thermocouple type in agreement with
 * whatever the operator last saved on the Thermocouples & Zones page --
 * zones_cfg_t::safety_tc_type, a setting SEPARATE from any main-board zone's
 * own tc_type (see that field's comment in zones_http.c for why: the safety
 * processor's sensor is different, physically independent hardware that has
 * no reason to match any particular zone's channel).
 *
 * DESIGN DECISION, stated plainly because the task asked for it explicitly:
 * this is a level-triggered sync, not an edge-triggered "send once when the
 * operator clicks Save" push, because zones_http.c (which owns the setting
 * and the web form) has no reference to the SafetyLinkClass instance to push
 * through -- only main.c does, as a local static, and main.c is off-limits
 * for this pass (see the include comment above). Instead, this function runs
 * on every safety_poll_task() tick (SAFETY_POLL_PERIOD_MS, typically a few
 * hundred ms) and compares the persisted desired value against
 * link->tc_type_last_sent, the last value THIS driver believes it
 * successfully broadcast:
 *   - Operator changes the setting while the link is up: picked up and sent
 *     within one poll period -- not instant, but no operator is watching a
 *     sub-second deadline on a config write.
 *   - Link is down when the setting changes: safety_link_send_set_config()
 *     still gets called (it always tries), fails silently exactly as every
 *     other fire-and-forget SAFETY_CMD_* does when nothing is listening, and
 *     tc_type_last_sent is only updated on ESP_OK -- so it is left stale and
 *     this function retries on every subsequent poll until the link comes
 *     back and a send actually succeeds. THE CHANGE IS NEVER SILENTLY
 *     DROPPED: it is re-applied automatically the moment the link recovers,
 *     which is the "safe answer" TODO.md's task description asked for.
 *   - Link drops and recovers with tc_type_last_sent already matching the
 *     desired value: safety_update_health()'s down->up transition (see its
 *     caller below) resets tc_type_last_sent to the 0xFF sentinel first, so
 *     this function resends unconditionally on the very next poll after a
 *     reconnect. This is deliberate belt-and-suspenders: a Pico that dropped
 *     off the link and came back may have rebooted in between (a genuine
 *     reboot is indistinguishable, from this side, from a link glitch that
 *     self-heals -- see LINK_PROTOCOL.md), and a rebooted Pico's
 *     config_store.h may not have persisted whatever was last pushed to it.
 *     Re-sending costs one harmless broadcast; NOT re-sending risks running
 *     with the two processors silently disagreeing about thermocouple type,
 *     which is exactly the failure TODO.md's task description warns
 *     against ("if the two processors disagree ... they will disagree about
 *     temperature, which defeats the whole point of an independent
 *     cross-check").
 *
 * Gated on zones_config_is_valid(): a board that has never loaded a real
 * zones config has zones_config_get_safety_tc_type() reading its zeroed
 * default (THERMO_TC_B, not THERMO_TC_K -- see that getter's comment), and
 * broadcasting that fabricated value to the Pico would be worse than
 * sending nothing. Nothing is sent at all until a real config exists,
 * matching zones_http.c's own "gate hardware effects on validity, not
 * merely on having read some bytes" discipline (s_zones_config_valid).
 *
 * UNTESTED, same as every other safety_link.c path this bench cannot
 * exercise right now: the optocouplers between the ESP and the Pico are
 * currently non-functional (see this task's own hard rules), so this
 * function has never observed a real link-down/link-up transition, only
 * been read against the header comments and the existing down_logged
 * edge-detect pattern it reuses. */
static void safety_sync_tc_type(SafetyLinkClass *link)
{
    if (!zones_config_is_valid()) {
        return;
    }
    uint8_t desired = 0;
    if (!zones_config_get_safety_tc_type(&desired)) {
        return; /* NULL out-pointer only; cannot happen with a local above,
                  * kept for the same "never trust a getter blindly" reason
                  * every other safety_link.c caller of an external getter
                  * follows. */
    }
    if (desired == link->tc_type_last_sent) {
        return; /* already sent this value and nothing has forced a resend */
    }
    esp_err_t err = safety_link_send_set_config(link, desired);
    if (err == ESP_OK) {
        link->tc_type_last_sent = desired;
        ESP_LOGI(TAG, "safety tc_type sync: sent %u", (unsigned)desired);
    }
    /* On failure, tc_type_last_sent is left as it was -- the next poll tries
     * again. No log spam here: safety_link_send_set_config() and the
     * link-down warning safety_update_health() already emits below cover
     * why this failed. */
}

/* docs/COMMISSIONING.md sec 3's fetch-on-change trigger: "every FW_VERSION
 * frame carries the Pico's config_crc... the ESP refetches only when that
 * CRC differs from its cache." peer_build_known gates this the same way it
 * gates every other peer_build_* consumer (safety_link.h's own field
 * comment) -- no opinion until a FW_VERSION frame has actually reached
 * config_crc, so a Pico that predates this frame's tail (or hasn't answered
 * yet) never triggers a fetch attempt that could only time out. Called only
 * when the link is up (safety_update_health()'s own gate), same
 * "don't even try while nothing is listening" discipline safety_sync_tc_
 * type() follows for SET_CONFIG. safety_cfg_store_maybe_refetch() itself is
 * the actual no-UART-traffic-unless-changed check -- this function only
 * supplies the live CRC to compare against. */
static void safety_sync_cfg_cache(SafetyLinkClass *link)
{
    bool known = false;
    uint16_t live_crc = 0;
    if (!safety_lock(link)) {
        return;
    }
    known = link->peer_build_known;
    live_crc = link->peer_config_crc;
    safety_unlock(link);

    if (!known) {
        return;
    }
    (void)safety_cfg_store_maybe_refetch(link, live_crc);
}

static void safety_update_health(SafetyLinkClass *link)
{
    bool up = false;
    bool policy = false;
    bool version_mismatch = false;
    bool version_known = false;
    bool update_in_progress = false;
    uint16_t age = SAFETY_LINK_AGE_NEVER;

    if (safety_lock(link)) {
        up = safety_link_up_locked(link);
        age = safety_age_ms_locked(link);
        policy = link->fault_on_link_loss;
        /* Fails CLOSED on an unknown peer version (audit 2026-08-27). This
         * used to be `peer_version_known && !peer_version_compatible`, i.e.
         * a peer whose version had never been established counted as
         * compatible and the link fault was cleared -- permitting every
         * relay-on path against a safety processor we had never actually
         * handshaken with. That is reachable in normal operation, not just
         * in theory: `up` is derived purely from telemetry recency, so a
         * Pico streaming status every 500 ms makes the link look healthy,
         * while its unsolicited boot FW_VERSION is a no-ACK broadcast that
         * can simply be missed. The Pico side already gets this right --
         * link_task.c treats an unset peer version as "unknown, therefore
         * too old". Unknown and incompatible now both assert the fault. */
        version_mismatch = !link->peer_version_known || !link->peer_version_compatible;
        version_known = link->peer_version_known;
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
            /* TODO.md owner-report item 3: force a tc_type resend on
             * reconnect -- see safety_sync_tc_type()'s comment for why a
             * link recovery is treated as "the Pico may have rebooted and
             * lost this" rather than trusted to still hold whatever was
             * last successfully sent. */
            link->tc_type_last_sent = 0xFFu;
        }
        safety_sync_tc_type(link);
        safety_sync_cfg_cache(link);
        /* 2026-09-10 opus review: the raise-guard in zones_http_post.c only
         * runs from that one POST handler, and treats a NULL link as
         * "nothing to guard" -- so a board that boots (or reconnects after a
         * Pico swap/reflash) with a Pico ceiling behind the ESP's already-
         * persisted zone config has nothing to close that gap until an
         * operator happens to POST the zones page again. Deliberately
         * called unconditionally here, every tick the link is up -- same
         * level-triggered, "keep retrying, never silently drop it"
         * discipline as safety_sync_tc_type() above, not gated on
         * link->down_logged, so this also covers the very first tick after
         * boot when the link comes up before ever having been observed
         * down (down_logged starts false, so a down_logged-gated call would
         * never fire that case at all -- this is the "no boot-time
         * reconciliation anywhere" gap the audit found). Cheap when nothing
         * needs correcting: safety_ceiling_sync_guard_raise() reads the
         * Pico's cached ceiling and only performs a real write+confirm UART
         * round trip when a raise is actually needed. */
        safety_ceiling_sync_reconcile_on_link_up(link);
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

    /* Both branches of version_mismatch fail closed below, unchanged. What
     * differs here is only how loudly each is reported.
     *
     * "Not heard yet" is the normal state for the first seconds of every
     * boot: the Pico's FW_VERSION is an unsolicited broadcast, so the link
     * can be carrying telemetry before the version lands. That was being
     * logged at ERROR on every healthy boot -- and an alarm that fires every
     * boot is one an operator learns to scroll past, which is exactly how
     * the real faults in this project stayed invisible (ROADMAP.md M10). It
     * is now reported at INFO inside a grace window, and escalates to ERROR
     * only if the version still has not arrived after
     * SAFETY_LINK_VERSION_GRACE_MS -- at which point it is no longer a boot
     * race but a peer that is not answering, which IS worth an error.
     *
     * A version that HAS been heard and is incompatible stays an immediate
     * ERROR: nothing about that resolves by waiting. */
    if (!version_mismatch) {
        link->version_unknown_since_valid = false;
    } else if (!version_known && !link->version_unknown_since_valid) {
        link->version_unknown_since_tick = (uint32_t)xTaskGetTickCount();
        link->version_unknown_since_valid = true;
    }

    bool version_unknown_past_grace =
        version_mismatch && !version_known && link->version_unknown_since_valid &&
        safety_elapsed_ms(link->version_unknown_since_tick) >= SAFETY_LINK_VERSION_GRACE_MS;
    bool want_loud = version_mismatch && (version_known || version_unknown_past_grace);

    if (version_mismatch != link->version_mismatch_logged) {
        if (version_mismatch) {
            if (version_known) {
                ESP_LOGE(TAG, "safety processor protocol version incompatible -- treating link as "
                              "down (Phase 7b.5, LINK_PROTOCOL.md sec 4)");
            } else {
                ESP_LOGI(TAG, "waiting for the safety processor's FW_VERSION broadcast -- heating "
                              "stays blocked until it arrives (normal for the first seconds of a boot)");
            }
        } else {
            ESP_LOGI(TAG, "safety processor protocol version now compatible");
        }
        link->version_mismatch_logged = version_mismatch;
        link->version_loud_logged = want_loud;
    } else if (want_loud && !link->version_loud_logged) {
        /* The grace window expired without a FW_VERSION ever arriving. */
        ESP_LOGE(TAG, "no FW_VERSION from the safety processor after %u ms -- treating the link as "
                      "down and blocking heat (Phase 7b.5, LINK_PROTOCOL.md sec 4)",
                 (unsigned)SAFETY_LINK_VERSION_GRACE_MS);
        link->version_loud_logged = true;
    } else if (!want_loud) {
        link->version_loud_logged = false;
    }

    if (policy) {
        safety_link_set_fault_source(link, SAFETY_FAULT_SRC_SAFETY_LINK, !up || version_mismatch);
    }
}

void safety_poll_task(void *arg)
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

        /* See safety_reset_stale_peer_info_if_link_down()'s own doc comment
         * (opus-review finding: "the boot_id evidence channel is single-shot
         * and unrecoverable") -- clears peer_version_known/pico_boot_id_known/
         * peer_build_known once the link is observed down, so the
         * !peer_version_known branch immediately below re-requests FW_VERSION
         * on this very iteration rather than trusting stale pre-drop state. */
        safety_reset_stale_peer_info_if_link_down(link);

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
        /* SAFETY_INBOX_LEN's comment (2026-08-25 measurement): the Pico
         * sustains ~3.7x overproduction against a single per-poll drain, so
         * this gap used to be one flat vTaskDelay() that left the inbox
         * unread for up to ~450ms at a time -- long enough for its 4 slots to
         * fill and start dropping BROADCAST frames well before the next
         * poll's drain ever ran, which is what made the web/LCD safety-link
         * indicator flicker between up/down (safety_link_up_locked() reacts
         * to real missed replies, not a display bug). Chunking the same
         * total wait into SAFETY_LINK_IDLE_TICK_MS-sized drains is the
         * "continuous drain" lever that measurement named as the actual fix
         * -- not a deeper queue, which that commit already showed cannot
         * help against a sustained-rate mismatch. Total elapsed time is
         * unchanged: each chunk either drains or plain-delays for the same
         * duration, exactly like the vTaskDelay it replaces.
         *
         * MUST take xact_lock (non-blocking) before draining: link->inbox is
         * the SAME queue safety_exchange()/safety_link_get_ct_cal()/etc read
         * from while holding that lock, and this loop runs in the gap AFTER
         * this task's own locked exchange already released it -- exactly
         * when another task (an HTTP handler calling, say,
         * safety_link_get_ct_cal()) can be mid-wait for its own reply. An
         * unlocked drain here would dequeue that reply first, land on the
         * `default:` case (nothing here asked for a CT_CAL reply), get
         * counted as unmatched, and the legitimate caller would time out --
         * a regression the flat vTaskDelay() this replaces could not cause,
         * since it never touched the inbox at all. When the lock is busy
         * (a real exchange IS in progress), skip draining this chunk: that
         * exchange's own safety_drain_inbox_ex() already drains everything
         * currently queued on every call, so nothing is lost, only deferred.
         *
         * The lock is held only long enough for an OPPORTUNISTIC, non-
         * blocking drain (wait_ms=0 -- takes whatever is already queued and
         * returns immediately) rather than for the whole chunk: holding a
         * mutex across a multi-hundred-ms blocking wait, then immediately
         * re-taking it next iteration with no other yield point in between,
         * risks starving another task's xSemaphoreTake(xact_lock, ...) on
         * the same mutex even though FreeRTOS eventually honors it (its
         * SAFETY_XACT_LOCK_TIMEOUT_MS budget is 5000ms, generous, but there
         * is no reason to spend any of it here). vTaskDelay(chunk_ms)
         * happens AFTER giving the lock back, so the actual pacing/yield is
         * always unlocked. */
        uint32_t remaining_ms = sleep_ms;
        while (remaining_ms > 0u) {
            uint32_t chunk_ms = (remaining_ms < SAFETY_LINK_IDLE_TICK_MS) ? remaining_ms : SAFETY_LINK_IDLE_TICK_MS;
            if (xSemaphoreTake(link->xact_lock, 0) == pdTRUE) {
                (void)safety_drain_inbox(link, 0);
                xSemaphoreGive(link->xact_lock);
            }
            vTaskDelay(pdMS_TO_TICKS(chunk_ms));
            remaining_ms -= chunk_ms;
        }
    }
}
