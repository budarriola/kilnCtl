// Host-test stub for esp_spiffs.h -- ESP-IDF's SPIFFS VFS wrapper. Added for
// docs/FILESYSTEM_PLAN.md step 2's log_store_mount.c host test
// (test_log_store_mount.c), which needs log_store_mount.c to compile/link on
// the host for BOTH its SPIFFS branch (CONFIG_KILNCTL_LOGS_LITTLEFS off,
// default) and its LittleFS branch (CONFIG_KILNCTL_LOGS_LITTLEFS on) -- see
// stubs/esp_littlefs.h for the LittleFS side, added earlier for
// cfg_fs_mount.c. Same "declared here, defined per test executable"
// convention as that file: every symbol below is declared only, with a
// trivial per-executable definition that records which mount call happened
// rather than touching any real filesystem.
#ifndef TEST_STUB_ESP_SPIFFS_H
#define TEST_STUB_ESP_SPIFFS_H

#include <stddef.h>

#include "esp_err.h"

typedef struct {
    const char *base_path;
    const char *partition_label;
    size_t max_files;
    bool format_if_mount_failed;
} esp_vfs_spiffs_conf_t;

esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *conf);
esp_err_t esp_spiffs_info(const char *partition_label, size_t *total_bytes, size_t *used_bytes);

#endif // TEST_STUB_ESP_SPIFFS_H
