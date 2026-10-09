// Host-test stub -- see esp_err.h's own header comment for why these exist.
// Added 2026-08-22 for crash_report.c's host tests.
//
// esp_core_dump_summary_t here is a type-only stand-in shaped like the real
// xtensa esp_core_dump_summary_t (esp-idf/components/espcoredump/include/
// esp_core_dump.h + include/port/xtensa/esp_core_dump_summary_port.h) --
// only the fields crash_report.c's fill_from_summary() actually reads.
// esp_core_dump_image_check() always reports "not found": crash_report.c's
// only caller of these functions is crash_report_init(), which host tests do
// not exercise (no real coredump exists on a host build) -- see
// test_crash_report.c's header comment for what IS tested instead
// (compute_crc/record_valid/persist/load, directly).
#ifndef TEST_STUB_ESP_CORE_DUMP_H
#define TEST_STUB_ESP_CORE_DUMP_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t bt[16];
    uint32_t depth;
    bool corrupted;
} esp_core_dump_bt_info_t;

typedef struct {
    uint32_t exc_cause;
    uint32_t exc_vaddr;
    /* Mirrors the real esp_core_dump_summary_extra_info_t (espcoredump/include/
     * port/xtensa/esp_core_dump_summary_port.h): the a-register set saved at the
     * exception. Only a0/a1 are consumed by crash_report.c -- a1 is the crashing
     * frame's STACK POINTER, the field that tells a null-struct-pointer
     * dereference apart from a null/garbage stack pointer. The full 16 are
     * declared so the stub keeps matching the real header's shape. */
    uint32_t exc_a[16];
} esp_core_dump_summary_extra_info_t;

typedef struct {
    char exc_task[16];
    uint32_t exc_pc;
    esp_core_dump_bt_info_t exc_bt_info;
    esp_core_dump_summary_extra_info_t ex_info;
} esp_core_dump_summary_t;

static inline esp_err_t esp_core_dump_image_check(void)
{
    return ESP_ERR_NOT_FOUND;
}

/* Call counter added for test_crash_report.c's
 * test_init_reaches_summary_fetch_when_coredump_present(): that test needs
 * to prove crash_report_init() actually GATES this call on
 * hal_sysinfo_coredump_present() rather than always (or never) reaching it --
 * crash_report_get() returning false is consistent with either branch, so it
 * cannot distinguish them on its own. */
static int s_esp_core_dump_get_summary_calls = 0;

static inline int test_esp_core_dump_get_summary_call_count(void)
{
    return s_esp_core_dump_get_summary_calls;
}

static inline void test_esp_core_dump_get_summary_reset_count(void)
{
    s_esp_core_dump_get_summary_calls = 0;
}

static inline esp_err_t esp_core_dump_get_summary(esp_core_dump_summary_t *summary)
{
    (void)summary;
    s_esp_core_dump_get_summary_calls++;
    return ESP_ERR_NOT_FOUND;
}

static inline esp_err_t esp_core_dump_image_erase(void)
{
    return ESP_OK;
}

#endif // TEST_STUB_ESP_CORE_DUMP_H
