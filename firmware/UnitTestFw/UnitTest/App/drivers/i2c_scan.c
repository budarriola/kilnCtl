#include "i2c_scan.h"

#include "esp_log.h"

static const char *TAG = "i2c_scan";

/* Valid 7-bit address range per the I2C spec -- 0x00-0x07 and 0x78-0x7F are
 * reserved (general call, high-speed mode, 10-bit addressing, etc.), not
 * usable by ordinary devices, so probing them would only risk confusing
 * whatever else is listening rather than finding anything real. */
#define I2C_SCAN_ADDR_MIN 0x08u
#define I2C_SCAN_ADDR_MAX 0x77u

/* Short: this only has to outlast a single START/address-byte/STOP, and a
 * sluggish/misbehaving device shouldn't be allowed to stall the whole scan
 * waiting on it. */
#define I2C_SCAN_PROBE_TIMEOUT_MS 50

void i2c_scan_bus(i2c_master_bus_handle_t bus)
{
    if (!bus) {
        ESP_LOGW(TAG, "scan skipped: no bus handle");
        return;
    }

    ESP_LOGI(TAG, "scanning I2C bus (0x%02X-0x%02X)...", I2C_SCAN_ADDR_MIN, I2C_SCAN_ADDR_MAX);

    unsigned found = 0;
    for (uint16_t addr = I2C_SCAN_ADDR_MIN; addr <= I2C_SCAN_ADDR_MAX; ++addr) {
        esp_err_t err = i2c_master_probe(bus, addr, I2C_SCAN_PROBE_TIMEOUT_MS);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "found device at 0x%02X", addr);
            found++;
        } else if (err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "probe 0x%02X: %s", addr, esp_err_to_name(err));
        }
    }

    if (found == 0) {
        ESP_LOGW(TAG, "scan complete: no devices responded");
    } else {
        ESP_LOGI(TAG, "scan complete: %u device(s) found", found);
    }
}
