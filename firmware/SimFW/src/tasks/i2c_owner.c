// i2c_owner.c -- see i2c_owner.h for the pin map, the provisional SDA/SCL
// GPIO choice, and the queue-then-apply-next-tick contract every public
// setter uses. This is the single place in SimFW that calls into
// hardware/i2c.h, hardware/gpio.h (for SDA/SCL only) or drivers/mcp23017.h
// (docs/DESIGN_NOTES.md section 4's single-owner-per-peripheral doctrine).
#include "i2c_owner.h"

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "task_priorities.h"
#include "drivers/mcp23017.h"

#define I2C_OWNER_STACK_WORDS (configMINIMAL_STACK_SIZE * 2u) // headroom for the two mcp23017_t handles + local buffers

// --- I2C0 bus configuration -------------------------------------------------
// PROVISIONAL pin choice -- see this module's header comment. GPIO4/GPIO5
// are the RP2040's conventional I2C0 default pins.
#define I2C_OWNER_SDA_GPIO  4u
#define I2C_OWNER_SCL_GPIO  5u
#define I2C_OWNER_I2C_PORT  i2c0
#define I2C_OWNER_BAUD_HZ   400000u // MCP23017 supports up to 1.7 MHz (datasheet); 400 kHz Fast-mode is a conservative, uncontested-bus default

#define MCP23017_ADDR_1 0x20u
#define MCP23017_ADDR_2 0x21u

// --- Exp1 (0x20) pin roles (docs/DESIGN_NOTES.md section 3.7) ----------------------
#define EXP1_PIN_K1          0u
#define EXP1_PIN_K2          1u
#define EXP1_PIN_K3          2u
#define EXP1_PIN_K5          3u
#define EXP1_PIN_K4          4u
#define EXP1_PIN_FAULT_LINE  5u
#define EXP1_PIN_ESTOP_DRIVE 6u
#define EXP1_PIN_DUT_POWER_MAIN   7u // fixture relay #1 -> J18 (GND_Main domain)
#define EXP1_PIN_J20_IO3     8u
#define EXP1_PIN_J20_IO4     9u
// docs/HARDWARE.md section 3.7 (resolved 2026-08-20, docs/BOM.md section 6):
// two independent DUT-power relays, one per 12V input, so that no shared
// copper ever bonds GND_Main and GND_Safty downstream of a single relay.
// Pin 10 is the second relay's control bit, deliberately named *_SAFETY (not
// "power2") so a reviewer sees a domain mismatch immediately if this bit
// were ever wired to the wrong relay.
#define EXP1_PIN_DUT_POWER_SAFETY 10u // fixture relay #2 -> J19 (GND_Safty domain)
// 11..15: 5 spare, default input+pullup.

#define EXP1_SENSE_MASK \
    ((uint16_t)((1u << EXP1_PIN_K1) | (1u << EXP1_PIN_K2) | (1u << EXP1_PIN_K3) | \
                (1u << EXP1_PIN_K5) | (1u << EXP1_PIN_K4) | (1u << EXP1_PIN_FAULT_LINE)))

// Pins 0..7 on exp1 are reserved fixed roles (relay sense, fault-line sense,
// E-stop drive, DUT-power-main); pins 8..9 (J20 IO_3/IO_4) are generic i/o,
// not reserved; pin 10 (DUT-power-safety) is reserved too, so the generic
// i2c_owner_io_*() path can never be used to gang it with the main relay or
// otherwise bypass its own named setter. Since the reserved set is no longer
// one contiguous run (0..7, then 10 alone), io_pin_allowed() below checks
// both explicitly instead of a single "<=" bound.
#define EXP1_RESERVED_MAX_PIN 7u

// --- Command queue (the only path any other task has to make i2c_owner
// touch I2C0 -- see i2c_owner.h) --------------------------------------------
typedef enum {
    I2C_OWNER_CMD_SET_ESTOP,
    I2C_OWNER_CMD_SET_DUT_POWER_MAIN,
    I2C_OWNER_CMD_SET_DUT_POWER_SAFETY,
    I2C_OWNER_CMD_IO_SET_DIR,
    I2C_OWNER_CMD_IO_WRITE,
} i2c_owner_cmd_type_t;

typedef struct {
    i2c_owner_cmd_type_t type;
    union {
        struct { bool open; } estop;
        struct { bool on; } dut_power; // shared by both SET_DUT_POWER_MAIN/SAFETY -- which
                                        // relay is which comes from cmd.type, never from a
                                        // domain field a caller could get wrong
        struct { i2c_owner_expander_t exp; uint8_t pin; bool input; bool pullup; } io_set_dir;
        struct { i2c_owner_expander_t exp; uint8_t pin; bool level; } io_write;
    } u;
} i2c_owner_cmd_t;

#define I2C_OWNER_CMD_QUEUE_DEPTH 8u

static QueueHandle_t s_cmd_queue = NULL;
static SemaphoreHandle_t s_state_mutex = NULL;
static TaskHandle_t s_task_handle = NULL;

static mcp23017_t s_exp1;
static mcp23017_t s_exp2;

// --- Mutex-guarded reader state (single writer: this task's own loop) -----
static i2c_owner_relay_states_t s_relay_states; // .valid starts false
static bool s_estop_open;      // last-commanded, defaults to false (loop closed) until a command says otherwise -- see i2c_owner.h's setter comment
static bool s_dut_power_main_on;   // last-commanded, defaults to false (off) at boot -- relay #1 / J18 / GND_Main
static bool s_dut_power_safety_on; // last-commanded, defaults to false (off) at boot -- relay #2 / J19 / GND_Safty.
                                    // Independently commanded; never derived from s_dut_power_main_on.

static i2c_owner_relay_edge_t s_edge_log[I2C_OWNER_EDGE_LOG_CAPACITY];
static size_t   s_edge_log_count;  // number of valid entries (<= capacity)
static size_t   s_edge_log_head;   // index of the oldest valid entry
static uint32_t s_edge_seq_next;   // next sequence number to assign

// Raw (non-debounced) last-seen level per expander pin, for the generic
// i2c_owner_io_read() API -- refreshed every scan tick alongside the
// debounced exp1 sense scan.
static uint16_t s_exp1_raw_word;
static uint16_t s_exp2_raw_word;
static bool     s_exp1_raw_valid;
static bool     s_exp2_raw_valid;

static mcp23017_debounce_t s_exp1_debounce;

static void state_lock(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
}

static void state_unlock(void)
{
    xSemaphoreGive(s_state_mutex);
}

// Appends one edge to the ring buffer. Caller holds s_state_mutex.
static void edge_log_push_locked(i2c_owner_signal_t signal, bool level, uint64_t time_us)
{
    size_t index;
    if (s_edge_log_count < I2C_OWNER_EDGE_LOG_CAPACITY) {
        index = (s_edge_log_head + s_edge_log_count) % I2C_OWNER_EDGE_LOG_CAPACITY;
        s_edge_log_count++;
    } else {
        // Full: overwrite the oldest entry and advance head -- the seq gap
        // this creates for a slow reader is the intended, documented loss
        // signal (i2c_owner.h / DESIGN_NOTES.md section 8.2), not a bug to avoid.
        index = s_edge_log_head;
        s_edge_log_head = (s_edge_log_head + 1u) % I2C_OWNER_EDGE_LOG_CAPACITY;
    }
    s_edge_log[index].seq = s_edge_seq_next++;
    s_edge_log[index].signal = signal;
    s_edge_log[index].level = level;
    s_edge_log[index].time_us = time_us;
}

static bool signal_from_exp1_pin(uint8_t pin, i2c_owner_signal_t *out)
{
    switch (pin) {
    case EXP1_PIN_K1: *out = I2C_OWNER_SIGNAL_K1; return true;
    case EXP1_PIN_K2: *out = I2C_OWNER_SIGNAL_K2; return true;
    case EXP1_PIN_K3: *out = I2C_OWNER_SIGNAL_K3; return true;
    case EXP1_PIN_K5: *out = I2C_OWNER_SIGNAL_K5; return true;
    case EXP1_PIN_K4: *out = I2C_OWNER_SIGNAL_K4; return true;
    case EXP1_PIN_FAULT_LINE: *out = I2C_OWNER_SIGNAL_FAULT_LINE; return true;
    default: return false;
    }
}

// One-time bring-up of exp1's fixed pin roles (docs/DESIGN_NOTES.md section 3.7):
// relay-sense + fault-line inputs (no internal pull-up -- the fixture drives
// a wetting voltage through the sensed contact per DESIGN_NOTES.md section 3.4, an
// internal pull-up would fight that), E-stop/DUT-power outputs idling
// de-asserted, J20 IO_3/IO_4 and the 6 spares defaulted to input+pullup (a
// safe, non-driving default for pins whose direction a future test may
// change via i2c_owner_io_set_dir()).
static void configure_exp1(void)
{
    for (uint8_t pin = EXP1_PIN_K1; pin <= EXP1_PIN_FAULT_LINE; pin++) {
        mcp23017_pin_set_dir(&s_exp1, pin, true);
        mcp23017_pin_set_pullup(&s_exp1, pin, false);
    }

    mcp23017_pin_write(&s_exp1, EXP1_PIN_ESTOP_DRIVE, false);
    mcp23017_pin_set_dir(&s_exp1, EXP1_PIN_ESTOP_DRIVE, false);
    mcp23017_pin_write(&s_exp1, EXP1_PIN_DUT_POWER_MAIN, false);
    mcp23017_pin_set_dir(&s_exp1, EXP1_PIN_DUT_POWER_MAIN, false);

    // J20 IO_3/IO_4 (generic, input+pullup default) and the 5 true spares.
    for (uint8_t pin = EXP1_PIN_J20_IO3; pin <= EXP1_PIN_J20_IO4; pin++) {
        mcp23017_pin_set_dir(&s_exp1, pin, true);
        mcp23017_pin_set_pullup(&s_exp1, pin, true);
    }

    // Relay #2 (safety domain, J19) idles de-asserted (off) too, just like
    // relay #1 -- both DUT-power relays default to off at boot, independently.
    mcp23017_pin_write(&s_exp1, EXP1_PIN_DUT_POWER_SAFETY, false);
    mcp23017_pin_set_dir(&s_exp1, EXP1_PIN_DUT_POWER_SAFETY, false);

    for (uint8_t pin = EXP1_PIN_DUT_POWER_SAFETY + 1u; pin <= 15u; pin++) {
        mcp23017_pin_set_dir(&s_exp1, pin, true);
        mcp23017_pin_set_pullup(&s_exp1, pin, true);
    }
}

// Exp2 is generic/spare per this pass (docs/DESIGN_NOTES.md section 3.6's `~FAULT`
// fallback role is speculative future work, deliberately not wired here) --
// every pin defaults to input+pullup, the same safe non-driving default
// exp1's spares get.
static void configure_exp2(void)
{
    for (uint8_t pin = 0u; pin <= 15u; pin++) {
        mcp23017_pin_set_dir(&s_exp2, pin, true);
        mcp23017_pin_set_pullup(&s_exp2, pin, true);
    }
}

// Drains every pending command (non-blocking) and applies it. Called once
// per scan tick, before the sense read, so a command's effect (e.g. a
// freshly-changed exp1 spare's new output level) is reflected in the same
// tick's raw-word snapshot.
static void apply_pending_commands(void)
{
    i2c_owner_cmd_t cmd;
    while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
        switch (cmd.type) {
        case I2C_OWNER_CMD_SET_ESTOP:
            if (mcp23017_pin_write(&s_exp1, EXP1_PIN_ESTOP_DRIVE, cmd.u.estop.open)) {
                state_lock();
                s_estop_open = cmd.u.estop.open;
                state_unlock();
            }
            break;
        case I2C_OWNER_CMD_SET_DUT_POWER_MAIN:
            if (mcp23017_pin_write(&s_exp1, EXP1_PIN_DUT_POWER_MAIN, cmd.u.dut_power.on)) {
                state_lock();
                s_dut_power_main_on = cmd.u.dut_power.on;
                state_unlock();
            }
            break;
        case I2C_OWNER_CMD_SET_DUT_POWER_SAFETY:
            if (mcp23017_pin_write(&s_exp1, EXP1_PIN_DUT_POWER_SAFETY, cmd.u.dut_power.on)) {
                state_lock();
                s_dut_power_safety_on = cmd.u.dut_power.on;
                state_unlock();
            }
            break;
        case I2C_OWNER_CMD_IO_SET_DIR: {
            mcp23017_t *dev = (cmd.u.io_set_dir.exp == I2C_OWNER_EXP_1) ? &s_exp1 : &s_exp2;
            mcp23017_pin_set_dir(dev, cmd.u.io_set_dir.pin, cmd.u.io_set_dir.input);
            mcp23017_pin_set_pullup(dev, cmd.u.io_set_dir.pin, cmd.u.io_set_dir.pullup);
            break;
        }
        case I2C_OWNER_CMD_IO_WRITE: {
            mcp23017_t *dev = (cmd.u.io_write.exp == I2C_OWNER_EXP_1) ? &s_exp1 : &s_exp2;
            mcp23017_pin_write(dev, cmd.u.io_write.pin, cmd.u.io_write.level);
            break;
        }
        }
    }
}

// One scan tick: refresh both expanders' raw GPIO words, debounce exp1's
// sense mask, and log/publish any confirmed edges. Docs/DESIGN_NOTES.md section 4.1:
// "relay-sense debounced scan (5-10 ms)".
static void scan_tick(void)
{
    uint16_t raw1;
    if (mcp23017_read_gpio_word(&s_exp1, &raw1)) {
        state_lock();
        s_exp1_raw_word = raw1;
        s_exp1_raw_valid = true;
        state_unlock();

        uint16_t changed;
        bool edge = mcp23017_debounce_update(&s_exp1_debounce, raw1, EXP1_SENSE_MASK, &changed);
        uint64_t now_us = time_us_64();

        if (edge) {
            state_lock();
            for (uint8_t pin = 0u; pin <= EXP1_PIN_FAULT_LINE; pin++) {
                uint16_t bit = (uint16_t)(1u << pin);
                if ((changed & bit) == 0) {
                    continue;
                }
                i2c_owner_signal_t signal;
                if (!signal_from_exp1_pin(pin, &signal)) {
                    continue;
                }
                bool level = (s_exp1_debounce.stable & bit) != 0;
                edge_log_push_locked(signal, level, now_us);
            }

            s_relay_states.k1_closed = (s_exp1_debounce.stable & (1u << EXP1_PIN_K1)) != 0;
            s_relay_states.k2_closed = (s_exp1_debounce.stable & (1u << EXP1_PIN_K2)) != 0;
            s_relay_states.k3_closed = (s_exp1_debounce.stable & (1u << EXP1_PIN_K3)) != 0;
            s_relay_states.k5_closed = (s_exp1_debounce.stable & (1u << EXP1_PIN_K5)) != 0;
            s_relay_states.k4_closed = (s_exp1_debounce.stable & (1u << EXP1_PIN_K4)) != 0;
            s_relay_states.fault_line_asserted = (s_exp1_debounce.stable & (1u << EXP1_PIN_FAULT_LINE)) != 0;
            s_relay_states.sample_time_us = now_us;
            s_relay_states.valid = true;
            state_unlock();
        } else if (!s_relay_states.valid && s_exp1_debounce.initialized) {
            // First-ever debounce settle for at least one bit already
            // happened at init seeding (mcp23017_debounce_update() seeds
            // `stable` on its first call without reporting an edge) -- push
            // the initial snapshot once so readers are not stuck at
            // .valid == false forever on a perfectly quiet bus.
            state_lock();
            s_relay_states.k1_closed = (s_exp1_debounce.stable & (1u << EXP1_PIN_K1)) != 0;
            s_relay_states.k2_closed = (s_exp1_debounce.stable & (1u << EXP1_PIN_K2)) != 0;
            s_relay_states.k3_closed = (s_exp1_debounce.stable & (1u << EXP1_PIN_K3)) != 0;
            s_relay_states.k5_closed = (s_exp1_debounce.stable & (1u << EXP1_PIN_K5)) != 0;
            s_relay_states.k4_closed = (s_exp1_debounce.stable & (1u << EXP1_PIN_K4)) != 0;
            s_relay_states.fault_line_asserted = (s_exp1_debounce.stable & (1u << EXP1_PIN_FAULT_LINE)) != 0;
            s_relay_states.sample_time_us = now_us;
            s_relay_states.valid = true;
            state_unlock();
        }
    }

    uint16_t raw2;
    if (mcp23017_read_gpio_word(&s_exp2, &raw2)) {
        state_lock();
        s_exp2_raw_word = raw2;
        s_exp2_raw_valid = true;
        state_unlock();
    }
}

static void i2c_owner_task_fn(void *arg)
{
    (void)arg;

    i2c_init(I2C_OWNER_I2C_PORT, I2C_OWNER_BAUD_HZ);
    gpio_set_function(I2C_OWNER_SDA_GPIO, GPIO_FUNC_I2C);
    gpio_set_function(I2C_OWNER_SCL_GPIO, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_OWNER_SDA_GPIO);
    gpio_pull_up(I2C_OWNER_SCL_GPIO);

    mcp23017_init(&s_exp1, I2C_OWNER_I2C_PORT, MCP23017_ADDR_1);
    mcp23017_init(&s_exp2, I2C_OWNER_I2C_PORT, MCP23017_ADDR_2);
    configure_exp1();
    configure_exp2();
    mcp23017_debounce_init(&s_exp1_debounce);

    for (;;) {
        apply_pending_commands();
        scan_tick();
        vTaskDelay(pdMS_TO_TICKS(MCP23017_DEBOUNCE_SCAN_MS));
    }
}

bool i2c_owner_start(void)
{
    s_cmd_queue = xQueueCreate(I2C_OWNER_CMD_QUEUE_DEPTH, sizeof(i2c_owner_cmd_t));
    if (s_cmd_queue == NULL) {
        return false;
    }

    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL) {
        return false;
    }

    memset(&s_relay_states, 0, sizeof(s_relay_states));

    BaseType_t ok = xTaskCreate(i2c_owner_task_fn, "i2c_owner", I2C_OWNER_STACK_WORDS, NULL,
                                 SIMFW_PRIO_I2C_OWNER, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SIMFW_CORE_ELASTIC_PATH);
    return true;
}

i2c_owner_relay_states_t i2c_owner_get_relay_states(void)
{
    i2c_owner_relay_states_t copy;
    state_lock();
    copy = s_relay_states;
    state_unlock();
    return copy;
}

size_t i2c_owner_get_relay_edges(i2c_owner_relay_edge_t *out, size_t max_out, uint32_t since_seq)
{
    if (!out || max_out == 0) {
        return 0;
    }

    size_t copied = 0;
    state_lock();
    for (size_t i = 0; i < s_edge_log_count && copied < max_out; i++) {
        size_t index = (s_edge_log_head + i) % I2C_OWNER_EDGE_LOG_CAPACITY;
        if (s_edge_log[index].seq > since_seq) {
            out[copied++] = s_edge_log[index];
        }
    }
    state_unlock();
    return copied;
}

bool i2c_owner_set_estop(bool open)
{
    if (!s_cmd_queue) {
        return false;
    }
    i2c_owner_cmd_t cmd = { .type = I2C_OWNER_CMD_SET_ESTOP, .u.estop = { .open = open } };
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool i2c_owner_set_dut_power_main(bool on)
{
    if (!s_cmd_queue) {
        return false;
    }
    i2c_owner_cmd_t cmd = { .type = I2C_OWNER_CMD_SET_DUT_POWER_MAIN, .u.dut_power = { .on = on } };
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool i2c_owner_set_dut_power_safety(bool on)
{
    if (!s_cmd_queue) {
        return false;
    }
    i2c_owner_cmd_t cmd = { .type = I2C_OWNER_CMD_SET_DUT_POWER_SAFETY, .u.dut_power = { .on = on } };
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

// Deprecated alias, kept for backward compatibility -- see i2c_owner.h's
// header comment above the declaration for why this means "main domain
// only," never "both."
bool i2c_owner_set_dut_power(bool on)
{
    return i2c_owner_set_dut_power_main(on);
}

bool i2c_owner_get_estop_open(void)
{
    bool v;
    state_lock();
    v = s_estop_open;
    state_unlock();
    return v;
}

bool i2c_owner_get_dut_power_main_on(void)
{
    bool v;
    state_lock();
    v = s_dut_power_main_on;
    state_unlock();
    return v;
}

bool i2c_owner_get_dut_power_safety_on(void)
{
    bool v;
    state_lock();
    v = s_dut_power_safety_on;
    state_unlock();
    return v;
}

// Deprecated alias, kept for backward compatibility -- reports the main
// relay only, matching i2c_owner_set_dut_power()'s "main domain only"
// meaning above.
bool i2c_owner_get_dut_power_on(void)
{
    return i2c_owner_get_dut_power_main_on();
}

static bool io_pin_allowed(i2c_owner_expander_t exp, uint8_t pin)
{
    if (pin > 15u) {
        return false;
    }
    if (exp == I2C_OWNER_EXP_1 &&
        (pin <= EXP1_RESERVED_MAX_PIN || pin == EXP1_PIN_DUT_POWER_SAFETY)) {
        return false; // fixed-role pins -- see i2c_owner.h
    }
    return true;
}

bool i2c_owner_io_set_dir(i2c_owner_expander_t exp, uint8_t pin, bool input, bool pullup)
{
    if (!s_cmd_queue || !io_pin_allowed(exp, pin)) {
        return false;
    }
    i2c_owner_cmd_t cmd = {
        .type = I2C_OWNER_CMD_IO_SET_DIR,
        .u.io_set_dir = { .exp = exp, .pin = pin, .input = input, .pullup = pullup },
    };
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool i2c_owner_io_write(i2c_owner_expander_t exp, uint8_t pin, bool level)
{
    if (!s_cmd_queue || !io_pin_allowed(exp, pin)) {
        return false;
    }
    i2c_owner_cmd_t cmd = {
        .type = I2C_OWNER_CMD_IO_WRITE,
        .u.io_write = { .exp = exp, .pin = pin, .level = level },
    };
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool i2c_owner_io_read(i2c_owner_expander_t exp, uint8_t pin, bool *level)
{
    if (!io_pin_allowed(exp, pin) || !level) {
        return false;
    }

    bool valid;
    uint16_t word;
    state_lock();
    if (exp == I2C_OWNER_EXP_1) {
        valid = s_exp1_raw_valid;
        word = s_exp1_raw_word;
    } else {
        valid = s_exp2_raw_valid;
        word = s_exp2_raw_word;
    }
    state_unlock();

    if (!valid) {
        return false;
    }
    *level = (word & (uint16_t)(1u << pin)) != 0;
    return true;
}
