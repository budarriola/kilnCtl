/* nvs_key_check.h -- compile-time guard against NVS key/namespace literals
 * that are too long for real hardware to ever accept.
 *
 * ESP-IDF's NVS_KEY_NAME_MAX_SIZE is 16 bytes INCLUDING the NUL terminator
 * (nvs.h), and nvs_page.cpp enforces the same limit on namespace strings
 * passed to nvs_open/nvs_open_from_partition, not just on keys -- both go
 * through Item::MAX_KEY_LENGTH = sizeof(key)-1 = 15 usable characters. A
 * 16th character is silently rejected at nvs_set_*()/nvs_open() time with
 * ESP_ERR_NVS_KEY_TOO_LONG / ESP_ERR_NVS_INVALID_NAME on real hardware.
 *
 * This class of bug shipped and ran unnoticed: zones_config_store.c's
 * "zone_normals_cfg" (16 chars) was introduced in ddbd024 and every write
 * under it failed at nvs_set_blob() from that commit onward, because the
 * pre-HAL-migration host stub (test/stubs/nvs.h) modeled one shared blob
 * slot with no key-length check at all, and the production setter did not
 * log its own failure. Apply NVS_KEY_LEN_CHECK() to every NVS_KEY_..,
 * NVS_NAMESPACE and ..._PARTITION string literal so a too-long key or
 * namespace fails the build instead of failing silently at runtime.
 *
 * sizeof() on a string literal includes its own NUL, so `sizeof(lit) - 1`
 * is the same character count nvs_page.cpp's strlen(key)/strlen(ns) check
 * uses.
 */
#ifndef KILNCTL_NVS_KEY_CHECK_H
#define KILNCTL_NVS_KEY_CHECK_H

#define NVS_KEY_LEN_CHECK(lit) \
    _Static_assert(sizeof(lit) - 1 <= 15, #lit " exceeds NVS's 15-character key/namespace limit (NVS_KEY_NAME_MAX_SIZE=16 including NUL)")

#endif /* KILNCTL_NVS_KEY_CHECK_H */
