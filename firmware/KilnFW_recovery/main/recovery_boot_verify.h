// recovery_boot_verify.h -- esp_ota_set_boot_partition() with a read-back.
//
// Mirror of KilnFW's App/drivers/persist/boot_partition_verify.[ch] (commit
// 75098657's helper). The recovery image is a separate IDF project and does not
// compile the application's tree, so this is a minimal copy, not a link. Keep
// the compare rule (address AND subtype) identical to the application's.
//
// esp_ota_set_boot_partition() returning ESP_OK says the otadata write call
// completed, not that the bootloader will pick the partition we asked for. This
// wrapper reads esp_ota_get_boot_partition() back; a mismatch is an ESP_LOGE and
// an error return, never a success the caller can report to HTTP or boot_guard.
#ifndef RECOVERY_BOOT_VERIFY_H
#define RECOVERY_BOOT_VERIFY_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

// Pure compare: true only if both are non-NULL and agree on address and subtype.
bool recovery_boot_partition_matches(const esp_partition_t *requested, const esp_partition_t *actual);

// ESP_OK only when the set succeeded AND the read-back matches. Returns the
// underlying error if the set failed (notably ESP_ERR_OTA_VALIDATE_FAILED, passed
// through unchanged), ESP_ERR_INVALID_ARG for NULL, ESP_ERR_INVALID_STATE if the
// read-back is NULL or differs.
esp_err_t recovery_boot_partition_set_and_verify(const esp_partition_t *requested);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_BOOT_VERIFY_H
