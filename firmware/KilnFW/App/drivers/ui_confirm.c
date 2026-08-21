#include "ui_confirm.h"

#include "ui_theme.h"

#define UI_CONFIRM_WIDTH_PX 400

typedef struct {
    ui_confirm_cb_t on_confirm;
    void           *user_data;
} ui_confirm_ctx_t;

/* Stashed in the msgbox's own user_data (lv_obj_set_user_data), not a static
 * -- more than one confirm dialog is never on screen at once in practice, but
 * there is no reason to make that a hidden assumption when the mbox object
 * already has a slot to carry it. Freed in confirm_close_cb() and
 * confirm_yes_cb(), whichever runs (each dialog closes exactly once). */
static void free_ctx(lv_obj_t *mbox)
{
    ui_confirm_ctx_t *ctx = (ui_confirm_ctx_t *)lv_obj_get_user_data(mbox);
    lv_free(ctx);
}

static void confirm_close_cb(lv_event_t *e)
{
    lv_obj_t *mbox = (lv_obj_t *)lv_event_get_user_data(e);
    free_ctx(mbox);
    lv_msgbox_close(mbox);
}

static void confirm_yes_cb(lv_event_t *e)
{
    lv_obj_t *mbox = (lv_obj_t *)lv_event_get_user_data(e);
    ui_confirm_ctx_t *ctx = (ui_confirm_ctx_t *)lv_obj_get_user_data(mbox);
    ui_confirm_cb_t on_confirm = ctx ? ctx->on_confirm : NULL;
    void *user_data = ctx ? ctx->user_data : NULL;
    lv_free(ctx);
    lv_msgbox_close(mbox);
    if (on_confirm) {
        on_confirm(user_data);
    }
}

/* Common footer-button styling: >= UI_THEME_MIN_TOUCH_TARGET_PX (72px) in
 * both dimensions, matching every other button on this codebase's touch-
 * target rule. Cancel is always the neutral card color -- "do nothing",
 * never "the other action's color". */
static void style_footer_button(lv_obj_t *btn, lv_color_t bg)
{
    lv_obj_set_size(btn, UI_THEME_MIN_TOUCH_TARGET_PX * 2, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_radius(btn, UI_THEME_CORNER_RADIUS_PX, 0);
}

void ui_confirm_show(const ui_confirm_params_t *params)
{
    if (!params) {
        return;
    }

    lv_obj_t *mbox = lv_msgbox_create(NULL);
    lv_obj_set_width(mbox, UI_CONFIRM_WIDTH_PX);
    if (params->title) {
        lv_msgbox_add_title(mbox, params->title);
    }
    if (params->body) {
        lv_msgbox_add_text(mbox, params->body);
    }

    ui_confirm_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    ctx->on_confirm = params->on_confirm;
    ctx->user_data = params->user_data;
    lv_obj_set_user_data(mbox, ctx);

    lv_obj_t *yes = lv_msgbox_add_footer_button(mbox, params->confirm_label ? params->confirm_label : "Confirm");
    style_footer_button(yes, params->confirm_color);
    lv_obj_add_event_cb(yes, confirm_yes_cb, LV_EVENT_CLICKED, mbox);

    lv_obj_t *no = lv_msgbox_add_footer_button(mbox, "Cancel");
    style_footer_button(no, UI_THEME_COLOR_CARD);
    lv_obj_add_event_cb(no, confirm_close_cb, LV_EVENT_CLICKED, mbox);

    lv_obj_t *footer = lv_msgbox_get_footer(mbox);
    lv_obj_set_height(footer, UI_THEME_MIN_TOUCH_TARGET_PX);
}
