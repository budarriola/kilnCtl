// recovery_main.c -- entry point for the OTA recovery image.
// docs/OTA_SINGLE_SLOT_PLAN.md, section 3: "anything beyond
// receive-an-image-and-write-it must justify itself against being one more
// thing that can fail in the one image that must never fail."
//
// SCOPE OF THIS PASS (section 8 step 1 -- build and measure the image; the
// board is never flashed as part of this pass, see the accompanying commit
// message and report for the full list of deliberate simplifications):
//   - Wi-Fi: station using the legacy single-network credential in
//     `wifi_nvs`, SoftAP fallback. recovery_wifi.c.
//   - HTTP: the six routes from section 3 item 2. recovery_http.c.
//   - LCD: one static line, minimal from-scratch SPI/SX1509 driver, no
//     touch, no LVGL. recovery_lcd.c.
// Deliberately excluded, per section 3's "Out" list: touch, config/zones/
// profiles access, the safety link, the Pico image, the control loop.
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "recovery_http.h"
#include "recovery_io.h"
#include "recovery_lcd.h"
#include "recovery_wifi.h"

static const char *TAG = "recovery_main";

void app_main(void)
{
    // Hold the kiln OFF before anything else -- before NVS init, which can
    // erase/abort and reboot-loop -- so relays IO0..IO3 (and IO5) are forced
    // low and driven, verified by read-back. Never blocks boot on failure
    // (uploads must still work) but sets a fault flag. Needs no NVS.
    recovery_io_hold_relays_off();

    ESP_LOGI(TAG, "KilnFW recovery image starting");

    // Recovery must NEVER erase any NVS partition it shares with the app:
    // not `kiln_nvs`/`wifi_nvs`, and not the default `nvs` partition either
    // (the app's own Wi-Fi driver/PHY data lives there, and a recovery boot
    // is exactly when the board is already in trouble). On any init failure
    // -- including NO_FREE_PAGES / NEW_VERSION_FOUND -- log it, record it so
    // the LCD, the page and /api/recovery/status say "nvs unavailable", and
    // carry on: uploads and the relay hold need no NVS.
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init() failed: %s -- NVS UNAVAILABLE, NOT erasing it; recovery "
                 "continues without NVS", esp_err_to_name(err));
        recovery_io_nvs_mark_failed(RECOVERY_NVS_FAIL_DEFAULT);
    }

    // recovery_wifi.c / recovery_http.c open wifi_nvs and kiln_nvs by
    // partition name, which fails with ESP_ERR_NVS_NOT_INITIALIZED unless
    // the partition was initialised first. Failures are logged, not fatal.
    static const char *const extra_parts[] = {"wifi_nvs", "kiln_nvs"};
    static const unsigned extra_bits[] = {RECOVERY_NVS_FAIL_WIFI, RECOVERY_NVS_FAIL_KILN};
    for (size_t i = 0; i < sizeof(extra_parts) / sizeof(extra_parts[0]); i++) {
        esp_err_t perr = nvs_flash_init_partition(extra_parts[i]);
        if (perr != ESP_OK) {
            // Never erased or reformatted here, whatever the error code.
            ESP_LOGE(TAG, "nvs_flash_init_partition(%s) failed: %s -- NVS UNAVAILABLE, NOT erasing",
                     extra_parts[i], esp_err_to_name(perr));
            recovery_io_nvs_mark_failed(extra_bits[i]);
        }
    }

    // If this boot is itself the fresh recovery image landing after an OTA
    // (the migration flash, or a future recovery-image update), confirm it
    // so the bootloader doesn't roll it back on the next reset. Harmless
    // no-op if this boot did not come from an OTA write.
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
    }

    recovery_lcd_show_message();
    recovery_wifi_start();
    recovery_http_start();

    ESP_LOGI(TAG, "recovery image up: wifi=%s", recovery_wifi_is_up() ? "up" : "down");
    // WARN level so it survives a trimmed log level: measured app_main stack
    // headroom, in bytes (uxTaskGetStackHighWaterMark is words * 1 on ESP-IDF).
    ESP_LOGW(TAG, "stack hwm: app_main min free = %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}
