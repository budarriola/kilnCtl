// The isolated UART link (ESP32-S3 <-> RP2040 safety processor): lifecycle
// (safety_link_start()/safety_link_stop()), small shared helpers (locking,
// staleness/age arithmetic), the core public status/control API
// (get_status/request_enable/ping/set_poll_period/get_stats/peer_version_
// status/peer_build_status), and the isolated fault-line (GPIO6) management.
//
// This file used to be all of safety_link.c (4183 lines). It was split
// along the seams the file's own section-banner comments already named:
//   - safety_link_frames.c   -- frame decode/apply + outbound broadcast
//                                builders (ANNOUNCE_VERSION/PUSH_CONTEXT)
//   - safety_link_inbox.c    -- inbox draining, late-reply stashing, the
//                                request/reply exchange primitive
//   - safety_link_poll.c     -- the periodic poll task + link-health sync
//   - safety_link_commands.c -- the safety_link_send_*()/get_ct_cal()/
//                                get_config_page() command senders
//   - safety_link_payload.c  -- PC-facing payload serializers
// What stays here is the lifecycle and the small pieces every other file
// needs (safety_lock/safety_unlock -- now `static inline` in safety_link_
// internal.h -- safety_elapsed_ms/safety_age_ms_locked/safety_link_up_
// locked/safety_reset_stale_peer_info_if_link_down, all declared there and
// still DEFINED here), plus the parts of the public API that don't belong
// to any of the other seams.
//
// VERBATIM relocation for everything that moved: same bodies, same call
// sites, only where each is DEFINED (and, for a handful of small helpers
// now called across a file boundary, whether it is `static`) changed. See
// safety_link_internal.h's own header comment for the full list of symbols
// whose linkage changed and why.
#include "safety_link.h"
#include "safety_link_frame.h"
#include "safety_link_internal.h"
#include "safety_trip_decision.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "hal_esp_common.h"
#include "hal_gpio.h"
#include "hal_sysinfo.h"
#include "hal_time.h"
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

/* The safety link permanently owns UART1 (settings.h: SAFETY_UART_PORT_NUM).
 * If the PC link is also configured onto UART1 the second uart_driver_install
 * simply fails at runtime, which is a confusing way to discover a
 * menuconfig mistake -- so say it at compile time instead. */
#if CONFIG_KILNCTL_UART_PORT_1
#error "The PC link and the safety link cannot share UART1: set KILNCTL_UART_PORT_0 (see App/drivers/Kconfig)."
#endif

/* Inbox depth. The far side is expected to answer one request at a time; the
 * extra slots absorb an unsolicited push landing next to a poll reply.
 *
 * DELIBERATELY STILL 4, measured 2026-08-25. This constant looks like the
 * obvious fix for the `uart_proto: uart1: inbox full for task 7, BROADCAST
 * dropped` flood -- roughly 2/3 of everything the Pico sends is discarded
 * here -- and it is not. Two link-stat samples 28 s apart (calibrated against
 * the 2 Hz poll, so the window is real and not assumed):
 *
 *     frames deframed   +280  -> 10.0 /s   what the Pico produces
 *     dequeued           +76  ->  2.7 /s   what this driver consumes
 *     broadcast dropped +204  ->  7.3 /s
 *
 * That is SUSTAINED 3.7x overproduction, not burstiness. Depth only ever buys
 * burst tolerance: against a permanent surplus, a deeper queue drops the same
 * frames a few hundred ms later, having spent PSRAM to do it (these inboxes
 * moved to PSRAM on 2026-08-20, so the cost is cheap -- cheapness is not the
 * argument for raising it, futility is the argument against).
 *
 * Note also that only ~2.7/s of the 10/s is explained by the cmd histogram's
 * status+diag+power counts, and the histogram only counts frames that were
 * actually DEQUEUED. So something is producing ~7 frames/s that nothing ever
 * consumes, and identifying what is the real work here. The two levers that
 * would actually help are reducing what the Pico pushes, or draining this
 * inbox continuously instead of once per 500 ms poll -- NOT this number.
 *
 * If you are here because of the drop flood: re-measure those two rates
 * first. If production still exceeds consumption, raising this cannot help,
 * and changing it will only make the symptom quieter for one release.
 * See docs/ and the safety-link congestion notes; the poll-period backoff in
 * SAFETY_LINK_BACKOFF_MAX_STREAK is a related, separate mitigation. */
#define SAFETY_INBOX_LEN 4

/* Depth of the separate (ESP, UART_TASK_ID_LOG) inbox on this link -- see its
 * registration site below for why a second, tiny queue exists at all. Kept
 * deliberately small for the identical reason SAFETY_INBOX_LEN above is: a
 * log line is explicitly allowed to be lost (best-effort, never blocking the
 * Pico that logged it, LINK_PROTOCOL.md sec 6), so a deeper queue would only
 * spend PSRAM to delay the same drop, not prevent it. */
#define SAFETY_LOG_INBOX_LEN 4

/* 2026-08-28: measured, not guessed -- stack_margin_register("safety_poll",
 * ...) below now reports this task's real uxTaskGetStackHighWaterMark(). At
 * 4096 the live reading was 1192 B free (29.1% headroom, [LOW]) under
 * ordinary bench traffic -- no commissioning POST, no LCD interaction, just
 * the routine GET_STATUS/DIAG/POWER poll plus an occasional config refetch.
 * The task's own crash record (crash_report.c, decoded via
 * xtensa-esp32s3-elf-addr2line) shows exc_task='safety_poll',
 * cause=IllegalInstruction, pc inside panic_abort/esp_system_abort, reached
 * through vTaskSwitchContext/_frxt_dispatch/_frxt_int_exit -- exactly
 * FreeRTOS's CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY check at a
 * tick-driven context switch (sdkconfig: CONFIG_FREERTOS_CHECK_STACKOVERFLOW_
 * CANARY=y), not a task-WDT timeout (that fires from a timer ISR, not this
 * call chain) -- and the backtrace is flagged corrupted past that point,
 * which is what unwinding into stack-overflow-scrambled memory looks like.
 * This is a plain size increase, nothing about timing or blocking -- the one
 * kind of change this task's own history (this file's "two previous attempts
 * ... put the ESP into a panic-reboot loop" comment, a few hundred lines
 * below) does NOT warn against; only extending safety_poll's blocking budget
 * is off limits. PSRAM-backed (MALLOC_CAP_SPIRAM below), so the extra 4 KB
 * costs none of the ~205 KB of scarce internal DRAM this board is already
 * short on -- only external RAM, of which ~8 MB was free. */
#define SAFETY_POLL_TASK_STACK    8192
#define SAFETY_POLL_TASK_PRIORITY 5

/* Every fault source this driver recognizes. A caller may only set or clear
 * bits that appear here: an unknown bit set would latch the fault line with no
 * named owner able to release it, and -- far worse -- a wildcard clear such as
 * ~0u would release every *other* source's assertion as a side effect. The
 * whole point of the source mask is that no reason can drop another reason's
 * fault, so the mask itself has to be closed.
 *
 * KEEP THIS IN SYNC WITH safety_fault_source_t. Every bit the enum
 * defines must appear here, or set_fault_source() refuses it outright.
 * SAFETY_FAULT_SRC_THERMAL_SANITY was added to the enum but not to this
 * mask, which silently defeated it at both of its call sites
 * (profile_executor.c's escalate_guard_trip(), autotune_engine.c's
 * escalate_and_abort()) for every RUNAWAY/MAX_TEMP/MIN_TEMP trip: the
 * isolated fault line to the Pico was never asserted, fault_sources never
 * gained the bit so relay_authority_on_blocked() gave no re-arm backstop,
 * and dashboard_http.c's THERMAL_SANITY display branch was dead code.
 * The local relay force-off still happened, so this cost the
 * defense-in-depth path, not the primary one -- which is exactly why
 * nothing surfaced it. An omission here fails CLOSED at the mask and
 * OPEN at the hazard, so it cannot be caught by testing the happy path. */
#define SAFETY_FAULT_SRC_ALL                                                       \
    ((uint32_t)(SAFETY_FAULT_SRC_MANUAL | SAFETY_FAULT_SRC_PC_LINK |               \
                SAFETY_FAULT_SRC_THERMO | SAFETY_FAULT_SRC_SAFETY_LINK |           \
                SAFETY_FAULT_SRC_APP | SAFETY_FAULT_SRC_THERMAL_SANITY))

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

/* safety_read_f32_le/u32_le/u16_le, safety_put_f32_le/u16_le/u32_le moved to
 * safety_link_frame.c/.h (pure byte<->value marshalling, no locking/hardware
 * -- see that header's own comment). Called the same way from here. */

uint32_t safety_elapsed_ms(TickType_t since)
{
    /* Unsigned tick subtraction, so the 32-bit tick counter's eventual wrap
     * (~497 days at the default 100 Hz) produces the right delta rather than
     * a nonsense one. */
    TickType_t delta = xTaskGetTickCount() - since;
    return (uint32_t)delta * portTICK_PERIOD_MS;
}

/* See safety_link.h's doc comment on safety_link_await_poll_fn/
 * safety_link_await_result_t for the full contract. This is the whole
 * extraction: a plain "poll until PENDING stops, or time runs out" loop,
 * with zero domain knowledge -- every "what does ACKED/UNKNOWN mean here"
 * decision lives in the caller's poll_fn, not here. */
safety_link_await_result_t safety_link_await_or_unknown(uint32_t timeout_ms, uint32_t poll_interval_ms,
                                                          safety_link_await_poll_fn poll_fn, void *ctx)
{
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        safety_link_await_poll_t verdict = poll_fn(ctx);
        if (verdict != SAFETY_LINK_AWAIT_PENDING) {
            return verdict;
        }
        if (safety_elapsed_ms(start) >= timeout_ms) {
            return SAFETY_LINK_AWAIT_UNKNOWN;
        }
        vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
    }
}

/* ------------------------------------------------------------------------ */
/* Cache / staleness                                                        */
/* ------------------------------------------------------------------------ */

/* Age of the cached status in ms, saturating just below the reserved
 * "never received" value so the two can never be confused. */
uint16_t safety_age_ms_locked(const SafetyLinkClass *link)
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
bool safety_link_up_locked(const SafetyLinkClass *link)
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

/* Opus-review finding: "the boot_id evidence channel is single-shot and
 * unrecoverable". peer_version_known/pico_boot_id_known/peer_build_known are
 * each a "have we ever decoded a FW_VERSION frame this session" latch, set
 * once by safety_apply_fw_version() and, before this fix, never cleared --
 * not even when the link is later observed down. The Pico's boot-time
 * FW_VERSION push is unsolicited and (now) a 4-copy burst (SaftyFW's
 * link_task.c), but it is still sent into a link that has JUST come out of
 * reset; if the whole burst is lost -- or the Pico rebooted while this side
 * still held stale "known" flags from BEFORE the link dropped -- this side
 * would otherwise keep believing it already knows the peer's boot_id/build/
 * protocol version forever, and never re-request. Concretely, that turns a
 * rollback that fully succeeded into a permanent UNKNOWN_TIMEOUT:
 * safety_link_rollback_boot_id_changed() compares against a boot_id that is
 * still the PRE-rollback value, and nothing on this side is polling to
 * refresh it.
 *
 * Called once per poll iteration (safety_poll_task); clears the three flags
 * whenever safety_link_up_locked() reads false, so the poll loop's own
 * "!peer_version_known -> re-request FW_VERSION every period" branch
 * (SAFETY_LINK_BACKOFF_MAX_STREAK's comment) now also covers "was known, but
 * that peer went away and may be a different peer/boot by the time it's
 * back", not just "never known yet". Extracted as its own function (rather
 * than left inline in the poll loop) specifically so this decision is
 * host-testable from a plain SafetyLinkClass -- see test_safety_link_
 * compile.c -- without needing the FreeRTOS poll loop itself running. */
void safety_reset_stale_peer_info_if_link_down(SafetyLinkClass *link)
{
    if (!link || !safety_lock(link)) {
        return;
    }
    /* review LOW-3: the up window scales with poll_period_ms; the fixed SAFETY_LINK_STALE_MS bound the rest of
     * the firmware uses must also trigger the clear, so a slow-poll config cannot keep stale peer info. */
    uint32_t stale_bound = link->poll_period_ms * SAFETY_LINK_UP_PERIODS; /* review LOW-5: scale with the poll period */
    if (stale_bound < (uint32_t)SAFETY_LINK_STALE_MS) {
        stale_bound = (uint32_t)SAFETY_LINK_STALE_MS;
    }
    if (stale_bound > 0xFFFEu) {
        stale_bound = 0xFFFEu;
    }
    if (!safety_link_up_locked(link) || safety_link_is_stale(safety_age_ms_locked(link), (uint16_t)stale_bound)) {
        link->peer_version_known = false;
        link->pico_boot_id_known = false;
        link->peer_build_known = false;
        /* kilnlink review LOW-4: the DIAG uptime baseline must not survive an outage (one spanning the
         * 49.7-day wrap would read as a reboot). Kept on the SAME condition as pico_boot_id_known so the
         * two reboot signals reset together. */
        link->pico_uptime_baseline_known = false;
        link->diag_reannounce_count = 0u;
        link->diag_reannounce_last_ms = 0u;
    }
    safety_unlock(link);
}

/* Drives GPIO6 from the current source mask. High = fault asserted = U1's LED
 * lit = the Pico's mainFault input pulled low (docs/HARDWARE.md). Called with
 * state_lock held so the pin and the mask can't disagree. */
static void safety_apply_fault_locked(SafetyLinkClass *link)
{
    hal_gpio_set(link->fault_io, link->fault_sources != 0u);
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

    /* GPIO_IS_VALID_OUTPUT_GPIO's pre-check (a bad Kconfig value would
     * otherwise be a shift past the width of the type, and the fault line is
     * the last thing on this board that should be driven by accident) is no
     * longer done here with the ESP-specific macro -- hal_gpio.h has no
     * portable equivalent. hal_gpio_init_out() below performs the same
     * validation (ESP-IDF's gpio_config() rejects an out-of-range/input-only
     * pin with ESP_ERR_INVALID_ARG, mapped to HAL_INVALID_ARG) and bring-up
     * still fails before the fault line is ever driven, just slightly later
     * in the call -- same pattern as panel_spi_bringup.c's ILI9488_init(). */

    memset(link, 0, sizeof(*link));
    link->fault_io = SAFETY_FAULT_IO;
    link->poll_period_ms = (uint16_t)SAFETY_POLL_PERIOD_MS;
    link->fault_on_link_loss = true; /* fail-safe; see safety_link.h */
    /* 2026-09-15 (Opus review F3): SafetyLinkClass::tc_type_last_sent and its
     * "0xFF means nothing sent yet" init used to live here, for the now-
     * removed safety_sync_tc_type() push (safety_link_poll.c). Removed along
     * with it -- the field is gone entirely, not merely unused. */
    /* memset above zeroed link_reply_us_min, but 0 is not the honest starting
     * value -- a real min_us of 0 would
     * misreport a legitimate reply as taking no time at all, and would
     * never be overwritten by safety_exchange()'s "if elapsed < min" check
     * once latched at 0. See safety_link_stats_t::link_reply_us_min's doc
     * comment; MAX31856.c's s_read_all_min_us uses the same UINT32_MAX
     * sentinel for the identical reason. */
    link->stats.link_reply_us_min = UINT32_MAX;
    /* Diagnostic identity only (Phase 7b.2), same spirit as SaftyFW's own
     * s_boot_id (link_task.c: "not a security or safety value, so true
     * entropy is not required") -- but the ESP has a real hardware RNG
     * (esp_random(), backed by the SAR ADC/RF noise per esp_random.h, reached
     * through hal_sysinfo_random_u32() as of the 2026-09-06 hal_sysinfo
     * migration), so there is no reason to fall back to a time-derived
     * pseudo-random value the way the Pico does. Lets the Pico notice "the
     * ESP just rebooted" from ANNOUNCE_VERSION alone, without polling for it. */
    link->esp_boot_id = (uint8_t)hal_sysinfo_random_u32();
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
        link->cached.power_channel_counts_avg[ch] = 0u;
    }
    link->cached.power_counts_valid = false;
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
    /* Idle level (0, de-asserted) is latched BEFORE the pin switches to
     * output -- hal_gpio_init_out()'s contract (hal_gpio.h) -- so there is no
     * longer even the brief post-gpio_config() window the old two-call
     * sequence had before gpio_set_level(0) ran. */
    err = hal_status_to_esp_err(hal_gpio_init_out(link->fault_io, false));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fault line gpio init(%d) failed: %s", link->fault_io, esp_err_to_name(err));
        goto fail_locks;
    }

    err = uart_owner_init(&link->owner, SAFETY_UART_PORT_NUM, SAFETY_TX_IO, SAFETY_RX_IO,
                           SAFETY_UART_BAUD_RATE, UART_OWNER_QUEUE_LEN, UART_OWNER_TASK_PRIORITY,
                           UART_OWNER_STACK_SIZE, tskNO_AFFINITY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_owner_init(uart%d) failed: %s", SAFETY_UART_PORT_NUM, esp_err_to_name(err));
        goto fail_locks;
    }

    /* Register THIS uart_owner's two tasks for stack measurement, under names
     * that say which link they serve.
     *
     * There are two uart_owner instances on this board -- main.c's PC-link one
     * and this safety-link one -- created from the same
     * UART_OWNER_STACK_SIZE. Only main.c's pair was ever registered, so
     * /api/status reported two comfortable margins while their siblings, the
     * ones carrying the safety telemetry that gates all heating, were
     * measured by nobody. Anyone resizing that shared Kconfig on the numbers
     * they could see would have been resizing a task they could not.
     *
     * The same "two instances, one identifier" trap this project has already
     * been bitten by in the shared UART log tags -- so these names carry the
     * link, not just the role. Kept under STACK_MARGIN_NAME_MAX (20) so
     * the report does not truncate them into near-identical strings, which
     * would reintroduce the very ambiguity these names exist to remove. */
    /* safety_owner_task (the request-queue worker) deleted 2026-09-06 (uart
     * collapse) along with uart_owner_task()/uart_owner_transfer() --
     * uart_owner_t no longer has a task_handle field, only event_task_handle. */
    stack_margin_register("safety_owner_evt", &link->owner.event_task_handle, UART_OWNER_STACK_SIZE);

    /* No line inversion on this link any more -- deliberately, on both ends.
     *
     * 2026-08-25: the TCMT1109 optocoupler pair (U2/U3, with R7/R12/R15) was
     * replaced on the board by one ADuM1201WT digital isolator, U6 in
     * hardware/mainBoard/SaftyProcessor.kicad_sch. Two things about that part
     * decide this code:
     *
     *   - It is NON-inverting. Its own truth table is straight positive
     *     logic: a high at VIx is a high at the matching VOx. An optocoupler
     *     inverts (driver high lights the LED, which pulls the receiver's
     *     collector low); the ADuM1201 does not.
     *   - Each channel is unidirectional, one per direction. Channel B
     *     carries VIB (pin 3, DataToSafty, this pin) to VOB (pin 6, the
     *     Pico's RX); channel A carries VIA (pin 7, the Pico's TX) to VOA
     *     (pin 2, SAFETY_RX_IO).
     *
     * So the barrier is transparent in both directions and there is nothing
     * left for an inversion to cancel. Both ends used to invert their own TX
     * (UART_SIGNAL_TXD_INV here, gpio_set_outover() in SaftyFW's
     * uart_owner.c) precisely to cancel one opto inversion each. Keeping
     * either one now BREAKS the link rather than fixing it: TXD_INV makes
     * this pin idle low, the isolator passes the low through unchanged, and
     * the RP2040's RX sits in a permanent break.
     *
     * That is not a prediction, it is what the bench showed with the new
     * isolator fitted and TXD_INV still in place: sent 18, received 0,
     * crc/framing errors 33 and climbing, and the Pico's RX pad sampled low
     * on 2954 of 3000 reads.
     *
     * These two must change together -- if one end is ever reverted to
     * inverting, the other has to be too, and the part on the board decides
     * which is right. SaftyFW's bootloader recovery mode
     * (bootloader/main.c's enter_recovery) never inverted at all, so with
     * this change the application and the bootloader agree for the first
     * time; under the optocouplers recovery mode had the wrong polarity. */

    /* GPIO4 (net DataFromSafty) is now driven by U6 pin 2, VOA -- a push-pull
     * CMOS output, not an open collector. U3's phototransistor and R15's 1k
     * pull-up are both gone from the board, so nothing external defines this
     * net's level any more; the isolator drives it both ways, and its idle is
     * ordinary mark because the far end's TX idles mark and the barrier does
     * not invert.
     *
     * The internal pull-up is kept anyway, for the one window where VOA is
     * NOT driving: if the safety domain (VDD2) is unpowered the ADuM1201's
     * watchdog forces VOA high, but during the main domain's own power-up
     * there is a moment before U6 is out of reset. A pull-up makes that
     * window read as idle mark rather than as a break, which is the harmless
     * interpretation. It is belt-and-braces now rather than load-bearing --
     * under the optocouplers it was the only thing defining the level. */
    err = hal_status_to_esp_err(hal_gpio_set_pull(SAFETY_RX_IO, HAL_GPIO_PULL_UP));
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

    /* Same measurement gap the uart_owner pair had: main.c registered its
     * uart_proto_rx and this one went unregistered, so the report showed one
     * comfortable margin for a stack size that sizes two tasks. */
    stack_margin_register("safety_proto_rx", &link->proto.rx_task_handle, UART_PROTOCOL_STACK_SIZE);

    /* Registered on *this* protocol instance (the isolated link), not on the
     * PC link's -- same task_id, two separate address spaces. This is the
     * inbox the Pico's replies land in. */
    err = uart_protocol_register_task(&link->proto, UART_TASK_ID_SAFETY, SAFETY_INBOX_LEN,
                                       &link->inbox);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register safety task failed: %s", esp_err_to_name(err));
        goto fail_proto;
    }

    /* SAFETY_CMD_... aside: task_id 5 is UART_TASK_ID_LOG, not a SAFETY_CMD
     * at all -- the Pico's log_task (firmware/SaftyFW/src/tasks/log_task.c)
     * addresses its console lines here with the SAME payload shape this
     * board's own uart_log_bridge.c uses for the PC link (CommonFW/docs/
     * LINK_PROTOCOL.md sec 6, "Frame F"). Before this registration, a frame
     * addressed to (ESP, task 5) on the isolated link hit no registered slot
     * and was silently discarded by uart_protocol_rx_task() -- every Pico
     * log line was lost. A tiny inbox (matches SAFETY_INBOX_LEN's own
     * congestion reasoning above): log_task is the lowest-priority task in
     * the system and this is best-effort by design (LINK_PROTOCOL.md's two
     * "non-negotiable" rules for Frame F) -- a full inbox drops the newest
     * BROADCAST via uart_protocol's own broadcast_dropped counter, which is
     * the correct outcome, not a bug to fix by growing this number. */
    /* NOT fatal, deliberately -- unlike UART_TASK_ID_SAFETY above. This inbox
     * carries nothing but best-effort console text; the transient internal-
     * SRAM/PSRAM exhaustion uart_protocol_register_task() documents (and
     * retries five times through) during Wi-Fi bring-up must never be able to
     * take the SAFETY LINK ITSELF down as collateral. On failure the link
     * comes up fully with log_inbox left NULL --
     * safety_link_service_log_relay() (safety_link_poll.c) checks for exactly
     * that and does nothing, so this boot simply relays no Pico log lines. */
    err = uart_protocol_register_task(&link->proto, UART_TASK_ID_LOG, SAFETY_LOG_INBOX_LEN,
                                       &link->log_inbox);
    if (err != ESP_OK) {
        link->log_inbox = NULL; /* register_task leaves *out untouched on failure */
        ESP_LOGE(TAG, "register safety log task failed: %s -- Pico log relay disabled this "
                      "boot, safety link itself unaffected", esp_err_to_name(err));
        err = ESP_OK;
    }

    /* Set before the task exists, not after: the poll task calls back into the
     * public API (safety_link_set_fault_source), which refuses to touch an
     * uninitialized link -- so the flag has to be true by the time it runs its
     * first iteration, not merely by the time start() returns. */
    link->initialized = true;
    /* 2026-08-22: PSRAM stack. safety_poll_task talks to the RP2040 only
     * through uart_protocol.c's hal_uart_send_blocking path, on the same
     * hal_uart_t owner->hal already installed (hal_uart_attach deleted,
     * docs/HW_ABSTRACTION.md); uart_owner.c owns the actual UART driver
     * install and keeps its own internal stack/task for the RX event side.
     * This task itself
     * never calls into flash/NVS -- per this file's own top-of-file comment,
     * "nothing reaches its flash until safety_link_send_commit_config()",
     * which is called by an HTTP handler, not from this poll loop. */
    if (xTaskCreatePinnedToCoreWithCaps(safety_poll_task, "safety_poll", SAFETY_POLL_TASK_STACK, link,
                                        SAFETY_POLL_TASK_PRIORITY, &link->poll_task, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "failed to create safety poll task");
        link->initialized = false;
        err = ESP_ERR_NO_MEM;
        goto fail_task;
    }
    /* 2026-08-28: measurement, not a fix -- see stack_margin.h's top comment.
     * safety_poll gained real work today (safety_cfg_store_maybe_refetch()'s
     * scratch safety_cfg_store_blob_t and kilnlink_config_page_t locals, both
     * now on this task's own frame) without ever having been registered here,
     * so nothing on the PC side could read its live high-water mark -- the
     * exact gap this file's own comment two registrations up (uart_proto_rx)
     * already flagged for a sibling task. */
    stack_margin_register("safety_poll", &link->poll_task, SAFETY_POLL_TASK_STACK);

    ESP_LOGI(TAG,
             "safety link up on uart%d (tx=%d rx=%d, %u baud, non-inverting "
             "across U6/ADuM1201), fault out=gpio%d, poll=%ums",
             SAFETY_UART_PORT_NUM, SAFETY_TX_IO, SAFETY_RX_IO,
             (unsigned)SAFETY_UART_BAUD_RATE, link->fault_io,
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

esp_err_t safety_link_get_diag_flags(SafetyLinkClass *link, bool *out_ever_received, uint8_t *out_flags)
{
    if (!link || !out_ever_received || !out_flags) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!safety_lock(link)) {
        return ESP_FAIL;
    }
    *out_ever_received = link->cached.diag_ever_received;
    *out_flags = link->cached.diag_flags;
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

    /* Refuse when no peer is there, instead of reporting success into the
     * void.
     *
     * REQUEST_ENABLE elicits no reply frame of its own, and since this call
     * site moved from the ACK'd DATA transport to BROADCAST (see the
     * "All four of these call sites had the same bug" comment on
     * safety_link_commit_config() above) there is no ACK either. So
     * safety_exchange() below now returns ESP_OK the moment the LOCAL UART
     * accepts the bytes -- with the Pico unpowered, unflashed, or sitting in
     * its bootloader, this function still answered ESP_OK. Confirmed on the
     * bench 2026-08-24 with the RP2040 held in reset: safety_request_enable()
     * reported success every time.
     *
     * That is the dangerous direction for this particular call. Every other
     * function here reports a missing peer honestly (a stale cache with
     * link_up = 0, per safety_link_get_status()'s contract); this one claimed
     * the safety processor had been asked to permit heating when nothing had
     * been asked anything. The header's promise of ESP_ERR_TIMEOUT "if the
     * far side never ACKed" silently became unimplementable when the ACK went
     * away, and was not updated with it.
     *
     * There is nothing to wait for, so the peer cannot be confirmed
     * positively. What CAN be checked, without blocking and without sending
     * anything, is the same cached link_up the rest of the driver already
     * treats as the authority on "is a safety processor answering": it is
     * maintained by the poll task from real received status frames and takes
     * a sustained silence to clear (SAFETY_LINK_UP_PERIODS), so it does not
     * flap on one dropped reply. Down means the last several polls got
     * nothing back, which is exactly the case this must not report as
     * success.
     *
     * Deliberately NOT the disable direction's problem too: enable=false is
     * allowed through regardless. Telling a peer that may or may not be
     * listening to STOP heating is the fail-safe direction, and refusing it
     * because the link looks down would be the one refusal that could leave
     * heat on. */
    if (enable) {
        bool peer_answering = false;
        if (!safety_lock(link)) {
            return ESP_FAIL;
        }
        peer_answering = safety_link_up_locked(link);
        safety_unlock(link);
        if (!peer_answering) {
            ESP_LOGW(TAG, "request_enable(1) refused: no safety processor answering "
                          "(link down) -- nothing was sent");
            return ESP_ERR_INVALID_STATE;
        }
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
    /* Same GET_STATUS send as safety_poll_task() (safety_link_poll.c) --
     * DO NOT DELETE as dead code; see that call site's comment. The Pico
     * never answers it, but decoding it still refreshes the Pico's own
     * link-liveness timestamp before its dispatch switch drops it, so this
     * is a real ESP->Pico heartbeat, not a no-op. */
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
    /* Same "report 0, not the UINT32_MAX sentinel, before any sample has
     * landed" convention as MAX31856_get_read_all_stats() -- see this
     * struct field's own doc comment. */
    if (out->link_reply_us_count == 0u) {
        out->link_reply_us_min = 0u;
    }
    safety_unlock(link);
    /* uart_owner counts the physical line errors (break/parity/frame and ring
     * overflows) for this port; they are the same class of problem as a
     * payload this driver had to reject, so the PC sees one number. Read
     * outside the lock -- it's a plain volatile counter owned by the UART
     * event task. */
    out->frame_errors += uart_owner_get_rx_error_count(&link->owner);
    /* Read outside state_lock, same reasoning as the rx_error_count call
     * just above: this is a plain counter owned by uart_protocol's own
     * tasks_lock, not this driver's state. Best-effort -- if task 7
     * somehow isn't registered (should not happen after safety_link_start()
     * has run), broadcast_dropped is simply left at whatever *out already
     * had (0, from the *out = link->stats copy above) rather than treated
     * as a hard failure of the whole stats read. */
    (void)uart_protocol_get_task_broadcast_dropped(&link->proto, UART_TASK_ID_SAFETY,
                                                    &out->broadcast_dropped);
    /* Deframer/dispatch-level counters, same best-effort/read-outside-lock
     * reasoning as broadcast_dropped just above -- these live on the
     * uart_protocol_t instance itself (per-port, not per-task), see that
     * struct's own doc comment. */
    (void)uart_protocol_get_deframe_stats(&link->proto, &out->frames_deframed,
                                          &out->frames_routed_nowhere,
                                          &out->frame_length_mismatch,
                                          &out->frame_crc_mismatch, &out->frame_resync);
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

/* TODO.md owner-report item 5 -- see safety_link.h's doc comment for the
 * "known" gating rule. Copies commit/datetime out with explicit lengths
 * (the wire strings are not null-terminated); commit_buf/datetime_buf may be
 * NULL if the caller doesn't want them, same as any other optional out
 * pointer here, but if non-NULL must have room for 64/32 bytes. */
esp_err_t safety_link_get_peer_build_status(SafetyLinkClass *link, bool *out_known,
                                             bool *out_dirty, uint8_t *commit_buf,
                                             uint8_t *out_commit_len, uint8_t *datetime_buf,
                                             uint8_t *out_datetime_len,
                                             uint8_t *out_config_version,
                                             uint16_t *out_config_crc)
{
    if (!link || !out_known) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!safety_lock(link)) {
        return ESP_FAIL;
    }
    *out_known = link->peer_build_known;
    if (out_dirty) {
        *out_dirty = link->peer_build_dirty;
    }
    if (commit_buf) {
        memcpy(commit_buf, link->peer_build_commit, sizeof(link->peer_build_commit));
    }
    if (out_commit_len) {
        *out_commit_len = link->peer_build_commit_len;
    }
    if (datetime_buf) {
        memcpy(datetime_buf, link->peer_build_datetime, sizeof(link->peer_build_datetime));
    }
    if (out_datetime_len) {
        *out_datetime_len = link->peer_build_datetime_len;
    }
    if (out_config_version) {
        *out_config_version = link->peer_config_version;
    }
    if (out_config_crc) {
        *out_config_crc = link->peer_config_crc;
    }
    safety_unlock(link);
    return ESP_OK;
}

/* 2026-09-24 fault-edge instrumentation (safety_link.h's safety_fault_edge_t
 * comment). Called only from inside safety_link_set_fault_source(), already
 * holding state_lock, with before/after already computed and known to
 * differ -- never logs (COMMON.md/CLAUDE.md: never log inside a spinlock),
 * only records. */
static void safety_record_fault_edge_locked(SafetyLinkClass *link, uint32_t before, uint32_t after)
{
    uint32_t rising = after & ~before;
    uint8_t first_set_bit = 0xFFu;
    for (uint8_t b = 0; b < SAFETY_LINK_FAULT_SRC_BIT_COUNT; b++) {
        uint32_t bit = (1u << b);
        if ((rising & bit) == 0u) {
            continue;
        }
        if (first_set_bit == 0xFFu) {
            first_set_bit = b;
        }
        if (link->fault_edge_rising_count[b] < UINT16_MAX) {
            link->fault_edge_rising_count[b]++;
        }
        link->fault_edge_last_rising_uptime_ms[b] =
            (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        link->fault_edge_last_rising_valid[b] = true;
    }

    safety_fault_edge_t *e = &link->fault_edge_ring[link->fault_edge_ring_head];
    e->uptime_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    time_t now = time(NULL);
    /* time(NULL) before SNTP sync reads as a small epoch offset (ESP-IDF
     * boots the RTC near 0), not a plausible 2020s+ date -- same "treat
     * anything before 2020-01-01 UTC as unsynced, store 0" convention as
     * profile_executor_run.c's run_started_unix_s. */
    e->unix_time_s = (now >= (time_t)1577836800) ? (uint32_t)now : 0u;
    e->source_mask_before = (uint8_t)before;
    e->source_mask_after = (uint8_t)after;
    e->first_set_bit = first_set_bit;

    link->fault_edge_ring_head = (link->fault_edge_ring_head + 1u) % SAFETY_LINK_FAULT_EDGE_RING_LEN;
    if (link->fault_edge_ring_count < SAFETY_LINK_FAULT_EDGE_RING_LEN) {
        link->fault_edge_ring_count++;
    }
    link->fault_edge_total_recorded++;
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
    uint64_t now_ms = hal_time_now_ms();
    if (assert_fault) {
        /* Re-assert of a bit already pending deassert simply cancels the
         * pending release -- the line never moved, so there is nothing to
         * restart, and its original assert timestamp stays put (SAFETY_FAULT_
         * MIN_HOLD_MS's own comment). A bit that is genuinely newly asserted
         * (was 0 in fault_sources) gets a fresh timestamp. */
        uint32_t newly_asserted = source_mask & ~link->fault_sources;
        for (uint8_t b = 0; b < SAFETY_LINK_FAULT_SRC_BIT_COUNT; b++) {
            if ((newly_asserted & (1u << b)) != 0u) {
                link->fault_assert_tick_ms[b] = now_ms;
            }
        }
        link->fault_sources |= source_mask;
        link->fault_pending_deassert_mask &= ~source_mask;
    } else {
        /* Per-bit: a bit that has not yet been continuously asserted for
         * SAFETY_FAULT_MIN_HOLD_MS is deferred (parked in fault_pending_
         * deassert_mask, still counted asserted in fault_sources) rather than
         * released immediately -- see that constant's comment in safety_link.h.
         * safety_link_service_pending_fault_deassert() finishes it later, from
         * the poll tick, once the hold time elapses. */
        uint32_t to_release_now = 0u;
        for (uint8_t b = 0; b < SAFETY_LINK_FAULT_SRC_BIT_COUNT; b++) {
            uint32_t bit = (1u << b);
            if ((source_mask & bit) == 0u || (link->fault_sources & bit) == 0u) {
                continue;
            }
            uint64_t elapsed = now_ms - link->fault_assert_tick_ms[b];
            if (elapsed >= SAFETY_FAULT_MIN_HOLD_MS) {
                to_release_now |= bit;
            } else {
                link->fault_pending_deassert_mask |= bit;
            }
        }
        link->fault_sources &= ~to_release_now;
        link->fault_pending_deassert_mask &= ~to_release_now;
    }
    uint32_t after = link->fault_sources;
    if (after != before) {
        safety_record_fault_edge_locked(link, before, after);
    }
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

void safety_link_service_pending_fault_deassert(SafetyLinkClass *link)
{
    /* SAFETY_FAULT_MIN_HOLD_MS's own comment (safety_link.h): finishes a
     * deassert safety_link_set_fault_source() deferred. Called from the poll
     * task on every pass (safety_link_poll.c) -- never blocks, never sleeps,
     * takes state_lock only for the short, bounded body below. */
    if (!link || !link->initialized) {
        return;
    }
    if (!safety_lock(link)) {
        return;
    }
    if (link->fault_pending_deassert_mask == 0u) {
        safety_unlock(link);
        return;
    }
    uint64_t now_ms = hal_time_now_ms();
    uint32_t before = link->fault_sources;
    uint32_t released = 0u;
    for (uint8_t b = 0; b < SAFETY_LINK_FAULT_SRC_BIT_COUNT; b++) {
        uint32_t bit = (1u << b);
        if ((link->fault_pending_deassert_mask & bit) == 0u) {
            continue;
        }
        uint64_t elapsed = now_ms - link->fault_assert_tick_ms[b];
        if (elapsed >= SAFETY_FAULT_MIN_HOLD_MS) {
            released |= bit;
        }
    }
    link->fault_sources &= ~released;
    link->fault_pending_deassert_mask &= ~released;
    uint32_t after = link->fault_sources;
    if (after != before) {
        safety_record_fault_edge_locked(link, before, after);
    }
    safety_apply_fault_locked(link);
    safety_unlock(link);

    if (after != before) {
        ESP_LOGW(TAG, "isolated fault line released after minimum hold time (sources 0x%02X -> 0x%02X)",
                 (unsigned)before, (unsigned)after);
    }
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

esp_err_t safety_link_get_fault_edges(SafetyLinkClass *link, safety_fault_edge_snapshot_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!safety_lock(link)) {
        return ESP_FAIL;
    }
    out->count = link->fault_edge_ring_count;
    out->total_recorded = link->fault_edge_total_recorded;
    /* Rotate ring[] into entries[] oldest-first so the caller never has to
     * reason about where the write head currently sits. When the ring has
     * not wrapped yet (count < RING_LEN), the oldest entry is simply index 0
     * and head is one past the newest -- the same formula below reduces to
     * that case correctly since (head - count) wraps to 0 via the unsigned
     * modulo when head == count. */
    for (uint32_t i = 0; i < out->count; i++) {
        uint32_t src = (link->fault_edge_ring_head + SAFETY_LINK_FAULT_EDGE_RING_LEN - out->count + i) %
                       SAFETY_LINK_FAULT_EDGE_RING_LEN;
        out->entries[i] = link->fault_edge_ring[src];
    }
    memcpy(out->counts.rising_count, link->fault_edge_rising_count, sizeof(out->counts.rising_count));
    memcpy(out->counts.last_rising_uptime_ms, link->fault_edge_last_rising_uptime_ms,
           sizeof(out->counts.last_rising_uptime_ms));
    memcpy(out->counts.last_rising_valid, link->fault_edge_last_rising_valid,
           sizeof(out->counts.last_rising_valid));
    safety_unlock(link);
    return ESP_OK;
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
