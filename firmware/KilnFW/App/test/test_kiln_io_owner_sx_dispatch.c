// Host test (HOST_TEST_COVERAGE_GAPS_ROUND2 R2-A / R2-3b): the REAL
// owner_task() dispatch for CMD_SET_RELAY, CMD_SET_RELAY_MASK,
// CMD_SET_RELAY_MASK_AUTHORIZED and CMD_SX_RESET, running over the REAL
// kiln_io.c and SX1509.c with the register-level fake chip from
// test_kiln_io_sx_fake.c (that file is #included whole, with its main()
// renamed, so the fake is not copied). Every relay assertion reads the fake
// chip's own registers and relay_off_tracker, not just the module's view.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define main sx_fake_unused_main
#include "test_kiln_io_sx_fake.c"
#undef main

#include "freertos/queue.h"
int g_stub_queue_ring_enabled = 0;
unsigned char g_stub_queue_ring[TEST_STUB_QUEUE_RING_MAX_CAPACITY][256];
unsigned long g_stub_queue_ring_item_len[TEST_STUB_QUEUE_RING_MAX_CAPACITY];
int g_stub_queue_ring_capacity = 0;
int g_stub_queue_ring_count = 0;
int g_stub_queue_ring_head = 0;
int g_stub_queue_send_calls = 0;
unsigned char g_stub_last_queue_item[256];

#include <setjmp.h>
static jmp_buf s_task_exit;
static BaseType_t test_recv_or_exit(QueueHandle_t q, void *out, TickType_t t)
{
    BaseType_t r = xQueueReceive(q, out, t);
    if (r != pdTRUE) longjmp(s_task_exit, 1);
    return r;
}
#define xQueueReceive(q, o, t) test_recv_or_exit((q), (o), (t))
#include "../drivers/owners/kiln_io_owner.c"
#undef xQueueReceive

#include "on_off_trigger_decide.h"
static bool s_danger = false;
bool danger_mode_active(void) { return s_danger; }
static bool s_updating = false;
bool ota_http_heat_blocked_by_update(char *reason_out, size_t reason_cap)
{
    if (s_updating && reason_out && reason_cap) snprintf(reason_out, reason_cap, "update in progress");
    return s_updating;
}
static bool s_safety_blocked = false;
bool relay_authority_on_blocked(SafetyLinkClass *safety, uint32_t *out_sources)
{
    (void)safety;
    if (out_sources) *out_sources = s_safety_blocked ? 0x04u : 0;
    return s_safety_blocked;
}
static uint8_t s_owned_mask = 0;
bool relay_authority_manual_blocked_by_owner(uint8_t relay_index)
{ return (s_owned_mask & (1u << (relay_index - 1u))) != 0; }
static bool s_crash_unack = false;
bool crash_report_has_unacknowledged(void) { return s_crash_unack; }
static bool s_profile_running = false;
void relay_authority_heat_run_active(bool *profile, bool *autotune)
{ if (profile) *profile = s_profile_running; if (autotune) *autotune = false; }
bool backup_import_restore_in_flight(void) { return false; }

static int s_dummy;
static owner_result_t dispatch(owner_cmd_t c)
{
    s_io = &g_io;
    s_cmd_queue = (QueueHandle_t)&s_dummy;
    s_slot_lock = xSemaphoreCreateMutex();
    s_slots[0].sem = xSemaphoreCreateBinary();
    s_slot_refcount[0] = 2;
    memset(&s_slots[0].result, 0, sizeof(s_slots[0].result));
    c.slot = 0;
    g_stub_queue_ring_enabled = 1;
    g_stub_queue_ring_capacity = 4;
    g_stub_queue_ring_head = 0;
    g_stub_queue_ring_count = 1;
    memcpy(g_stub_queue_ring[0], &c, sizeof(c));
    g_stub_queue_ring_item_len[0] = sizeof(c);
    if (setjmp(s_task_exit) == 0) owner_task(NULL);
    g_stub_queue_ring_enabled = 0;
    return s_slots[0].result;
}
static owner_result_t d_relay(uint8_t relay, bool on)
{
    owner_cmd_t c; memset(&c, 0, sizeof(c));
    c.type = CMD_SET_RELAY; c.args.set_relay.relay = relay; c.args.set_relay.on = on;
    return dispatch(c);
}
static owner_result_t d_mask(int type, uint8_t mask, uint8_t value)
{
    owner_cmd_t c; memset(&c, 0, sizeof(c));
    c.type = type; c.args.set_relay_mask.mask = mask; c.args.set_relay_mask.value = value;
    return dispatch(c);
}
static owner_result_t d_reset(bool hard)
{
    owner_cmd_t c; memset(&c, 0, sizeof(c));
    c.type = CMD_SX_RESET; c.args.sx_reset.hard = hard;
    return dispatch(c);
}
static void fresh(void)
{
    s_danger = false; s_safety_blocked = false; s_owned_mask = 0;
    s_updating = false; s_crash_unack = false; s_profile_running = false;
    relay_off_tracker_reset_all();
    setup_ready();
}

static void test_set_relay_dispatch(void)
{
    TEST_SECTION("CMD_SET_RELAY dispatch against the fake chip");
    fresh();
    owner_result_t r = d_relay(1, true);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_OK && r.err == ESP_OK, "relay 1 ON ok");
    TEST_CHECK(chip_relays_logical() == 0x01, "chip drives exactly relay 1");
    TEST_CHECK(g_io.relay_shadow == 0x01, "relay_shadow follows");
    TEST_CHECK(relay_off_tracker_held_s(0x01) == 0.0f, "off-tracker: relay 1 is ON, held 0");

    fake_time_advance_ms(5000u);
    r = d_relay(1, false);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_OK && chip_relays_logical() == 0, "relay 1 OFF drops the coil");
    fake_time_advance_ms(3000u);
    TEST_CHECK(relay_off_tracker_held_s(0x01) == 3.0f, "off-tracker holds exactly 3 s since the OFF write");

    r = d_relay(0, true);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_ERR_RANGE && chip_relays_logical() == 0, "relay 0 -> ERR_RANGE");
    r = d_relay(5, true);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_ERR_RANGE && chip_relays_logical() == 0, "relay 5 -> ERR_RANGE");

    s_owned_mask = 0x02;
    r = d_relay(2, true);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_ERR_OWNED && chip_relays_logical() == 0, "owned relay -> ERR_OWNED, chip untouched");
    s_owned_mask = 0;

    s_safety_blocked = true;
    r = d_relay(3, true);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_ERR_SAFETY && r.safety_sources == 0x04u, "safety fault -> ERR_SAFETY with sources");
    TEST_CHECK(chip_relays_logical() == 0, "nothing energised on refusal");
    r = d_relay(3, false);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_OK, "relay OFF is never safety-gated");
    s_safety_blocked = false;

    F.fail_forever = 1;
    r = d_relay(4, true);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_ERR_IO_FAIL && r.err != ESP_OK, "dead bus -> ERR_IO_FAIL");
    TEST_CHECK(relay_off_tracker_held_s(0x08) == ON_OFF_HOLD_SETTLED_S, "failed write is not noted as ON in the tracker (never-on sentinel)");
    F.fail_forever = 0;
}

static void test_set_relay_mask_dispatch(void)
{
    TEST_SECTION("CMD_SET_RELAY_MASK / _AUTHORIZED dispatch against the fake chip");
    fresh();
    owner_result_t r = d_mask(CMD_SET_RELAY_MASK, 0x05, 0x05);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_OK && chip_relays_logical() == 0x05, "mask 0x05 energises relays 1 and 3");
    r = d_mask(CMD_SET_RELAY_MASK, 0x01, 0x00);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_OK && chip_relays_logical() == 0x04, "clearing relay 1 leaves relay 3");
    TEST_CHECK(relay_off_tracker_held_s(0x04) == 0.0f, "relay 3 still ON in tracker");

    s_owned_mask = 0x08;
    r = d_mask(CMD_SET_RELAY_MASK, 0x08, 0x08);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_ERR_OWNED && chip_relays_logical() == 0x04, "owned relay in mask -> ERR_OWNED");
    r = d_mask(CMD_SET_RELAY_MASK, 0x07, 0x00);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_OK && chip_relays_logical() == 0x00, "mask not touching the owned relay passes");
    s_owned_mask = 0;

    s_safety_blocked = true;
    r = d_mask(CMD_SET_RELAY_MASK, 0x02, 0x02);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_ERR_SAFETY && chip_relays_logical() == 0, "safety fault blocks mask ON");
    r = d_mask(CMD_SET_RELAY_MASK_AUTHORIZED, 0x02, 0x02);
    TEST_CHECK(r.err == ESP_OK && chip_relays_logical() == 0x02, "AUTHORIZED mask bypasses the manual gates");
    TEST_CHECK(relay_off_tracker_held_s(0x02) == 0.0f, "authorized write feeds the off-tracker");
    r = d_mask(CMD_SET_RELAY_MASK_AUTHORIZED, 0x02, 0x00);
    TEST_CHECK(r.err == ESP_OK && chip_relays_logical() == 0, "authorized OFF drops the coil");
    s_safety_blocked = false;
}

static void test_sx_reset_dispatch(void)
{
    TEST_SECTION("CMD_SX_RESET dispatch against the fake chip");
    fresh();
    (void)d_mask(CMD_SET_RELAY_MASK, 0x0F, 0x0F);
    TEST_CHECK(chip_relays_logical() == 0x0F && g_io.relay_shadow == 0x0F, "precondition: all four relays on");
    fake_time_advance_ms(2000u);
    owner_result_t r = d_reset(false);
    TEST_CHECK(r.err == ESP_OK, "soft reset ok");
    TEST_CHECK(chip_relays_logical() == 0, "no coil driven after the reset");
    TEST_CHECK(g_io.relay_shadow == 0, "relay_shadow cleared");
    fake_time_advance_ms(4000u);
    TEST_CHECK(relay_off_tracker_held_s(0x0F) == 4.0f, "off-tracker records all relays OFF at the reset (held 4 s)");

    r = d_relay(2, true);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_OK && chip_relays_logical() == 0x02, "relay ON after reset drives the chip");

    fresh();
    (void)d_relay(1, true);
    r = d_reset(true);
    TEST_CHECK(r.err == ESP_ERR_INVALID_STATE, "hard reset without a GPIO -> ESP_ERR_INVALID_STATE");
    TEST_CHECK(chip_relays_logical() == 0x01 && g_io.relay_shadow == 0x01, "refused reset leaves the relay as it was");
    TEST_CHECK(relay_off_tracker_held_s(0x01) == 0.0f, "refused reset does not claim the relay went OFF");

    g_exp.reset_gpio = 7;
    r = d_reset(true);
    TEST_CHECK(r.err == ESP_OK && chip_relays_logical() == 0 && g_io.relay_shadow == 0, "wired hard reset drops the relay");
    fake_time_advance_ms(1000u);
    TEST_CHECK(relay_off_tracker_held_s(0x01) == 1.0f, "wired hard reset noted OFF in the tracker (held 1 s)");
    g_exp.reset_gpio = -1;

    fresh();
    (void)d_relay(1, true);
    F.fail_forever = 1;
    r = d_reset(false);
    TEST_CHECK(r.err != ESP_OK, "reset on a dead bus is an error");
    TEST_CHECK(relay_off_tracker_held_s(0x01) == 0.0f, "failed reset does not note OFF in the tracker");
    F.fail_forever = 0;
}

/* K7 review LOW-3: the owner's CMD_ALL_RELAYS_OFF notes OFF in the tracker only on ESP_OK; the
 * unserialised fail-safe path (lock timeout) returns 0x10C and must not be recorded as a verified OFF. */
static void test_all_off_command_notes_tracker_only_on_ok(void)
{
    TEST_SECTION("CMD_ALL_RELAYS_OFF: tracker noted on ESP_OK, not on the 0x10C unserialised result (K7 LOW-3)");
    fresh();
    (void)d_relay(1, true);
    TEST_CHECK(relay_off_tracker_held_s(0x01) == 0.0f, "precondition: relay 1 ON in tracker");
    owner_cmd_t c; memset(&c, 0, sizeof(c));
    c.type = CMD_ALL_RELAYS_OFF;
    g_test_stub_semaphore_fail_nth = 1; /* the kiln_io lock take inside the all-off times out */
    owner_result_t r = dispatch(c);
    g_test_stub_semaphore_fail_nth = 0;
    TEST_CHECK(r.err == KILN_IO_ERR_UNSERIALISED_OFF, "unserialised all-off reports 0x10C");
    fake_time_advance_ms(1000u); /* held_s is 0 at the instant of a note; advance so a wrong note shows */
    TEST_CHECK(relay_off_tracker_held_s(0x01) == 0.0f, "0x10C is not noted as OFF in the tracker");

    fresh();
    (void)d_relay(1, true);
    r = dispatch(c);
    TEST_CHECK(r.err == ESP_OK, "locked all-off ok");
    fake_time_advance_ms(1000u);
    TEST_CHECK(relay_off_tracker_held_s(0x01) == 1.0f, "ESP_OK all-off is noted in the tracker");
}

static void test_relay_on_refusal_branches(void)
{
    TEST_SECTION("relay-ON refusal branches (UPDATING / CRASH_UNACK / RUNNING) through owner_task");
    fresh();
    s_updating = true;
    owner_result_t r = d_relay(1, true);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_ERR_UPDATING && chip_relays_logical() == 0, "update in progress -> ERR_UPDATING, chip untouched");
    r = d_mask(CMD_SET_RELAY_MASK, 0x03, 0x03);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_ERR_UPDATING && chip_relays_logical() == 0, "mask ON during update -> ERR_UPDATING");
    r = d_relay(1, false);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_OK, "relay OFF is never update-gated");

    fresh();
    s_crash_unack = true;
    r = d_relay(2, true);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_ERR_CRASH_UNACK && chip_relays_logical() == 0, "unacked crash -> ERR_CRASH_UNACK");
    r = d_mask(CMD_SET_RELAY_MASK, 0x04, 0x04);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_ERR_CRASH_UNACK && chip_relays_logical() == 0, "mask ON with unacked crash -> ERR_CRASH_UNACK");

    fresh();
    s_profile_running = true;
    r = d_relay(3, true);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_ERR_RUNNING && chip_relays_logical() == 0, "firing active -> ERR_RUNNING");
    r = d_mask(CMD_SET_RELAY_MASK, 0x08, 0x08);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_ERR_RUNNING && chip_relays_logical() == 0, "mask ON during firing -> ERR_RUNNING");
    s_danger = true;
    r = d_relay(3, true);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_ERR_RUNNING && chip_relays_logical() == 0, "danger mode does not bypass the firing gate");
    s_danger = false;

    fresh();
    s_updating = true; s_crash_unack = true; s_profile_running = true;
    r = d_relay(1, true);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_ERR_UPDATING, "update refusal reported before crash/firing");
    s_safety_blocked = true;
    r = d_relay(1, true);
    TEST_CHECK(r.relay_result == KILN_IO_OWNER_RELAY_ERR_SAFETY, "safety fault reported first");
}

int main(void)
{
    g_test_stub_semaphore_take_default = 1;
    test_set_relay_dispatch();
    test_set_relay_mask_dispatch();
    test_sx_reset_dispatch();
    test_all_off_command_notes_tracker_only_on_ok();
    test_relay_on_refusal_branches();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
