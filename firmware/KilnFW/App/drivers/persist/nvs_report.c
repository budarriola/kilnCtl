#include "nvs_report.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "hal_kv.h"

static const char *TAG = "nvs_report";

static nvs_report_section_t s_sections[NVS_REPORT_MAX_SECTIONS];
static size_t s_count;

static const char *const kPartitions[NVS_REPORT_MAX_SECTIONS] = {
    "wifi_nvs",
    "kiln_nvs",
    "profiles_nvs",
    NULL,
};

void nvs_report_capture(void)
{
    s_count = 0;
    for (size_t i = 0; kPartitions[i] != NULL && s_count < NVS_REPORT_MAX_SECTIONS; i++) {
        const char *name = kPartitions[i];
        nvs_report_section_t *sec = &s_sections[s_count++];
        sec->name = name;

        const esp_partition_t *part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, name);
        sec->present = (part != NULL);
        if (!sec->present) {
            sec->mounted = false;
            ESP_LOGW(TAG, "partition '%s' not present in flashed partition table", name);
            continue;
        }

        /* hal_kv_mount_probe(), NOT hal_kv_init_partition() -- every module
         * that owns one of these partitions has already called its own
         * version of the erase-retry init before nvs_report_capture() runs
         * (see main.c ordering), so this only needs to OBSERVE the outcome.
         * hal_kv_init_partition() would erase-and-retry on its own if it
         * found NO_FREE_PAGES/NEW_VERSION_FOUND, which is never this
         * function's call to make -- a report/diagnostics pass must never
         * itself destroy the very data it is reporting on. */
        hal_status_t err = hal_kv_mount_probe(name);
        sec->mounted = (err == HAL_OK);
        if (!sec->mounted) {
            ESP_LOGW(TAG, "partition '%s' present but not mounted: %s", name, hal_status_to_name(err));
        }
    }
}

const nvs_report_section_t *nvs_report_get(size_t *out_count)
{
    if (out_count) {
        *out_count = s_count;
    }
    return s_sections;
}
