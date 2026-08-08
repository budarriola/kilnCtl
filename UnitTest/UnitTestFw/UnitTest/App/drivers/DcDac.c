#include "DcDac.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "sdkconfig.h"
#include "settings.h"
#include <stdbool.h>

static const char *TAG = "DcDac";

/* Runs the blocking I2C init on its own task and reports back through ctx,
 * entirely internal to this file -- DcDac_start (the only thing App/main.c
 * calls) is what the rest of the firmware sees. */
typedef struct {
    DcDacClass *dac;
    SemaphoreHandle_t ready;
    esp_err_t init_result;
} mcp4728_init_ctx_t;

static void mcp4728_i2c_task(void *arg)
{
    mcp4728_init_ctx_t *ctx = (mcp4728_init_ctx_t *)arg;
    esp_err_t err = DcDac_init_default(ctx->dac, I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO,
                                        I2C_MASTER_FREQ_HZ,
                                        MCP4728_DAC_VARIANT);
    ctx->init_result = err;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DcDac_init failed: %s", esp_err_to_name(err));
    }

    xSemaphoreGive(ctx->ready);
    vTaskDelete(NULL);
}

esp_err_t DcDac_start(DcDacClass *dac)
{
    if (!dac) {
        return ESP_ERR_INVALID_ARG;
    }

    mcp4728_init_ctx_t ctx = { .dac = dac, .init_result = ESP_FAIL };
    ctx.ready = xSemaphoreCreateBinary();
    if (!ctx.ready) {
        ESP_LOGE(TAG, "Failed to create ready semaphore");
        return ESP_ERR_NO_MEM;
    }

    /* ctx lives on this function's stack, but that's safe: DcDac_start
     * doesn't return until mcp4728_i2c_task has given ctx.ready (its very
     * last action before self-deleting), so the task never outlives ctx. */
    BaseType_t created = xTaskCreatePinnedToCore(mcp4728_i2c_task, "mcp4728_i2c_task", 4096,
                                                  &ctx, 5, NULL, tskNO_AFFINITY);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create I2C init task");
        vSemaphoreDelete(ctx.ready);
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(ctx.ready, portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(ctx.ready);
        return ESP_ERR_TIMEOUT;
    }
    vSemaphoreDelete(ctx.ready);
    return ctx.init_result;
}

esp_err_t DcDac_init_default(DcDacClass *dac, int sda_gpio, int scl_gpio, uint32_t clk_hz, DcDacVariant variant)
{
    return DcDac_init(dac, sda_gpio, scl_gpio, DCDAC_DEFAULT_I2C_ADDR, clk_hz, variant);
}

esp_err_t DcDac_init(DcDacClass *dac, int sda_gpio, int scl_gpio, uint8_t addr, uint32_t clk_hz, DcDacVariant variant)
{
    if (!dac) return ESP_ERR_INVALID_ARG;
    dac->addr = addr;
    dac->variant = variant;
    // set conservative default command format; may be overridden below
    dac->fmt.base_fast_write = 0x00;
    dac->fmt.base_write_update = 0x40;
    dac->fmt.base_eeprom_write = 0x60;
    dac->fmt.read_len = 8;

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &dac->bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = clk_hz,
    };
    err = i2c_master_bus_add_device(dac->bus, &dev_config, &dac->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        i2c_del_master_bus(dac->bus);
        return err;
    }

    err = i2c_owner_init(&dac->owner, dac->bus, 8, 5, 4096, tskNO_AFFINITY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_owner_init failed: %s", esp_err_to_name(err));
        i2c_master_bus_rm_device(dac->dev);
        i2c_del_master_bus(dac->bus);
        return err;
    }
    dac->owner_initialized = true;
    ESP_LOGI(TAG, "DcDac initialized on SDA=%d SCL=%d addr=0x%02X", sda_gpio, scl_gpio, addr);

    // If variant is unknown, try to detect now
    if (dac->variant == DCDAC_VARIANT_UNKNOWN) {
        DcDacVariant detected;
        esp_err_t derr = DcDac_detect_variant(dac, &detected);
        if (derr == ESP_OK) {
            dac->variant = detected;
            ESP_LOGI(TAG, "Auto-detected variant: %d", detected);
        } else {
            ESP_LOGW(TAG, "Variant auto-detect failed: %s", esp_err_to_name(derr));
        }
    }

    // Apply sane defaults per known variants if user supplied one
    switch (dac->variant) {
        case DCDAC_VARIANT_MCP4728_AD:
            dac->fmt.base_fast_write = 0x00;
            dac->fmt.base_write_update = 0x40;
            dac->fmt.base_eeprom_write = 0x60;
            dac->fmt.read_len = 8;
            break;
        case DCDAC_VARIANT_BASIC_ACK:
            dac->fmt.base_fast_write = 0x00;
            dac->fmt.base_write_update = 0x00;
            dac->fmt.base_eeprom_write = 0x00;
            dac->fmt.read_len = 0;
            break;
        case DCDAC_VARIANT_READABLE:
        default:
            dac->fmt.base_fast_write = 0x00;
            dac->fmt.base_write_update = 0x40;
            dac->fmt.base_eeprom_write = 0x60;
            dac->fmt.read_len = 8;
            break;
    }
    return ESP_OK;
}

esp_err_t DcDac_submit_request(DcDacClass *dac, const DcDacRequest *request)
{
    if (!dac || !request) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (request->type) {
        case DCDAC_REQUEST_TYPE_WRITE_RAW:
            return DcDac_write_raw(dac, request->data, request->data_len);
        case DCDAC_REQUEST_TYPE_SET_CHANNEL:
            return DcDac_set_channel(dac, request->channel, request->value);
        case DCDAC_REQUEST_TYPE_SET_CHANNEL_PERCENT:
            return DcDac_set_channel_percent(dac, request->channel, request->percent);
        case DCDAC_REQUEST_TYPE_SET_ALL_CHANNELS:
            if (!request->values) return ESP_ERR_INVALID_ARG;
            return DcDac_write_all_and_update(dac, request->values);
        case DCDAC_REQUEST_TYPE_SET_ALL_PERCENT:
            if (!request->percents) return ESP_ERR_INVALID_ARG;
            return DcDac_set_all_percent(dac, request->percents);
        case DCDAC_REQUEST_TYPE_POWER_DOWN:
            return DcDac_power_down_channel(dac, request->channel, request->power_mode);
        case DCDAC_REQUEST_TYPE_EEPROM_WRITE:
            return DcDac_eeprom_write_channel(dac, request->channel, request->value);
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t DcDac_write_raw(DcDacClass *dac, const uint8_t *data, size_t len)
{
    if (!dac || !data || len == 0) return ESP_ERR_INVALID_ARG;
    if (!dac->owner_initialized) {
        return i2c_master_transmit(dac->dev, data, len, 1000);
    }
    return i2c_owner_transfer(&dac->owner, dac->dev, data, len, NULL, 0, 1000);
}

esp_err_t DcDac_set_channel(DcDacClass *dac, uint8_t channel, uint16_t value)
{
    // By default, use write-and-update for immedate output change.
    return DcDac_write_and_update_channel(dac, channel, value);
}

esp_err_t DcDac_set_channel_percent(DcDacClass *dac, uint8_t channel, float percent)
{
    if (!dac) return ESP_ERR_INVALID_ARG;
    if (percent <= 0.0f) return DcDac_set_channel(dac, channel, 0);
    if (percent >= 100.0f) return DcDac_set_channel(dac, channel, 4095);
    uint16_t v = (uint16_t)((percent / 100.0f) * 4095.0f + 0.5f);
    return DcDac_set_channel(dac, channel, v);
}

esp_err_t DcDac_set_all_percent(DcDacClass *dac, const float percents[4])
{
    if (!dac || !percents) return ESP_ERR_INVALID_ARG;
    uint16_t vals[4];
    for (int i = 0; i < 4; ++i) {
        float p = percents[i];
        if (p <= 0.0f) vals[i] = 0;
        else if (p >= 100.0f) vals[i] = 4095;
        else vals[i] = (uint16_t)((p / 100.0f) * 4095.0f + 0.5f);
    }
    return DcDac_write_all_and_update(dac, vals);
}

esp_err_t DcDac_write_and_update_channel(DcDacClass *dac, uint8_t channel, uint16_t value)
{
    if (!dac) return ESP_ERR_INVALID_ARG;
    if (channel > 3) return ESP_ERR_INVALID_ARG;
    if (value > 4095) value = 4095;

    /* Compose a 3-byte Write-and-Update command. Control bits below are
       chosen to follow a common MCP4728 write-and-update pattern; verify
       against the datasheet for your device and adjust control bits if
       necessary. */
    uint8_t buf[3];
    uint8_t base = dac->fmt.base_write_update;
    if (dac->variant == DCDAC_VARIANT_BASIC_ACK) base = dac->fmt.base_fast_write;
    uint8_t ctrl = base | ((channel & 0x03) << 1);
    buf[0] = ctrl;
    buf[1] = (uint8_t)(value >> 4);
    buf[2] = (uint8_t)((value & 0x0F) << 4);
    return DcDac_write_raw(dac, buf, sizeof(buf));
}

esp_err_t DcDac_write_all_and_update(DcDacClass *dac, const uint16_t values[4])
{
    if (!dac || !values) return ESP_ERR_INVALID_ARG;
    uint8_t buf[12];
    for (int ch = 0; ch < 4; ++ch) {
        uint16_t v = values[ch];
        if (v > 4095) v = 4095;
        uint8_t base = dac->fmt.base_write_update;
        if (dac->variant == DCDAC_VARIANT_BASIC_ACK) base = dac->fmt.base_fast_write;
        uint8_t ctrl = base | ((ch & 0x03) << 1);
        buf[ch*3 + 0] = ctrl;
        buf[ch*3 + 1] = (uint8_t)(v >> 4);
        buf[ch*3 + 2] = (uint8_t)((v & 0x0F) << 4);
    }
    return DcDac_write_raw(dac, buf, sizeof(buf));
}

esp_err_t DcDac_power_down_channel(DcDacClass *dac, uint8_t channel, uint8_t power_mode)
{
    if (!dac) return ESP_ERR_INVALID_ARG;
    if (channel > 3) return ESP_ERR_INVALID_ARG;
    power_mode &= 0x03; // only two bits

    /* Power-down control often lives in control byte; build a small payload
       that sets the power bits for the given channel. This implementation
       uses the control byte pattern with the power bits set in positions
       commonly seen on 12-bit I2C DACs—verify with your datasheet. */
    uint8_t base = dac->fmt.base_eeprom_write;
    if (dac->variant == DCDAC_VARIANT_BASIC_ACK) base = dac->fmt.base_fast_write;
    uint8_t ctrl = base | ((channel & 0x03) << 1) | (power_mode & 0x03);
    uint8_t buf[3] = { ctrl, 0x00, 0x00 };
    return DcDac_write_raw(dac, buf, sizeof(buf));
}

esp_err_t DcDac_eeprom_write_channel(DcDacClass *dac, uint8_t channel, uint16_t value)
{
    if (!dac) return ESP_ERR_INVALID_ARG;
    if (channel > 3) return ESP_ERR_INVALID_ARG;
    if (value > 4095) value = 4095;

    /* Compose EEPROM write payload using configured base */
    uint8_t base = dac->fmt.base_eeprom_write;
    uint8_t ctrl = base | ((channel & 0x03) << 1);
    uint8_t buf[3];
    buf[0] = ctrl;
    buf[1] = (uint8_t)(value >> 4);
    buf[2] = (uint8_t)((value & 0x0F) << 4);

    // EEPROM write can take longer than a normal write; use a longer timeout.
    // Route through the owner (like DcDac_write_raw does) rather than
    // calling i2c_master_transmit directly, so this can't jump ahead of or
    // interleave with other already-queued transactions on the same bus.
    if (!dac->owner_initialized) {
        return i2c_master_transmit(dac->dev, buf, sizeof(buf), 5000);
    }
    return i2c_owner_transfer(&dac->owner, dac->dev, buf, sizeof(buf), NULL, 0, 5000);
}

esp_err_t DcDac_detect_variant(DcDacClass *dac, DcDacVariant *out_variant)
{
    if (!dac || !out_variant) return ESP_ERR_INVALID_ARG;
    uint8_t buf[8] = {0};

    // Heuristic 1: try a read-only probe (many MCP4728 variants support a
    // read command that returns status + DAC values). Use transmit_receive
    // with an empty tx buffer if supported by the transport helper. If the
    // API does not accept empty tx, fall back to trying a zero-byte write
    // followed by read. Route through the owner when it's already up (it is,
    // by the time DcDac_init calls this) so the probe can't interleave with
    // other queued transactions on the same bus.
    esp_err_t err = dac->owner_initialized
        ? i2c_owner_transfer(&dac->owner, dac->dev, NULL, 0, buf, sizeof(buf), 500)
        : i2c_master_transmit_receive(dac->dev, NULL, 0, buf, sizeof(buf), 500);
    if (err == ESP_OK) {
        // Check if response looks non-trivial (not all 0xFF or 0x00)
        bool non_trivial = false;
        for (size_t i = 0; i < sizeof(buf); ++i) {
            if (buf[i] != 0x00 && buf[i] != 0xFF) { non_trivial = true; break; }
        }
        if (non_trivial) {
            *out_variant = DCDAC_VARIANT_READABLE;
            return ESP_OK;
        }
    }

    // Heuristic 2: try a small write and see if device ACKs
    uint8_t probe_write[1] = { 0x00 };
    err = dac->owner_initialized
        ? i2c_owner_transfer(&dac->owner, dac->dev, probe_write, sizeof(probe_write), NULL, 0, 200)
        : i2c_master_transmit(dac->dev, probe_write, sizeof(probe_write), 200);
    if (err == ESP_OK) {
        *out_variant = DCDAC_VARIANT_BASIC_ACK;
        return ESP_OK;
    }

    *out_variant = DCDAC_VARIANT_UNKNOWN;
    return ESP_OK;
}

esp_err_t DcDac_deinit(DcDacClass *dac)
{
    if (!dac) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_OK;
    if (dac->owner_initialized) {
        esp_err_t e = i2c_owner_deinit(&dac->owner);
        if (e != ESP_OK) err = e;
        dac->owner_initialized = false;
    }
    if (dac->dev) {
        esp_err_t e = i2c_master_bus_rm_device(dac->dev);
        if (e != ESP_OK) err = e;
    }
    if (dac->bus) {
        esp_err_t e = i2c_del_master_bus(dac->bus);
        if (e != ESP_OK) err = e;
    }
    return err;
}

esp_err_t DcDac_set_variant(DcDacClass *dac, DcDacVariant variant)
{
    if (!dac) return ESP_ERR_INVALID_ARG;
    dac->variant = variant;
    ESP_LOGI(TAG, "DcDac variant set to %d", variant);
    return ESP_OK;
}

esp_err_t DcDac_set_command_format(DcDacClass *dac, const DcDacCmdFormat *fmt)
{
    if (!dac || !fmt) return ESP_ERR_INVALID_ARG;
    dac->fmt = *fmt;
    ESP_LOGI(TAG, "DcDac command format updated");
    return ESP_OK;
}
