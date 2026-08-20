#include "thermo_owner.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "thermo_owner";

#define THERMO_OWNER_QUEUE_LEN 8
/* Same reasoning as kiln_io_owner.c's own KILN_IO_OWNER_WAIT_MS: how long a
 * producer waits for the owner task to answer once its command is queued,
 * not the owner task's own receive timeout (it has no periodic duty of its
 * own, so it blocks portMAX_DELAY on the queue itself). A producer that
 * times out treats the result exactly like an SPI failure -- fail closed. */
#define THERMO_OWNER_WAIT_MS 200

typedef enum {
    CMD_CONFIG_CHANNEL,
    CMD_SET_THRESHOLDS,
    CMD_SET_CJ_OFFSET,
    CMD_TRIGGER_ONE_SHOT,
    CMD_READ,
    CMD_READ_ALL,
    CMD_READ_FAULTS,
    CMD_CLEAR_FAULTS,
    CMD_READ_REG,
    CMD_WRITE_REG,
} cmd_type_t;

/* One result shape big enough for whichever command produced it -- queue
 * entries and result slots are both small, fixed-size, and stack-allocated
 * by the caller (never heap), same discipline as kiln_io_owner.c. */
typedef struct {
    esp_err_t err;
    MAX31856Reading read_result;
    MAX31856Reading read_all_results[MAX31856_CHANNEL_COUNT];
    size_t read_all_count;
    uint8_t fault_sr;
    uint8_t fault_mask;
    uint8_t reg_buf[MAX31856_MAX_BURST_LEN];
    size_t reg_len;
} owner_result_t;

typedef struct {
    cmd_type_t type;
    owner_result_t *result; /* caller-owned, filled by the owner task */
    SemaphoreHandle_t done; /* caller-owned binary semaphore, given last */
    union {
        struct { uint8_t channel; uint8_t tc_type; uint8_t avg_mode; bool filter_50hz;
                 bool auto_convert; } config_channel;
        struct { uint8_t channel; float tc_high_c, tc_low_c; int8_t cj_high_c, cj_low_c; }
            set_thresholds;
        struct { uint8_t channel; float offset_c; } set_cj_offset;
        struct { uint8_t channel; } channel_only; /* TRIGGER_ONE_SHOT, READ, READ_FAULTS,
                                                    * CLEAR_FAULTS */
        struct { size_t max_readings; } read_all;
        struct { uint8_t channel; uint8_t reg; size_t len; } read_reg;
        struct { uint8_t channel; uint8_t reg; uint8_t value; } write_reg;
    } args;
} owner_cmd_t;

static QueueHandle_t s_cmd_queue;
static MAX31856BusClass *s_bus;

/* ---- Owner task -- the only code that ever calls MAX31856_*() below ---- */

static void owner_task(void *arg)
{
    (void)arg;

    for (;;) {
        owner_cmd_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        owner_result_t r;
        memset(&r, 0, sizeof(r));
        r.err = ESP_FAIL;

        /* Every case except READ_ALL needs one channel; look it up once and
         * let the ESP_ERR_NOT_FOUND fall through uniformly -- same "NULL
         * means report this channel as failed" contract MAX31856_bus_channel()
         * itself documents. */
        MAX31856Class *ch = NULL;
        if (cmd.type != CMD_READ_ALL) {
            uint8_t channel;
            switch (cmd.type) {
            case CMD_CONFIG_CHANNEL: channel = cmd.args.config_channel.channel; break;
            case CMD_SET_THRESHOLDS: channel = cmd.args.set_thresholds.channel; break;
            case CMD_SET_CJ_OFFSET: channel = cmd.args.set_cj_offset.channel; break;
            case CMD_READ_REG: channel = cmd.args.read_reg.channel; break;
            case CMD_WRITE_REG: channel = cmd.args.write_reg.channel; break;
            default: channel = cmd.args.channel_only.channel; break;
            }
            ch = MAX31856_bus_channel(s_bus, channel);
            if (!ch) {
                r.err = ESP_ERR_NOT_FOUND;
                if (cmd.type == CMD_READ) {
                    /* MAX31856_read()'s own failure contract: never leave the
                     * result stale/uninitialized -- NaN temperatures,
                     * spi_failed set, so a caller that skips checking err
                     * still can't mistake this for a real reading. */
                    r.read_result.channel = channel;
                    r.read_result.tc_temperature_c = NAN;
                    r.read_result.cj_temperature_c = NAN;
                    r.read_result.spi_failed = true;
                }
                goto answer;
            }
        }

        switch (cmd.type) {
        case CMD_CONFIG_CHANNEL:
            r.err = MAX31856_config_channel(ch, cmd.args.config_channel.tc_type,
                                            cmd.args.config_channel.avg_mode,
                                            cmd.args.config_channel.filter_50hz,
                                            cmd.args.config_channel.auto_convert);
            break;
        case CMD_SET_THRESHOLDS:
            r.err = MAX31856_set_thresholds(ch, cmd.args.set_thresholds.tc_high_c,
                                            cmd.args.set_thresholds.tc_low_c,
                                            cmd.args.set_thresholds.cj_high_c,
                                            cmd.args.set_thresholds.cj_low_c);
            break;
        case CMD_SET_CJ_OFFSET:
            r.err = MAX31856_set_cj_offset(ch, cmd.args.set_cj_offset.offset_c);
            break;
        case CMD_TRIGGER_ONE_SHOT:
            r.err = MAX31856_trigger_one_shot(ch);
            break;
        case CMD_READ:
            r.err = MAX31856_read(ch, &r.read_result);
            break;
        case CMD_READ_ALL:
            r.read_all_count = 0;
            r.err = MAX31856_read_all(s_bus, r.read_all_results,
                                      cmd.args.read_all.max_readings > MAX31856_CHANNEL_COUNT
                                          ? MAX31856_CHANNEL_COUNT
                                          : cmd.args.read_all.max_readings,
                                      &r.read_all_count);
            break;
        case CMD_READ_FAULTS:
            r.err = MAX31856_read_faults(ch, &r.fault_sr, &r.fault_mask);
            break;
        case CMD_CLEAR_FAULTS:
            r.err = MAX31856_clear_faults(ch);
            break;
        case CMD_READ_REG:
            r.reg_len = cmd.args.read_reg.len;
            r.err = MAX31856_read_reg(ch, cmd.args.read_reg.reg, r.reg_buf, r.reg_len);
            break;
        case CMD_WRITE_REG:
            r.err = MAX31856_write_reg(ch, cmd.args.write_reg.reg, cmd.args.write_reg.value);
            break;
        }

answer:
        if (cmd.result) {
            *cmd.result = r;
        }
        if (cmd.done) {
            xSemaphoreGive(cmd.done);
        }
    }
}

esp_err_t thermo_owner_start(MAX31856BusClass *bus)
{
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }

    s_bus = bus;

    s_cmd_queue = xQueueCreate(THERMO_OWNER_QUEUE_LEN, sizeof(owner_cmd_t));
    if (!s_cmd_queue) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreatePinnedToCore(owner_task, "thermo_owner", 4096, NULL, 5, NULL,
                                                 tskNO_AFFINITY);
    if (created != pdPASS) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---- Generic post-and-wait, identical shape to kiln_io_owner.c's ---- */

static bool post_and_wait(owner_cmd_t *cmd, owner_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->err = ESP_ERR_INVALID_STATE; /* fail closed if the owner never answers */

    if (!s_cmd_queue) {
        return false;
    }

    /* Static, stack-resident semaphore -- same fix as uart_owner_transfer()
     * and i2c_owner_transfer() (2026-08-20): removes this owner's
     * contribution to the per-call internal-SRAM churn that was starving
     * Wi-Fi AP client handshakes. */
    StaticSemaphore_t done_storage;
    SemaphoreHandle_t done = xSemaphoreCreateBinaryStatic(&done_storage);
    if (!done) {
        return false;
    }

    cmd->result = result;
    cmd->done = done;

    bool ok = false;
    if (xQueueSend(s_cmd_queue, cmd, 0) == pdTRUE) {
        ok = xSemaphoreTake(done, pdMS_TO_TICKS(THERMO_OWNER_WAIT_MS)) == pdTRUE;
        if (!ok) {
            ESP_LOGW(TAG, "owner task did not answer within %ums -- treating as failed",
                     (unsigned)THERMO_OWNER_WAIT_MS);
        }
    }

    vSemaphoreDelete(done);
    return ok;
}

/* ---- Configuration ------------------------------------------------------*/

esp_err_t thermo_owner_command_config_channel(uint8_t channel, uint8_t tc_type, uint8_t avg_mode,
                                              bool filter_50hz, bool auto_convert)
{
    owner_cmd_t cmd = { .type = CMD_CONFIG_CHANNEL,
                        .args.config_channel = { .channel = channel, .tc_type = tc_type,
                                                 .avg_mode = avg_mode, .filter_50hz = filter_50hz,
                                                 .auto_convert = auto_convert } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

esp_err_t thermo_owner_command_set_thresholds(uint8_t channel, float tc_high_c, float tc_low_c,
                                              int8_t cj_high_c, int8_t cj_low_c)
{
    owner_cmd_t cmd = { .type = CMD_SET_THRESHOLDS,
                        .args.set_thresholds = { .channel = channel, .tc_high_c = tc_high_c,
                                                 .tc_low_c = tc_low_c, .cj_high_c = cj_high_c,
                                                 .cj_low_c = cj_low_c } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

esp_err_t thermo_owner_command_set_cj_offset(uint8_t channel, float offset_c)
{
    owner_cmd_t cmd = { .type = CMD_SET_CJ_OFFSET,
                        .args.set_cj_offset = { .channel = channel, .offset_c = offset_c } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

/* ---- Measurement ---------------------------------------------------------*/

esp_err_t thermo_owner_command_trigger_one_shot(uint8_t channel)
{
    owner_cmd_t cmd = { .type = CMD_TRIGGER_ONE_SHOT, .args.channel_only = { .channel = channel } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

esp_err_t thermo_owner_command_read(uint8_t channel, MAX31856Reading *out)
{
    owner_cmd_t cmd = { .type = CMD_READ, .args.channel_only = { .channel = channel } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        /* Same never-stale contract as the ESP_ERR_NOT_FOUND path in
         * owner_task(): the timeout/no-queue case is exactly as much "this
         * reading cannot be trusted" as a bad channel is. */
        if (out) {
            memset(out, 0, sizeof(*out));
            out->channel = channel;
            out->tc_temperature_c = NAN;
            out->cj_temperature_c = NAN;
            out->spi_failed = true;
        }
        return ESP_ERR_TIMEOUT;
    }
    if (out) {
        *out = r.read_result;
    }
    return r.err;
}

esp_err_t thermo_owner_command_read_all(MAX31856Reading *out, size_t max_readings,
                                        size_t *out_count)
{
    owner_cmd_t cmd = { .type = CMD_READ_ALL, .args.read_all = { .max_readings = max_readings } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        if (out_count) {
            *out_count = 0;
        }
        return ESP_ERR_TIMEOUT;
    }
    size_t n = r.read_all_count;
    if (n > max_readings) {
        n = max_readings; /* defensive; the owner task already clamps to this */
    }
    if (out && n > 0) {
        memcpy(out, r.read_all_results, n * sizeof(*out));
    }
    if (out_count) {
        *out_count = n;
    }
    return r.err;
}

esp_err_t thermo_owner_command_read_faults(uint8_t channel, uint8_t *out_sr, uint8_t *out_mask)
{
    owner_cmd_t cmd = { .type = CMD_READ_FAULTS, .args.channel_only = { .channel = channel } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    if (out_sr) {
        *out_sr = r.fault_sr;
    }
    if (out_mask) {
        *out_mask = r.fault_mask;
    }
    return r.err;
}

esp_err_t thermo_owner_command_clear_faults(uint8_t channel)
{
    owner_cmd_t cmd = { .type = CMD_CLEAR_FAULTS, .args.channel_only = { .channel = channel } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

/* ---- Raw register passthrough --------------------------------------------*/

esp_err_t thermo_owner_command_read_reg(uint8_t channel, uint8_t reg, uint8_t *out_buf, size_t len)
{
    if (len > sizeof(((owner_result_t *)0)->reg_buf)) {
        return ESP_ERR_INVALID_ARG;
    }
    owner_cmd_t cmd = { .type = CMD_READ_REG,
                        .args.read_reg = { .channel = channel, .reg = reg, .len = len } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    if (r.err == ESP_OK && out_buf) {
        memcpy(out_buf, r.reg_buf, r.reg_len);
    }
    return r.err;
}

esp_err_t thermo_owner_command_write_reg(uint8_t channel, uint8_t reg, uint8_t value)
{
    owner_cmd_t cmd = { .type = CMD_WRITE_REG,
                        .args.write_reg = { .channel = channel, .reg = reg, .value = value } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}
