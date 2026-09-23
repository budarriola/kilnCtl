// Host-test stub -- see stubs/esp_err.h for why these exist. Added
// 2026-08-27 for ota_http.c's host tests (test_ota_http.c). Every function
// here is declared only, defined per-executable (same "declared once,
// defined per test file" split stubs/esp_http_server.h already uses) --
// none of these is ever invoked by test_ota_http.c's tests (only
// ota_http_verify_request()/ota_http_authenticate_request() are called
// directly), but the whole translation unit must still link.
#ifndef TEST_STUB_ESP_PARTITION_H
#define TEST_STUB_ESP_PARTITION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t address;
    uint32_t size;
    char label[17];
    int type;
    int subtype;
    bool encrypted;
} esp_partition_t;

esp_err_t esp_partition_write(const esp_partition_t *partition, size_t dst_offset, const void *src, size_t size);
esp_err_t esp_partition_erase_range(const esp_partition_t *partition, size_t offset, size_t size);
uint32_t esp_partition_get_main_flash_sector_size(void);

/* Added for partition_info_http.c's host tests (GET /api/partitions) --
 * same "declared here, defined per test executable" convention as the
 * three functions above. Numeric values match the real ESP-IDF
 * esp_partition_type_t/esp_partition_subtype_t encoding (components/
 * esp_partition/include/esp_partition.h) so a host test's canned data and
 * a real board's live partition table use the identical constants. */
typedef int esp_partition_type_t;
typedef int esp_partition_subtype_t;
#define ESP_PARTITION_TYPE_APP 0x00
#define ESP_PARTITION_TYPE_DATA 0x01
#define ESP_PARTITION_SUBTYPE_ANY 0xff
/* Added for ota_pico_relay.c's host test (test_ota_pico_relay.c) -- value
 * matches real ESP-IDF's ESP_PARTITION_SUBTYPE_DATA_UNDEFINED encoding,
 * same convention as the constants above. */
#define ESP_PARTITION_SUBTYPE_DATA_UNDEFINED 0x06

typedef struct esp_partition_iterator_opaque_t *esp_partition_iterator_t;

esp_partition_iterator_t esp_partition_find(esp_partition_type_t type, esp_partition_subtype_t subtype, const char *label);
const esp_partition_t *esp_partition_get(esp_partition_iterator_t iterator);
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t iterator);
esp_err_t esp_partition_iterator_release(esp_partition_iterator_t iterator);

/* Added for cfg_fs_mount.c's host tests (test_cfg_fs_mount_reentrancy.c) --
 * same "declared here, defined per test executable" convention as the
 * others above. */
const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                                 const char *label);
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t src_offset, void *dst, size_t size);

#endif // TEST_STUB_ESP_PARTITION_H
