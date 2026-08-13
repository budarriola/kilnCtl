// DcDac public header
#ifndef DCDAC_H
#define DCDAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_owner.h"

#define DCDAC_DEFAULT_I2C_ADDR 0x60u

/* A plain read of the MCP4728 returns 6 bytes per channel (status + DAC
 * register + status + EEPROM copy), 4 channels, always in that order. */
#define DCDAC_READ_FRAME_LEN 24u

typedef enum {
    DCDAC_VARIANT_UNKNOWN = 0,
    DCDAC_VARIANT_READABLE,    // device responds to read-style probe
    DCDAC_VARIANT_BASIC_ACK,   // device ACKs basic writes but not readable
    DCDAC_VARIANT_MCP4728_AD,  // explicit MCP4728-AD variant
} DcDacVariant;

typedef struct {
    uint8_t base_fast_write;
    uint8_t base_write_update;
    uint8_t base_eeprom_write;
    uint8_t read_len;
} DcDacCmdFormat;

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    i2c_owner_t owner;
    bool owner_initialized;
    uint8_t addr;
    DcDacVariant variant;
    DcDacCmdFormat fmt;
} DcDacClass;

typedef enum {
    DCDAC_REQUEST_TYPE_WRITE_RAW = 0,
    DCDAC_REQUEST_TYPE_SET_CHANNEL,
    DCDAC_REQUEST_TYPE_SET_CHANNEL_PERCENT,
    DCDAC_REQUEST_TYPE_SET_ALL_CHANNELS,
    DCDAC_REQUEST_TYPE_SET_ALL_PERCENT,
    DCDAC_REQUEST_TYPE_POWER_DOWN,
    DCDAC_REQUEST_TYPE_EEPROM_WRITE,
} DcDacRequestType;

typedef struct {
    DcDacRequestType type;
    uint8_t channel;
    uint16_t value;
    float percent;
    uint8_t power_mode;
    const uint8_t *data;
    size_t data_len;
    const uint16_t *values;
    const float *percents;
    uint32_t timeout_ms;
} DcDacRequest;

esp_err_t DcDac_init(DcDacClass *dac, int sda_gpio, int scl_gpio, uint8_t addr, uint32_t clk_hz, DcDacVariant variant);
esp_err_t DcDac_init_default(DcDacClass *dac, int sda_gpio, int scl_gpio, uint32_t clk_hz, DcDacVariant variant);
esp_err_t DcDac_submit_request(DcDacClass *dac, const DcDacRequest *request);

/* Arbitrary bytes straight to the part. The only write here that is NOT
 * read-back verified -- the driver can't know what register state the caller
 * intended -- though transport errors are still retried like everywhere else. */
esp_err_t DcDac_write_raw(DcDacClass *dac, const uint8_t *data, size_t len);

/* Raw 24-byte read frame (len must be DCDAC_READ_FRAME_LEN), and the decoded
 * 12-bit DAC-register / EEPROM values for one channel. Either out pointer may
 * be NULL. Only meaningful on a readable part (MCP4728-AD / READABLE). */
esp_err_t DcDac_read_registers(DcDacClass *dac, uint8_t *out, size_t len);
esp_err_t DcDac_read_channel(DcDacClass *dac, uint8_t channel, uint16_t *out_dac,
                             uint16_t *out_eeprom);
esp_err_t DcDac_set_channel(DcDacClass *dac, uint8_t channel, uint16_t value);
esp_err_t DcDac_write_and_update_channel(DcDacClass *dac, uint8_t channel, uint16_t value);
esp_err_t DcDac_write_all_and_update(DcDacClass *dac, const uint16_t values[4]);
esp_err_t DcDac_power_down_channel(DcDacClass *dac, uint8_t channel, uint8_t power_mode);
esp_err_t DcDac_set_channel_percent(DcDacClass *dac, uint8_t channel, float percent);
esp_err_t DcDac_set_all_percent(DcDacClass *dac, const float percents[4]);
esp_err_t DcDac_deinit(DcDacClass *dac);
esp_err_t DcDac_eeprom_write_channel(DcDacClass *dac, uint8_t channel, uint16_t value);
esp_err_t DcDac_detect_variant(DcDacClass *dac, DcDacVariant *out_variant);
esp_err_t DcDac_set_variant(DcDacClass *dac, DcDacVariant variant);
esp_err_t DcDac_set_command_format(DcDacClass *dac, const DcDacCmdFormat *fmt);

/* Single-call bootstrap: runs DcDac_init_default on its own task (since I2C
 * init is blocking work that shouldn't run on the caller's task) and blocks
 * until it either succeeds or fails, so app_main can just do:
 *
 *   static DcDacClass dac;
 *   if (DcDac_start(&dac) != ESP_OK) return;
 *
 * with the task/semaphore choreography entirely hidden in DcDac.c. */
esp_err_t DcDac_start(DcDacClass *dac);

#endif // DCDAC_H
