// Host-test stub (update_fetch/update_http_refusals): bootloader_common_check_chip_validity is defined
// in fake_support.c with the real header min/max chip-revision rule for a chip at full revision 1.
#ifndef TEST_STUB_UF_BOOTLOADER_COMMON_H
#define TEST_STUB_UF_BOOTLOADER_COMMON_H

#include "esp_err.h"
#include "esp_image_format.h"

esp_err_t bootloader_common_check_chip_validity(const esp_image_header_t *img_hdr, esp_image_type type);

#endif
