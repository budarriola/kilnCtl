// ui_num_pad -- shared tap-to-edit modal for a single number or short text
// value, built for the LCD profile builder (Step 1's name field, Step 2's
// Target/Ramp/Dwell cards) so those three call sites do not each write their
// own lv_textarea + lv_keyboard wiring.
//
// Explicitly NOT lv_spinbox: ui_page_network.c already creates both an
// lv_textarea and an lv_keyboard for its Wi-Fi password entry, so both
// widgets are already linked into the firmware image and this modal's
// incremental flash cost is close to zero. lv_spinbox is enabled in Kconfig
// (CONFIG_LV_USE_SPINBOX=y) but referenced nowhere in this codebase, so it is
// presently stripped by --gc-sections -- pulling it in for this one modal
// would add real, avoidable bytes on a build that is already at 3% flash
// free (see ROADMAP.md M8). A +/- stepper to reach a value like 1222C would
// also be a poor input method compared to typing it.
//
// Overlay, not page content: built once, lazily, as a child of
// lv_layer_top() -- the exact same system layer ui_confirm.c's
// lv_msgbox_create(NULL) auto-parents to (see ui_confirm.h's header comment).
// lv_layer_top() is independent of whichever page's flex-column screen is
// currently loaded, so this modal takes no part in that page's layout and
// consumes none of its no-scroll content budget -- the FLEX TRAP this
// codebase's ui_page_home.c header comment warns about (a plain child of a
// flex column gets positioned by the layout and eats content height) simply
// does not apply to an object that was never made a child of that column in
// the first place.
#ifndef UI_NUM_PAD_H
#define UI_NUM_PAD_H

#include <stddef.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_NUM_PAD_MODE_NUMBER, /* LV_KEYBOARD_MODE_NUMBER, value clamped to [min, max] on Done */
    UI_NUM_PAD_MODE_TEXT,   /* free text, bounded only by max_len */
} ui_num_pad_mode_t;

/* Called once, after the modal has already closed. accepted is false if the
 * user tapped Cancel (text/value are unchanged from whatever was passed as
 * initial_*, callers that only care about acceptance can ignore them in that
 * case). For MODE_NUMBER, value is the parsed, clamped result and text is
 * that same value formatted back to a string (decimals digits); for
 * MODE_TEXT, text is what was typed and value is always 0. */
typedef void (*ui_num_pad_done_cb_t)(bool accepted, const char *text, float value, void *user_data);

typedef struct {
    const char *caption;       /* e.g. "Target C" / "Profile Name" */
    ui_num_pad_mode_t mode;
    const char *initial_text;  /* MODE_TEXT: prefill verbatim. MODE_NUMBER: ignored (initial_value used instead) */
    float initial_value;       /* MODE_NUMBER: prefill, formatted with `decimals` digits */
    float min;                 /* MODE_NUMBER only; min == max means unbounded (rare -- most call
                                 * sites here have a real profiles_http.c-derived range) */
    float max;                 /* MODE_NUMBER only */
    int decimals;              /* MODE_NUMBER only: 0 for whole C/rate/minutes values */
    uint32_t max_len;          /* MODE_TEXT only: lv_textarea_set_max_length() */
    ui_num_pad_done_cb_t on_done; /* may be NULL */
    void *user_data;
} ui_num_pad_params_t;

/* Builds (first call) or reconfigures (later calls) the singleton modal and
 * shows it. Only one instance is ever needed -- like ui_confirm.c, nothing in
 * this codebase shows two of these at once. */
void ui_num_pad_show(const ui_num_pad_params_t *params);

#ifdef __cplusplus
}
#endif

#endif // UI_NUM_PAD_H
