// auth_reset_gesture.h -- the pure, host-testable state machine behind the
// physical credential-reset gesture. docs/WEB_AUTH_PLAN.md item 10.
//
// Owner-confirmed gesture (2026-09-16, closing item 13's open assumption):
// with E-stop asserted and no firing/heat active, tap each of the four LCD
// corners once, in order -- top-left, top-right, bottom-left, bottom-right --
// within a 10 s window. That arms the reset and shows a banner; a separate,
// explicit confirm (a ui_confirm dialog, owned elsewhere) within 30 s of
// arming actually clears the administrator credential.
//
// Deliberately does NOT know about LVGL, touch coordinates, hit zones, the
// E-stop GPIO/safety-link decode, or the credential store's on-disk format.
// Those are: the LCD-owning agent's corner hit zones and confirm dialog
// (docs/WEB_AUTH_PLAN.md item 7/8-LCD), the safety-link status decode
// (safety_link.h's SAFETY_FLAG_ESTOP, already populated -- see that header),
// and the credential-storage module (item 2, "kiln_auth" NVS namespace) that
// another session is adding. This file only decides, given corner-tap
// events and three booleans supplied by the caller, whether the sequence
// should progress, abandon, arm, or (on confirm) fire the clear -- and it
// fires the clear through one function-pointer seam
// (auth_reset_gesture_state_t.clear_credentials_fn) so wiring in the real
// credential-store reset function is a one-line assignment at the call site
// that constructs the state:
// s.clear_credentials_fn = web_auth_store_clear_for_physical_reset;
// (firmware/KilnFW/App/drivers/persist/web_auth_store.h) -- that header now
// exists and its function's signature already matches
// auth_reset_gesture_clear_fn exactly (bool (*)(void)), so no adapter is
// needed. See auth_reset_gesture_wiring.c for the real wiring.
//
// Host-testable, no ESP-IDF dependency -- same discipline as ota_auth.h
// (see App/test/build_host_tests.ps1).
#ifndef KILNCTL_AUTH_RESET_GESTURE_H
#define KILNCTL_AUTH_RESET_GESTURE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Order is the owner-confirmed sequence, fixed -- not configurable, because
// a configurable order would just move the "which order" question onto a
// second thing that can be gotten wrong.
typedef enum {
    AUTH_RESET_CORNER_TOP_LEFT = 0,
    AUTH_RESET_CORNER_TOP_RIGHT,
    AUTH_RESET_CORNER_BOTTOM_LEFT,
    AUTH_RESET_CORNER_BOTTOM_RIGHT,
    AUTH_RESET_CORNER_COUNT
} auth_reset_gesture_corner_t;

// 10 s to complete all four corner taps in order (plan item 10 step 3).
#define AUTH_RESET_GESTURE_SEQUENCE_WINDOW_MS 10000u
// 30 s from arming to the explicit confirm (plan item 10 step 4's banner:
// "confirm within 30 s").
#define AUTH_RESET_GESTURE_CONFIRM_WINDOW_MS  30000u

// The seam: called only from auth_reset_gesture_confirm() on a successful,
// unexpired confirm. Must itself verify the clear by read-back before
// returning true (boot_guard_mark_healthy()'s discipline -- never trust an
// NVS write's return code) and must not be called from a PSRAM-stacked task
// or dispatched to the flash worker from code already running on it. Returns
// false on any failure to clear (the state machine still returns to idle
// either way -- see auth_reset_gesture_confirm()'s doc comment -- because
// retrying immediately without the operator re-arming is not this module's
// call to make).
typedef bool (*auth_reset_gesture_clear_fn)(void);

typedef struct {
    // Index into the fixed corner order of the next tap this sequence
    // expects. 0 means idle/no sequence in progress.
    uint8_t  next_index;
    // ms timestamp of the first correct tap in the current in-progress
    // sequence. Only meaningful while 0 < next_index < AUTH_RESET_CORNER_COUNT.
    uint32_t sequence_started_ms;
    // True once all four corners have been tapped in order inside the
    // window -- armed, waiting for an explicit confirm.
    bool     armed;
    // ms timestamp at which `armed` became true. Confirm must land within
    // AUTH_RESET_GESTURE_CONFIRM_WINDOW_MS of this.
    uint32_t armed_at_ms;
    // The seam (see auth_reset_gesture_clear_fn's doc comment above). NULL
    // is a valid, safe value -- it means "not wired to a real credential
    // store yet", and confirm() then reports AUTH_RESET_CONFIRM_NOT_WIRED
    // rather than crashing or silently doing nothing indistinguishable from
    // success.
    auth_reset_gesture_clear_fn clear_credentials_fn;
} auth_reset_gesture_state_t;

// Zeroes `s` and clears any armed/in-progress sequence. Does NOT touch
// clear_credentials_fn -- callers set that once at construction (or pass it
// again explicitly) since it is configuration, not runtime state.
void auth_reset_gesture_reset(auth_reset_gesture_state_t *s);

// The three preconditions that gate the WHOLE gesture, not just its final
// confirm -- checked on every tap, not only at sequence start, so releasing
// E-stop or a firing starting mid-sequence aborts immediately rather than
// leaving a partially-armed sequence waiting to be exploited later. Plan
// item 10, step 1-2: E-stop asserted, no firing in progress, heat not
// granted.
bool auth_reset_gesture_preconditions_met(bool estop_asserted, bool firing_active, bool heat_enabled);

typedef enum {
    // Preconditions not met (E-stop not asserted, a firing is active, or
    // heat is enabled) -- the tap is discarded with no state change at all.
    // This is the "must not be reachable in a way that distracts from the
    // panel's safety function" requirement: outside the precondition
    // window, corner taps are inert, full stop.
    AUTH_RESET_TAP_IGNORED_PRECONDITIONS = 0,
    // Already armed -- extra taps while waiting for confirm are ignored
    // rather than restarting or re-arming anything.
    AUTH_RESET_TAP_IGNORED_ALREADY_ARMED,
    // Wrong corner, or the 10 s window since the first correct tap elapsed
    // -- sequence abandoned cleanly (next_index reset to 0). A subsequent
    // correct sequence starts fresh; this is not a "stuck" or a "locked"
    // state.
    AUTH_RESET_TAP_ABANDONED,
    // Correct corner, sequence continues (not yet all four).
    AUTH_RESET_TAP_PROGRESS,
    // Fourth correct corner within the window -- now armed. Caller should
    // show the "AUTH RESET ARMED -- confirm within 30 s" banner and log
    // ESP_LOGE at arm (plan item 10's logging requirement; done by the
    // caller since this module has no logging dependency).
    AUTH_RESET_TAP_ARMED,
} auth_reset_gesture_tap_result_t;

// Feeds one corner-tap event at time `now_ms`. `estop_asserted`,
// `firing_active` and `heat_enabled` are read fresh on every call -- this
// function has no memory of them between calls, so a caller that samples
// them per-tap (rather than latching a stale value from sequence start)
// gets the "abandons cleanly" behaviour for free.
auth_reset_gesture_tap_result_t auth_reset_gesture_on_corner_tap(
    auth_reset_gesture_state_t *s, auth_reset_gesture_corner_t corner, uint32_t now_ms,
    bool estop_asserted, bool firing_active, bool heat_enabled);

typedef enum {
    // Cleared successfully (clear_credentials_fn returned true). State
    // returns to idle.
    AUTH_RESET_CONFIRM_OK = 0,
    // Nothing is armed (never armed, or already consumed) -- ignored.
    AUTH_RESET_CONFIRM_NOT_ARMED,
    // Was armed, but the 30 s confirm window elapsed. Treated the same as
    // NOT_ARMED for the caller's purposes but reported distinctly so a
    // caller can log "reset gesture expired" rather than "no gesture in
    // progress". State returns to idle either way.
    AUTH_RESET_CONFIRM_EXPIRED,
    // Armed and within the window, but clear_credentials_fn is NULL (the
    // real credential-store reset has not been wired in yet). State returns
    // to idle -- confirming again requires re-arming the gesture, which is
    // deliberate: a wired-but-failing clear and an unwired one both fail
    // safe by not looping silently.
    AUTH_RESET_CONFIRM_NOT_WIRED,
    // Armed and within the window, clear_credentials_fn was called and
    // returned false (its own read-back verification failed). State returns
    // to idle for the same reason as NOT_WIRED.
    AUTH_RESET_CONFIRM_CLEAR_FAILED,
} auth_reset_gesture_confirm_result_t;

// Explicit confirm (the ui_confirm dialog's confirm button, owned
// elsewhere). Always leaves `s` idle afterward, whatever the result --
// "One deliberate confirmation, not a second ambiguous tap" (plan item 10)
// means there is no retry loop here; a failed or expired confirm requires
// re-arming the whole gesture, including its E-stop and no-heat
// preconditions, not just pressing confirm again.
auth_reset_gesture_confirm_result_t auth_reset_gesture_confirm(auth_reset_gesture_state_t *s, uint32_t now_ms);

// Explicit cancel (the ui_confirm dialog's cancel, or a tap outside it, or
// -- per plan item 8's "must never block a safety action" -- anything else
// the caller wants to treat as "abandon this"). Idempotent: cancelling an
// already-idle state is a harmless no-op. Always leaves `s` idle.
void auth_reset_gesture_cancel(auth_reset_gesture_state_t *s);

// True if `s` is currently armed AND the confirm window has not yet
// elapsed. Lets a caller (e.g. a periodic UI tick) decide whether to keep
// showing the armed banner without mutating state -- unlike
// auth_reset_gesture_confirm(), calling this never transitions `s`.
bool auth_reset_gesture_is_armed_and_live(const auth_reset_gesture_state_t *s, uint32_t now_ms);

// --- Board-wide singleton, wired to the real credential store --------------
//
// Exactly one physical panel exists, so exactly one gesture state is needed
// board-wide. This lives here (not in an LCD-owned file) so the seam wiring
// is a one-time, reviewed step in the module that owns
// auth_reset_gesture_state_t, per the standing instruction to keep
// corner-tap detection in its own module rather than editing the LCD
// PIN/keypad module that will call into it. First call lazily constructs
// the instance with clear_credentials_fn already wired to
// web_auth_store_clear_for_physical_reset() (firmware/KilnFW/App/drivers/
// persist/web_auth_store.h); every call thereafter returns the same
// pointer. Not thread-safe by itself -- callers must only touch it from the
// single UI/LVGL task that owns touch input, same assumption every other
// LVGL-adjacent module in this tree already makes.
auth_reset_gesture_state_t *auth_reset_gesture_singleton(void);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_AUTH_RESET_GESTURE_H
