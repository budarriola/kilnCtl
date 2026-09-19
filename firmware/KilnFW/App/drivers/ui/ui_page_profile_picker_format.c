#include "ui_page_profile_picker_format.h"

#include <stdio.h>

#include "profiles_builtin.h" /* PROFILE_BUILTIN_ID_BASE only -- no LVGL, no ESP-IDF beyond esp_err.h's
                                * type-only stub (see build_host_tests.ps1's own header comment on the
                                * App/test/stubs/ include path). */
#include "profiles_types.h"   /* PROFILES_MAX_COUNT only. */

void ui_page_profile_picker_format_label(const char *name, bool is_favorite, char *out, size_t out_cap)
{
    if (out == NULL || out_cap == 0) {
        return;
    }
    const char *safe_name = name ? name : "";
    snprintf(out, out_cap, "%s%s", is_favorite ? "* " : "", safe_name);
}

bool ui_page_profile_picker_is_deletable(uint8_t id)
{
    return id < PROFILES_MAX_COUNT;
}

uint8_t ui_page_profile_picker_format_page_count(uint8_t id_count, uint8_t rows_per_page)
{
    if (rows_per_page == 0) {
        return 1;
    }
    if (id_count == 0) {
        return 1; /* still show "1 of 1" / an empty page rather than 0 pages */
    }
    return (uint8_t)((id_count + rows_per_page - 1) / rows_per_page);
}

uint16_t ui_page_profile_picker_format_row_index(uint8_t page, uint8_t slot, uint8_t rows_per_page)
{
    return (uint16_t)((uint16_t)page * rows_per_page + slot);
}

uint8_t ui_page_profile_picker_format_clamp_page(uint8_t page, uint8_t id_count, uint8_t rows_per_page)
{
    uint8_t total_pages = ui_page_profile_picker_format_page_count(id_count, rows_per_page);
    if (page >= total_pages) {
        return (uint8_t)(total_pages - 1);
    }
    return page;
}
