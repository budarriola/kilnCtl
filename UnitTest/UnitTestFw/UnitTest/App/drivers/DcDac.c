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
    /* MCP4728 command bases (datasheet 5.6, the C2/C1/C0 + W1/W0 field):
     *   0x00 Fast Write            0x40 Multi-Write (DAC register only)
     *   0x50 Sequential Write      0x58 Single Write (DAC register + EEPROM)
     *   0x60 Write I2C address bits
     * base_eeprom_write used to be 0x60 -- the address-write command, which
     * needs an LDAC edge and NACKs without one, so every power-down and EEPROM
     * write silently failed. Read-back verification is what turned that up. */
    // set conservative default command format; may be overridden below
    dac->fmt.base_fast_write = 0x00;
    dac->fmt.base_write_update = 0x40;
    dac->fmt.base_eeprom_write = 0x58;
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
            dac->fmt.base_eeprom_write = 0x58;
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
            dac->fmt.base_eeprom_write = 0x58;
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

/* ---- transport + read-back verification ---------------------------------
 *
 * A read of the MCP4728 (no register pointer -- just a plain read) returns 24
 * bytes: 6 per channel, in channel order A..D. Per channel:
 *
 *   [0] RDY/#BSY, POR, channel, address bits   (status of the DAC register)
 *   [1] VREF PD1 PD0 Gx D11 D10 D9 D8          (DAC register, as stored)
 *   [2] D7 D6 D5 D4 D3 D2 D1 D0
 *   [3] same status layout, for the EEPROM copy
 *   [4] VREF PD1 PD0 Gx D11..D8                (EEPROM copy)
 *   [5] D7..D0
 *
 * Bytes [1]/[2] (and [4]/[5]) are byte-for-byte what mcp4728_pack_data_bytes
 * sends, so verifying a write is a plain compare -- no decoding needed.
 * RDY/#BSY is 1 when the part is idle; it drops to 0 for the duration of an
 * EEPROM write (up to ~50 ms), during which the EEPROM copy still reads back
 * the old value, so anything that touches EEPROM has to wait for it. */
#define DCDAC_READ_BYTES_PER_CH (DCDAC_READ_FRAME_LEN / 4u)
#define DCDAC_STATUS_RDY_BIT    0x80u
#define DCDAC_EEPROM_READY_TIMEOUT_MS 100
#define DCDAC_EEPROM_POLL_MS    5
#define DCDAC_READ_TIMEOUT_MS   500

/* What a write is expected to leave behind, for the read-back check. */
typedef struct {
    uint8_t hi[4];         // expected byte [1] (and [4]) per channel
    uint8_t lo[4];         // expected byte [2] (and [5]) per channel
    uint8_t channel_mask;  // bit N set = verify channel N
    bool check_eeprom;     // command also writes EEPROM: wait for RDY, verify the copy
} dcdac_expect_t;

static bool dcdac_readback_supported(const DcDacClass *dac)
{
    /* BASIC_ACK is by definition a part that ACKs writes but can't be read
     * back, and UNKNOWN means detection failed -- don't invent read traffic
     * for either; the write's ACK is all the confirmation available. */
    return dac->variant == DCDAC_VARIANT_MCP4728_AD || dac->variant == DCDAC_VARIANT_READABLE;
}

static esp_err_t dcdac_transmit(DcDacClass *dac, const uint8_t *data, size_t len,
                                uint32_t timeout_ms)
{
    if (!dac->owner_initialized) {
        return i2c_master_transmit(dac->dev, data, len, timeout_ms);
    }
    return i2c_owner_transfer(&dac->owner, dac->dev, data, len, NULL, 0, timeout_ms);
}

static esp_err_t dcdac_read_frame(DcDacClass *dac, uint8_t frame[DCDAC_READ_FRAME_LEN])
{
    if (!dac->owner_initialized) {
        return i2c_master_receive(dac->dev, frame, DCDAC_READ_FRAME_LEN, DCDAC_READ_TIMEOUT_MS);
    }
    return i2c_owner_transfer(&dac->owner, dac->dev, NULL, 0, frame, DCDAC_READ_FRAME_LEN,
                              DCDAC_READ_TIMEOUT_MS);
}

static esp_err_t dcdac_read_frame_ready(DcDacClass *dac, uint8_t frame[DCDAC_READ_FRAME_LEN])
{
    uint32_t waited_ms = 0;
    while (true) {
        esp_err_t err = dcdac_read_frame(dac, frame);
        if (err != ESP_OK) return err;
        if (frame[0] & DCDAC_STATUS_RDY_BIT) return ESP_OK;
        if (waited_ms >= DCDAC_EEPROM_READY_TIMEOUT_MS) {
            ESP_LOGW(TAG, "still busy (RDY/#BSY low) %lu ms after an EEPROM write",
                     (unsigned long)waited_ms);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(DCDAC_EEPROM_POLL_MS));
        waited_ms += DCDAC_EEPROM_POLL_MS;
    }
}

static esp_err_t dcdac_verify(DcDacClass *dac, const dcdac_expect_t *expect, const char *what)
{
    uint8_t frame[DCDAC_READ_FRAME_LEN];
    esp_err_t err = expect->check_eeprom ? dcdac_read_frame_ready(dac, frame)
                                         : dcdac_read_frame(dac, frame);
    if (err != ESP_OK) {
        return err;
    }

    esp_err_t result = ESP_OK;
    for (uint8_t ch = 0; ch < 4; ++ch) {
        if (!(expect->channel_mask & (1u << ch))) continue;
        const uint8_t *slice = &frame[ch * DCDAC_READ_BYTES_PER_CH];
        if (slice[1] != expect->hi[ch] || slice[2] != expect->lo[ch]) {
            ESP_LOGW(TAG, "%s: ch%u DAC register reads 0x%02X%02X, expected 0x%02X%02X",
                     what, ch, slice[1], slice[2], expect->hi[ch], expect->lo[ch]);
            result = ESP_ERR_INVALID_RESPONSE;
        }
        if (expect->check_eeprom && (slice[4] != expect->hi[ch] || slice[5] != expect->lo[ch])) {
            ESP_LOGW(TAG, "%s: ch%u EEPROM reads 0x%02X%02X, expected 0x%02X%02X",
                     what, ch, slice[4], slice[5], expect->hi[ch], expect->lo[ch]);
            result = ESP_ERR_INVALID_RESPONSE;
        }
    }
    return result;
}

/* Write, read back, compare -- and if any of those three steps fails, say so
 * and do the whole thing again, up to I2C_WRITE_RETRY_ATTEMPTS times. `expect`
 * NULL (or a part that can't be read back) degrades to write-and-retry, since
 * the ACK is then the only confirmation available. */
static esp_err_t dcdac_write_verified(DcDacClass *dac, const uint8_t *payload, size_t len,
                                      uint32_t timeout_ms, const dcdac_expect_t *expect,
                                      const char *what)
{
    if (!dac || !dac->dev || !payload || len == 0) return ESP_ERR_INVALID_ARG;

    const bool verify = expect && dcdac_readback_supported(dac);
    if (expect && !verify) {
        ESP_LOGD(TAG, "%s: variant %d is not readable, sending without read-back check",
                 what, dac->variant);
    }

    esp_err_t err = ESP_FAIL;
    for (unsigned attempt = 1; attempt <= I2C_WRITE_RETRY_ATTEMPTS; ++attempt) {
        err = dcdac_transmit(dac, payload, len, timeout_ms);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s failed (attempt %u/%u): %s", what, attempt,
                     (unsigned)I2C_WRITE_RETRY_ATTEMPTS, esp_err_to_name(err));
            continue;
        }
        if (!verify) {
            return ESP_OK;
        }

        err = dcdac_verify(dac, expect, what);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "%s did not verify (attempt %u/%u): %s", what, attempt,
                 (unsigned)I2C_WRITE_RETRY_ATTEMPTS, esp_err_to_name(err));
    }

    ESP_LOGE(TAG, "%s failed after %u attempts: %s", what,
             (unsigned)I2C_WRITE_RETRY_ATTEMPTS, esp_err_to_name(err));
    return err;
}

/* Raw bytes from the UART bridge: the driver has no idea what register state
 * they're supposed to produce, so there is nothing to compare a read-back
 * against. Transport errors are still retried. */
esp_err_t DcDac_write_raw(DcDacClass *dac, const uint8_t *data, size_t len)
{
    if (!dac || !data || len == 0) return ESP_ERR_INVALID_ARG;
    return dcdac_write_verified(dac, data, len, 1000, NULL, "raw write");
}

esp_err_t DcDac_read_registers(DcDacClass *dac, uint8_t *out, size_t len)
{
    if (!dac || !dac->dev || !out || len != DCDAC_READ_FRAME_LEN) return ESP_ERR_INVALID_ARG;
    return dcdac_read_frame(dac, out);
}

esp_err_t DcDac_read_channel(DcDacClass *dac, uint8_t channel, uint16_t *out_dac,
                             uint16_t *out_eeprom)
{
    if (!dac || channel > 3) return ESP_ERR_INVALID_ARG;
    uint8_t frame[DCDAC_READ_FRAME_LEN];
    esp_err_t err = dcdac_read_frame(dac, frame);
    if (err != ESP_OK) return err;
    const uint8_t *slice = &frame[channel * DCDAC_READ_BYTES_PER_CH];
    if (out_dac)    *out_dac    = (uint16_t)(((slice[1] & 0x0F) << 8) | slice[2]);
    if (out_eeprom) *out_eeprom = (uint16_t)(((slice[4] & 0x0F) << 8) | slice[5]);
    return ESP_OK;
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

/* MCP4728 single/sequential-write data-byte layout (datasheet Fig 5-13,
 * confirmed against Adafruit_MCP4728's setChannelValue()): the 12-bit value
 * is NOT simply value>>4 / value<<4 across two bytes -- that's the packing
 * for a left-justified-in-16-bits DAC (e.g. SPI parts like the MCP4921),
 * which this is not. Byte 1's *top* nibble is VREF/PD1/PD0/Gx control bits,
 * not data:
 *
 *   byte1 = VREF PD1 PD0 Gx D11 D10 D9 D8
 *   byte2 = D7 D6 D5 D4 D3 D2 D1 D0
 *
 * The previous (value>>4)/((value&0xF)<<4) packing put data bits where PD1:
 * PD0 live -- at full scale (0xFFF) that sent PD1:PD0 = 11, which is a
 * power-down encoding: it disconnects the output amplifier and ties the pin
 * through a ~500k resistor to ground instead of driving it. That -- not
 * just VREF picking the wrong reference -- is why the channel measured
 * barely above zero instead of anywhere near full scale.
 *
 * VREF=0 selects VDD (this board's 3.3V rail) as the reference, giving the
 * full 0-VCC output range; VREF=1 selects the internal 2.048V reference
 * instead and is never what DcDac_set_channel_percent's callers want. PD1:
 * PD0=00 is normal (not powered down). Gx is ignored when VREF=VDD. */
#define MCP4728_VREF_VDD   0x00u
#define MCP4728_PD_NORMAL  0x00u
#define MCP4728_GAIN_X1    0x00u

static inline void mcp4728_pack_data_bytes(uint16_t value, uint8_t out[2])
{
    uint16_t config = (MCP4728_VREF_VDD << 15) | (MCP4728_PD_NORMAL << 13) | (MCP4728_GAIN_X1 << 12);
    uint16_t word = config | (value & 0x0FFF);
    out[0] = (uint8_t)(word >> 8);
    out[1] = (uint8_t)(word & 0xFF);
}

esp_err_t DcDac_write_and_update_channel(DcDacClass *dac, uint8_t channel, uint16_t value)
{
    if (!dac) return ESP_ERR_INVALID_ARG;
    if (channel > 3) return ESP_ERR_INVALID_ARG;
    if (value > 4095) value = 4095;

    uint8_t buf[3];
    uint8_t base = dac->fmt.base_write_update;
    if (dac->variant == DCDAC_VARIANT_BASIC_ACK) base = dac->fmt.base_fast_write;
    uint8_t ctrl = base | ((channel & 0x03) << 1);
    buf[0] = ctrl;
    mcp4728_pack_data_bytes(value, &buf[1]);

    /* Multi-write touches the DAC register only, so the EEPROM copy is left
     * alone and must not be part of the comparison. */
    dcdac_expect_t expect = { .channel_mask = (uint8_t)(1u << channel), .check_eeprom = false };
    expect.hi[channel] = buf[1];
    expect.lo[channel] = buf[2];
    return dcdac_write_verified(dac, buf, sizeof(buf), 1000, &expect, "write-and-update");
}

esp_err_t DcDac_write_all_and_update(DcDacClass *dac, const uint16_t values[4])
{
    if (!dac || !values) return ESP_ERR_INVALID_ARG;
    uint8_t buf[12];
    dcdac_expect_t expect = { .channel_mask = 0x0F, .check_eeprom = false };
    for (int ch = 0; ch < 4; ++ch) {
        uint16_t v = values[ch];
        if (v > 4095) v = 4095;
        uint8_t base = dac->fmt.base_write_update;
        if (dac->variant == DCDAC_VARIANT_BASIC_ACK) base = dac->fmt.base_fast_write;
        uint8_t ctrl = base | ((ch & 0x03) << 1);
        buf[ch*3 + 0] = ctrl;
        mcp4728_pack_data_bytes(v, &buf[ch*3 + 1]);
        expect.hi[ch] = buf[ch*3 + 1];
        expect.lo[ch] = buf[ch*3 + 2];
    }
    return dcdac_write_verified(dac, buf, sizeof(buf), 1000, &expect, "write-all-and-update");
}

esp_err_t DcDac_power_down_channel(DcDacClass *dac, uint8_t channel, uint8_t power_mode)
{
    if (!dac) return ESP_ERR_INVALID_ARG;
    if (channel > 3) return ESP_ERR_INVALID_ARG;
    power_mode &= 0x03; // only two bits (PD1:PD0)

    /* PD1:PD0 are control bits in the *data* byte (bits 6:5 of byte1, same
     * position mcp4728_pack_data_bytes always zeros for MCP4728_PD_NORMAL),
     * not in the command/channel byte -- the previous version OR'd
     * power_mode into ctrl's low bits, which land on the UDAC bit and
     * nothing else, so it never actually powered anything down. Keep VREF/
     * Gx at the same normal-operation values used elsewhere; only PD
     * changes here. Data bits (D11:D0) are irrelevant while powered down,
     * left at 0. */
    uint8_t base = dac->fmt.base_eeprom_write;
    if (dac->variant == DCDAC_VARIANT_BASIC_ACK) base = dac->fmt.base_fast_write;
    uint8_t ctrl = base | ((channel & 0x03) << 1);
    uint16_t config = (MCP4728_VREF_VDD << 15) | ((uint16_t)power_mode << 13) | (MCP4728_GAIN_X1 << 12);
    uint8_t buf[3] = { ctrl, (uint8_t)(config >> 8), (uint8_t)(config & 0xFF) };

    /* base_eeprom_write is the Single Write command: it stores to EEPROM as
     * well as the DAC register, so both copies are checked and the read-back
     * has to wait out RDY/#BSY. */
    dcdac_expect_t expect = {
        .channel_mask = (uint8_t)(1u << channel),
        .check_eeprom = (base == dac->fmt.base_eeprom_write),
    };
    expect.hi[channel] = buf[1];
    expect.lo[channel] = buf[2];
    return dcdac_write_verified(dac, buf, sizeof(buf), 1000, &expect, "power-down");
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
    mcp4728_pack_data_bytes(value, &buf[1]);

    // EEPROM write can take longer than a normal write; use a longer timeout.
    // Route through the owner (like DcDac_write_raw does) rather than
    // calling i2c_master_transmit directly, so this can't jump ahead of or
    // interleave with other already-queued transactions on the same bus.
    // Verify both copies once the part reports itself ready again -- a failed
    // EEPROM write is the one that silently survives a power cycle.
    dcdac_expect_t expect = {
        .channel_mask = (uint8_t)(1u << channel),
        .check_eeprom = true,
    };
    expect.hi[channel] = buf[1];
    expect.lo[channel] = buf[2];
    return dcdac_write_verified(dac, buf, sizeof(buf), 5000, &expect, "EEPROM write");
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
