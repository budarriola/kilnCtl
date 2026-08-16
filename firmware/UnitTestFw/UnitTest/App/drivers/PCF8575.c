#include "PCF8575.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "settings.h"

static const char *TAG = "PCF8575";

/* The part has no command/register byte: a write is just the two port bytes,
 * a read is just the two port bytes back. Low byte is P00-P07, high byte is
 * P10-P17 -- i.e. little-endian, which is also how the UART payloads carry
 * the 16-bit port value (see uart_task_ids.h). */
#define PCF8575_PORT_BYTES 2

/* One transfer only ever moves 2 bytes; this just has to cover a bus that's
 * momentarily busy with another device's queued transaction. */
#define PCF8575_TIMEOUT_MS 1000

/* Same rationale as i2c_scan.c's probe timeout: long enough for one
 * START/address/STOP, short enough that a sluggish device can't stall the
 * whole 8-address sweep. */
#define PCF8575_PROBE_TIMEOUT_MS 50

/* Every device on this shared bus runs at the one project-wide clock
 * (CONFIG_I2C_MASTER_FREQUENCY) -- a slower device would otherwise be talked
 * to at whatever rate its own driver picked. */
#define PCF8575_I2C_CLK_HZ I2C_MASTER_FREQ_HZ

static bool pcf8575_addr_valid(uint8_t addr)
{
    return addr >= PCF8575_ADDR_MIN && addr <= PCF8575_ADDR_MAX;
}

static esp_err_t pcf8575_add_device(PCF8575Class *exp, uint8_t addr)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = PCF8575_I2C_CLK_HZ,
    };
    return i2c_master_bus_add_device(exp->bus, &dev_config, &exp->dev);
}

static esp_err_t pcf8575_transfer(PCF8575Class *exp, const uint8_t *tx, size_t tx_len,
                                  uint8_t *rx, size_t rx_len)
{
    if (exp->owner_initialized) {
        return i2c_owner_transfer(&exp->owner, exp->dev, tx, tx_len, rx, rx_len,
                                  PCF8575_TIMEOUT_MS);
    }
    if (tx && tx_len > 0) {
        return i2c_master_transmit(exp->dev, tx, tx_len, PCF8575_TIMEOUT_MS);
    }
    return i2c_master_receive(exp->dev, rx, rx_len, PCF8575_TIMEOUT_MS);
}

esp_err_t PCF8575_init(PCF8575Class *exp, i2c_master_bus_handle_t bus, uint8_t addr)
{
    if (!exp || !bus || !pcf8575_addr_valid(addr)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(exp, 0, sizeof(*exp));
    exp->bus = bus;
    exp->addr = addr;
    exp->shadow = PCF8575_PORT_POWER_ON_STATE;

    esp_err_t err = pcf8575_add_device(exp, addr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device(0x%02X) failed: %s", addr, esp_err_to_name(err));
        return err;
    }

    /* Independent i2c_owner on the same (already-existing) bus handle -- this
     * driver never creates or destroys the bus itself, same arrangement as
     * SSD1306_init. */
    err = i2c_owner_init(&exp->owner, bus, 8, 5, 4096, tskNO_AFFINITY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_owner_init failed: %s", esp_err_to_name(err));
        i2c_master_bus_rm_device(exp->dev);
        exp->dev = NULL;
        return err;
    }
    exp->owner_initialized = true;

    /* Deliberately no port write here: the pins come out of power-on reset
     * all-high (which the shadow already reflects), and anything wired to
     * them shouldn't be yanked low just because the firmware restarted. */
    ESP_LOGI(TAG, "PCF8575 initialized addr=0x%02X", addr);
    return ESP_OK;
}

esp_err_t PCF8575_deinit(PCF8575Class *exp)
{
    if (!exp) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_OK;
    if (exp->owner_initialized) {
        esp_err_t e = i2c_owner_deinit(&exp->owner);
        if (e != ESP_OK) err = e;
        exp->owner_initialized = false;
    }
    if (exp->dev) {
        esp_err_t e = i2c_master_bus_rm_device(exp->dev);
        if (e != ESP_OK) err = e;
        exp->dev = NULL;
    }
    // The bus itself belongs to whoever created it (DcDac); never delete it here.
    return err;
}

esp_err_t PCF8575_start(PCF8575Class *exp, i2c_master_bus_handle_t bus)
{
    if (!exp || !bus) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Probe before committing to the address, same reasoning as
     * PCF8575_set_address: an absent part turns the read-back below into a
     * NACKed transfer that the driver keeps retrying -- bus reset and all --
     * for the whole PCF8575_TIMEOUT_MS. A single probe costs one
     * START/address/STOP and PCF8575_PROBE_TIMEOUT_MS at worst. */
    esp_err_t probe_err = i2c_master_probe(bus, PCF8575_I2C_ADDR, PCF8575_PROBE_TIMEOUT_MS);
    if (probe_err != ESP_OK) {
        ESP_LOGE(TAG, "no PCF8575 at 0x%02X: %s", PCF8575_I2C_ADDR,
                 esp_err_to_name(probe_err));
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = PCF8575_init(exp, bus, PCF8575_I2C_ADDR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCF8575_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* One read-back so a part that's configured but not actually present
     * shows up as an error line at boot (forwarded to the PC like any other
     * ESP_LOGx -- see uart_log_bridge.h) rather than only failing later on
     * the first command from the GUI. */
    uint16_t port = 0;
    err = PCF8575_read_port(exp, &port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCF8575 at 0x%02X did not answer a read: %s", exp->addr,
                 esp_err_to_name(err));
        PCF8575_deinit(exp);
        return err;
    }

    ESP_LOGI(TAG, "PCF8575 ready at 0x%02X, port reads 0x%04X", exp->addr, port);
    return ESP_OK;
}

esp_err_t PCF8575_set_address(PCF8575Class *exp, uint8_t addr)
{
    if (!exp || !exp->bus || !pcf8575_addr_valid(addr)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (addr == exp->addr && exp->dev) {
        return ESP_OK;
    }

    /* Refuse to point at an address nothing lives on. i2c_master_bus_add_device
     * happily registers any address -- the failure only shows up later, one
     * NACKed transfer at a time, each costing a bus reset plus a retry (~1.2 s)
     * and answering queries with silence. Probing first keeps the instance on
     * a device that actually exists. */
    esp_err_t probe_err = i2c_master_probe(exp->bus, addr, PCF8575_PROBE_TIMEOUT_MS);
    if (probe_err != ESP_OK) {
        ESP_LOGW(TAG, "re-address to 0x%02X refused: nothing answered there (%s); still on 0x%02X",
                 addr, esp_err_to_name(probe_err), exp->addr);
        return ESP_ERR_NOT_FOUND;
    }

    i2c_master_dev_handle_t old_dev = exp->dev;
    exp->dev = NULL;
    esp_err_t err = pcf8575_add_device(exp, addr);
    if (err != ESP_OK) {
        /* Keep the old handle so the instance stays usable at its previous
         * address rather than being left with no device at all. */
        exp->dev = old_dev;
        ESP_LOGE(TAG, "re-address to 0x%02X failed: %s (still on 0x%02X)", addr,
                 esp_err_to_name(err), exp->addr);
        return err;
    }
    if (old_dev) {
        i2c_master_bus_rm_device(old_dev);
    }

    exp->addr = addr;
    /* A different chip's outputs have nothing to do with the old one's, so
     * the shadow goes back to the power-on assumption. */
    exp->shadow = PCF8575_PORT_POWER_ON_STATE;
    ESP_LOGI(TAG, "PCF8575 now addressed at 0x%02X", addr);
    return ESP_OK;
}

esp_err_t PCF8575_scan(i2c_master_bus_handle_t bus, uint8_t *out_addrs, size_t max_addrs,
                       size_t *out_count)
{
    if (!bus) return ESP_ERR_INVALID_ARG;

    size_t found = 0;
    for (uint8_t addr = PCF8575_ADDR_MIN; addr <= PCF8575_ADDR_MAX; ++addr) {
        esp_err_t err = i2c_master_probe(bus, addr, PCF8575_PROBE_TIMEOUT_MS);
        if (err == ESP_OK) {
            if (out_addrs && found < max_addrs) {
                out_addrs[found] = addr;
            }
            found++;
        } else if (err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "probe 0x%02X: %s", addr, esp_err_to_name(err));
        }
    }

    if (out_count) {
        *out_count = (out_addrs && found > max_addrs) ? max_addrs : found;
    }
    ESP_LOGI(TAG, "address scan (0x%02X-0x%02X): %u responded", PCF8575_ADDR_MIN,
             PCF8575_ADDR_MAX, (unsigned)found);
    return ESP_OK;
}

esp_err_t PCF8575_write_port(PCF8575Class *exp, uint16_t value)
{
    if (!exp || !exp->dev) return ESP_ERR_INVALID_ARG;

    uint8_t buf[PCF8575_PORT_BYTES] = {
        (uint8_t)(value & 0xFF),        // P00-P07
        (uint8_t)((value >> 8) & 0xFF), // P10-P17
    };

    /* Only the bits written as 0 are verifiable. A 0 turns on the strong
     * pull-down, so the pin must read back 0 no matter what is wired to it.
     * A 1 leaves just the weak (~100 uA) current source, so the pin reads
     * whatever the outside world is doing with it -- a 1 that reads back 0 is
     * an input being held low, which is normal operation, not a failed write.
     * With value == 0xFFFF the mask is empty and the ACK alone is the check. */
    const uint16_t verify_mask = (uint16_t)~value;

    esp_err_t err = ESP_FAIL;
    for (unsigned attempt = 1; attempt <= I2C_WRITE_RETRY_ATTEMPTS; ++attempt) {
        err = pcf8575_transfer(exp, buf, sizeof(buf), NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "write 0x%04X to 0x%02X failed (attempt %u/%u): %s", value,
                     exp->addr, attempt, (unsigned)I2C_WRITE_RETRY_ATTEMPTS,
                     esp_err_to_name(err));
            continue;
        }

        /* The part latched it as far as the bus is concerned; keep the shadow
         * in step even if the read-back below then disagrees, since that is
         * still the last value this driver drove. */
        exp->shadow = value;
        if (verify_mask == 0) {
            return ESP_OK;
        }

        uint16_t readback = 0;
        esp_err_t read_err = PCF8575_read_port(exp, &readback);
        if (read_err != ESP_OK) {
            ESP_LOGW(TAG, "write 0x%04X to 0x%02X: read-back failed (attempt %u/%u): %s",
                     value, exp->addr, attempt, (unsigned)I2C_WRITE_RETRY_ATTEMPTS,
                     esp_err_to_name(read_err));
            err = read_err;
            continue;
        }

        uint16_t wrong = (uint16_t)(readback & verify_mask);
        if (wrong == 0) {
            return ESP_OK;
        }

        /* Pins that should be driven low but aren't: either the write didn't
         * take, or something external is fighting the pull-down hard enough to
         * hold them high (a shorted output, a driver on the same net). */
        ESP_LOGW(TAG, "write 0x%04X to 0x%02X: read back 0x%04X, pins 0x%04X not low "
                      "(attempt %u/%u)",
                 value, exp->addr, readback, wrong, attempt,
                 (unsigned)I2C_WRITE_RETRY_ATTEMPTS);
        err = ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGE(TAG, "write 0x%04X to 0x%02X failed after %u attempts: %s", value, exp->addr,
             (unsigned)I2C_WRITE_RETRY_ATTEMPTS, esp_err_to_name(err));
    return err;
}

esp_err_t PCF8575_read_port(PCF8575Class *exp, uint16_t *out_value)
{
    if (!exp || !exp->dev || !out_value) return ESP_ERR_INVALID_ARG;

    uint8_t buf[PCF8575_PORT_BYTES] = { 0, 0 };
    esp_err_t err = pcf8575_transfer(exp, NULL, 0, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }
    *out_value = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    return ESP_OK;
}

esp_err_t PCF8575_write_pin(PCF8575Class *exp, uint8_t pin, bool level)
{
    if (!exp || pin >= PCF8575_PIN_COUNT) return ESP_ERR_INVALID_ARG;
    uint16_t mask = (uint16_t)(1u << pin);
    return level ? PCF8575_set_mask(exp, mask) : PCF8575_clear_mask(exp, mask);
}

esp_err_t PCF8575_read_pin(PCF8575Class *exp, uint8_t pin, bool *out_level)
{
    if (!exp || pin >= PCF8575_PIN_COUNT || !out_level) return ESP_ERR_INVALID_ARG;
    uint16_t port = 0;
    esp_err_t err = PCF8575_read_port(exp, &port);
    if (err != ESP_OK) {
        return err;
    }
    *out_level = (port & (uint16_t)(1u << pin)) != 0;
    return ESP_OK;
}

esp_err_t PCF8575_set_mask(PCF8575Class *exp, uint16_t mask)
{
    if (!exp) return ESP_ERR_INVALID_ARG;
    return PCF8575_write_port(exp, (uint16_t)(exp->shadow | mask));
}

esp_err_t PCF8575_clear_mask(PCF8575Class *exp, uint16_t mask)
{
    if (!exp) return ESP_ERR_INVALID_ARG;
    return PCF8575_write_port(exp, (uint16_t)(exp->shadow & (uint16_t)~mask));
}

esp_err_t PCF8575_toggle_mask(PCF8575Class *exp, uint16_t mask)
{
    if (!exp) return ESP_ERR_INVALID_ARG;
    return PCF8575_write_port(exp, (uint16_t)(exp->shadow ^ mask));
}

uint16_t PCF8575_get_shadow(const PCF8575Class *exp)
{
    return exp ? exp->shadow : PCF8575_PORT_POWER_ON_STATE;
}
