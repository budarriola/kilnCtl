/* hal_sysinfo_esp.c -- ESP-IDF backend for interface/hal_sysinfo.h.
 *
 * Phase 1b ("adapt"): implements the Phase-0 interface against ESP-IDF
 * v6.0.2 (C:\esp\v6.0.2\esp-idf), grounded in hal_sysinfo.h's own consumer
 * census: esp_reset_reason() (dashboard_http.c:403, ui_page_diagnostics.c:
 * 520, crash_report.c:312), esp_ota_get_running_partition() (dashboard_
 * http.c:122, ui_page_diagnostics.c:522, partition_info_http.c:94,
 * ota_http_esp.c:485), esp_app_get_description() (dashboard_http.c:387,
 * ui_page_diagnostics.c:511), the ESP32-S3 on-die temperature sensor
 * (board_temps.c's install/enable/get_celsius/uninstall sequence,
 * TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80) range -- board_temps.c's own
 * 2026-08-20 fix comment on why that exact range, reproduced verbatim
 * here), esp_random() (safety_link.c:358), and crash_report.c's
 * esp_core_dump_image_check()/_erase() pair. Wired into
 * firmware/hwAbstraction/idf/hwabstraction_esp/CMakeLists.txt as part of the
 * hwabstraction_esp component (pulled into KilnFW's build transitively, not
 * listed directly in firmware/KilnFW/App/drivers/CMakeLists.txt).
 *
 * esp_partition stays read-only here; OTA writes (esp_ota_set_boot_
 * partition, esp_ota_begin/write/end) are explicitly out of scope per the
 * plan and this header. Full esp_core_dump_summary_t parsing likewise
 * stays out of scope -- crash_report.c's fill_from_summary()/crash_report_
 * dump_id() keep taking the live IDF type directly, above this header.
 *
 * No interface mismatch found: every hal_sysinfo.h operation maps 1:1 onto
 * one real IDF call (or, for the temperature sensor, the same install/
 * enable/get/uninstall lifecycle board_temps.c already uses), and the
 * header's explicit scope limits (read-only partitions, no full core-dump
 * summary) match this tree's real usage exactly.
 */
#include "hal_sysinfo.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "driver/temperature_sensor.h"

#include "hal_esp_common.h"

/* main_boot_early.c logs hal_sysinfo_reset_reason()'s result as
 * "esp_reset_reason=%d" -- i.e. it assumes the hal_reset_reason_t value it
 * gets back is numerically identical to the underlying esp_reset_reason_t
 * value, not just semantically mapped by the switch below. hal_reset_reason_t
 * was deliberately declared in the same 0..15 order as esp_reset_reason_t to
 * make that true; enforce it at compile time so a future reordering of
 * either enum cannot silently break that log line. */
_Static_assert((int)HAL_RESET_UNKNOWN     == (int)ESP_RST_UNKNOWN,     "hal_reset_reason_t must mirror esp_reset_reason_t numerically");
_Static_assert((int)HAL_RESET_POWERON     == (int)ESP_RST_POWERON,     "hal_reset_reason_t must mirror esp_reset_reason_t numerically");
_Static_assert((int)HAL_RESET_EXT         == (int)ESP_RST_EXT,         "hal_reset_reason_t must mirror esp_reset_reason_t numerically");
_Static_assert((int)HAL_RESET_SW          == (int)ESP_RST_SW,          "hal_reset_reason_t must mirror esp_reset_reason_t numerically");
_Static_assert((int)HAL_RESET_PANIC       == (int)ESP_RST_PANIC,       "hal_reset_reason_t must mirror esp_reset_reason_t numerically");
_Static_assert((int)HAL_RESET_INT_WDT     == (int)ESP_RST_INT_WDT,     "hal_reset_reason_t must mirror esp_reset_reason_t numerically");
_Static_assert((int)HAL_RESET_TASK_WDT    == (int)ESP_RST_TASK_WDT,    "hal_reset_reason_t must mirror esp_reset_reason_t numerically");
_Static_assert((int)HAL_RESET_WDT         == (int)ESP_RST_WDT,         "hal_reset_reason_t must mirror esp_reset_reason_t numerically");
_Static_assert((int)HAL_RESET_DEEPSLEEP   == (int)ESP_RST_DEEPSLEEP,   "hal_reset_reason_t must mirror esp_reset_reason_t numerically");
_Static_assert((int)HAL_RESET_BROWNOUT    == (int)ESP_RST_BROWNOUT,    "hal_reset_reason_t must mirror esp_reset_reason_t numerically");
_Static_assert((int)HAL_RESET_SDIO        == (int)ESP_RST_SDIO,        "hal_reset_reason_t must mirror esp_reset_reason_t numerically");
_Static_assert((int)HAL_RESET_USB         == (int)ESP_RST_USB,         "hal_reset_reason_t must mirror esp_reset_reason_t numerically");
_Static_assert((int)HAL_RESET_JTAG        == (int)ESP_RST_JTAG,        "hal_reset_reason_t must mirror esp_reset_reason_t numerically");
_Static_assert((int)HAL_RESET_EFUSE       == (int)ESP_RST_EFUSE,       "hal_reset_reason_t must mirror esp_reset_reason_t numerically");
_Static_assert((int)HAL_RESET_PWR_GLITCH  == (int)ESP_RST_PWR_GLITCH,  "hal_reset_reason_t must mirror esp_reset_reason_t numerically");
_Static_assert((int)HAL_RESET_CPU_LOCKUP  == (int)ESP_RST_CPU_LOCKUP, "hal_reset_reason_t must mirror esp_reset_reason_t numerically");

hal_reset_reason_t hal_sysinfo_reset_reason(void) {
    /* esp_reset_reason_t's real values, mapped 1:1 -- this header hands
     * back the enum only; both real call sites (dashboard_http.c's
     * reset_reason_name(), ui_page_diagnostics.c's reset_reason_str())
     * keep their own display-string tables above this backend, per
     * hal_sysinfo.h's own comment on why the two must not be collapsed. */
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return HAL_RESET_POWERON;
        case ESP_RST_EXT:       return HAL_RESET_EXT;
        case ESP_RST_SW:        return HAL_RESET_SW;
        case ESP_RST_PANIC:     return HAL_RESET_PANIC;
        case ESP_RST_INT_WDT:   return HAL_RESET_INT_WDT;
        case ESP_RST_TASK_WDT:  return HAL_RESET_TASK_WDT;
        case ESP_RST_WDT:       return HAL_RESET_WDT;
        case ESP_RST_DEEPSLEEP: return HAL_RESET_DEEPSLEEP;
        case ESP_RST_BROWNOUT:  return HAL_RESET_BROWNOUT;
        case ESP_RST_SDIO:      return HAL_RESET_SDIO;
        case ESP_RST_USB:       return HAL_RESET_USB;
        case ESP_RST_JTAG:      return HAL_RESET_JTAG;
        case ESP_RST_EFUSE:     return HAL_RESET_EFUSE;
        case ESP_RST_PWR_GLITCH:return HAL_RESET_PWR_GLITCH;
        case ESP_RST_CPU_LOCKUP:return HAL_RESET_CPU_LOCKUP;
        case ESP_RST_UNKNOWN:
        default:                return HAL_RESET_UNKNOWN;
    }
}

hal_status_t hal_sysinfo_get_running_partition(hal_sysinfo_partition_info_t *out) {
    if (!out) {
        return HAL_INVALID_ARG;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return HAL_IO;
    }
    memset(out, 0, sizeof(*out));
    /* esp_partition_t::label is a 16-byte array, not guaranteed NUL-
     * terminated if a label ever used all 16 bytes -- copy N-1 and let the
     * destination's own NUL (from the memset above) terminate it, same
     * defensive stance dashboard_http.c already takes on every fixed-size
     * IDF field. */
    memcpy(out->label, running->label, sizeof(out->label) - 1);
    out->address = (uint32_t)running->address;
    out->size = (uint32_t)running->size;
    return HAL_OK;
}

void hal_sysinfo_get_build_info(hal_sysinfo_build_info_t *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    const esp_app_desc_t *app_desc = esp_app_get_description();
    out->valid = (app_desc != NULL);
    if (app_desc) {
        /* esp_app_desc_t's version/date/time are fixed-size arrays, not
         * guaranteed NUL-terminated if a build ever filled one completely --
         * snprintf's size argument only bounds the WRITE into out->*, not how
         * far it reads looking for a NUL in app_desc->*, so "%s" here could
         * over-read past the field. Bound the read explicitly instead, same
         * defensive memcpy(dst, src, sizeof(dst)-1) stance
         * hal_sysinfo_get_running_partition() already takes on
         * esp_partition_t::label above, relying on this function's own
         * memset() above for the terminating NUL. */
        memcpy(out->version, app_desc->version, sizeof(out->version) - 1);
        memcpy(out->date, app_desc->date, sizeof(out->date) - 1);
        memcpy(out->time, app_desc->time, sizeof(out->time) - 1);
    }
}

/* This peripheral's install-once/enable-once handle. board_temps.c
 * (firmware/KilnFW/App/drivers/hw/board_temps.c) is the sole caller of the
 * hal_sysinfo_temp_* lifecycle as of the 2026-09-06 migration -- see that
 * file's board_temps_start() header comment. (Formerly an OWNERSHIP HAZARD
 * lived here: board_temps.c used to own a second, independent
 * temperature_sensor_handle_t and call temperature_sensor_install()/_enable()
 * on this same physical sensor directly, which would have failed had
 * anything ever also called hal_sysinfo_temp_init() -- the IDF driver only
 * supports one live install. Resolved by deleting that second lifecycle, not
 * by adding a guard: board_temps.c now calls only the functions below.) */
static temperature_sensor_handle_t s_tsens;
static bool s_tsens_ready;

hal_status_t hal_sysinfo_temp_init(void) {
    if (s_tsens_ready) {
        return HAL_OK; /* board_temps_start()'s own already-up no-op */
    }
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&cfg, &s_tsens);
    if (err != ESP_OK) {
        s_tsens = NULL;
        return hal_esp_err_to_status(err);
    }
    err = temperature_sensor_enable(s_tsens);
    if (err != ESP_OK) {
        temperature_sensor_uninstall(s_tsens);
        s_tsens = NULL;
        return hal_esp_err_to_status(err);
    }
    s_tsens_ready = true;
    return HAL_OK;
}

hal_status_t hal_sysinfo_temp_read_celsius(float *out_c) {
    if (!out_c) {
        return HAL_INVALID_ARG;
    }
    if (!s_tsens_ready || !s_tsens) {
        return HAL_NOT_READY;
    }
    esp_err_t err = temperature_sensor_get_celsius(s_tsens, out_c);
    return hal_esp_err_to_status(err);
}

hal_status_t hal_sysinfo_temp_deinit(void) {
    if (!s_tsens_ready || !s_tsens) {
        return HAL_NOT_READY;
    }
    esp_err_t err = temperature_sensor_disable(s_tsens);
    esp_err_t err2 = temperature_sensor_uninstall(s_tsens);
    s_tsens = NULL;
    s_tsens_ready = false;
    if (err != ESP_OK) {
        return hal_esp_err_to_status(err);
    }
    return hal_esp_err_to_status(err2);
}

uint32_t hal_sysinfo_random_u32(void) {
    return esp_random();
}

void hal_sysinfo_fill_random(void *buf, size_t len) {
    if (len == 0) {
        return;
    }
    esp_fill_random(buf, len);
}

bool hal_sysinfo_coredump_present(void) {
    return esp_core_dump_image_check() == ESP_OK;
}

hal_status_t hal_sysinfo_coredump_erase(void) {
    esp_err_t err = esp_core_dump_image_erase();
    return hal_esp_err_to_status(err);
}

/* espcoredump's on-flash image stores its own total length as a little-
 * endian uint32_t at partition offset 0 (core_dump_flash.c's
 * BLANK_COREDUMP_SIZE == 0xFFFFFFFF sentinel for "never written", same
 * constant crash_report.c's neighbor core_dump_flash.c compares against).
 * Reading it directly here (rather than adding a second, competing
 * "core-dump summary" abstraction) keeps this file's one job -- raw
 * partition bytes -- separate from crash_report.c's parsed-summary job. */
#define KILNCTL_COREDUMP_BLANK_LEN 0xFFFFFFFFu

static const esp_partition_t *find_coredump_partition(void) {
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
}

hal_status_t hal_sysinfo_coredump_get_info(hal_sysinfo_coredump_info_t *out) {
    if (!out) {
        return HAL_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    const esp_partition_t *part = find_coredump_partition();
    if (!part) {
        return HAL_IO; /* no coredump partition in the partition table at all */
    }
    out->partition_size = (uint32_t)part->size;
    out->present = hal_sysinfo_coredump_present();

    uint32_t data_len = KILNCTL_COREDUMP_BLANK_LEN;
    esp_err_t err = esp_partition_read(part, 0, &data_len, sizeof(data_len));
    if (err != ESP_OK) {
        return hal_esp_err_to_status(err);
    }
    out->data_len = data_len;
    return HAL_OK;
}

hal_status_t hal_sysinfo_coredump_read(uint32_t offset, void *buf, uint32_t len) {
    if (!buf || len == 0) {
        return HAL_INVALID_ARG;
    }
    const esp_partition_t *part = find_coredump_partition();
    if (!part) {
        return HAL_IO;
    }
    if ((uint64_t)offset + (uint64_t)len > (uint64_t)part->size) {
        return HAL_INVALID_ARG; /* refuse an out-of-range span outright, never truncate silently */
    }
    esp_err_t err = esp_partition_read(part, offset, buf, len);
    return hal_esp_err_to_status(err);
}
