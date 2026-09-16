#include "kiln_board_identity.h"

#include <string.h>

#include "esp_crc.h" /* esp_crc32_le() -- same primitive kiln_package.c already uses (host-
                       * testable via test/stubs/esp_crc.h, a real CRC32, not a fake). */
#include "esp_mac.h" /* esp_efuse_mac_get_default() -- host-testable via test/stubs/esp_mac.h,
                       * which returns a fixed fake MAC so this module links and behaves
                       * deterministically off-target. */

static bool s_override_active = false;
static uint32_t s_override_value = 0;

void kiln_board_identity_set_test_override(bool active, uint32_t value)
{
    s_override_active = active;
    s_override_value = value;
}

uint32_t kiln_board_identity_get(void)
{
    if (s_override_active) {
        return s_override_value;
    }
    uint8_t mac[6] = {0};
    (void)esp_efuse_mac_get_default(mac); /* on any failure, mac[] stays all-zero, still a
                                            * deterministic (if degenerate) id -- never left
                                            * uninitialized. */
    return esp_crc32_le(0, mac, sizeof(mac));
}
