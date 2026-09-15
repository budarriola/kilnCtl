// Host test for kiln_io_owner.c's relay-pin "touches a relay" gates --
// TODO.md "Audit 2026-08-27 -- open items": IO_CMD_SX_LED_DRIVER could PWM a
// relay coil with no safety gate, no ownership gate and no Relay2<->Relay4
// remap, and never updated relay_shadow, unlike its two correct neighbours
// (SX_WRITE_REG/SX_SET_DIR). SX_SET_PULLUP/SX_SET_OPENDRAIN/SX_SET_INT_MASK
// had the same missing-check shape (reaching the expander with no relay-pin
// check at all).
//
// The fix refuses all four unconditionally when the target touches a relay
// pin -- see kiln_io_owner.c's sx_mask_touches_relay()/
// sx_led_driver_touches_relay() doc comments for why total refusal, not the
// ordinary relay-ON gate (safety fault + ownership), is the right call here:
// none of these subcommands has any legitimate reason to touch a relay pin
// in the first place, so refusing entirely makes the other three gaps (no
// remap needed, no relay_shadow to forget) moot rather than needing four
// separate fixes.
//
// Own executable (own main(), not merged into test_main.c/
// kilnctl_host_tests.exe), same convention as test_board_temps.c/
// test_ota_http.c/etc: it #includes kiln_io_owner.c directly to reach its
// `static` sx_mask_touches_relay()/sx_led_driver_touches_relay() predicates
// -- the actual decision the fix hinges on -- with no other seam. Also links
// the REAL kiln_io.c so kiln_io_relay_pin_mask() (the source of truth these
// predicates call) is the genuine board mapping from settings.h, not a
// hand-copied constant that could silently drift from it.
//
// What this suite deliberately does NOT cover: owner_task()'s actual command
// dispatch (the switch statement wiring these predicates to
// KILN_IO_OWNER_SX_REFUSED_RELAY) or CMD_SX_RESET's relay_shadow-clearing.
// Both require the owner task to actually run and answer through the
// post_and_wait()/queue/semaphore machinery, which this project's FreeRTOS
// host-test stubs (App/test/stubs/freertos/*.h) deliberately never do --
// xQueueSend()/xSemaphoreTake() never really deliver, same limitation
// test_owner_slot_pool.c's header comment documents for the exact same
// module. No existing test in this codebase exercises owner_task() end to
// end for ANY command (including the pre-existing, already-correct
// SET_RELAY/SX_WRITE_REG paths) for that reason -- this suite stays
// consistent with that boundary rather than inventing a new one.
#include <stdint.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "freertos/queue.h" // for TEST_STUB_QUEUE_RING_MAX_CAPACITY, ahead of the definitions below

// stubs/freertos/queue.h's xQueueCreate()/xQueueSend() reference these as
// extern (its own header comment: each test .c that wants them defines the
// backing storage, to avoid a multiple-definition error across the several
// test_*.c translation units that all include this header). kiln_io_owner.c
// calls xQueueCreate() from kiln_io_owner_start(), which THIS suite never
// calls -- these exist purely so the translation unit links, same as every
// other stub body below. Ring disabled (0): none of these tests touches the
// owner task's queue at all, only the pure sx_*_touches_relay() predicates.
int g_stub_queue_ring_enabled = 0;
unsigned char g_stub_queue_ring[TEST_STUB_QUEUE_RING_MAX_CAPACITY][256];
unsigned long g_stub_queue_ring_item_len[TEST_STUB_QUEUE_RING_MAX_CAPACITY];
int g_stub_queue_ring_capacity = 0;
int g_stub_queue_ring_count = 0;
int g_stub_queue_ring_head = 0;
int g_stub_queue_send_calls = 0;
unsigned char g_stub_last_queue_item[256];

#include "../drivers/owners/kiln_io_owner.c"

// ---- link-time stub bodies -------------------------------------------------
// None of these is reachable from sx_mask_touches_relay()/
// sx_led_driver_touches_relay() (the only functions under test), but every
// symbol kiln_io_owner.c/kiln_io.c reference must resolve at link time --
// same "wider stub surface than the test itself touches" reasoning
// test_board_temps.c/test_ota_http.c already document.

esp_err_t SX1509_write_reg(SX1509Class *e, uint8_t reg, uint8_t value)
{
    (void)e; (void)reg; (void)value; return ESP_OK;
}
esp_err_t SX1509_read_regs(SX1509Class *e, uint8_t reg, uint8_t *out, size_t len)
{
    (void)e; (void)reg; if (out && len) memset(out, 0, len); return ESP_OK;
}
esp_err_t SX1509_set_dir(SX1509Class *e, uint16_t dir_mask) { (void)e; (void)dir_mask; return ESP_OK; }
esp_err_t SX1509_set_pullup(SX1509Class *e, uint16_t mask) { (void)e; (void)mask; return ESP_OK; }
esp_err_t SX1509_set_pulldown(SX1509Class *e, uint16_t mask) { (void)e; (void)mask; return ESP_OK; }
esp_err_t SX1509_set_open_drain(SX1509Class *e, uint16_t mask) { (void)e; (void)mask; return ESP_OK; }
esp_err_t SX1509_set_debounce(SX1509Class *e, uint16_t enable_mask, uint8_t config)
{
    (void)e; (void)enable_mask; (void)config; return ESP_OK;
}
esp_err_t SX1509_set_interrupt(SX1509Class *e, uint16_t mask, uint32_t sense)
{
    (void)e; (void)mask; (void)sense; return ESP_OK;
}
esp_err_t SX1509_get_interrupt_source(SX1509Class *e, uint16_t *out_mask, bool clear)
{
    (void)e; (void)clear; if (out_mask) *out_mask = 0; return ESP_OK;
}
esp_err_t SX1509_write_port(SX1509Class *e, uint16_t value) { (void)e; (void)value; return ESP_OK; }
esp_err_t SX1509_write_masked(SX1509Class *e, uint16_t mask, uint16_t value)
{
    (void)e; (void)mask; (void)value; return ESP_OK;
}
esp_err_t SX1509_read_port(SX1509Class *e, uint16_t *out_value)
{
    (void)e; if (out_value) *out_value = 0; return ESP_OK;
}
esp_err_t SX1509_write_pin(SX1509Class *e, uint8_t pin, bool level) { (void)e; (void)pin; (void)level; return ESP_OK; }
esp_err_t SX1509_read_pin(SX1509Class *e, uint8_t pin, bool *out_level)
{
    (void)e; (void)pin; if (out_level) *out_level = false; return ESP_OK;
}
esp_err_t SX1509_led_driver(SX1509Class *e, uint8_t pin, bool enable, uint8_t intensity)
{
    (void)e; (void)pin; (void)enable; (void)intensity; return ESP_OK;
}
esp_err_t SX1509_reset(SX1509Class *e, bool hard) { (void)e; (void)hard; return ESP_OK; }
uint16_t SX1509_get_shadow(const SX1509Class *e) { (void)e; return 0; }
uint16_t SX1509_get_dir_shadow(const SX1509Class *e) { (void)e; return 0xFFFFu; }
bool SX1509_irq_asserted(const SX1509Class *e) { (void)e; return false; }
int SX1509_get_irq_gpio(const SX1509Class *e) { (void)e; return -1; }
esp_err_t SX1509_scan(i2c_master_bus_handle_t bus, uint8_t *out_addrs, size_t max_addrs, size_t *out_count)
{
    (void)bus; (void)out_addrs; (void)max_addrs; if (out_count) *out_count = 0; return ESP_OK;
}

bool danger_mode_active(void) { return false; }
bool ota_http_heat_blocked_by_update(char *reason_out, size_t reason_cap)
{
    (void)reason_out; (void)reason_cap; return false;
}
bool relay_authority_on_blocked(SafetyLinkClass *safety, uint32_t *out_sources)
{
    (void)safety; if (out_sources) *out_sources = 0; return false;
}
bool relay_authority_manual_blocked_by_owner(uint8_t relay_index) { (void)relay_index; return false; }
// Mutable (not hardcoded false) so test_relay_on_blocked_gates_on_unacknowledged_crash_report()
// below can drive relay_on_blocked() through both states -- see that test.
static bool s_stub_crash_unacked = false;
bool crash_report_has_unacknowledged(void) { return s_stub_crash_unacked; }

// -----------------------------------------------------------------------------

static void test_relay_pin_mask_is_the_four_relay_pins(void)
{
    // Sanity anchor for every check below: settings.h puts Relay1..Relay4 on
    // SX1509 pins 0-3 (kiln_io.c's KILN_IO_RELAY_MASK), so the mask under
    // test had better be exactly bits 0-3 and nothing else.
    uint16_t mask = kiln_io_relay_pin_mask();
    TEST_CHECK(mask == 0x000Fu, "kiln_io_relay_pin_mask() is pins 0-3 (Relay1..Relay4), settings.h");
}

static void test_led_driver_refused_on_every_relay_pin(void)
{
    // THE bug: IO_CMD_SX_LED_DRIVER reached the expander with no check at
    // all before this fix. Prove the predicate that now gates it catches
    // EVERY relay pin, not just pin 0.
    for (uint8_t pin = 0; pin < KILN_IO_RELAY_COUNT; pin++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "SX_LED_DRIVER pin %u (a relay pin) is refused", pin);
        TEST_CHECK(sx_led_driver_touches_relay(pin), msg);
    }
}

static void test_led_driver_still_works_on_a_genuine_led_pin(void)
{
    // The other half of "prove both directions": a gate that refuses
    // everything is not a fix. IO_5/IO_6/IO_7 (pins 11-13, kiln_io.h's top
    // comment) and the LCD control pins (14, 15) are genuinely not relays --
    // SX_LED_DRIVER must still be allowed through on those.
    const uint8_t genuine_led_pins[] = { 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    for (size_t i = 0; i < sizeof(genuine_led_pins) / sizeof(genuine_led_pins[0]); i++) {
        uint8_t pin = genuine_led_pins[i];
        char msg[96];
        snprintf(msg, sizeof(msg), "SX_LED_DRIVER pin %u (not a relay pin) is NOT refused", pin);
        TEST_CHECK(!sx_led_driver_touches_relay(pin), msg);
    }
}

static void test_mask_gate_refuses_any_mask_that_touches_a_relay_bit(void)
{
    // Used by SX_SET_PULLUP/SX_SET_OPENDRAIN/SX_SET_INT_MASK (and SX_SET_DIR,
    // already correct before this pass). A mask with even ONE relay bit set,
    // mixed in with unrelated non-relay bits, must still be refused --
    // all-or-nothing, same shape as the existing manual relay-mask gate.
    TEST_CHECK(sx_mask_touches_relay(0x0001u), "mask touching only Relay1's bit (0x0001) is refused");
    TEST_CHECK(sx_mask_touches_relay(0x0008u), "mask touching only Relay4's bit (0x0008) is refused");
    TEST_CHECK(sx_mask_touches_relay(0xFFFFu), "mask touching everything (0xFFFF) is refused");
    TEST_CHECK(sx_mask_touches_relay(0x8001u),
               "mask touching a relay bit PLUS unrelated high bits (0x8001) is refused, not just the pure case");
}

static void test_mask_gate_allows_masks_that_never_touch_a_relay_bit(void)
{
    // The other direction: IO_4..IO15 (bits 4-15) carry no relay, and 0
    // touches nothing at all -- neither should ever be refused.
    TEST_CHECK(!sx_mask_touches_relay(0x0000u), "the empty mask (0x0000) is never refused");
    TEST_CHECK(!sx_mask_touches_relay(0xFFF0u), "mask touching every non-relay bit (0xFFF0) is NOT refused");
    TEST_CHECK(!sx_mask_touches_relay(0x0010u), "mask touching only IO_1's bit (bit 4, 0x0010) is NOT refused");
}

// -----------------------------------------------------------------------------
// relay_on_blocked() -- the crash_report gate added 2026-09-15 (docs/audits/
// manual_relay_readiness_gating_options_2026-09-15.md, option B). Calls the
// REAL static relay_on_blocked() (reachable because this file #includes
// kiln_io_owner.c directly), driving the crash_report_has_unacknowledged()
// stub above through both states.
// -----------------------------------------------------------------------------
static void test_relay_on_blocked_gates_on_unacknowledged_crash_report(void)
{
    TEST_SECTION("relay_on_blocked() -- unacknowledged crash report refuses manual relay-ON, "
                 "reported via out_crash_unack, and clears once acknowledged");

    uint32_t sources = 0;
    bool updating = false;
    bool crash_unack = false;

    s_stub_crash_unacked = false;
    TEST_CHECK(relay_on_blocked(&sources, &updating, &crash_unack) == false,
               "no unacknowledged crash report, no other gate tripped -- relay-ON is NOT blocked");
    TEST_CHECK(crash_unack == false, "out_crash_unack stays false when nothing was refused");

    updating = false;
    crash_unack = false;
    s_stub_crash_unacked = true;
    TEST_CHECK(relay_on_blocked(&sources, &updating, &crash_unack) == true,
               "an unacknowledged crash report alone blocks manual relay-ON");
    TEST_CHECK(crash_unack == true, "out_crash_unack is set so the caller reports ERR_CRASH_UNACK, not ERR_SAFETY");
    TEST_CHECK(updating == false, "out_updating is untouched by the crash-report gate");

    // Back to acknowledged -- the gate must clear, not latch.
    crash_unack = false;
    s_stub_crash_unacked = false;
    TEST_CHECK(relay_on_blocked(&sources, &updating, &crash_unack) == false,
               "once acknowledged, relay-ON is unblocked again on the very next call");

    s_stub_crash_unacked = false; // leave the stub in its default state for any test after this one
}

int main(void)
{
    TEST_SECTION("kiln_io_owner relay-pin gates");

    test_relay_pin_mask_is_the_four_relay_pins();
    test_led_driver_refused_on_every_relay_pin();
    test_led_driver_still_works_on_a_genuine_led_pin();
    test_mask_gate_refuses_any_mask_that_touches_a_relay_bit();
    test_mask_gate_allows_masks_that_never_touch_a_relay_bit();
    test_relay_on_blocked_gates_on_unacknowledged_crash_report();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
