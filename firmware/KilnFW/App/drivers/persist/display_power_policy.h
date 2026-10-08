// display_power_policy -- the pure decision core for "should the display be
// on, off, or forced on for an error right now, and does an incoming touch
// act on the UI or get swallowed as a wake gesture." Owner request 2026-09-04
// (docs/UI_PLAN.md / ROADMAP.md M-series "the UI the owner actually asked
// for" follow-up): brightness control, a selectable idle-off timeout (1/5/
// 10/15/60 min or Never), "keep display on while firing" overriding that
// timeout, first-touch-only-wakes (never acts on the UI underneath), and
// "display on error" forcing the display on until dismissed by a touch, then
// resuming normal timeout behaviour.
//
// PURE ON PURPOSE, same split as SaftyFW's max31856_fault_pin_policy.c /
// discrete_pin_policy.c and this file's own KilnFW sibling
// backlight_pwm.h's backlight_duty_percent_for_state(): no LVGL, no NVS, no
// FreeRTOS, no ESP-IDF headers at all. Every input the decision needs is a
// plain argument; every output is a plain return value. This is what makes
// the interaction between "keep on while firing", "wake touch swallowed",
// and "display on error" host-testable exhaustively instead of only
// reachable on real hardware with a stopwatch.
//
// CALLER CONTRACT (for whichever module wires this to real touch/timer
// events -- screen_idle.c and/or lvgl_port.c own that hookup and are NOT
// modified by this pass; this header is the seam they wire against):
//   - Call display_power_policy_step() once per touch event AND once per
//     idle-timeout poll tick (screen_idle_task's existing ~50ms poll cadence
//     is a fine cadence for the latter) -- pass touch_event=true only on the
//     tick a real or injected press edge actually happened, exactly once for
//     that edge, same "a press edge, not a level" discipline screen_idle.c's
//     own touch handling already uses.
//   - `current_state` is the caller's own last returned `state` -- this
//     function does not remember anything between calls, the caller does.
//   - Whenever this call returns swallow_touch=true OR the touch was passed
//     through untouched (touch_event=true and result.state==DISPLAY_POWER_ON
//     without having been OFF/ERROR_HOLD the moment before), the caller must
//     still update ITS OWN last-activity timestamp to `now_ms` before the
//     next call -- exactly like screen_idle_mark_active() already does for
//     every touch today. This function never reads or writes that timestamp
//     itself except via the `last_touch_ms` argument the caller supplies.
//   - error_entered_this_tick is an EDGE, not a level: true only on the tick
//     error_active transitions from false to true (or the first tick of a
//     fresh error after a previous one was dismissed). This is deliberate,
//     not an oversight -- see this header's comment on that field below for
//     why a level-triggered `error_active` cannot express "resume normal
//     behaviour after the dismissing touch" on its own.
#ifndef DISPLAY_POWER_POLICY_H
#define DISPLAY_POWER_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The six choices offered on the display settings page. Values are stable
// (persisted to NVS by display_power_cfg.c) -- do not renumber; add new
// entries before DISPLAY_TIMEOUT_COUNT only.
typedef enum {
    DISPLAY_TIMEOUT_1_MIN = 0,
    DISPLAY_TIMEOUT_5_MIN = 1,
    DISPLAY_TIMEOUT_10_MIN = 2,
    DISPLAY_TIMEOUT_15_MIN = 3,
    DISPLAY_TIMEOUT_60_MIN = 4,
    DISPLAY_TIMEOUT_NEVER = 5,
    DISPLAY_TIMEOUT_COUNT = 6,
} display_timeout_setting_t;

// Milliseconds for each timeout_setting_t, or UINT32_MAX for
// DISPLAY_TIMEOUT_NEVER (a sentinel -- display_power_policy_step() never
// actually compares against it; DISPLAY_TIMEOUT_NEVER is branched on
// directly. UINT32_MAX is returned rather than 0 so a caller who
// mistakenly used this value as a real comparison bound fails obviously
// slow, not obviously-instant-timeout). Returns 0 for any value outside
// display_timeout_setting_t's defined range (defensive default: an
// out-of-range persisted/corrupt setting must not read back as an
// arbitrarily long or a "never" timeout).
uint32_t display_power_timeout_ms(display_timeout_setting_t setting);

// true if `setting` is one of the DISPLAY_TIMEOUT_* enumerators above --
// callers loading a persisted value use this the same way unit_pref_start()
// range-checks its own stored byte before trusting it.
bool display_power_timeout_setting_is_valid(display_timeout_setting_t setting);

typedef enum {
    DISPLAY_POWER_ON = 0,
    DISPLAY_POWER_OFF = 1,
    // Forced-on for an active error, per the "display on error" switch.
    // Distinct from DISPLAY_POWER_ON so the dismissing touch can be
    // recognized and swallowed even if the display was already ON when the
    // error arrived (rule 5's "keep it on until a touch happens" applies
    // whether the display was already lit or not).
    DISPLAY_POWER_ERROR_HOLD = 2,
} display_power_state_t;

typedef struct {
    uint32_t now_ms;
    uint32_t last_touch_ms;             // caller's own last-activity stamp; ignored on a tick where touch_event is true (see header comment)
    display_timeout_setting_t timeout_setting;
    bool firing_active;
    bool keep_on_while_firing;          // the "keep display on while firing" switch
    // Level: an error condition currently exists. Read by nothing in this
    // function directly -- kept as an argument only so a future consumer
    // (e.g. a status readout) can be handed the same input struct; the
    // decision itself keys off error_entered_this_tick below.
    bool error_active;
    // Edge, true for exactly one call: the tick error_active first became
    // true. A LEVEL-triggered "is there an error" input cannot express rule
    // 5 correctly: once the dismissing touch has resumed normal timeout
    // behaviour, the error may still be active (not yet cleared) -- a level
    // check would force DISPLAY_POWER_ERROR_HOLD right back on for every
    // later poll tick until the error clears, defeating the "resume normal
    // timeout behaviour" half of the rule. The caller (whoever tracks
    // error_active over time -- not this module) is responsible for
    // computing this edge, the same way screen_idle.c already treats a
    // touch as a press EDGE rather than re-acting on every poll tick a
    // finger happens to still be down.
    bool error_entered_this_tick;
    bool display_on_error;              // the "display on error" switch
    display_power_state_t current_state; // caller's last returned state; DISPLAY_POWER_ON is the correct value to pass on the very first call (matches screen_idle_init()'s screen_on = true default)
    bool touch_event;                   // true exactly on the tick a touch press EDGE (real or injected) occurred
} display_power_input_t;

typedef struct {
    display_power_state_t state;
    // true if `touch_event` was true this call AND that touch must NOT act
    // on whatever UI element is underneath it -- rule 4 (a wake-only touch)
    // and rule 5's last sentence (the error-dismissing touch is swallowed
    // too). Always false when touch_event was false.
    bool swallow_touch;
} display_power_result_t;

// The whole decision, in one pure step. See this header's top comment for
// the calling contract (once per touch edge, once per idle-poll tick).
display_power_result_t display_power_policy_step(const display_power_input_t *in);


// ---------------------------------------------------------------------------
// Touch gate: press-edge tracking with a release debounce (2026-10-07).
//
// A physical touch controller can drop a sample or two mid-touch (a failed
// I2C read, a single not-pressed frame). Each such dropout looked like a
// release, so the very next pressed sample was a "new" press edge: for a
// touch that woke the screen, the screen was already ON by then, so the
// policy passed this re-press through to LVGL and a wake tap acted on the
// widget underneath. The gate treats a re-press within
// DISPLAY_POWER_TOUCH_REPRESS_MS of the release of a SWALLOWED touch as a
// continuation of that same touch (still swallowed, policy not re-run).
// Pure: no locking, the caller (screen_idle.c) holds its own lock.
#define DISPLAY_POWER_TOUCH_REPRESS_MS 250u

typedef struct {
    bool held;            // a press is currently down (edge already taken)
    bool held_swallow;    // verdict of the current / most recent touch
    bool have_release;    // last_release_ms is meaningful
    uint32_t last_release_ms;
} display_power_touch_gate_t;

// A pressed sample. Returns true iff this is a genuine NEW press edge that
// the caller must feed to display_power_policy_step() with touch_event=true
// (and then store its verdict with display_power_touch_gate_record()).
// Returns false for a held repeat or a debounced re-press; the caller then
// reports gate->held_swallow.
bool display_power_touch_gate_press(display_power_touch_gate_t *gate, uint32_t now_ms);

// Store the policy's swallow verdict for the edge just taken.
void display_power_touch_gate_record(display_power_touch_gate_t *gate, bool swallow);

// A release sample.
void display_power_touch_gate_release(display_power_touch_gate_t *gate, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif // DISPLAY_POWER_POLICY_H
