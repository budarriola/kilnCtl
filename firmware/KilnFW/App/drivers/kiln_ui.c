#include "kiln_ui.h"

#include <string.h>

#include "esp_log.h"

#include "ui_page_config.h"
#include "ui_page_home.h"
#include "ui_page_temperature.h"

static const char *TAG = "kiln_ui";

/* A handful of pages (home, settings, temperature, config -- see TODO.md
 * 10.3's page list), not a dynamic set: a fixed array avoids pulling in
 * dynamic allocation for something this small and bounded. Raise this if
 * 10.3 ends up wanting more top-level pages than that. */
#define KILN_UI_MAX_PAGES 16

typedef struct {
    const char *name;          /* borrowed, see kiln_ui_register_page */
    kiln_ui_page_build_fn build;
    lv_obj_t *screen;          /* NULL until first shown */
} kiln_ui_page_t;

static kiln_ui_page_t s_pages[KILN_UI_MAX_PAGES];
static size_t s_page_count;
static const char *s_current_page_name;

static kiln_ui_page_t *find_page(const char *name)
{
    for (size_t i = 0; i < s_page_count; ++i) {
        if (strcmp(s_pages[i].name, name) == 0) return &s_pages[i];
    }
    return NULL;
}

esp_err_t kiln_ui_init(void)
{
    memset(s_pages, 0, sizeof(s_pages));
    s_page_count = 0;
    s_current_page_name = NULL;

    /* Built-in page 0, ui_page_home.c -- see kiln_ui.h: this file (kiln_ui.c)
     * is the registry/switcher only, every page's actual widget tree lives in
     * its own ui_page_*.c/.h so no single file accumulates every screen. */
    esp_err_t err = kiln_ui_register_page("home", ui_page_home_build);
    if (err != ESP_OK) return err;

    /* TODO.md 10.3's "Configuration"/"Temperature" nav items -- stubs for
     * now (see ui_page_config.c/.h, ui_page_temperature.c/.h), registered
     * here so ui_page_home.c's nav buttons have somewhere real to
     * kiln_ui_show() and the page-switching mechanism gets exercised by more
     * than the one page it's had until now. */
    err = kiln_ui_register_page("config", ui_page_config_build);
    if (err != ESP_OK) return err;
    err = kiln_ui_register_page("temperature", ui_page_temperature_build);
    if (err != ESP_OK) return err;

    return kiln_ui_show("home");
}

esp_err_t kiln_ui_register_page(const char *name, kiln_ui_page_build_fn build)
{
    if (!name || !build) return ESP_ERR_INVALID_ARG;
    if (find_page(name)) return ESP_ERR_INVALID_STATE; /* names are unique */
    if (s_page_count >= KILN_UI_MAX_PAGES) return ESP_ERR_NO_MEM;

    s_pages[s_page_count].name = name;
    s_pages[s_page_count].build = build;
    s_pages[s_page_count].screen = NULL;
    s_page_count++;
    return ESP_OK;
}

esp_err_t kiln_ui_show(const char *name)
{
    kiln_ui_page_t *page = find_page(name);
    if (!page) {
        ESP_LOGE(TAG, "kiln_ui_show(\"%s\"): no such page", name ? name : "(null)");
        return ESP_ERR_NOT_FOUND;
    }

    if (!page->screen) {
        page->screen = page->build();
        if (!page->screen) {
            ESP_LOGE(TAG, "page \"%s\" build() returned NULL", page->name);
            return ESP_FAIL;
        }
    }

    lv_screen_load(page->screen);
    s_current_page_name = page->name;
    return ESP_OK;
}

const char *kiln_ui_current_page(void)
{
    return s_current_page_name;
}
