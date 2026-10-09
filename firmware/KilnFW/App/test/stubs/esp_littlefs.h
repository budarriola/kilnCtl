// Host-test stub for esp_littlefs.h -- ESP-IDF's LittleFS VFS wrapper.
// Added for cfg_fs_mount.c's host tests (test_cfg_fs_mount_reentrancy.c),
// which needs cfg_fs_mount.c to compile/link on the host to exercise
// cfg_fs_write_atomic_device()'s real re-entrancy guard -- the ONE function
// this test cares about. Same "declared here, defined per test executable"
// convention as stubs/esp_partition.h: every symbol below is declared only,
// with a trivial per-executable definition, and none of them is ever
// actually invoked by this test (it never calls cfg_fs_mount_device()
// itself, only cfg_fs_write_atomic_device()).
#ifndef TEST_STUB_ESP_LITTLEFS_H
#define TEST_STUB_ESP_LITTLEFS_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_partition.h"

typedef struct {
    const char *base_path;
    const char *partition_label;
    const esp_partition_t *partition;
    bool format_if_mount_failed;
} esp_vfs_littlefs_conf_t;

esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *conf);
esp_err_t esp_vfs_littlefs_unregister(const char *partition_label);
esp_err_t esp_littlefs_format(const char *partition_label);
esp_err_t esp_littlefs_info(const char *partition_label, size_t *total_bytes, size_t *used_bytes);

#endif // TEST_STUB_ESP_LITTLEFS_H
