// boot_guard -- the "did the last N boots actually come up healthy" counter
// behind the owner's watchdog-recovery request (ROADMAP.md: "a watchdog such
// that if the processor locks up for more than 5 minutes it enters a
// WiFi-enabled OTA mode for recovery").
//
// WHY THIS EXISTS AND WHAT IT CANNOT DO
// -----------------------------------------------------------------------
// A CPU that is genuinely wedged -- spinning with interrupts disabled, or
// stuck in a fault loop -- cannot run application code to "enter OTA mode".
// Every workable path here resets the chip first (CONFIG_ESP_TASK_WDT_PANIC,
// the RTC watchdog in rtc_watchdog.h, or a plain crash) and makes the
// recovery decision on the NEXT boot. This module is that decision: it
// counts boots that have not yet been confirmed healthy, and if that count
// crosses RECOVERY_MODE_BOOT_THRESHOLD, the next boot comes up in recovery
// mode -- relays latched off, Wi-Fi + the OTA HTTP routes served, nothing
// else -- so an operator can flash a fix instead of the board reset-looping
// forever silently.
//
// Same NVS-struct-with-a-version-and-a-guard-against-corruption convention
// as run_state.c (own key, own namespace-adjacent partition, load-tolerant:
// a missing/corrupt/wrong-version record is treated as "count 0", never a
// reason to fail bring-up -- see run_state.h's identical reasoning). Unlike
// run_state.c, this record also carries a CRC32 over its own bytes: a
// silently-corrupted boot counter is worse than a silently-corrupted run
// breadcrumb, because it can either wrongly force recovery mode (a false
// "you're stuck" that costs nothing but confusion) or -- far worse -- wrongly
// suppress it (a genuinely reset-looping board with a corrupted-to-zero
// counter would never recover). A version/size check alone (run_state.c's
// bar) does not catch a bit flip that leaves both intact; a CRC does.
//
// HOW THE COUNTER MOVES
// -----------------------------------------------------------------------
// boot_guard_init() increments the persisted count and persists the new
// value immediately, before anything else in this boot's app_main() can
// crash -- an increment that only lived in RAM until some later "healthy"
// point would never survive the very boot it exists to detect. It must run
// AFTER the relays are latched off (kiln_io_init()) and before any subsystem
// that could plausibly wedge the board, so a lockup anywhere downstream of
// it is counted.
//
// boot_guard_mark_healthy() clears the count to 0. main.c calls this from
// the SAME place ota_rollback_confirm_task() already decides the app is
// alive (NVS readable + web server up), NOT from that decision's full bar of
// also requiring a live safety-link frame exchange -- see main.c's call site
// for why: this board is currently run with its safety link permanently
// down (no RP2040 answering), and requiring link_up here would mean the
// counter can never clear on such a board, walking it into recovery mode
// after RECOVERY_MODE_BOOT_THRESHOLD ordinary reboots. See main.c's comment
// at the call site for the exact condition used instead.
#ifndef BOOT_GUARD_H
#define BOOT_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Consecutive un-confirmed-healthy boots before the NEXT boot comes up in
 * recovery mode instead of its normal bring-up. 3 gives a board that is
 * merely slow to reach "healthy" (e.g. Wi-Fi taking a few tries to
 * associate) room to recover on its own before recovery mode kicks in, while
 * still catching a genuine boot loop within a few reset cycles rather than
 * dozens. */
#define RECOVERY_MODE_BOOT_THRESHOLD 3

// Recovery mode is ARMED (2026-08-22). Set to 0 to build it in but never
// enter it -- the counter, the threshold logic and the reporting all stay
// live either way; only ENTERING the mode is gated by this.
//
// It was briefly shipped disabled, and the reason is worth keeping: forcing
// recovery_mode true on the bench showed the mode did not survive boot.
// Recovery mode skips the profile executor, the autotune engine and the
// rules task, but code that DOES still run went on calling into them and
// asserted on mutexes those modules create in their _start():
//
//   1. ui_page_home.c, building the LCD home screen:
//        assert failed: xQueueSemaphoreTake queue.c:1709 (( pxQueue ))
//      Fixed by not starting LVGL in recovery mode (see main.c).
//   2. Then a SECOND, different task at the same assert. The crash_report/
//      coredump path caught that one with a clean backtrace and addr2line
//      named it exactly:
//        safety_poll_task              (safety_link.c:1414)
//        safety_build_and_send_context (safety_link.c:527)
//        profile_executor_get_status   (profile_executor.c:2333)
//        xQueueSemaphoreTake           -- on the executor's NULL mutex
//      i.e. the safety link's own poll task asks the executor for status
//      every cycle, and recovery mode never started the executor.
//
// Two found by accident meant the fix was not a list of call sites to gate
// but making those modules safe to call before their _start(). That is now
// done: every public entry point in profile_executor.c and autotune_engine.c
// guards on its NULL mutex and returns a clean "not running" answer instead
// of asserting (rules_task.c needed no guard -- it holds no mutex), each
// covered by its own host test in App/test/test_*_prestart.c.
//
// Verified on hardware after that hardening, with recovery_mode forced true:
// the board came up, served /ota, /api/ota/challenge, /diagnostics and
// /api/status, held 200s for two solid minutes, kept every relay off, and
// answered a profile start with "profile executor not started" rather than
// panicking. POST /api/ota/esp/recovery_exit was exercised in the same boot
// and rebooted the board on a correctly signed request.
//
// If you change what recovery mode skips, re-run that bench check before
// trusting it again: force *out_recovery_mode = true in boot_guard.c's
// next_boot_count(), build, flash, and confirm the board serves the OTA
// routes and stays up for several minutes. A recovery mode that panic-loops
// is strictly worse than not having one -- it is precisely the failure the
// feature exists to prevent.
#define RECOVERY_MODE_ENABLED 1

/* Must be called exactly once per boot, early in app_main() -- AFTER
 * kiln_io_init() has latched every relay off (this module's own increment
 * must never be the reason relays come up in an unknown state), and BEFORE
 * any subsystem that could plausibly hang. Brings up its own NVS partition
 * handle (same "kiln_nvs" partition as run_state.c/relay_cycles.c),
 * increments the persisted "unconfirmed" counter, and persists the new
 * value before returning. Safe to call more than once (only the first call
 * in a boot does anything -- see boot_guard.c), but every real call site
 * should call it exactly once.
 *
 * Returns ESP_OK even when the underlying NVS write failed -- same
 * load/store-tolerant convention as run_state.c/relay_cycles.c: a lost
 * counter write costs one boot's worth of recovery-tracking accuracy, never
 * a reason to fail app_main. The failure is logged loudly regardless,
 * because a boot-guard that silently stops counting is exactly the failure
 * mode that would leave a genuinely reset-looping board never entering
 * recovery. */
esp_err_t boot_guard_init(void);

/* True if boot_guard_init() found the counter already at or above
 * RECOVERY_MODE_BOOT_THRESHOLD when this boot started (i.e. the threshold
 * had already been crossed by prior boots, BEFORE this boot's own
 * increment) -- main.c reads this once, right after boot_guard_init(), to
 * decide whether to skip starting profile_executor/autotune_engine/
 * rules_task this boot. Stable for the life of the boot; it does not change
 * when boot_guard_mark_healthy() is later called (that only affects the
 * NEXT boot). */
bool boot_guard_is_recovery_mode(void);

/* Clears the persisted counter to 0 -- the board is confirmed to have
 * booted successfully enough that continuing to reset-loop is no longer a
 * live risk. Does NOT retroactively change boot_guard_is_recovery_mode()'s
 * answer for the boot that is currently running -- see that function's doc
 * comment.
 *
 * Returns true only once the clear has been READ BACK and verified to be
 * actually 0 in NVS -- not merely once the write call reported success.
 * This distinction is the whole point (2026-09-08 recovery-loop audit,
 * docs/audits/boot_guard_recovery_loop_2026-09-08.md): hal_kv_set_blob()/
 * hal_kv_commit() reporting HAL_OK is necessary but was found NOT to be
 * sufficient evidence the byte pattern a later boot reads back actually
 * changed -- on real hardware the persisted "unconfirmed boot" count was
 * observed to stay pinned at its pre-clear value across every boot even
 * though this function's write calls returned HAL_OK every time and set
 * healthy_marked=true in RAM (confirmed via a live JTAG read of s_bg mid-
 * boot). Trusting the write's return code alone is exactly the "logging
 * unchecked success" bug class this project has hit before elsewhere
 * (CLAUDE.md's "Safety calls logging unchecked success" entry) -- here the
 * return code itself was unreliable, not merely uninspected, so the fix is
 * a read-back, not just a tighter `if`.
 *
 * NOT idempotent in the old "only tries once" sense: unlike before, a
 * caller whose first attempt fails verification SHOULD call this again
 * (main_network_http.c's main_ota_rollback_confirm_task() does, on its
 * existing poll cadence, instead of giving up after one try) -- see that
 * call site. Once verified, further calls are cheap no-ops (checks
 * s_bg.healthy_marked before touching NVS again). */
bool boot_guard_mark_healthy(void);

/* Explicit "this was a deliberate flash, not a boot that proved itself
 * healthy" reset -- docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md.
 *
 * boot_confirm_is_healthy() (and therefore boot_guard_mark_healthy()) is
 * gated on nvs_report_capture()'s ONE-SHOT snapshot of three NVS partitions
 * taken once, early, and never re-sampled for the rest of the boot -- see
 * that module's own header. A board that samples that snapshot during a
 * genuinely transient window (most plausibly moments after flash_firmware()
 * resets the chip, before every partition has finished mounting) never gets
 * a second chance to confirm healthy that boot, even though nothing is
 * actually wrong with it: this module's own counter still increments and
 * persists normally (it has its own, independent NVS partition handle --
 * see boot_guard_init()), so a run of ordinary development flashes can walk
 * a perfectly good board into RECOVERY_MODE_BOOT_THRESHOLD for a reason that
 * has nothing to do with whether the firmware can boot.
 *
 * This function exists for a TOOL that knows it just performed a deliberate
 * flash (flash_firmware()'s verify step, once it confirms the new build is
 * actually running) to say so directly, bypassing that flaky auto-health
 * snapshot entirely. Same verified-clear-with-retry contract as
 * boot_guard_mark_healthy() (returns true only once the clear is read back
 * confirmed, not merely written), and arms the SAME RTC stuck-counter marker
 * boot_guard_counter_is_stuck() reads -- both functions share one
 * clear-and-arm implementation, so there is exactly one "did the last clear
 * verify" fact carried into the next boot, never two independently-tracked
 * ones that could disagree and leave a stuck counter with no path to clear
 * it. Unlike boot_guard_mark_healthy(), does NOT require boot_guard_init()
 * to have run this boot and does NOT short-circuit on healthy_marked --
 * a deliberate reset is not "the same event" as an automatic confirmation,
 * even though both end up doing the same NVS write.
 *
 * Does NOT change boot_guard_is_recovery_mode()'s answer for the boot this
 * is called from (same as boot_guard_mark_healthy() -- that decision was
 * already made, once, at boot_guard_init()); it only affects the NEXT boot.
 * A genuinely failing board that is never deliberately flashed again is
 * completely unaffected: nothing calls this function on its behalf, so its
 * counter keeps climbing and it still trips recovery mode after
 * RECOVERY_MODE_BOOT_THRESHOLD boots exactly as before. */
bool boot_guard_reset_counter(void);

/* For diagnostics/tests: the persisted "unconfirmed" count as loaded (or
 * incremented to) this boot. */
uint32_t boot_guard_get_boot_count(void);

/* Read-only re-read of the count actually sitting in NVS right now (as
 * opposed to boot_guard_get_boot_count(), which is this BOOT's in-RAM
 * count -- fixed at boot_guard_init() time and never re-read from flash
 * afterward). This is what a caller needs to tell "the persisted counter
 * was really cleared just now" apart from "this boot's own count, which was
 * 1 before either function existed and always will be right after a fresh
 * boot" -- the confusion the boot_guard_reset route's response caused before
 * this accessor existed (a "cleared and verified" reset reporting the same
 * boot_count=1 it always does, indistinguishable from a no-op to a caller
 * reading the JSON alone).
 *
 * Read-only: touches a strict NVS read only, never a write path -- but it
 * still requires an internal-RAM task stack, same as
 * boot_guard_mark_healthy()/boot_guard_reset_counter()'s write paths. A
 * flash read disables the cache exactly as a flash write does, so a
 * PSRAM-stacked task is NOT safe here either; this call must not be made
 * from one.
 *
 * Returns false (leaving *out_count untouched) if out_count is NULL,
 * boot_guard_init() has not yet run this boot (no lock, no partition handle
 * to read from), or the record could not be read back cleanly (an open/get
 * error other than "not found", a length mismatch, or a failed CRC/version
 * check) -- unlike load_count(), this does NOT collapse a read failure to 0,
 * since doing so would fabricate a "persisted_count":0 that a caller (e.g.
 * the boot_guard_reset HTTP route) could easily read as a confirmed clear.
 * Otherwise returns true and sets *out_count to the value actually read from
 * NVS right now; 0 still merges two distinct states that both read as "0"
 * (a verified clear, and a record that has genuinely never been written) --
 * this call can tell "unreadable" apart from "0", but not "cleared" apart
 * from "never written". */
bool boot_guard_get_persisted_count(uint32_t *out_count);

/* STUCK-COUNTER ESCAPE -- see the long comment of the same name in
 * boot_guard.c for the hardware evidence behind this.
 *
 * Pure predicate, no I/O, exposed for test_boot_guard.c. Answers: "did the
 * PREVIOUS boot verify a clear to 0, and did this boot nevertheless load a
 * non-zero count?" -- i.e. is the persisted record refusing to change in
 * flash while every in-boot API call and read-back reports success?
 *
 * `rtc_magic`/`rtc_marked_healthy` come from RTC slow memory, which is NOT
 * NVS and NOT zeroed on a software reset (it IS lost on a power cycle,
 * which just means the escape re-arms from scratch after a power cycle --
 * one extra recovery-mode boot at worst, never a permanent trap).
 *
 * A true answer means the boot-guard counter cannot do its job and must not
 * be allowed to hold the board in recovery mode; boot_guard_init() then
 * treats the loaded count as 0. This can never release a genuinely
 * reset-looping board: rtc_marked_healthy is only ever set by a boot that
 * reached boot_confirm_is_healthy() AND verified its own clear, which is
 * exactly the set of boots whose counter the existing code already zeroes. */
bool boot_guard_counter_is_stuck(uint32_t rtc_magic, uint32_t rtc_marked_healthy,
                                 uint32_t loaded_count);

/* THE single definition of "this boot is healthy enough to (a) cancel OTA
 * rollback (esp_ota_mark_app_valid_cancel_rollback()) and (b) clear this
 * module's boot-guard counter (boot_guard_mark_healthy())". main.c's
 * ota_rollback_confirm_task() calls this ONE function for both decisions --
 * see that call site for why two separately-invented definitions of
 * "healthy" was the actual bug being fixed here, 2026-08-22.
 *
 * DELIBERATELY DOES NOT TAKE A SAFETY-LINK ARGUMENT.
 * -----------------------------------------------------------------------
 * The bar used to also require safety_link_get_status()->link_up, live,
 * every poll. On a board with no RP2040 attached or answering -- which is
 * this project's own bench board, right now -- that condition is never
 * true, for the life of the boot, no matter how long the polling loop
 * waits. Two things depended on that same bar: OTA rollback confirmation
 * (so a freshly OTA'd image never left PENDING_VERIFY, and the bootloader
 * silently reverted it on the next reset -- confirmed on hardware,
 * 2026-08-22: "OTA rollback not yet confirmed ... safety_link_up=0" logged
 * forever, then a boot to the OLD image on any later reset) and, as of this
 * pass, boot_guard's recovery-mode counter (which would have walked a
 * perfectly healthy board into recovery mode after
 * RECOVERY_MODE_BOOT_THRESHOLD ordinary reboots, for the same reason).
 *
 * A missing safety processor is a real, already-acknowledged condition on
 * this board -- see ota_interlock.h's OTA_INTERLOCK_REFUSED_NEEDS_ACK, which
 * exists specifically so an operator can explicitly accept "no safety link"
 * and proceed anyway. It must not ALSO silently revert every OTA update and
 * silently walk the board toward recovery mode; both of those are exactly
 * the "quietly undoes what the operator asked for, for a reason they were
 * never told" failure this rewrite removes. The safety link's actual
 * up/down state is still read and logged LOUDLY at the call site whenever
 * this predicate is satisfied without it -- degraded and announced, not
 * silently dropped.
 *
 * nvs_ok / web_ok / ota_routes_ok are exactly what main.c already computes
 * before creating ota_rollback_confirm_task() (nvs_report_get()'s mounted
 * flags, dashboard_http_start()'s and ota_http_start()'s return codes) --
 * all three fixed at boot, none of them can become true later if false at
 * boot, same as the old nvs_ok/web_ok halves of the bar always were.
 * ota_routes_ok is the new half replacing the safety-link check: "the OTA
 * HTTP routes this whole recovery mechanism depends on actually came up" is
 * a real precondition for "safe to update again", where "an isolated
 * safety-processor link happens to be wired up and answering" never was. */
bool boot_confirm_is_healthy(bool nvs_ok, bool web_ok, bool ota_routes_ok);

/* What ota_rollback_confirm_task() (main.c) should do this poll, given the
 * running partition's type and the same nvs/web/ota_routes flags
 * boot_confirm_is_healthy() already takes. Extracted as its own pure
 * predicate (2026-08-24) because "is this boot healthy" and "does an OTA
 * rollback-cancel even apply here" are two separate questions that main.c
 * used to conflate: esp_ota_mark_app_valid_cancel_rollback() is only
 * meaningful when running from an OTA slot (ota_0/ota_1) that the bootloader
 * put into PENDING_VERIFY -- calling it while running from the `factory`
 * partition (the slot the JTAG-flash bench path in tools/PcTools writes) has
 * nothing to cancel and reliably returns ESP_FAIL, which used to be logged as
 * an ERROR every single boot despite being entirely expected.
 *
 * BOOT_CONFIRM_SKIP_NOT_HEALTHY  -- not yet healthy; keep polling (same as
 *                                   before this predicate existed).
 * BOOT_CONFIRM_SKIP_FACTORY      -- healthy, but running from `factory`: do
 *                                   NOT call esp_ota_mark_app_valid_cancel_rollback()
 *                                   (log why at INFO instead of ERROR), but
 *                                   DO still call boot_guard_mark_healthy() --
 *                                   that counter's job is independent of
 *                                   which partition type is running.
 * BOOT_CONFIRM_CONFIRM_OTA_SLOT  -- healthy and running from an OTA slot:
 *                                   the existing behavior, unchanged --
 *                                   call the rollback-cancel API, log its
 *                                   result, then mark healthy. */
typedef enum {
    BOOT_CONFIRM_SKIP_NOT_HEALTHY = 0,
    BOOT_CONFIRM_SKIP_FACTORY,
    BOOT_CONFIRM_CONFIRM_OTA_SLOT,
} boot_confirm_action_t;

boot_confirm_action_t boot_confirm_decide(bool is_factory_partition, bool nvs_ok, bool web_ok,
                                           bool ota_routes_ok);

#ifdef __cplusplus
}
#endif

#endif // BOOT_GUARD_H
