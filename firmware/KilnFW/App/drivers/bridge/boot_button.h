// boot_button -- the "I lost the AP password" recovery hatch, requested by
// the repo owner: a long press on the ESP32-S3's BOOT button (GPIO0) while
// the firmware is RUNNING opens a short, auto-closing window during which
// the OTA HTTP routes (ota_http.c's ota_http_verify_request()) skip their
// HMAC-over-the-AP-password check, so an operator who no longer knows the
// Wi-Fi AP password can still flash a fix.
//
// WHAT THIS DELIBERATELY DOES NOT DO -- read this before touching either
// side of the OTA auth path
// -----------------------------------------------------------------------
//   - It changes NO stored secret. The AP password is untouched in NVS;
//     this only suspends ota_http_verify_request()'s comparison for a
//     bounded window, the same way watchdog_cfg.c suspends the task-
//     watchdog PANIC action without touching CONFIG_ESP_TASK_WDT_EN.
//   - It does not disable auth permanently. BOOT_BUTTON_WINDOW_MS below is
//     the entire lifetime of one opening; there is no persisted "auth is
//     off" flag anywhere, on purpose -- this must never survive past the
//     boot that opened it, let alone across boots.
//   - It does NOT survive a reboot. boot_button_start()'s state lives
//     entirely in RAM (a plain file-scope struct, no NVS record). A reboot
//     -- for any reason, including one this same window's OTA update
//     causes -- closes it. An operator who needs it again after rebooting
//     has to hold the button again, which is the point: this is a
//     deliberate, one-shot, physically-present action, not a mode.
//
// THE ROM-DOWNLOAD-MODE CAVEAT -- why "while running" is load-bearing
// -----------------------------------------------------------------------
// GPIO0 is not an ordinary button to this chip. The ESP32-S3's first-stage
// ROM bootloader samples GPIO0 at reset/power-on: held LOW at that instant,
// the chip enters UART/USB serial-download mode instead of starting the
// application at all. Application code -- this module included -- cannot
// intercept, delay, or opt out of that sample; it happens before a single
// instruction of app_main() has run. Consequently:
//
//   - This feature ONLY works as "press and hold the BOOT button WHILE THE
//     BOARD IS RUNNING" -- i.e. after boot_button_start() is already
//     polling the pin from a live task. Every operator-facing string this
//     module or its callers show MUST say exactly that.
//   - It must NEVER be described as "hold the button and power-cycle" or
//     any wording a reader could mistake for that -- holding GPIO0 low
//     across a reset does not reach this code at all; it drops the board
//     into ROM download mode, which looks like a hung/dead board to
//     anyone not specifically expecting it and is a strictly worse
//     failure mode for a locked-out operator to stumble into.
//
// WHY A FIRING BLOCKS IT
// -----------------------------------------------------------------------
// Opening the window does not itself touch a relay or a setpoint, but the
// OTA routes it unlocks end in flashing a new image and, for the ESP side,
// an implied reboot to run it -- profile_executor.h's own contract is
// explicit that a firing never resumes itself across a reboot ("no auto-
// resume", see profile_executor.c's header comment), so accepting an
// update mid-firing is equivalent to an operator-invisible way to abort a
// run that is actively heating a kiln. boot_button_step() below refuses to
// open the window whenever profile_executor_get_status() reports
// PROFILE_EXEC_RUNNING or PROFILE_EXEC_PAUSED, the same two states
// ota_interlock.c's normal (authenticated) OTA path already treats as
// blocking. PROFILE_EXEC_IDLE, PROFILE_EXEC_DONE, and PROFILE_EXEC_FAULTED
// are all allowed to open it: none of the three has any run whose relays
// are still commanded on (IDLE never started one, DONE ended cleanly with
// relays off, and FAULTED is the latched "guard already dropped every
// relay and is waiting on an operator" state -- see profile_exec_state_t's
// own doc comment) -- FAULTED in particular is exactly the state a
// genuinely locked-out operator is likeliest to be staring at, having lost
// the password AND the board is stuck faulted, and this hatch exists
// precisely so that operator is not doubly stuck.
//
// HOW THIS FITS TOGETHER WITH ota_http.c
// -----------------------------------------------------------------------
// ota_http_verify_request() (ota_http.c) checks boot_button_ota_bypass_
// active() first, before any lockout/nonce work, for ALL FOUR of its
// contexts ("esp", "pico", "esp-rollback", "recovery") -- see that
// function's own comment for why: the whole point of this hatch is "the
// operator cannot produce a valid HMAC any more", and that is exactly as
// true for a Pico update or a rollback as it is for a plain ESP update, so
// scoping the bypass to only one context would leave the other three still
// unreachable to the very operator this feature exists to unblock.
//
// TASK / STACK PLACEMENT
// -----------------------------------------------------------------------
// boot_button_start() polls GPIO0 from a small dedicated FreeRTOS task
// (~100 ms, no ISR -- a mechanical button held/released by a human has no
// sub-100ms timing requirement, and an ISR would only add complexity this
// doesn't need). That task is created with plain xTaskCreate(), i.e. an
// INTERNAL-RAM stack -- see ota_http.c's ota_recovery_exit_reboot_task()
// and profile_executor.c's own task-creation comment for the reason this
// project treats as a standing rule: a task whose stack lives in PSRAM
// cannot run with the flash cache disabled
// (esp_task_stack_is_sane_cache_disabled() asserts on it), and several
// paths this task's own logic can end up adjacent to (OTA writes, a
// reboot) do disable the cache. A PSRAM-stack variant here would risk the
// exact same class of crash for no benefit -- this task's stack is tiny.
#ifndef BOOT_BUTTON_H
#define BOOT_BUTTON_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// How long GPIO0 must read continuously pressed (ACTIVE LOW) before this
// counts as a deliberate long press, not an incidental tap. 10 s: long
// enough that a normal accidental brush of the button (or the momentary
// press some flashing tools pulse GPIO0 with, outside of a reset -- this
// project's OpenOCD-based flashing per this repo's own project memory
// does not touch GPIO0 at all, but a stray touch is still worth guarding
// against) cannot open the window, while still being a press any operator
// can deliberately hold with a fingertip without it feeling broken.
#define BOOT_BUTTON_HOLD_MS 10000u

// How long the OTA-auth bypass window stays open once opened, auto-closing
// on its own with no operator action required. ~5 minutes: long enough for
// an operator to walk to their laptop, open the OTA page, and push an
// image over a slow Wi-Fi link, short enough that a board left unattended
// afterward does not sit indefinitely reachable by anyone on the network
// with no password at all.
#define BOOT_BUTTON_WINDOW_MS 300000u /* 5 minutes */

// --- Pure state machine (host-testable, no I/O) ----------------------------
// Mirrors boot_guard.c's split of next_boot_count()/record_is_valid() as
// pure functions callable directly from App/test/test_boot_button.c with no
// FreeRTOS/GPIO dependency.

typedef struct {
    bool     pressed_last;    /* GPIO0 read as pressed on the previous step() call */
    uint32_t press_started_ms; /* now_ms at the tick pressed_last first went true; meaningless while !pressed_last */
    bool     fired_this_press; /* OPEN_REQUESTED already returned once for the CURRENT continuous press */
} boot_button_press_state_t;

typedef enum {
    BOOT_BUTTON_EVENT_NONE = 0,
    BOOT_BUTTON_EVENT_OPEN_REQUESTED, /* the hold just crossed BOOT_BUTTON_HOLD_MS -- fires exactly once per press */
} boot_button_event_t;

// One step of the debounce-free hold-detector. `pressed` is the current,
// already-active-low-corrected GPIO0 reading (true == button held down).
// `now_ms` is this tick's timestamp (xTaskGetTickCount()*portTICK_PERIOD_MS
// on-target, an incrementing counter in tests) -- comparisons against it use
// the same `(uint32_t)(now_ms - started_ms) >= threshold` subtraction
// ota_auth.c's nonce-expiry check uses, which is correct across a uint32_t
// millisecond wraparound because unsigned subtraction wraps the same way
// the clock itself does (both mod 2^32), unlike a direct `now_ms >=
// started_ms + threshold` comparison, which is not.
//
// Fires BOOT_BUTTON_EVENT_OPEN_REQUESTED exactly once per continuous press,
// on the tick the hold first reaches BOOT_BUTTON_HOLD_MS -- `fired_this_press`
// latches so a continued hold past the threshold (an operator who keeps
// holding after the LCD/log announces the window is open) does not re-fire
// on every subsequent call. Resets on release (`pressed` false), so the
// very next press starts a fresh hold from zero and can fire again.
boot_button_event_t boot_button_step(boot_button_press_state_t *st, bool pressed, uint32_t now_ms);

// --- On-target I/O + task -----------------------------------------------

// Configures GPIO0 as an input with the internal pull-up enabled (ACTIVE
// LOW: pressed reads 0) and starts the ~100ms poll task (plain xTaskCreate,
// internal-RAM stack -- see this header's top comment). Call once from
// app_main(), in BOTH a normal boot and a recovery-mode boot -- see main.c's
// call site for why recovery mode, of all boots, is exactly the one where an
// operator is most likely to need this. Logs and returns without starting
// anything if GPIO configuration fails; a missing bypass hatch degrades to
// "the operator has to use the normal password path," never a crash.
void boot_button_start(void);

// True only while a bypass window is currently open AND has not yet
// expired. Evaluated fresh on every call against the current clock (NOT a
// cached "is open" flag last updated by the poll task) so a task that is
// itself stalled or starved for longer than BOOT_BUTTON_WINDOW_MS can never
// leave this returning true past the real deadline -- ota_http_verify_
// request() calls this on every single request, so its answer must be
// correct at read time, not merely "correct as of the last time the task
// happened to run."
bool boot_button_ota_bypass_active(void);

// 0 when the window is not open (or has already expired); otherwise the
// number of milliseconds left before it auto-closes, for the LCD/web UI
// countdown. Same on-read expiry evaluation as boot_button_ota_bypass_
// active() above.
uint32_t boot_button_bypass_remaining_ms(void);

// Closes the window immediately, if one is open, and logs `source` (e.g.
// "OTA update completed", "operator request") so a later reader of the log
// can see WHY it closed early rather than just that it eventually expired.
// A no-op (still logs, at a lower level) if no window is currently open.
void boot_button_close_bypass(const char *source);

#ifdef __cplusplus
}
#endif

#endif // BOOT_BUTTON_H
