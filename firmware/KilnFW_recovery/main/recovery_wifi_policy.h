// recovery_wifi_policy.h -- pure decisions of the recovery Wi-Fi bring-up, kept
// free of ESP-IDF types so a host test can exercise them
// (check_recovery_wifi_policy.ps1).
#ifndef RECOVERY_WIFI_POLICY_H
#define RECOVERY_WIFI_POLICY_H

#include <stdbool.h>

// recovery_wifi_start() may hand the per-boot SoftAP passphrase to the driver
// (esp_wifi_set_config / esp_wifi_start) only if esp_wifi_set_storage(
// WIFI_STORAGE_RAM) returned ESP_OK (0). Any other value means the driver might
// persist that passphrase to NVS, so AP bring-up must be refused.
static inline bool rwifi_may_configure_ap(int storage_rc)
{
    return storage_rc == 0;
}

#endif // RECOVERY_WIFI_POLICY_H
