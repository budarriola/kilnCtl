// recovery_io.c -- see recovery_io.h.
#include "recovery_io.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_timer.h"
#include "recovery_hold.h"

static const char *TAG = "recovery_io";

// App/drivers/Kconfig defaults (KILNCTL_I2C_SDA_IO/SCL_IO, SX1509_I2C_ADDR,
// SX1509_RESET_IO).
#define I2C_PORT       I2C_NUM_0
#define I2C_SDA_GPIO   8
#define I2C_SCL_GPIO   9
#define I2C_CLK_HZ     100000
#define SX1509_ADDR    0x3Eu
#define SX1509_RESET_GPIO 10

// App/drivers/hw/SX1509.h register map. 16-bit values go out as one
// transfer starting at the bank-B (high byte, pins 15:8) address, high byte
// first (auto-increment is the power-on default).
#define SX1509_REG_DIR_B  0x0Eu
#define SX1509_REG_DATA_B 0x10u
#define SX1509_REG_RESET  0x7Du

// settings.h: SX1509_RELAY1_PIN..4 = 0..3; SX1509_LCD_DC_PIN = 15,
// SX1509_LCD_RESET_PIN = 14 (CONFIG_KILNCTL_DISPLAY_SWAP_DC_RESET off, the
// default, per DISPLAY_ST7796_WIRING.md).
#define RELAY_MASK   ((uint16_t)0x000Fu)
// IO5 = SX1509_IO2_PIN, the opto-isolated output to J25: kiln_io.c's
// KILN_IO_OUTPUT_MASK drives it as an output too (RegDir 0x3FD0 there with
// the LCD pins), so recovery does the same and holds it low.
#define IO2_J25_BIT  ((uint16_t)(1u << 5))
#define HOLD_MASK    ((uint16_t)(RELAY_MASK | IO2_J25_BIT))
#define LCD_DC_BIT   ((uint16_t)(1u << 15))
#define LCD_RST_BIT  ((uint16_t)(1u << 14))
#define LCD_MASK     ((uint16_t)(LCD_DC_BIT | LCD_RST_BIT))

// Output latches loaded before the pins become outputs: relays low, LCD D/C
// high (data) and ~RESET high (not held in reset) -- kiln_io.c's
// KILN_IO_SAFE_DATA. RegDir: 1 = input, so outputs are the cleared bits.
#define SAFE_DATA    ((uint16_t)LCD_MASK)
#define OUT_DIR_MASK ((uint16_t)(HOLD_MASK | LCD_MASK))
#define DIR_VALUE    ((uint16_t)~OUT_DIR_MASK)

#define HOLD_ATTEMPTS 3

static void start_hold_task(void);

static bool s_i2c_ready = false;
static bool s_verified = false;
static bool s_fault = true; // pessimistic until the hold is verified
static uint16_t s_data = SAFE_DATA;

static esp_err_t write16(uint8_t reg, uint16_t v)
{
    uint8_t buf[3] = {reg, (uint8_t)(v >> 8), (uint8_t)v};
    return i2c_master_write_to_device(I2C_PORT, SX1509_ADDR, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

static esp_err_t read16(uint8_t reg, uint16_t *out)
{
    uint8_t rx[2] = {0, 0};
    esp_err_t err = i2c_master_write_read_device(I2C_PORT, SX1509_ADDR, &reg, 1, rx, 2,
                                                  pdMS_TO_TICKS(100));
    if (err == ESP_OK) {
        *out = (uint16_t)((rx[0] << 8) | rx[1]);
    }
    return err;
}

static esp_err_t i2c_bring_up(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_CLK_HZ,
    };
    esp_err_t err = i2c_param_config(I2C_PORT, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_driver_install(I2C_PORT, cfg.mode, 0, 0, 0);
}

// Hard reset via the dedicated ~RESET GPIO (SX1509.c SX1509_reset(hard):
// 10 us pulse, then wait out t_RESET), then the software-reset magic pair as
// a belt-and-braces second path.
static void expander_reset(bool first)
{
    if (first) {
        gpio_set_level(SX1509_RESET_GPIO, 1);
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << SX1509_RESET_GPIO,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&io);
        gpio_set_level(SX1509_RESET_GPIO, 1);
    }
    gpio_set_level(SX1509_RESET_GPIO, 0);
    esp_rom_delay_us(10);
    gpio_set_level(SX1509_RESET_GPIO, 1);
    // CONFIG_FREERTOS_HZ=100 makes pdMS_TO_TICKS(5)==0, so busy-wait for real.
    esp_rom_delay_us(5000);

    uint8_t m1[2] = {SX1509_REG_RESET, 0x12};
    uint8_t m2[2] = {SX1509_REG_RESET, 0x34};
    if (i2c_master_write_to_device(I2C_PORT, SX1509_ADDR, m1, 2, pdMS_TO_TICKS(100)) == ESP_OK) {
        (void)i2c_master_write_to_device(I2C_PORT, SX1509_ADDR, m2, 2, pdMS_TO_TICKS(100));
        esp_rom_delay_us(5000);
    }
}

static bool hold_once(bool first)
{
    expander_reset(first);

    // Latches first (pins are inputs after reset, so nothing moves), then
    // directions: kiln_io_init()'s "data before dir" order.
    esp_err_t err = write16(SX1509_REG_DATA_B, SAFE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SX1509 0x%02X RegData write failed: %s", SX1509_ADDR, esp_err_to_name(err));
        return false;
    }
    err = write16(SX1509_REG_DIR_B, DIR_VALUE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SX1509 RegDir write failed: %s", esp_err_to_name(err));
        return false;
    }
    s_data = SAFE_DATA;

    uint16_t data = 0, dir = 0;
    if (read16(SX1509_REG_DATA_B, &data) != ESP_OK || read16(SX1509_REG_DIR_B, &dir) != ESP_OK) {
        ESP_LOGE(TAG, "SX1509 read-back failed");
        return false;
    }
    bool dir_ok = (uint16_t)(dir & OUT_DIR_MASK) == 0; // relay + LCD pins are outputs
    bool data_ok = (uint16_t)(data & HOLD_MASK) == 0; // relay pins read low
    if (!dir_ok || !data_ok) {
        ESP_LOGE(TAG, "RELAY HOLD MISMATCH: RegDir=0x%04X (want output bits 0x%04X clear) "
                      "RegData=0x%04X (want hold bits 0x%04X low)",
                 dir, OUT_DIR_MASK, data, HOLD_MASK);
        return false;
    }
    ESP_LOGI(TAG, "relays IO0..IO3 and IO5 held low and driven: RegDir=0x%04X RegData=0x%04X", dir, data);
    return true;
}

void recovery_io_hold_relays_off(void)
{
    esp_err_t err = i2c_bring_up();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bring-up failed: %s -- RELAY CTRL FAULT", esp_err_to_name(err));
        return;
    }
    s_i2c_ready = true;

    for (int i = 0; i < HOLD_ATTEMPTS; i++) {
        if (hold_once(i == 0)) {
            s_verified = true;
            s_fault = false;
            start_hold_task();
            return;
        }
        ESP_LOGW(TAG, "relay hold attempt %d/%d failed", i + 1, HOLD_ATTEMPTS);
    }
    s_fault = true;
    ESP_LOGE(TAG, "RELAY CTRL FAULT: could not verify relays low after %d attempts. Recovery "
                  "keeps running (uploads must still work); heaters are NOT confirmed off "
                  "by this image.", HOLD_ATTEMPTS);
    // Keep trying every second: a late-recovering expander gets the hold applied.
    start_hold_task();
}

static rhold_state_t s_hold;
static portMUX_TYPE s_hold_mux = portMUX_INITIALIZER_UNLOCKED;

bool recovery_io_relay_fault(void)
{
    rhold_state_t snap;
    portENTER_CRITICAL(&s_hold_mux);
    snap = s_hold;
    portEXIT_CRITICAL(&s_hold_mux);
    return rhold_effective_fault(s_fault, &snap);
}

// ---- periodic hold watchdog -------------------------------------------------
// Serialises s_data/expander writes between this task and recovery_io_set_lcd_pins
// (the LCD task), so a re-assert can never write back a stale LCD-pin value.
static SemaphoreHandle_t s_io_lock;
static StaticSemaphore_t s_io_lock_buf;
static TaskHandle_t s_hold_task;

#define HOLD_TASK_STACK_BYTES 3072
#define HOLD_TASK_PERIOD_MS   1000

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static bool read_hold_regs(uint16_t *dir, uint16_t *data)
{
    return read16(SX1509_REG_DIR_B, dir) == ESP_OK && read16(SX1509_REG_DATA_B, data) == ESP_OK;
}

static void hold_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HOLD_TASK_PERIOD_MS));
        xSemaphoreTake(s_io_lock, portMAX_DELAY);
        uint16_t dir = 0, data = 0;
        bool read_ok = read_hold_regs(&dir, &data);
        uint32_t now = uptime_s();
        rhold_action_t act;
        bool was_fault;
        portENTER_CRITICAL(&s_hold_mux);
        was_fault = s_hold.fault;
        act = rhold_observe(&s_hold, read_ok, dir, data, OUT_DIR_MASK, HOLD_MASK, now);
        portEXIT_CRITICAL(&s_hold_mux);
        if (act == RHOLD_REASSERT) {
            if (!was_fault) {
                ESP_LOGE(TAG, "RELAY HOLD LOST at %us: read_ok=%d RegDir=0x%04X RegData=0x%04X "
                              "-- re-asserting", (unsigned)now, (int)read_ok, dir, data);
            }
            // Same order as the boot hold: latches first, then directions. s_data's
            // relay bits are always 0, so this can never raise a relay.
            bool ok = write16(SX1509_REG_DATA_B, s_data) == ESP_OK &&
                      write16(SX1509_REG_DIR_B, DIR_VALUE) == ESP_OK &&
                      read_hold_regs(&dir, &data) &&
                      rhold_regs_match(dir, data, OUT_DIR_MASK, HOLD_MASK);
            portENTER_CRITICAL(&s_hold_mux);
            rhold_reassert_result(&s_hold, ok, uptime_s());
            portEXIT_CRITICAL(&s_hold_mux);
            if (!ok) {
                ESP_LOGE(TAG, "relay hold re-assert did NOT verify (RegDir=0x%04X RegData=0x%04X)",
                         dir, data);
            }
        }
        xSemaphoreGive(s_io_lock);
    }
}

void recovery_io_hold_status(recovery_io_hold_status_t *out)
{
    rhold_state_t snap;
    portENTER_CRITICAL(&s_hold_mux);
    snap = s_hold;
    portEXIT_CRITICAL(&s_hold_mux);
    out->task_running = s_hold_task != NULL;
    out->fault = snap.fault;
    out->fault_valid = snap.fault_seen_s_valid;
    out->fault_s = snap.fault_s;
    out->last_ok_valid = snap.ever_ok;
    out->last_ok_s = snap.last_ok_s;
    out->mismatch_count = snap.mismatch_count;
    out->reassert_fail_count = snap.reassert_fail_count;
    // Local report: the recovery image has no stack_margin API (that lives in
    // the main app), so the task's own high-water mark is surfaced here.
    out->task_stack_free_bytes =
        s_hold_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_hold_task) : 0;
}

static void start_hold_task(void)
{
    s_io_lock = xSemaphoreCreateMutexStatic(&s_io_lock_buf);
    rhold_init(&s_hold);
    // Seed "last ok" from the boot verification so status is meaningful before
    // the first tick.
    if (s_verified) {
        s_hold.ever_ok = true;
        s_hold.last_ok_s = uptime_s();
    }
    if (xTaskCreate(hold_task, "relay_hold", HOLD_TASK_STACK_BYTES, NULL, 3, &s_hold_task) !=
        pdPASS) {
        s_hold_task = NULL;
        ESP_LOGE(TAG, "could not start the relay hold watchdog task");
    }
}

static volatile unsigned s_nvs_failed;

void recovery_io_nvs_mark_failed(unsigned bit)
{
    s_nvs_failed |= bit;
}

unsigned recovery_io_nvs_failed_mask(void)
{
    return s_nvs_failed;
}

bool recovery_io_relays_verified_off(void)
{
    rhold_state_t snap;
    portENTER_CRITICAL(&s_hold_mux);
    snap = s_hold;
    portEXIT_CRITICAL(&s_hold_mux);
    return rhold_effective_verified_off(s_verified, &snap);
}

esp_err_t recovery_io_set_lcd_pins(bool dc_high, bool reset_high)
{
    if (!s_i2c_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    // The whole read-modify-write of s_data is under s_io_lock, the same lock
    // hold_task() takes, so neither side can act on a stale s_data. This is a
    // leaf: no other lock is taken while it is held.
    // s_data's relay bits are always 0 (SAFE_DATA), so a single 16-bit write
    // can never raise a relay.
    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    uint16_t next = (uint16_t)(s_data & ~LCD_MASK);
    if (dc_high) {
        next |= LCD_DC_BIT;
    }
    if (reset_high) {
        next |= LCD_RST_BIT;
    }
    esp_err_t err = ESP_OK;
    if (next != s_data) {
        err = write16(SX1509_REG_DATA_B, next);
        if (err == ESP_OK) {
            s_data = next;
        }
    }
    xSemaphoreGive(s_io_lock);
    return err;
}
