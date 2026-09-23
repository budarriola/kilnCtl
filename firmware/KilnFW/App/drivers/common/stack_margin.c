#include "stack_margin.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "stack_margin";

typedef struct {
    char name[STACK_MARGIN_NAME_MAX];
    TaskHandle_t *handle_slot; /* the caller's own TaskHandle_t*, read fresh every time -- see stack_margin.h */
    uint32_t configured_stack_bytes;
} stack_margin_entry_t;

static stack_margin_entry_t s_entries[STACK_MARGIN_MAX_TASKS];
static size_t s_count;

bool stack_margin_register(const char *name, void *task_handle_slot, uint32_t configured_stack_bytes)
{
    if (!name || name[0] == '\0' || !task_handle_slot) {
        ESP_LOGE(TAG, "register() called with a NULL/empty name or NULL handle slot -- not registered");
        return false;
    }

    // Idempotent by (name, handle_slot): a handful of registration sites --
    // the three OTA background-task launchers (ota_http_esp.c/ota_http_pico.c/
    // ota_http_recovery.c) among them -- call this once per invocation of a
    // POST handler that can run more than once a boot, always with the same
    // literal name and the same file-scope TaskHandle_t* slot. Without this
    // check every repeat POST appended a fresh row, and since the registry
    // has no removal path, enough repeat calls silently walk it to
    // STACK_MARGIN_MAX_TASKS, after which registration logs and stops --
    // for every task registered anywhere in the firmware, not just the
    // repeat offender. A call with the same name but a DIFFERENT slot is not
    // this case -- it is either a real name collision between two distinct
    // tasks or a caller bug -- so that one is still logged and refused
    // rather than silently accepted or silently appended.
    // Compares only the first STACK_MARGIN_NAME_MAX-1 (19) chars of `name` --
    // matches how names are stored below (name[STACK_MARGIN_NAME_MAX]).
    for (size_t i = 0; i < s_count; i++) {
        if (strncmp(s_entries[i].name, name, STACK_MARGIN_NAME_MAX - 1) == 0) {
            if (s_entries[i].handle_slot == (TaskHandle_t *)task_handle_slot) {
                return true; // already registered by this exact call site -- no-op
            }
            // A mismatched configured_stack_bytes on this same (name, slot)
            // pair is not checked or reported here -- first registration wins.
            ESP_LOGE(TAG, "register() called for '%s' with a different handle slot than its "
                          "existing registration -- not re-registered (name collision?)",
                     name);
            return false;
        }
    }

    if (s_count >= STACK_MARGIN_MAX_TASKS) {
        ESP_LOGE(TAG, "registry full (%u/%u) -- '%s' not registered; raise STACK_MARGIN_MAX_TASKS",
                 (unsigned)s_count, (unsigned)STACK_MARGIN_MAX_TASKS, name);
        return false;
    }

    stack_margin_entry_t *e = &s_entries[s_count];
    strncpy(e->name, name, STACK_MARGIN_NAME_MAX - 1);
    e->name[STACK_MARGIN_NAME_MAX - 1] = '\0';
    e->handle_slot = (TaskHandle_t *)task_handle_slot;
    e->configured_stack_bytes = configured_stack_bytes;
    s_count++;
    return true;
}

size_t stack_margin_count(void)
{
    return s_count;
}

bool stack_margin_read(size_t index, const char **name, uint32_t *configured_stack_bytes,
                        uint32_t *hwm_bytes, stack_margin_level_t *level, bool *alive)
{
    if (index >= s_count) {
        return false;
    }
    const stack_margin_entry_t *e = &s_entries[index];
    TaskHandle_t handle = *e->handle_slot; /* fresh read -- see stack_margin.h's registration comment */

    if (name) {
        *name = e->name;
    }
    if (configured_stack_bytes) {
        *configured_stack_bytes = e->configured_stack_bytes;
    }

    if (handle == NULL) {
        if (hwm_bytes) {
            *hwm_bytes = 0;
        }
        if (level) {
            *level = STACK_MARGIN_LEVEL_OK;
        }
        if (alive) {
            *alive = false;
        }
        return true;
    }

    /* uxTaskGetStackHighWaterMark() returns WORDS -- see stack_margin_calc.h's
     * STACK_MARGIN_WORD_BYTES comment for why that conversion is not
     * optional. */
    UBaseType_t hwm_words = uxTaskGetStackHighWaterMark(handle);
    uint32_t bytes = stack_margin_words_to_bytes((uint32_t)hwm_words);

    if (hwm_bytes) {
        *hwm_bytes = bytes;
    }
    if (level) {
        *level = stack_margin_classify(bytes, e->configured_stack_bytes);
    }
    if (alive) {
        *alive = true;
    }
    return true;
}
