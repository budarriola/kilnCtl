#include "kiln_io_owner.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "relay_authority.h"

static const char *TAG = "kiln_io_owner";

#define KILN_IO_OWNER_QUEUE_LEN 8
/* How long a producer waits for the owner task to answer once its command
 * is queued -- not the owner task's own receive timeout (it has no other
 * periodic duty, so it blocks portMAX_DELAY on the queue itself). Generous
 * against a slow I2C bus stall; a producer that times out treats the
 * result exactly like an I2C failure -- fail closed, never "assume it
 * worked" (this header's own doc comment). */
#define KILN_IO_OWNER_WAIT_MS 200

typedef enum {
    CMD_SET_RELAY,
    CMD_SET_RELAY_MASK,
    CMD_SET_RELAY_MASK_AUTHORIZED,
    CMD_SET_IO,
    CMD_SET_IO_DIR,
    CMD_ALL_RELAYS_OFF,
    CMD_READ,
    CMD_SX_WRITE_REG,
    CMD_SX_READ_REG,
    CMD_SX_SET_DIR,
    CMD_SX_SET_PULLUP,
    CMD_SX_SET_OPENDRAIN,
    CMD_SX_SET_DEBOUNCE,
    CMD_SX_SET_INT_MASK,
    CMD_SX_LED_DRIVER,
    CMD_SX_RESET,
    CMD_SX_SCAN,
} cmd_type_t;

/* One result shape big enough for whichever command produced it -- queue
 * entries and result slots are both small, fixed-size, and stack-allocated
 * by the caller (never heap), same discipline as every other FreeRTOS queue
 * in this codebase. */
typedef struct {
    esp_err_t err;                 /* raw esp_err_t from the driver call, when relevant */
    kiln_io_owner_relay_result_t relay_result;
    kiln_io_owner_sx_result_t sx_result;
    uint32_t safety_sources;
    kiln_io_state_t read_state;
    uint8_t sx_buf[16];
    size_t sx_len;
} owner_result_t;

typedef struct {
    cmd_type_t type;
    owner_result_t *result; /* caller-owned, filled by the owner task */
    SemaphoreHandle_t done; /* caller-owned binary semaphore, given last */
    union {
        struct { uint8_t relay; bool on; } set_relay;
        struct { uint8_t mask, value; } set_relay_mask;
        struct { uint8_t index; bool level; } set_io;
        struct { uint8_t index; bool input, pullup; } set_io_dir;
        struct { uint8_t reg, value; } sx_write_reg;
        struct { uint8_t reg; size_t len; } sx_read_reg;
        struct { uint16_t mask; } sx_mask16;
        struct { uint16_t mask; uint8_t config; } sx_set_debounce;
        struct { uint16_t mask; uint32_t sense; } sx_set_int_mask;
        struct { uint8_t pin; bool enable; uint8_t intensity; } sx_led_driver;
        struct { bool hard; } sx_reset;
        struct { size_t max_found; } sx_scan;
    } args;
} owner_cmd_t;

static QueueHandle_t s_cmd_queue;
static kiln_io_t *s_io;
static SafetyLinkClass *s_safety;

/* ---- Gate logic -- the one place these checks live now (this header's
 * top comment). Ported from uart_bridge.c's io_relay_on_blocked()/
 * sx_write_reg_touches_relay_on()/sx_set_dir_touches_relay(), unchanged in
 * substance. ---- */

static bool relay_on_blocked(uint32_t *out_sources)
{
    return relay_authority_on_blocked(s_safety, out_sources);
}

static bool sx_write_reg_touches_relay_on(uint8_t reg, uint8_t new_byte, uint32_t *out_sources)
{
    uint16_t relay_mask = kiln_io_relay_pin_mask();
    uint8_t relay_bits_in_byte;
    if (reg == SX1509_REG_DATA_A) {
        relay_bits_in_byte = (uint8_t)(relay_mask & 0xFFu);
    } else if (reg == SX1509_REG_DATA_B) {
        relay_bits_in_byte = (uint8_t)((relay_mask >> 8) & 0xFFu);
    } else {
        return false; /* not a RegData write -- no pin state changes */
    }
    if ((new_byte & relay_bits_in_byte) == 0) {
        return false; /* doesn't set any relay pin high */
    }
    return relay_on_blocked(out_sources);
}

static bool sx_set_dir_touches_relay(uint16_t dir_mask)
{
    return (dir_mask & kiln_io_relay_pin_mask()) != 0;
}

/* ---- Owner task -- the only code that ever calls kiln_io_ or SX1509_ functions below ---- */

static void handle_set_relay(const owner_cmd_t *cmd, owner_result_t *r)
{
    uint8_t relay = cmd->args.set_relay.relay;
    bool on = cmd->args.set_relay.on;

    if (relay < 1 || relay > KILN_IO_RELAY_COUNT) {
        r->relay_result = KILN_IO_OWNER_RELAY_ERR_RANGE;
        return;
    }
    if (relay_authority_manual_blocked_by_owner(relay)) {
        r->relay_result = KILN_IO_OWNER_RELAY_ERR_OWNED;
        return;
    }
    if (on && relay_on_blocked(&r->safety_sources)) {
        r->relay_result = KILN_IO_OWNER_RELAY_ERR_SAFETY;
        return;
    }
    r->err = kiln_io_set_relay(s_io, relay, on);
    r->relay_result = (r->err == ESP_OK) ? KILN_IO_OWNER_RELAY_OK : KILN_IO_OWNER_RELAY_ERR_IO_FAIL;
}

static void handle_set_relay_mask(const owner_cmd_t *cmd, owner_result_t *r)
{
    uint8_t mask = cmd->args.set_relay_mask.mask;
    uint8_t value = cmd->args.set_relay_mask.value;

    for (uint8_t ri = 1; ri <= KILN_IO_RELAY_COUNT; ri++) {
        if ((mask & (1u << (ri - 1u))) && relay_authority_manual_blocked_by_owner(ri)) {
            r->relay_result = KILN_IO_OWNER_RELAY_ERR_OWNED;
            return;
        }
    }
    bool any_on = (mask & value) != 0;
    if (any_on && relay_on_blocked(&r->safety_sources)) {
        r->relay_result = KILN_IO_OWNER_RELAY_ERR_SAFETY;
        return;
    }
    r->err = kiln_io_set_relay_mask(s_io, mask, value);
    r->relay_result = (r->err == ESP_OK) ? KILN_IO_OWNER_RELAY_OK : KILN_IO_OWNER_RELAY_ERR_IO_FAIL;
}

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

        switch (cmd.type) {
        case CMD_SET_RELAY:
            handle_set_relay(&cmd, &r);
            break;
        case CMD_SET_RELAY_MASK:
            handle_set_relay_mask(&cmd, &r);
            break;
        case CMD_SET_RELAY_MASK_AUTHORIZED:
            /* No ownership/safety gate -- see kiln_io_owner.h's top comment.
             * The caller (profile_executor.c/autotune_engine.c) already
             * applied its own zone-level gate before posting this. */
            r.err = kiln_io_set_relay_mask(s_io, cmd.args.set_relay_mask.mask,
                                           cmd.args.set_relay_mask.value);
            break;
        case CMD_SET_IO:
            r.err = kiln_io_set_io(s_io, cmd.args.set_io.index, cmd.args.set_io.level);
            break;
        case CMD_SET_IO_DIR:
            r.err = kiln_io_set_io_dir(s_io, cmd.args.set_io_dir.index, cmd.args.set_io_dir.input,
                                       cmd.args.set_io_dir.pullup);
            break;
        case CMD_ALL_RELAYS_OFF:
            r.err = kiln_io_all_relays_off(s_io);
            break;
        case CMD_READ:
            r.err = kiln_io_read(s_io, &r.read_state);
            break;
        case CMD_SX_WRITE_REG:
            if (sx_write_reg_touches_relay_on(cmd.args.sx_write_reg.reg, cmd.args.sx_write_reg.value,
                                              &r.safety_sources)) {
                r.sx_result = KILN_IO_OWNER_SX_REFUSED_RELAY;
                break;
            }
            r.err = SX1509_write_reg(s_io->exp, cmd.args.sx_write_reg.reg, cmd.args.sx_write_reg.value);
            r.sx_result = (r.err == ESP_OK) ? KILN_IO_OWNER_SX_OK : KILN_IO_OWNER_SX_IO_FAIL;
            break;
        case CMD_SX_READ_REG:
            r.sx_len = cmd.args.sx_read_reg.len;
            r.err = SX1509_read_regs(s_io->exp, cmd.args.sx_read_reg.reg, r.sx_buf, r.sx_len);
            break;
        case CMD_SX_SET_DIR:
            if (sx_set_dir_touches_relay(cmd.args.sx_mask16.mask)) {
                r.sx_result = KILN_IO_OWNER_SX_REFUSED_RELAY;
                break;
            }
            r.err = SX1509_set_dir(s_io->exp, cmd.args.sx_mask16.mask);
            r.sx_result = (r.err == ESP_OK) ? KILN_IO_OWNER_SX_OK : KILN_IO_OWNER_SX_IO_FAIL;
            break;
        case CMD_SX_SET_PULLUP:
            r.err = SX1509_set_pullup(s_io->exp, cmd.args.sx_mask16.mask);
            break;
        case CMD_SX_SET_OPENDRAIN:
            r.err = SX1509_set_open_drain(s_io->exp, cmd.args.sx_mask16.mask);
            break;
        case CMD_SX_SET_DEBOUNCE:
            r.err = SX1509_set_debounce(s_io->exp, cmd.args.sx_set_debounce.mask,
                                        cmd.args.sx_set_debounce.config);
            break;
        case CMD_SX_SET_INT_MASK:
            r.err = SX1509_set_interrupt(s_io->exp, cmd.args.sx_set_int_mask.mask,
                                         cmd.args.sx_set_int_mask.sense);
            break;
        case CMD_SX_LED_DRIVER:
            r.err = SX1509_led_driver(s_io->exp, cmd.args.sx_led_driver.pin, cmd.args.sx_led_driver.enable,
                                      cmd.args.sx_led_driver.intensity);
            break;
        case CMD_SX_RESET:
            r.err = SX1509_reset(s_io->exp, cmd.args.sx_reset.hard);
            break;
        case CMD_SX_SCAN: {
            uint8_t found[SX1509_ADDR_COUNT];
            size_t count = 0;
            r.err = SX1509_scan(s_io->exp->bus, found, sizeof(found), &count);
            if (r.err == ESP_OK) {
                if (count > cmd.args.sx_scan.max_found) {
                    count = cmd.args.sx_scan.max_found;
                }
                memcpy(r.sx_buf, found, count);
                r.sx_len = count;
            }
            break;
        }
        }

        if (cmd.result) {
            *cmd.result = r;
        }
        if (cmd.done) {
            xSemaphoreGive(cmd.done);
        }
    }
}

esp_err_t kiln_io_owner_start(kiln_io_t *io, SafetyLinkClass *safety)
{
    if (!io || !io->exp) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!safety) {
        ESP_LOGW(TAG, "starting with no safety link -- all relay-ON commands will be refused "
                      "until one is wired in (safety wins, see docs/SAFETY_MODEL.md)");
    }

    s_io = io;
    s_safety = safety;

    s_cmd_queue = xQueueCreate(KILN_IO_OWNER_QUEUE_LEN, sizeof(owner_cmd_t));
    if (!s_cmd_queue) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreatePinnedToCore(owner_task, "kiln_io_owner", 4096, NULL, 5, NULL,
                                                 tskNO_AFFINITY);
    if (created != pdPASS) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---- Generic post-and-wait, shared by every producer below ---- */

static bool post_and_wait(owner_cmd_t *cmd, owner_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->err = ESP_ERR_INVALID_STATE; /* fail closed if the owner never answers */

    if (!s_cmd_queue) {
        return false;
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        return false;
    }

    cmd->result = result;
    cmd->done = done;

    bool ok = false;
    if (xQueueSend(s_cmd_queue, cmd, 0) == pdTRUE) {
        ok = xSemaphoreTake(done, pdMS_TO_TICKS(KILN_IO_OWNER_WAIT_MS)) == pdTRUE;
        if (!ok) {
            ESP_LOGW(TAG, "owner task did not answer within %ums -- treating as failed",
                     (unsigned)KILN_IO_OWNER_WAIT_MS);
        }
    }

    vSemaphoreDelete(done);
    return ok;
}

/* ---- MANUAL producers ---- */

kiln_io_owner_relay_result_t kiln_io_owner_command_set_relay(uint8_t relay, bool on,
                                                              uint32_t *out_safety_sources)
{
    owner_cmd_t cmd = { .type = CMD_SET_RELAY, .args.set_relay = { .relay = relay, .on = on } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return KILN_IO_OWNER_RELAY_ERR_TIMEOUT;
    }
    if (out_safety_sources) {
        *out_safety_sources = r.safety_sources;
    }
    return r.relay_result;
}

kiln_io_owner_relay_result_t kiln_io_owner_command_set_relay_mask(uint8_t mask, uint8_t value,
                                                                   uint32_t *out_safety_sources)
{
    owner_cmd_t cmd = { .type = CMD_SET_RELAY_MASK,
                        .args.set_relay_mask = { .mask = mask, .value = value } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return KILN_IO_OWNER_RELAY_ERR_TIMEOUT;
    }
    if (out_safety_sources) {
        *out_safety_sources = r.safety_sources;
    }
    return r.relay_result;
}

/* ---- AUTHORIZED producer ---- */

esp_err_t kiln_io_owner_command_set_relay_mask_authorized(uint8_t mask, uint8_t value)
{
    owner_cmd_t cmd = { .type = CMD_SET_RELAY_MASK_AUTHORIZED,
                        .args.set_relay_mask = { .mask = mask, .value = value } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

/* ---- Digital IO ---- */

esp_err_t kiln_io_owner_command_set_io(uint8_t index, bool level)
{
    owner_cmd_t cmd = { .type = CMD_SET_IO, .args.set_io = { .index = index, .level = level } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

esp_err_t kiln_io_owner_command_set_io_dir(uint8_t index, bool input, bool pullup)
{
    owner_cmd_t cmd = { .type = CMD_SET_IO_DIR,
                        .args.set_io_dir = { .index = index, .input = input, .pullup = pullup } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

esp_err_t kiln_io_owner_command_all_relays_off(void)
{
    owner_cmd_t cmd = { .type = CMD_ALL_RELAYS_OFF };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

esp_err_t kiln_io_owner_command_read(kiln_io_state_t *out_state)
{
    owner_cmd_t cmd = { .type = CMD_READ };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    if (out_state) {
        *out_state = r.read_state;
    }
    return r.err;
}

/* ---- Raw SX1509 passthrough ---- */

kiln_io_owner_sx_result_t kiln_io_owner_command_sx_write_reg(uint8_t reg, uint8_t value,
                                                              uint32_t *out_safety_sources)
{
    owner_cmd_t cmd = { .type = CMD_SX_WRITE_REG, .args.sx_write_reg = { .reg = reg, .value = value } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return KILN_IO_OWNER_SX_TIMEOUT;
    }
    if (out_safety_sources) {
        *out_safety_sources = r.safety_sources;
    }
    return r.sx_result;
}

esp_err_t kiln_io_owner_command_sx_read_reg(uint8_t reg, uint8_t *out_buf, size_t len)
{
    if (len > sizeof(((owner_result_t *)0)->sx_buf)) {
        return ESP_ERR_INVALID_ARG;
    }
    owner_cmd_t cmd = { .type = CMD_SX_READ_REG, .args.sx_read_reg = { .reg = reg, .len = len } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    if (r.err == ESP_OK && out_buf) {
        memcpy(out_buf, r.sx_buf, r.sx_len);
    }
    return r.err;
}

kiln_io_owner_sx_result_t kiln_io_owner_command_sx_set_dir(uint16_t dir_mask)
{
    owner_cmd_t cmd = { .type = CMD_SX_SET_DIR, .args.sx_mask16 = { .mask = dir_mask } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return KILN_IO_OWNER_SX_TIMEOUT;
    }
    return r.sx_result;
}

esp_err_t kiln_io_owner_command_sx_set_pullup(uint16_t mask)
{
    owner_cmd_t cmd = { .type = CMD_SX_SET_PULLUP, .args.sx_mask16 = { .mask = mask } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

esp_err_t kiln_io_owner_command_sx_set_opendrain(uint16_t mask)
{
    owner_cmd_t cmd = { .type = CMD_SX_SET_OPENDRAIN, .args.sx_mask16 = { .mask = mask } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

esp_err_t kiln_io_owner_command_sx_set_debounce(uint16_t mask, uint8_t config)
{
    owner_cmd_t cmd = { .type = CMD_SX_SET_DEBOUNCE,
                        .args.sx_set_debounce = { .mask = mask, .config = config } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

esp_err_t kiln_io_owner_command_sx_set_int_mask(uint16_t mask, uint32_t sense)
{
    owner_cmd_t cmd = { .type = CMD_SX_SET_INT_MASK,
                        .args.sx_set_int_mask = { .mask = mask, .sense = sense } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

esp_err_t kiln_io_owner_command_sx_led_driver(uint8_t pin, bool enable, uint8_t intensity)
{
    owner_cmd_t cmd = { .type = CMD_SX_LED_DRIVER,
                        .args.sx_led_driver = { .pin = pin, .enable = enable, .intensity = intensity } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

esp_err_t kiln_io_owner_command_sx_reset(bool hard)
{
    owner_cmd_t cmd = { .type = CMD_SX_RESET, .args.sx_reset = { .hard = hard } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

esp_err_t kiln_io_owner_command_sx_scan(uint8_t *out_found, size_t max_found, size_t *out_count)
{
    if (max_found > sizeof(((owner_result_t *)0)->sx_buf)) {
        max_found = sizeof(((owner_result_t *)0)->sx_buf);
    }
    owner_cmd_t cmd = { .type = CMD_SX_SCAN, .args.sx_scan = { .max_found = max_found } };
    owner_result_t r;
    if (!post_and_wait(&cmd, &r)) {
        return ESP_ERR_TIMEOUT;
    }
    if (r.err == ESP_OK) {
        if (out_found) {
            memcpy(out_found, r.sx_buf, r.sx_len);
        }
        if (out_count) {
            *out_count = r.sx_len;
        }
    }
    return r.err;
}
