// boot_partition_verify.h -- esp_ota_set_boot_partition() with a read-back.
//
// esp_ota_set_boot_partition() returning ESP_OK says the otadata write call
// completed, not that the bootloader will pick the partition we asked for
// (a bad otadata sector write has been observed to report success). Every
// KilnFW caller therefore goes through boot_partition_set_and_verify(), which
// reads esp_ota_get_boot_partition() back and compares address and subtype to
// the request; a mismatch is an ESP_LOGE naming both partitions and an error
// return, never a success the caller could report to HTTP or boot_guard.
//
// The standalone recovery image (firmware/KilnFW_recovery) has its own call
// sites only in recovery_http.c and does not compile this file.
#ifndef BOOT_PARTITION_VERIFY_H
#define BOOT_PARTITION_VERIFY_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

// Pure compare: true only if both are non-NULL and agree on address and subtype.
bool boot_partition_matches(const esp_partition_t *requested, const esp_partition_t *actual);

// Returns ESP_OK only when the set succeeded AND the read-back matches.
// Returns the underlying error if the set failed, ESP_ERR_INVALID_ARG for a NULL
// request, ESP_ERR_INVALID_STATE if the read-back is NULL or differs.
esp_err_t boot_partition_set_and_verify(const esp_partition_t *requested);

#ifdef __cplusplus
}
#endif

#endif // BOOT_PARTITION_VERIFY_H
