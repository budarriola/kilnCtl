// Host test for thermo_owner.c (HOST_TEST_COVERAGE_GAPS round 2, R2-9).
//
// #includes thermo_owner.c directly and drives the REAL owner_task() over the
// delivering ring-queue stub (stubs/freertos/queue.h), same harness as
// test_kiln_io_owner.c's dispatch section. The MAX31856 driver itself is
// faked below (channel lookup + one recorder per call), so what is asserted is
// thermo_owner's own logic: which channel and which arguments each command
// reaches the driver with, the NOT_FOUND / never-stale reading contract, the
// READ_ALL clamp, the module-owned result-slot pool lifecycle (alloc, release,
// zero on recycle, exhaustion), and the producer-side fail-closed fills.
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "freertos/queue.h"

int g_stub_queue_ring_enabled = 0;
unsigned char g_stub_queue_ring[TEST_STUB_QUEUE_RING_MAX_CAPACITY][256];
unsigned long g_stub_queue_ring_item_len[TEST_STUB_QUEUE_RING_MAX_CAPACITY];
int g_stub_queue_ring_capacity = 0;
int g_stub_queue_ring_count = 0;
int g_stub_queue_ring_head = 0;
int g_stub_queue_send_calls = 0;
unsigned char g_stub_last_queue_item[256];

// owner_task() loops forever on xQueueReceive(); an empty ring longjmps out.
static jmp_buf s_task_exit;
static BaseType_t test_recv_or_exit(QueueHandle_t q, void *out, TickType_t t)
{
    BaseType_t r = xQueueReceive(q, out, t);
    if (r != pdTRUE) {
        longjmp(s_task_exit, 1);
    }
    return r;
}
#define xQueueReceive(q, o, t) test_recv_or_exit((q), (o), (t))
#include "../drivers/owners/thermo_owner.c"
#undef xQueueReceive

// ---- fake MAX31856 driver -------------------------------------------------
static MAX31856Class s_fake_ch[MAX31856_CHANNEL_COUNT];
static MAX31856BusClass s_fake_bus;
static bool s_ch_present[MAX31856_CHANNEL_COUNT];

static struct {
    int calls;
    MAX31856Class *ch;
    uint8_t u8a, u8b, u8c;
    bool b1, b2;
    float f1, f2;
    int8_t i1, i2;
    size_t len;
    size_t max_readings;
} s_last;
static esp_err_t s_rc = ESP_OK;
static MAX31856Reading s_reading_to_return;
static size_t s_read_all_count_to_return;
static uint8_t s_reg_bytes[MAX31856_MAX_BURST_LEN];

static void rec_reset(void)
{
    memset(&s_last, 0, sizeof(s_last));
    s_rc = ESP_OK;
}

MAX31856Class *MAX31856_bus_channel(MAX31856BusClass *bus, uint8_t channel)
{
    if (bus != &s_fake_bus || channel >= MAX31856_CHANNEL_COUNT || !s_ch_present[channel]) {
        return NULL;
    }
    return &s_fake_ch[channel];
}
esp_err_t MAX31856_config_channel(MAX31856Class *ch, uint8_t tc_type, uint8_t avg_mode, bool filter_50hz, bool auto_convert)
{
    s_last.calls++; s_last.ch = ch; s_last.u8a = tc_type; s_last.u8b = avg_mode; s_last.b1 = filter_50hz; s_last.b2 = auto_convert;
    return s_rc;
}
esp_err_t MAX31856_set_thresholds(MAX31856Class *ch, float tc_high_c, float tc_low_c, int8_t cj_high_c, int8_t cj_low_c)
{
    s_last.calls++; s_last.ch = ch; s_last.f1 = tc_high_c; s_last.f2 = tc_low_c; s_last.i1 = cj_high_c; s_last.i2 = cj_low_c;
    return s_rc;
}
esp_err_t MAX31856_set_cj_offset(MAX31856Class *ch, float offset_c)
{
    s_last.calls++; s_last.ch = ch; s_last.f1 = offset_c;
    return s_rc;
}
esp_err_t MAX31856_trigger_one_shot(MAX31856Class *ch)
{
    s_last.calls++; s_last.ch = ch;
    return s_rc;
}
esp_err_t MAX31856_read(MAX31856Class *ch, MAX31856Reading *out)
{
    s_last.calls++; s_last.ch = ch;
    *out = s_reading_to_return;
    return s_rc;
}
esp_err_t MAX31856_read_all(MAX31856BusClass *bus, MAX31856Reading *out, size_t max_readings, size_t *out_count)
{
    (void)bus;
    s_last.calls++; s_last.max_readings = max_readings;
    for (size_t i = 0; i < max_readings && i < MAX31856_CHANNEL_COUNT; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].channel = (uint8_t)i;
        out[i].tc_temperature_c = 100.0f + (float)i;
    }
    *out_count = s_read_all_count_to_return;
    return s_rc;
}
esp_err_t MAX31856_read_faults(MAX31856Class *ch, uint8_t *out_sr, uint8_t *out_mask)
{
    s_last.calls++; s_last.ch = ch;
    *out_sr = 0x5Au; *out_mask = 0xA5u;
    return s_rc;
}
esp_err_t MAX31856_clear_faults(MAX31856Class *ch)
{
    s_last.calls++; s_last.ch = ch;
    return s_rc;
}
esp_err_t MAX31856_read_reg(MAX31856Class *ch, uint8_t reg, uint8_t *buf, size_t len)
{
    s_last.calls++; s_last.ch = ch; s_last.u8a = reg; s_last.len = len;
    memcpy(buf, s_reg_bytes, len);
    return s_rc;
}
esp_err_t MAX31856_write_reg(MAX31856Class *ch, uint8_t reg, uint8_t value)
{
    s_last.calls++; s_last.ch = ch; s_last.u8a = reg; s_last.u8b = value;
    return s_rc;
}

// ---- harness ---------------------------------------------------------------
static int s_lock_dummy;

static void module_init(void)
{
    s_bus = &s_fake_bus;
    for (int i = 0; i < MAX31856_CHANNEL_COUNT; i++) s_ch_present[i] = true;
    s_cmd_queue = (QueueHandle_t)&s_lock_dummy;
    s_slot_lock = xSemaphoreCreateMutex();
    for (size_t i = 0; i < THERMO_OWNER_SLOT_COUNT; i++) {
        s_slots[i].sem = xSemaphoreCreateBinary();
        s_slot_refcount[i] = 0;
        memset(&s_slots[i].result, 0, sizeof(s_slots[i].result));
    }
    g_stub_queue_ring_enabled = 0;
    g_stub_queue_ring_count = 0;
    g_stub_queue_ring_head = 0;
    rec_reset();
}

// Run the real owner_task() on one hand-built command; slot 0 is held by both
// sides so the owner does not recycle (and zero) it before we read it.
static owner_result_t dispatch(owner_cmd_t c)
{
    s_slot_refcount[0] = 2;
    memset(&s_slots[0].result, 0, sizeof(s_slots[0].result));
    c.slot = 0;
    g_stub_queue_ring_enabled = 1;
    g_stub_queue_ring_capacity = 8;
    g_stub_queue_ring_head = 0;
    g_stub_queue_ring_count = 1;
    memcpy(g_stub_queue_ring[0], &c, sizeof(c));
    g_stub_queue_ring_item_len[0] = sizeof(c);
    if (setjmp(s_task_exit) == 0) {
        owner_task(NULL);
    }
    g_stub_queue_ring_enabled = 0;
    return s_slots[0].result;
}

static owner_cmd_t mk(cmd_type_t t)
{
    owner_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.type = t;
    return c;
}

static void test_dispatch_each_command_reaches_driver(void)
{
    TEST_SECTION("owner_task dispatch: each command reaches its driver call with the exact channel and arguments");
    module_init();
    owner_cmd_t c;
    owner_result_t r;

    c = mk(CMD_CONFIG_CHANNEL);
    c.args.config_channel.channel = 2; c.args.config_channel.tc_type = 7; c.args.config_channel.avg_mode = 3;
    c.args.config_channel.filter_50hz = true; c.args.config_channel.auto_convert = false;
    r = dispatch(c);
    TEST_CHECK(r.err == ESP_OK && s_last.calls == 1 && s_last.ch == &s_fake_ch[2], "config: channel 2 reached");
    TEST_CHECK(s_last.u8a == 7 && s_last.u8b == 3 && s_last.b1 && !s_last.b2, "config: args tc 7, avg 3, 50Hz on, auto off");

    rec_reset();
    c = mk(CMD_SET_THRESHOLDS);
    c.args.set_thresholds.channel = 1; c.args.set_thresholds.tc_high_c = 1300.5f; c.args.set_thresholds.tc_low_c = -20.25f;
    c.args.set_thresholds.cj_high_c = 85; c.args.set_thresholds.cj_low_c = -40;
    r = dispatch(c);
    TEST_CHECK(r.err == ESP_OK && s_last.ch == &s_fake_ch[1], "thresholds: channel 1");
    TEST_CHECK(s_last.f1 == 1300.5f && s_last.f2 == -20.25f && s_last.i1 == 85 && s_last.i2 == -40, "thresholds: all four values exact");

    rec_reset();
    c = mk(CMD_SET_CJ_OFFSET);
    c.args.set_cj_offset.channel = 0; c.args.set_cj_offset.offset_c = -1.5f;
    r = dispatch(c);
    TEST_CHECK(r.err == ESP_OK && s_last.ch == &s_fake_ch[0] && s_last.f1 == -1.5f, "cj offset: channel 0, -1.5");

    rec_reset();
    c = mk(CMD_TRIGGER_ONE_SHOT); c.args.channel_only.channel = 2;
    r = dispatch(c);
    TEST_CHECK(r.err == ESP_OK && s_last.calls == 1 && s_last.ch == &s_fake_ch[2], "one shot: channel 2");

    rec_reset();
    c = mk(CMD_CLEAR_FAULTS); c.args.channel_only.channel = 1;
    r = dispatch(c);
    TEST_CHECK(r.err == ESP_OK && s_last.ch == &s_fake_ch[1], "clear faults: channel 1");

    rec_reset();
    c = mk(CMD_READ_FAULTS); c.args.channel_only.channel = 0;
    r = dispatch(c);
    TEST_CHECK(r.err == ESP_OK && r.fault_sr == 0x5Au && r.fault_mask == 0xA5u, "read faults: sr and mask carried back exactly");

    rec_reset();
    memset(&s_reading_to_return, 0, sizeof(s_reading_to_return));
    s_reading_to_return.channel = 2; s_reading_to_return.tc_temperature_c = 812.25f; s_reading_to_return.cj_temperature_c = 24.5f;
    s_reading_to_return.fault_status = 0x04; s_reading_to_return.age_ms = 321;
    c = mk(CMD_READ); c.args.channel_only.channel = 2;
    r = dispatch(c);
    TEST_CHECK(r.err == ESP_OK && s_last.ch == &s_fake_ch[2], "read: channel 2");
    TEST_CHECK(r.read_result.tc_temperature_c == 812.25f && r.read_result.cj_temperature_c == 24.5f &&
               r.read_result.fault_status == 0x04 && r.read_result.age_ms == 321, "read: reading carried back unmodified");

    rec_reset();
    for (int i = 0; i < MAX31856_MAX_BURST_LEN; i++) s_reg_bytes[i] = (uint8_t)(0xC0 + i);
    c = mk(CMD_READ_REG); c.args.read_reg.channel = 1; c.args.read_reg.reg = 0x0C; c.args.read_reg.len = 4;
    r = dispatch(c);
    TEST_CHECK(r.err == ESP_OK && s_last.u8a == 0x0C && s_last.len == 4 && r.reg_len == 4, "read_reg: reg 0x0C, len 4");
    TEST_CHECK(r.reg_buf[0] == 0xC0 && r.reg_buf[3] == 0xC3, "read_reg: bytes carried back");

    rec_reset();
    c = mk(CMD_WRITE_REG); c.args.write_reg.channel = 2; c.args.write_reg.reg = 0x01; c.args.write_reg.value = 0xB7;
    r = dispatch(c);
    TEST_CHECK(r.err == ESP_OK && s_last.ch == &s_fake_ch[2] && s_last.u8a == 0x01 && s_last.u8b == 0xB7, "write_reg: reg 0x01 value 0xB7");

    rec_reset();
    s_rc = ESP_ERR_INVALID_RESPONSE;
    c = mk(CMD_TRIGGER_ONE_SHOT); c.args.channel_only.channel = 0;
    r = dispatch(c);
    TEST_CHECK(r.err == ESP_ERR_INVALID_RESPONSE, "driver error code passes through unchanged");
}

static void test_dispatch_missing_channel_contract(void)
{
    TEST_SECTION("owner_task dispatch: absent channel -> NOT_FOUND, driver untouched, READ never stale");
    module_init();
    s_ch_present[1] = false;
    owner_cmd_t c;
    owner_result_t r;

    c = mk(CMD_TRIGGER_ONE_SHOT); c.args.channel_only.channel = 1;
    r = dispatch(c);
    TEST_CHECK(r.err == ESP_ERR_NOT_FOUND && s_last.calls == 0, "absent channel: NOT_FOUND, driver never called");

    c = mk(CMD_WRITE_REG); c.args.write_reg.channel = 9; c.args.write_reg.reg = 1; c.args.write_reg.value = 2;
    r = dispatch(c);
    TEST_CHECK(r.err == ESP_ERR_NOT_FOUND && s_last.calls == 0, "out-of-range channel 9: NOT_FOUND, no write");

    c = mk(CMD_READ); c.args.channel_only.channel = 1;
    r = dispatch(c);
    TEST_CHECK(r.err == ESP_ERR_NOT_FOUND && s_last.calls == 0, "READ of absent channel: NOT_FOUND");
    TEST_CHECK(r.read_result.channel == 1 && isnan(r.read_result.tc_temperature_c) && isnan(r.read_result.cj_temperature_c) &&
               r.read_result.spi_failed && r.read_result.age_ms == MAX31856_READING_AGE_UNKNOWN,
               "READ of absent channel: NaN temps, spi_failed, age unknown");

    c = mk(CMD_READ_FAULTS); c.args.channel_only.channel = 1;
    r = dispatch(c);
    TEST_CHECK(r.err == ESP_ERR_NOT_FOUND && r.fault_sr == 0 && r.fault_mask == 0, "READ_FAULTS of absent channel: zero fault bytes");
}

static void test_dispatch_read_all_clamp(void)
{
    TEST_SECTION("owner_task dispatch: READ_ALL needs no channel and clamps max_readings to the channel count");
    module_init();
    s_ch_present[0] = s_ch_present[1] = s_ch_present[2] = false;
    owner_cmd_t c = mk(CMD_READ_ALL);
    c.args.read_all.max_readings = 99;
    s_read_all_count_to_return = 3;
    owner_result_t r = dispatch(c);
    TEST_CHECK(r.err == ESP_OK && s_last.calls == 1, "READ_ALL runs even with no channel present (bus-level)");
    TEST_CHECK(s_last.max_readings == MAX31856_CHANNEL_COUNT, "max_readings clamped to MAX31856_CHANNEL_COUNT");
    TEST_CHECK(r.read_all_count == 3 && r.read_all_results[2].tc_temperature_c == 102.0f, "results carried back");
    rec_reset();
    c.args.read_all.max_readings = 2;
    s_read_all_count_to_return = 2;
    r = dispatch(c);
    TEST_CHECK(s_last.max_readings == 2 && r.read_all_count == 2, "smaller request passes through unchanged");
}

static void test_slot_release_and_recycle(void)
{
    TEST_SECTION("slot pool: owner half release recycles and zeroes only when the client half is gone");
    module_init();
    owner_cmd_t c = mk(CMD_READ_FAULTS);
    c.args.channel_only.channel = 0;
    // Client already gave up (timeout): only the owner half (1) remains.
    s_slot_refcount[3] = 1;
    s_slots[3].result.fault_sr = 0x77;
    c.slot = 3;
    g_stub_queue_ring_enabled = 1;
    g_stub_queue_ring_capacity = 8;
    g_stub_queue_ring_head = 0;
    g_stub_queue_ring_count = 1;
    memcpy(g_stub_queue_ring[0], &c, sizeof(c));
    g_stub_queue_ring_item_len[0] = sizeof(c);
    if (setjmp(s_task_exit) == 0) {
        owner_task(NULL);
    }
    g_stub_queue_ring_enabled = 0;
    TEST_CHECK(s_slot_refcount[3] == 0, "late owner answer frees a slot the client abandoned");
    uint8_t zero[sizeof(s_slots[3].result)];
    memset(zero, 0, sizeof(zero));
    TEST_CHECK(memcmp(&s_slots[3].result, zero, sizeof(zero)) == 0, "freed slot's result is zeroed (no late answer leaks to the next command)");

    // Client still waiting (2): the owner must NOT recycle, result stays readable.
    module_init();
    c.slot = 4;
    s_slot_refcount[4] = 2;
    g_stub_queue_ring_enabled = 1;
    g_stub_queue_ring_capacity = 8;
    g_stub_queue_ring_head = 0;
    g_stub_queue_ring_count = 1;
    memcpy(g_stub_queue_ring[0], &c, sizeof(c));
    g_stub_queue_ring_item_len[0] = sizeof(c);
    if (setjmp(s_task_exit) == 0) {
        owner_task(NULL);
    }
    g_stub_queue_ring_enabled = 0;
    TEST_CHECK(s_slot_refcount[4] == 1 && s_slots[4].result.fault_sr == 0x5Au, "client still holds: slot kept with the answer");
}

static void test_producers_fail_closed_and_pool(void)
{
    TEST_SECTION("producers: no queue, owner silent, ring full, pool exhaustion, argument guards");
    module_init();
    s_cmd_queue = NULL;
    MAX31856Reading rd;
    memset(&rd, 0x11, sizeof(rd));
    TEST_CHECK(thermo_owner_command_read(1, &rd) == ESP_ERR_TIMEOUT, "no queue: read -> TIMEOUT");
    TEST_CHECK(rd.channel == 1 && isnan(rd.tc_temperature_c) && isnan(rd.cj_temperature_c) && rd.spi_failed &&
               rd.age_ms == MAX31856_READING_AGE_UNKNOWN, "no queue: caller's reading is filled never-stale");
    size_t cnt = 99;
    MAX31856Reading all[MAX31856_CHANNEL_COUNT];
    TEST_CHECK(thermo_owner_command_read_all(all, MAX31856_CHANNEL_COUNT, &cnt) == ESP_ERR_TIMEOUT && cnt == 0, "no queue: read_all -> TIMEOUT, count 0");
    TEST_CHECK(thermo_owner_command_write_reg(0, 1, 2) == ESP_ERR_TIMEOUT, "no queue: write_reg -> TIMEOUT");
    TEST_CHECK(thermo_owner_command_config_channel(0, 1, 2, false, true) == ESP_ERR_TIMEOUT, "no queue: config -> TIMEOUT");
    TEST_CHECK(thermo_owner_command_read(1, NULL) == ESP_ERR_TIMEOUT, "NULL out tolerated on failure");

    uint8_t buf[32];
    TEST_CHECK(thermo_owner_command_read_reg(0, 1, buf, MAX31856_MAX_BURST_LEN + 1) == ESP_ERR_INVALID_ARG, "read_reg len > burst max refused before queueing");
    TEST_CHECK(thermo_owner_command_read_reg(0, 1, buf, MAX31856_MAX_BURST_LEN) == ESP_ERR_TIMEOUT, "read_reg len == burst max accepted (then times out here)");

    // Owner silent: the command IS queued, the wait times out, the client half is
    // released and the owner half stays pending until the owner answers.
    module_init();
    g_stub_queue_ring_enabled = 1;
    s_cmd_queue = xQueueCreate(THERMO_OWNER_QUEUE_LEN, sizeof(owner_cmd_t));
    TEST_CHECK(thermo_owner_command_trigger_one_shot(2) == ESP_ERR_TIMEOUT, "owner silent: TIMEOUT");
    TEST_CHECK(g_stub_queue_ring_count == 1, "the command was queued");
    owner_cmd_t queued;
    memcpy(&queued, g_stub_queue_ring[0], sizeof(queued));
    TEST_CHECK(queued.type == CMD_TRIGGER_ONE_SHOT && queued.args.channel_only.channel == 2, "queued item carries type and channel");
    TEST_CHECK(s_slot_refcount[queued.slot] == 1, "after timeout only the owner half of the slot is still held");
    // Now the owner answers late: slot recycles.
    if (setjmp(s_task_exit) == 0) {
        owner_task(NULL);
    }
    TEST_CHECK(s_slot_refcount[queued.slot] == 0, "late answer recycles the slot");

    // Exhaustion: fill every slot with abandoned commands (queue big enough that the ring is not the limit).
    module_init();
    g_stub_queue_ring_enabled = 1;
    s_cmd_queue = xQueueCreate(64, sizeof(owner_cmd_t));
    g_stub_queue_ring_capacity = 64;
    for (int i = 0; i < THERMO_OWNER_SLOT_COUNT; i++) {
        TEST_CHECK(thermo_owner_command_clear_faults(0) == ESP_ERR_TIMEOUT, "abandon one command");
    }
    int held = 0;
    for (int i = 0; i < THERMO_OWNER_SLOT_COUNT; i++) held += s_slot_refcount[i];
    TEST_CHECK(held == THERMO_OWNER_SLOT_COUNT, "every slot is held by exactly one pending owner half");
    int queued_before = g_stub_queue_ring_count;
    TEST_CHECK(thermo_owner_command_clear_faults(0) == ESP_ERR_TIMEOUT, "pool exhausted: TIMEOUT");
    TEST_CHECK(g_stub_queue_ring_count == queued_before, "pool exhausted: nothing queued");

    // Ring full (queue send fails): both halves released at once, slot free again.
    module_init();
    g_stub_queue_ring_enabled = 1;
    s_cmd_queue = xQueueCreate(THERMO_OWNER_QUEUE_LEN, sizeof(owner_cmd_t));
    g_stub_queue_ring_count = g_stub_queue_ring_capacity;
    TEST_CHECK(thermo_owner_command_clear_faults(0) == ESP_ERR_TIMEOUT, "queue full: TIMEOUT");
    int any = 0;
    for (int i = 0; i < THERMO_OWNER_SLOT_COUNT; i++) any += s_slot_refcount[i];
    TEST_CHECK(any == 0, "queue full: slot fully released, none leaked");
    g_stub_queue_ring_enabled = 0;
}

static void test_start_guards(void)
{
    TEST_SECTION("thermo_owner_start: NULL bus refused");
    module_init();
    TEST_CHECK(thermo_owner_start(NULL) == ESP_ERR_INVALID_ARG, "NULL bus -> INVALID_ARG");
    TEST_CHECK(s_bus == &s_fake_bus, "a refused start does not replace the bus");
}

int main(void)
{
    test_dispatch_each_command_reaches_driver();
    test_dispatch_missing_channel_contract();
    test_dispatch_read_all_clamp();
    test_slot_release_and_recycle();
    test_producers_fail_closed_and_pool();
    test_start_guards();
    printf("thermo_owner: %d checks, %d failures\n", g_test_count, g_test_failures);
    return g_test_failures ? 1 : 0;
}
