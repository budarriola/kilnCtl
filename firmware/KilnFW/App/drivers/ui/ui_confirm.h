// ui_confirm -- shared Yes/Cancel confirmation overlay for the LCD.
//
// Factored out of ui_page_home.c's Start/Stop confirmation dialogs (see that
// file's "Start/Stop confirmation overlay" header comment for the full
// rationale) so a second call site -- the new Profiles-hub detail page's
// START action -- does not copy-paste a second lv_msgbox implementation.
// Built on lv_msgbox exactly as ui_page_home.c's original did:
//   - lv_msgbox_create(NULL) parents the box to an auto-created full-screen
//     backdrop on lv_layer_top(), never into a page's flex column, so it
//     takes no part in that layout and consumes none of the page's no-scroll
//     content budget. See ui_page_home.c's FLEX TRAP comment for why that
//     matters on this codebase specifically.
//   - The backdrop is a plain, unhandled-click lv_obj -- a stray tap outside
//     the two footer buttons is swallowed and does nothing, so the dialog is
//     cancel-safe by default.
//   - LV_USE_MSGBOX is already enabled (CONFIG_LV_USE_MSGBOX=y) and its
//     object code already linked in by ui_page_home.c/ui_page_network.c, so
//     this helper costs no additional flash beyond its own small body.
#ifndef UI_CONFIRM_H
#define UI_CONFIRM_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ui_confirm_cb_t)(void *user_data);

typedef struct {
    const char *title;          /* e.g. "Confirm Start" */
    const char *body;           /* already-formatted text -- callers that need
                                  * profile-specific detail (zone names, a
                                  * profile name) build that string themselves
                                  * (snprintf into a stack buffer) before
                                  * calling; this helper does no formatting of
                                  * its own, same division of labor
                                  * show_start_confirm() had before this was
                                  * factored out. */
    const char *confirm_label;  /* e.g. "Start" / "Stop" */
    lv_color_t  confirm_color;  /* e.g. UI_THEME_ACCENT_4 (start-green) or
                                  * UI_THEME_ACCENT_5 (stop-red) -- caller's
                                  * choice, this helper has no opinion on
                                  * which accent means what. */
    ui_confirm_cb_t on_confirm; /* called AFTER the dialog is closed; may be
                                  * NULL (dialog just closes on confirm) */
    void       *user_data;      /* passed through to on_confirm unchanged */
} ui_confirm_params_t;

/* Builds and shows the dialog. Cancel always closes with no callback.
 * Confirm closes THEN calls on_confirm(user_data) (in that order, so
 * on_confirm never has to worry about the still-open msgbox). Safe to call
 * from any page's event callback -- the dialog parents itself to
 * lv_layer_top(), independent of whichever page is currently showing. */
void ui_confirm_show(const ui_confirm_params_t *params);

/* Closes the most recently shown dialog, if it is still open, exactly as if
 * Cancel had been pressed (on_confirm is NOT called). Used by ui_lcd_lock.c
 * when an LCD session re-locks: a Confirm Start/Stop dialog opened under a
 * session that has since expired must not stay tappable by someone who never
 * entered the PIN (owner decision 2026-09-28). LVGL task only. */
void ui_confirm_close_open(void);

#ifdef __cplusplus
}
#endif

#endif // UI_CONFIRM_H
