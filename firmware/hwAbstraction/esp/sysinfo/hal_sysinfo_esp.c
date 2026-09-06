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
 * esp_core_dump_image_check()/_erase() pair. Not wired into any
 * CMakeLists yet -- see
 * firmware/hwAbstraction/test/compile_esp_backends.ps1.
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

#include <string.h>

#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "driver/temperature_sensor.h"

#include "hal_esp_common.h"

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
        snprintf(out->version, sizeof(out->version), "%s", app_desc->version);
        snprintf(out->date, sizeof(out->date), "%s", app_desc->date);
        snprintf(out->time, sizeof(out->time), "%s", app_desc->time);
    }
}

/* board_temps.c's own install-once/enable-once handle, reproduced here --
 * see this file's header comment for why (-10, 80) is the exact range,
 * not a rederivation. */
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

bool hal_sysinfo_coredump_present(void) {
    return esp_core_dump_image_check() == ESP_OK;
}

hal_status_t hal_sysinfo_coredump_erase(void) {
    esp_err_t err = esp_core_dump_image_erase();
    return hal_esp_err_to_status(err);
}
