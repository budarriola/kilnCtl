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
// safety_link_internal.h. safety_sync_cfg_cache()
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
#include "heat_enable.h"
#include "ct_leak_alarm_service.h" /* H9 CT alarm, one 2 Hz pass per loop below */
#include "stack_margin.h"
#include "freertos/idf_additions.h"
#include "settings.h"
#include "uart_task_ids.h"
#include "kiln_cfg_store.h"
#include "config_divergence.h" /* CONFIG_DIVERGENCE_REASON_MAX */
#include "safety_ceiling_sync.h"

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

/* uart_log_bridge_relay_safety() -- see safety_link_service_log_relay()
 * below. Declared here, not #include "../bridge/uart_log_bridge.h": that
 * header's own #include of uart_protocol.h (needed for uart_protocol_t*
 * elsewhere in that header) resolves via a relative path that collides
 * with the host-test stub uart_protocol.h this translation unit picks up
 * when test_safety_link_compile.c textually #includes this file --
 * redefinition errors (uart_owner_t, uart_proto_message_t, etc.) in the
 * host build even though the target build was fine. A single extern decl
 * of just the one function this file calls sidesteps it; keep the
 * signature in sync with uart_log_bridge.h by hand. */
bool uart_log_bridge_relay_safety(uint8_t level, const char *text, uint8_t text_len);

/* 2026-09-15 (Opus review F3): this used to pull zones_config_get_safety_
 * tc_type() for a since-removed push to the Pico (see safety_update_health()'s
 * own comment below) -- the include stays because other zones_config_
 * accessors.h getters (zones_config_is_valid()) are still used in this file. */
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
/* 2026-09-15 owner decision (Opus review F3, "the commissioning page owns
 * the type"): this file used to push zones_cfg_t::safety_tc_type to the
 * Pico here (safety_sync_tc_type(), edge- and level-triggered, on every
 * poll tick and forced again on reconnect) and mark it "synced" the instant
 * the local UART enqueue returned ESP_OK -- never confirmed by the Pico.
 * The Pico's own commissioning page (SET_PARAM/COMMIT_CONFIG,
 * safety_commissioning_page.html) is now the sole writer of its tc_type
 * (SaftyFW's config_store.h); the ESP's zones config keeps the field
 * (deprecated, see zones_config_json.h's own comment on it, so no
 * ZONES_CFG_VERSION bump is needed) purely to READ BACK and DISPLAY what
 * the Pico last reported, never to push a value at it. Removing this
 * function does not touch safety_ceiling_sync.c's broadened-field
 * divergence check (safety_ceiling_sync_is_standing_diverged() and
 * friends): that comparator already reads the Pico's own reported value
 * independently via safety_cfg_store_get_by_index(), never through
 * tc_type_last_sent, so it keeps detecting a real ESP/Pico tc_type
 * mismatch exactly as before. */

/* docs/COMMISSIONING.md sec 3's fetch-on-change trigger: "every FW_VERSION
 * frame carries the Pico's config_crc... the ESP refetches only when that
 * CRC differs from its cache." peer_build_known gates this the same way it
 * gates every other peer_build_* consumer (safety_link.h's own field
 * comment) -- no opinion until a FW_VERSION frame has actually reached
 * config_crc, so a Pico that predates this frame's tail (or hasn't answered
 * yet) never triggers a fetch attempt that could only time out. Called only
 * when the link is up (safety_update_health()'s own gate) -- same
 * "don't even try while nothing is listening" discipline the removed
 * safety_sync_tc_type() used to follow for SET_CONFIG (see the 2026-09-15
 * comment above). safety_cfg_store_maybe_refetch() itself is the actual
 * no-UART-traffic-unless-changed check -- this function only supplies the
 * live CRC to compare against. */
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
        }
        safety_sync_cfg_cache(link);
        /* 2026-09-10 opus review: the raise-guard in zones_http_post.c only
         * runs from that one POST handler, and treats a NULL link as
         * "nothing to guard" -- so a board that boots (or reconnects after a
         * Pico swap/reflash) with a Pico ceiling behind the ESP's already-
         * persisted zone config has nothing to close that gap until an
         * operator happens to POST the zones page again. Deliberately
         * called unconditionally here, every tick the link is up -- same
         * level-triggered, "keep retrying, never silently drop it"
         * discipline safety_sync_cfg_cache() above follows, not gated on
         * link->down_logged, so this also covers the very first tick after
         * boot when the link comes up before ever having been observed
         * down (down_logged starts false, so a down_logged-gated call would
         * never fire that case at all -- this is the "no boot-time
         * reconciliation anywhere" gap the audit found). Cheap when nothing
         * needs correcting: safety_ceiling_sync_guard_raise() reads the
         * Pico's cached ceiling and only performs a real write+confirm UART
         * round trip when a raise is actually needed.
         *
         * 2026-09-22 (opus review, advisory adopted): non-blocking form --
         * this call is level-triggered every tick, so skipping it while a
         * kiln_cfg_swap.c reconcile is already in flight (blocking form,
         * kiln_cfg_swap.c step 10/11) is a no-op, not a missed event; the
         * next tick re-checks the same condition. See safety_ceiling_sync.h's
         * doc comment on this entry point. */
        safety_ceiling_sync_reconcile_on_link_up_nonblocking(link);
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

/* Hand-declared, same convention safety_cfg_store.c/relay_cycles.c/
 * factory_reset.c/zones_config_store.c already use for the flash worker
 * (see flash_worker.h's own doc comment) -- avoids pulling in the whole
 * UART bridge API for one call. The BOUNDED variant deliberately: this task
 * drives the safety link's own liveness heartbeat, and blocking it for the
 * unbounded duration of some other caller's flash job (a profile import, a
 * cfg_fs write) would stall the GET_STATUS poll the Pico's S6b LINK_DEAD
 * guard watches for. */
esp_err_t uart_bridge_ext_post_on_flash_worker(void (*fn)(void *arg));

/* How often to log a refused post. A refusal is a normal "not yet", and the
 * poll below runs every SAFETY_POLL_RECAPTURE_INTERVAL_MS, so logging every
 * one would spam the console for as long as the worker stayed busy --
 * but logging none of them is exactly the silence flash_worker.h forbids.
 * Log the first, then at most one summary a minute carrying the suppressed
 * count, so a genuinely stuck worker stays visible without flooding. */
#define SAFETY_POLL_RECAPTURE_LOG_INTERVAL_MS 60000u

/* How often to even look at the pending flag. The recapture is a
 * housekeeping write, not a safety deadline, and safety_poll_task's own loop
 * runs at roughly 2 Hz -- checking every iteration would add a pointless
 * lock take per tick. */
#define SAFETY_POLL_RECAPTURE_INTERVAL_MS 5000u

/* 2026-09-15 review follow-up (review_divergence_wiring_60d6552f_2026-09-15.md,
 * HIGH 1 and items A/B/C): the deferred Pico-half recapture poll used to run
 * on the home page's 1 Hz LVGL refresh timer. That timer is created inside
 * ui_page_home.c's lazy build(), so on a boot that never opens the home page
 * (a touch_cal boot, say) the poll simply did not exist; it was also nested
 * inside a widget-non-NULL check and died with the page. A divergence
 * detector must not depend on which screen the operator opened, so it lives
 * here now: safety_poll_task always runs, is display-independent, and is
 * already the task that drives the divergence evaluation this recapture is
 * gated on.
 *
 * Reachability: this is not reachable on-worker. safety_poll_task is created
 * by safety_link_start() and is never bx_flash_worker, so the dispatch below
 * can never re-enter the worker from itself (flash_worker.h's re-entrancy
 * rule).
 *
 * The dispatch is mandatory, not stylistic: safety_poll_task's 8192 B stack
 * is PSRAM-backed, and kiln_cfg_store_autosave_from_live()'s deferred-
 * recapture path performs an NVS write, which aborts outright from a
 * PSRAM stack (project memory: "PSRAM stack + NVS = panic"). */
/* Posted through uart_bridge_ext_post_on_flash_worker(), which carries no
 * `arg` at all -- so this job can never be the task that set an autosave
 * target override, and the NULL dispatcher below says exactly that.
 * Calling the identity-carrying entry point directly, rather than the
 * plain wrapper that would forward NULL for us, is deliberate on two
 * counts: it states the "not the override owner" property at the call
 * site instead of leaving it implicit, and it keeps the wrapper's frame
 * off the DEEPEST enumerated bx_flash_worker dispatch target -- this job.
 * That frame measured +32 B and pushed bx_flash_worker's lower bound from
 * 3792 B to 3824 B, over its ceiling in check_all_task_stack_budgets.py. */
static void safety_poll_pico_half_recapture_job(void *arg)
{
    (void)arg;
    char reason[CONFIG_DIVERGENCE_REASON_MAX];
    reason[0] = '\0';
    if (!kiln_cfg_store_autosave_from_live_for_dispatcher(NULL, reason, sizeof(reason))) {
        ESP_LOGW("safety_poll", "deferred Pico-half recapture autosave failed: %s",
                 reason[0] ? reason : "(no reason given)");
    }
}

static void safety_poll_service_pico_half_recapture(void)
{
    static TickType_t s_last_check_ticks = 0;
    static bool s_checked_once = false;

    TickType_t now = xTaskGetTickCount();
    if (s_checked_once &&
        (now - s_last_check_ticks) < pdMS_TO_TICKS(SAFETY_POLL_RECAPTURE_INTERVAL_MS)) {
        return;
    }
    s_last_check_ticks = now;
    s_checked_once = true;

    /* Never recapture while still diverged -- autosave_from_live() would
     * just re-defer and re-log. The recapture exists to clear a flag that
     * was deferred BECAUSE of a divergence, once that divergence is gone. */
    if (safety_ceiling_sync_is_standing_diverged(NULL, 0)) {
        return;
    }
    if (!kiln_cfg_store_pico_half_recapture_pending()) {
        return;
    }
    /* Second-review MEDIUM: while the safety_cfg_store cache has never been
     * fetched, the autosave defers its Pico-half recapture again (it will not
     * capture an unfetched cache), re-sets the dirty flag, and still bumps
     * s_kiln_cfg_rev with a real LittleFS+NVS write. Posting here every 5 s
     * would repeat that forever on a board with no Pico or a failing refetch.
     * Wait until has_data; the flag stays set and is serviced then. */
    if (!safety_cfg_store_has_data()) {
        return;
    }

    /* POST, not dispatch-and-await (2026-09-16, HIGH 1 of the adversarial
     * review of 60d6552f). This used to call uart_bridge_ext_run_on_flash_
     * worker_timeout() with a 50 ms cap, on the belief that the cap bounded
     * how long this task could be held up. It does not: that cap bounds only
     * the wait to ACQUIRE the worker -- once acquired, the job is awaited
     * with xSemaphoreTake(s_bx_done, portMAX_DELAY), unbounded (see
     * flash_worker.h, and uart_bridge_ext.c's own comment on that take). The
     * job here is kiln_cfg_store_autosave_from_live(), an NVS write, not the
     * "short job by inspection" that contract requires.
     *
     * Why that mattered on THIS task specifically: the GET_STATUS send a few
     * lines below is the ESP->Pico liveness heartbeat, and safety_poll_task
     * is its only sender. This call sits immediately before it, so a slow or
     * stuck flash write delayed the heartbeat by exactly its own duration --
     * past link_timeout_s (10.0 s default) the Pico trips S6b LINK_DEAD and
     * ruins a firing. Fails safe, but spuriously.
     *
     * uart_bridge_ext_post_on_flash_worker() returns immediately in every
     * case; the job runs later on bx_flash_worker's own internal-SRAM stack,
     * which is what satisfies the PSRAM-stack hazard described above. NOW it
     * is genuinely fire-and-forget, and the retry path below is what makes
     * that correct: the pending flag stays set until the job actually
     * completes, so a refused post is picked up SAFETY_POLL_RECAPTURE_
     * INTERVAL_MS later. */
    esp_err_t post_err = uart_bridge_ext_post_on_flash_worker(safety_poll_pico_half_recapture_job);

    /* Never silence (flash_worker.h: a refusal "must be shown as a real
     * 'busy, try again' outcome, not silence"). The previous code (void)-cast
     * this away entirely, so a worker stuck for hours looked identical to a
     * recapture that had succeeded. */
    static TickType_t s_last_log_ticks = 0;
    static bool s_logged_once = false;
    static uint32_t s_suppressed = 0;
    if (post_err != ESP_OK) {
        bool due = !s_logged_once ||
                   (now - s_last_log_ticks) >= pdMS_TO_TICKS(SAFETY_POLL_RECAPTURE_LOG_INTERVAL_MS);
        if (due) {
            ESP_LOGW("safety_poll",
                     "deferred Pico-half recapture not posted to the flash worker: %s "
                     "(still pending, retrying in %u ms; %u earlier refusal(s) suppressed)",
                     esp_err_to_name(post_err), (unsigned)SAFETY_POLL_RECAPTURE_INTERVAL_MS,
                     (unsigned)s_suppressed);
            s_last_log_ticks = now;
            s_logged_once = true;
            s_suppressed = 0;
        } else {
            s_suppressed++;
        }
    }
}

/* How many (ESP, UART_TASK_ID_LOG) messages to drain per poll-loop pass --
 * bounded so a burst of Pico log lines can never turn this into an
 * unbounded loop on a task that also has to keep GET_STATUS/DIAG/POWER
 * moving. SAFETY_LOG_INBOX_LEN (safety_link.c) is only 4 deep, so 4 is
 * already "drain it completely, once, every pass" -- any log line beyond
 * that arrived faster than this loop can be scheduled and is exactly the
 * case uart_protocol's own broadcast_dropped counter (not this loop) is
 * supposed to absorb, per Frame F's "best-effort and droppable" rule. */
#define SAFETY_LOG_RELAY_MAX_PER_PASS 4u

/* Drains SafetyLinkClass::log_inbox (registered in safety_link.c's start(),
 * "Frame F" / CommonFW/docs/LINK_PROTOCOL.md sec 6) and relays each message
 * onward through uart_log_bridge_relay_safety(). Non-blocking throughout
 * (xQueueReceive with a zero timeout): called every safety_poll_task pass,
 * including the `period == 0` (polling off) branch below, so a Pico log
 * line is never held up behind the ESP's own poll cadence, and this task
 * never itself waits on anything the Pico may never send. */
static void safety_link_service_log_relay(SafetyLinkClass *link)
{
    if (!link || !link->log_inbox) {
        return;
    }
    uart_proto_message_t msg;
    for (unsigned i = 0; i < SAFETY_LOG_RELAY_MAX_PER_PASS; i++) {
        if (xQueueReceive(link->log_inbox, &msg, 0) != pdTRUE) {
            break;
        }
        /* Untrusted wire input (CommonFW/README.md rule 6): a LOG payload
         * with no bytes at all (length 0, no level byte) is malformed --
         * discard silently, same convention as safety_link_inbox.c's own
         * decode-failure handling, rather than reading payload[0] out of
         * bounds. */
        if (msg.length < 1u) {
            /* DEFERRED, not overlooked: this discard -- and the two other
             * places a relayed Pico log line can be lost (the ESP-side
             * inbox-full drop, counted by uart_protocol's per-task
             * broadcast_dropped, and the bridge-queue-full drop, counted by
             * uart_log_bridge.c's s_dropped_lines) -- are NOT yet visible as
             * a single dropped-LOG-frame count on the diagnostic frame.
             * Surfacing one there needs a wire protocol version bump and new
             * codec infrastructure, which this change deliberately does not
             * take; tracked in tools/PcTools/TODO.md ("log relay" item 4).
             * Until then, loss is accounted for only by those two existing,
             * separate counters, and a malformed zero-length payload by
             * neither -- read this comment, not a clean counter, before
             * concluding no line was lost. */
            continue;
        }
        uint8_t level = msg.payload[0];
        const char *text = (const char *)&msg.payload[1];
        uint8_t text_len = (uint8_t)(msg.length - 1u);
        (void)uart_log_bridge_relay_safety(level, text, text_len);
    }
}

void safety_poll_task(void *arg)
{
    SafetyLinkClass *link = (SafetyLinkClass *)arg;
    /* DO NOT DELETE THIS SEND as "dead code" just because the Pico's
     * link_task.c dispatch switch has no case for SAFETY_CMD_GET_STATUS and
     * never answers it (confirmed, docs/audits/
     * safety_link_get_status_timeout_counter_2026-09-10.md, and the
     * 2026-09-10 opus review that followed it) -- what looks unanswered on
     * this side is load-bearing on the OTHER side: every cleanly decoded
     * frame updates the Pico's s_last_valid_frame_tick/s_valid_frame_seen
     * (firmware/SaftyFW/src/tasks/link_task.c around line 2347-2348) BEFORE
     * the dispatch switch falls through to `default:` and drops the
     * unrecognized command -- so this is the ESP->Pico liveness heartbeat,
     * not a no-op. Deleting it would trip the Pico's own S6b LINK_DEAD
     * guard. What genuinely IS dead on the ESP side is only the "wait for a
     * matched reply" half of this exchange -- see safety_exchange()'s
     * (safety_link_inbox.c) and safety_link_status_wait_is_real_timeout()'s
     * (safety_link.h) own comments for that fix; this send itself stays
     * exactly as-is. */
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
        bool reannounce_owed = false;
        if (safety_lock(link)) {
            period = link->poll_period_ms;
            reannounce_owed = link->reannounce_pending;
            link->reannounce_pending = false;
            safety_unlock(link);
        }
        if (reannounce_owed) {
            /* A Pico boot_id change was observed by safety_apply_fw_version()
             * -- on WHATEVER task happened to be draining the inbox at the
             * time, possibly one with far less stack than this task's own
             * 8192 B -- and deferred here rather than sent from there. See
             * reannounce_pending's doc comment (safety_link.h) and
             * docs/audits/profile_executor_panic_2026-09-10_root_cause.md. */
            safety_link_send_announce_version_burst(link);
        }
        /* Same deferral, same reason, for safety_apply_diag()'s stale-S6a
         * boot-clear send -- see boot_clear_pending's doc comment
         * (safety_link.h). No-op whenever nothing is pending. */
        safety_link_service_boot_clear_if_pending(link);
        /* 2026-09-15 fix: heat_enable_release()'s REQUEST_ENABLE(false) send
         * is deferred off its caller's stack (profile_executor's, most
         * often -- see docs/audits/profile_executor_coredump_2026-09-15.md)
         * the same way boot_clear_pending is deferred off safety_apply_diag's.
         * No-op whenever nothing is pending; safe with no module lock held. */
        heat_enable_service_pending_release();
        /* H9 CT alarm: current with every relay off. Self-throttled, no lock held, no task of its own. */
        ct_leak_alarm_service(link);
        safety_poll_service_pico_half_recapture();
        safety_link_service_log_relay(link);
        /* SAFETY_FAULT_MIN_HOLD_MS's own comment (safety_link.h) -- finishes a
         * deassert deferred by safety_link_set_fault_source() once its hold
         * time has elapsed. Runs every pass, including the period==0 (polling
         * off) branch below, so a deferred release is never stranded just
         * because polling is disabled. No-op, cheap, whenever nothing is
         * pending. */
        safety_link_service_pending_fault_deassert(link);

        if (period == 0u) {
            /* Polling off: still drain anything the peer pushes unsolicited,
             * so an enabled-but-unpolled link isn't blind. */
            (void)safety_drain_inbox(link, SAFETY_LINK_IDLE_TICK_MS);
            continue;
        }

        TickType_t started = xTaskGetTickCount();
        esp_err_t poll_err = safety_exchange(link, request, sizeof(request), true);

        /* 2026-09-10, finding 5 (docs/audits/
         * safety_link_get_status_timeout_counter_2026-09-10.md follow-up
         * review): the real partial-loss signal, deliberately independent of
         * poll_err/no_reply_streak below -- those reflect only THIS
         * exchange's own phase-dependent ~345 ms wait, which is exactly what
         * made the original stats.timeouts counter useless (see that field's
         * own doc comment). This instead compares total applied STATUS
         * frames (frames_received) across the WHOLE iteration -- covering
         * this exchange's own wait plus every other drain that ran since the
         * previous iteration's snapshot (an opportunistic pre-drain, an
         * idle-tick drain) -- against the previous iteration's snapshot, so
         * it measures elapsed-time push throughput rather than this
         * request's own timing. See safety_link_status_push_gap_observed()'s
         * (safety_link.h) doc comment for the full reasoning. */
        uint32_t frames_received_now = 0;
        if (safety_lock(link)) {
            frames_received_now = link->stats.frames_received;
            safety_unlock(link);
        }
        if (link->push_gap_baseline_valid &&
            safety_link_status_push_gap_observed(link->push_gap_baseline_frames_received,
                                                  frames_received_now)) {
            if (safety_lock(link)) {
                link->stats.timeouts++;
                safety_unlock(link);
            }
        }
        link->push_gap_baseline_frames_received = frames_received_now;
        link->push_gap_baseline_valid = true;

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
            /* Same servicer as above -- a long poll period must not strand a
             * pending fault-hold release for the whole sleep_ms; this applies
             * it within one chunk (<=SAFETY_LINK_IDLE_TICK_MS) of its hold
             * time elapsing instead. */
            safety_link_service_pending_fault_deassert(link);
            vTaskDelay(pdMS_TO_TICKS(chunk_ms));
            remaining_ms -= chunk_ms;
        }
    }
}
