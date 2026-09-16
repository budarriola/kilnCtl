// Host-test stub for ESP-IDF's esp_mac.h -- see esp_err.h's own header
// comment for why these exist. Added 2026-09-16 for kiln_board_identity.c
// (docs/KILN_PROFILES_PLAN.md section 5.3 rows 2/3, board-id mint/compare).
//
// Returns a FIXED, deterministic fake MAC -- not random, not zero -- so
// kiln_board_identity_get() is stable across host-test runs with no
// override set. A test that needs a SECOND, distinct board identity (to
// exercise the "package crossed boards" path) uses
// kiln_board_identity_set_test_override() instead of relying on this stub
// ever returning anything else.
#ifndef TEST_STUB_ESP_MAC_H
#define TEST_STUB_ESP_MAC_H

#include <stdint.h>

#include "esp_err.h"

static inline esp_err_t esp_efuse_mac_get_default(uint8_t *mac)
{
    static const uint8_t fake[6] = {0x24, 0x6F, 0x28, 0x11, 0x22, 0x33};
    for (int i = 0; i < 6; i++) {
        mac[i] = fake[i];
    }
    return ESP_OK;
}

#endif // TEST_STUB_ESP_MAC_H
