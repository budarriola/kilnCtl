// ROADMAP.md M13's TASK 2: safety_link.c is ~3900 lines of the KilnFW <->
// SaftyFW link -- several of the defects fixed this week (the stashed
// commit-rejection frame, the reply-window race, the trip-event capture
// logic) lived inside it, and until this file, none of it could compile off
// -target: it pulls in driver/gpio.h, driver/uart.h, a dozen kilnlink/*
// codecs, and safety_cfg_store.h. safety_trip_decision.c was pulled OUT of
// this file for exactly that reason (three lines of boolean algebra were
// worth extracting so they could be host-tested); this file does the other
// half of the job the extraction sidestepped -- getting the REST of
// safety_link.c to compile and link off-target at all, following the same
// precedent as ota_http.c (commit da4918c, "Host-test the OTA auth
// decisions") and zones_http.c/profile_executor.c/profiles_http.c before it:
// #include the real .c directly (its own header comment lists which static
// functions have no other seam) rather than restate its logic somewhere
// host-friendly.
//
// What this closes: safety_apply_status() -- the Frame A (GET_STATUS) wire
// decode -- had zero host coverage. It has two real decisions worth pinning:
// (1) it accepts EITHER the V1 (23-byte) or V2 (24-byte) status frame, never
// rejecting one in favor of the other (2026-08-23 skew-safety fix, see the
// function's own comment) and (2) SAFETY_FLAG_TEMP_VALID is the sole
// authority for whether tc_temp_c/cj_temp_c are trusted -- a peer that sends
// live-looking floats with the flag clear must still read back as NaN, not
// as a plausible temperature. Also pins safety_link_versions_compatible()'s
// two-sided protocol/min_compatible comparison.
//
// What this does NOT close: the poll task's own scheduling/timing logic
// (safety_poll_task itself, the initial-announce burst cadence, the
// reply-window race class of bug), safety_link_start()'s real hardware
// init path (UART/GPIO), or anything behind safety_exchange()'s blocking
// request/reply cycle -- those either need real time-under-test or a much
// larger fake uart_owner/uart_protocol that actually queues and replies,
// which is future work, not this pass. This file proves the FILE compiles
// and links off-target, and exercises the two static decode functions with
// the clearest "cause matters more than remedy" stakes (a plausible-looking
// fake temperature reading a bad decode could hand an operator).
//
// The ~15 external functions (MAX31856_*/kiln_io_*/thermo_owner_*/
// zones_config_*/safety_cfg_store_*, uart_owner_*/uart_protocol_*) below are
// FAKE bodies -- safety_link.c calls them but this test never reaches those
// call sites (safety_link_start()/safety_poll_task() are never invoked),
// so "compiles and links" is all they need to provide, same convention as
// test_backup_import.c's zones_config_*() fakes.

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "test_common.h"

#include "MAX31856.h"
#include "kiln_io.h"
#include "profile_executor.h"
#include "thermo_owner.h"
#include "zones_http.h"
#include "safety_cfg_store.h"

uint32_t esp_random(void) { return 0; }

MAX31856Class *MAX31856_bus_channel(MAX31856BusClass *bus, uint8_t channel)
{ (void)bus; (void)channel; return NULL; }
esp_err_t MAX31856_get_config(MAX31856Class *ch, MAX31856Config *out_cfg)
{ (void)ch; (void)out_cfg; return ESP_FAIL; }
esp_err_t MAX31856_read_all(MAX31856BusClass *bus, MAX31856Reading *out, size_t max_readings, size_t *out_count)
{ (void)bus; (void)out; (void)max_readings; (void)out_count; return ESP_FAIL; }
uint8_t kiln_io_get_relay_shadow(const kiln_io_t *io) { (void)io; return 0; }
void profile_executor_get_status(profile_exec_status_t *out) { (void)out; }
bool safety_cfg_store_maybe_refetch(SafetyLinkClass *link, uint16_t live_config_crc)
{ (void)link; (void)live_config_crc; return false; }
bool safety_cfg_store_refetch(SafetyLinkClass *link, uint16_t config_crc)
{ (void)link; (void)config_crc; return false; }
esp_err_t thermo_owner_command_read_all(MAX31856Reading *out, size_t max_readings, size_t *out_count)
{ (void)out; (void)max_readings; (void)out_count; return ESP_FAIL; }
bool zones_config_get_safety_tc_type(uint8_t *out_tc_type) { (void)out_tc_type; return false; }
bool zones_config_is_valid(void) { return true; }

esp_err_t uart_owner_init(uart_owner_t *owner, uart_port_t port, int tx_io, int rx_io,
                           int baud_rate, unsigned queue_len, unsigned task_priority,
                           uint32_t stack_depth, int core_id)
{ (void)owner; (void)port; (void)tx_io; (void)rx_io; (void)baud_rate; (void)queue_len;
  (void)task_priority; (void)stack_depth; (void)core_id; return ESP_FAIL; }
esp_err_t uart_owner_deinit(uart_owner_t *owner) { (void)owner; return ESP_OK; }
uint32_t uart_owner_get_rx_error_count(const uart_owner_t *owner) { (void)owner; return 0; }
esp_err_t uart_owner_transfer(uart_owner_t *owner, const uint8_t *tx_buffer, size_t tx_length,
                               uint8_t *rx_buffer, size_t rx_length, size_t *rx_length_out,
                               uint32_t timeout_ms)
{ (void)owner; (void)tx_buffer; (void)tx_length; (void)rx_buffer; (void)rx_length;
  (void)rx_length_out; (void)timeout_ms; return ESP_FAIL; }

esp_err_t uart_protocol_init(uart_protocol_t *proto, uart_owner_t *owner,
                              uart_proto_device_t own_device, unsigned task_priority,
                              uint32_t stack_depth, int core_id)
{ (void)proto; (void)owner; (void)own_device; (void)task_priority; (void)stack_depth;
  (void)core_id; return ESP_FAIL; }
esp_err_t uart_protocol_deinit(uart_protocol_t *proto) { (void)proto; return ESP_OK; }
esp_err_t uart_protocol_register_task(uart_protocol_t *proto, uint8_t task_id,
                                       unsigned inbox_len, QueueHandle_t *out_inbox)
{ (void)proto; (void)task_id; (void)inbox_len; if (out_inbox) *out_inbox = NULL; return ESP_FAIL; }
esp_err_t uart_protocol_unregister_task(uart_protocol_t *proto, uint8_t task_id)
{ (void)proto; (void)task_id; return ESP_OK; }
esp_err_t uart_protocol_get_task_broadcast_dropped(uart_protocol_t *proto, uint8_t task_id,
                                                    uint32_t *out)
{ (void)proto; (void)task_id; if (out) *out = 0; return ESP_OK; }
esp_err_t uart_protocol_get_deframe_stats(uart_protocol_t *proto, uint32_t *out_frames_deframed,
                                          uint32_t *out_frames_routed_nowhere,
                                          uint32_t *out_frame_length_mismatch,
                                          uint32_t *out_frame_crc_mismatch,
                                          uint32_t *out_frame_resync)
{ (void)proto;
  if (out_frames_deframed) *out_frames_deframed = 0;
  if (out_frames_routed_nowhere) *out_frames_routed_nowhere = 0;
  if (out_frame_length_mismatch) *out_frame_length_mismatch = 0;
  if (out_frame_crc_mismatch) *out_frame_crc_mismatch = 0;
  if (out_frame_resync) *out_frame_resync = 0;
  return ESP_OK; }
esp_err_t uart_protocol_receive(QueueHandle_t inbox, uart_proto_message_t *out_msg, TickType_t wait_ticks)
{ (void)inbox; (void)out_msg; (void)wait_ticks; return ESP_ERR_TIMEOUT; }
esp_err_t uart_protocol_send(uart_protocol_t *proto, uart_proto_device_t dst_device,
                              uint8_t dst_task, uint8_t src_task, const uint8_t *payload,
                              size_t length, uint32_t ack_timeout_ms)
{ (void)proto; (void)dst_device; (void)dst_task; (void)src_task; (void)payload; (void)length;
  (void)ack_timeout_ms; return ESP_FAIL; }
esp_err_t uart_protocol_send_broadcast(uart_protocol_t *proto, uart_proto_device_t dst_device,
                                        uint8_t dst_task, uint8_t src_task,
                                        const uint8_t *payload, size_t length)
{ (void)proto; (void)dst_device; (void)dst_task; (void)src_task; (void)payload; (void)length;
  return ESP_FAIL; }

// Same reasoning/precedent as test_ota_http.c and test_safety_cfg_store.c
// (both header-commented at length): stubs/freertos/semphr.h's
// xSemaphoreTake() deliberately always returns pdFALSE on a non-NULL
// handle, which is fine for every OTHER host test (none of them run code
// INSIDE the locked section), but safety_apply_status()'s actual decode
// logic runs inside safety_lock()/safety_unlock() -- a Take that never
// succeeds would make every test below dead-end at "lock failed, frame
// dropped" instead of reaching the code under test. Redirect to a local,
// always-succeeds-on-a-non-NULL-handle replacement for just this file's
// #include of safety_link.c, then restore it immediately after.
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static inline BaseType_t safety_link_test_xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    assert(sem != NULL && "xSemaphoreTake on a NULL handle -- would assert/panic on real FreeRTOS");
    (void)ticks;
    return pdTRUE; // host tests are single-threaded -- a valid handle is always available
}
#define xSemaphoreTake safety_link_test_xSemaphoreTake

#include "../drivers/safety_link.c"

#undef xSemaphoreTake

// ---------------------------------------------------------------------

static void set_status_frame(uint8_t *p, uint8_t flags, float tc_c, float cj_c,
                              uint8_t tc_fault, float ia, float ib, float ic)
{
    memset(p, 0, SAFETY_LINK_STATUS_FRAME_LEN_V2);
    p[0] = SAFETY_CMD_GET_STATUS;
    p[1] = flags;
    memcpy(&p[2], &tc_c, sizeof(float));
    memcpy(&p[6], &cj_c, sizeof(float));
    p[10] = tc_fault;
    memcpy(&p[11], &ia, sizeof(float));
    memcpy(&p[15], &ib, sizeof(float));
    memcpy(&p[19], &ic, sizeof(float));
}

static SafetyLinkClass make_link(void)
{
    SafetyLinkClass link;
    memset(&link, 0, sizeof(link));
    link.state_lock = xSemaphoreCreateMutex();
    link.xact_lock = xSemaphoreCreateMutex();
    return link;
}

static void test_apply_status_accepts_v1_and_v2_lengths(void)
{
    TEST_SECTION("safety_apply_status -- V1 (23B) and V2 (24B) frames BOTH decode "
                 "(2026-08-23 skew-safety fix: rejecting either would silence Frame A "
                 "whenever one side of the link is on the other frame version)");

    SafetyLinkClass link = make_link();
    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));

    set_status_frame(msg.payload, (uint8_t)(SAFETY_FLAG_TEMP_VALID), 123.5f, 24.0f, 0, 1.0f, 2.0f, 3.0f);
    msg.length = SAFETY_LINK_STATUS_FRAME_LEN_V1;
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "V1 (23-byte) frame is accepted");
    TEST_CHECK(link.cached.tx_dropped_known == false, "V1 frame leaves tx_dropped_known false (no byte 23 to read)");

    msg.payload[23] = 7;
    msg.length = SAFETY_LINK_STATUS_FRAME_LEN_V2;
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "V2 (24-byte) frame is accepted");
    TEST_CHECK(link.cached.tx_dropped_known == true, "V2 frame sets tx_dropped_known true");
    TEST_CHECK(link.cached.tx_dropped_sat == 7, "V2 frame's byte 23 becomes tx_dropped_sat");

    // Re-assert V1 AFTER the V2 frame: safety_link.c's own comment says a
    // peer that regresses from V2 to V1 mid-session must not leave a stale
    // "known" flag set. cached.tx_dropped_known is already true from the V2
    // frame above, so this only proves the else-branch fires if it is
    // actually observed flipping back to false here.
    set_status_frame(msg.payload, (uint8_t)(SAFETY_FLAG_TEMP_VALID), 123.5f, 24.0f, 0, 1.0f, 2.0f, 3.0f);
    msg.length = SAFETY_LINK_STATUS_FRAME_LEN_V1;
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "V1 frame after a V2 frame is still accepted");
    TEST_CHECK(link.cached.tx_dropped_known == false,
               "a peer regressing from V2 to V1 mid-session clears the stale tx_dropped_known flag");

    msg.length = 22;
    link.stats.frame_errors = 0;
    TEST_CHECK(safety_apply_status(&link, &msg) == false, "a length that is neither V1 nor V2 is rejected");
    TEST_CHECK(link.stats.frame_errors == 1, "the rejection is counted as a frame error");
}

static void test_apply_status_temp_valid_flag_is_sole_authority(void)
{
    TEST_SECTION("safety_apply_status -- SAFETY_FLAG_TEMP_VALID is the ONLY thing that "
                 "makes tc_temp_c/cj_temp_c trusted; a plausible-looking float with the "
                 "flag clear must still read back as NaN, not as a real temperature");

    SafetyLinkClass link = make_link();
    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.length = SAFETY_LINK_STATUS_FRAME_LEN_V1;

    // Flag SET: a live-looking reading passes through untouched.
    set_status_frame(msg.payload, (uint8_t)SAFETY_FLAG_TEMP_VALID, 950.25f, 22.0f, 0, 0, 0, 0);
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "decode succeeds");
    TEST_CHECK(link.cached.tc_temp_c == 950.25f, "TEMP_VALID set: tc_temp_c is trusted as-sent");
    TEST_CHECK(link.cached.cj_temp_c == 22.0f, "TEMP_VALID set: cj_temp_c is trusted as-sent");

    // Flag CLEAR: even a firmware bug that "helpfully" sent a real-looking
    // number instead of NaN must not leak through -- this is exactly the
    // gap a bad decode here would hand an operator: a plausible kiln
    // temperature that is not actually being reported as valid.
    set_status_frame(msg.payload, 0x00, 950.25f, 22.0f, 0, 0, 0, 0);
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "decode still succeeds");
    TEST_CHECK(isnan(link.cached.tc_temp_c), "TEMP_VALID clear: tc_temp_c is forced to NaN despite a real-looking payload float");
    TEST_CHECK(isnan(link.cached.cj_temp_c), "TEMP_VALID clear: cj_temp_c is forced to NaN despite a real-looking payload float");
}

static void test_apply_status_ignores_peer_link_up_and_fault_bits(void)
{
    TEST_SECTION("safety_apply_status -- bits 0/1 (LINK_UP, FAULT) describe OUR view, "
                 "never the peer's -- whatever the Pico puts there is dropped, not trusted "
                 "(safety_link.h's own contract: those bits are ESP-owned)");

    SafetyLinkClass link = make_link();
    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.length = SAFETY_LINK_STATUS_FRAME_LEN_V1;

    // A peer that (incorrectly) sets both LINK_UP and FAULT alongside a
    // valid-temp flag must not have those two bits survive into our cache.
    uint8_t flags = (uint8_t)(SAFETY_FLAG_LINK_UP | SAFETY_FLAG_FAULT | SAFETY_FLAG_TEMP_VALID);
    set_status_frame(msg.payload, flags, 500.0f, 20.0f, 0, 0, 0, 0);
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "decode succeeds");
    TEST_CHECK((link.cached.flags & SAFETY_FLAG_LINK_UP) == 0, "peer-sent LINK_UP bit is dropped, not cached");
    TEST_CHECK((link.cached.flags & SAFETY_FLAG_FAULT) == 0, "peer-sent FAULT bit is dropped, not cached");
    TEST_CHECK((link.cached.flags & SAFETY_FLAG_TEMP_VALID) != 0, "every OTHER flag bit (TEMP_VALID here) still passes through");
}

static void test_versions_compatible_is_two_sided(void)
{
    TEST_SECTION("safety_link_versions_compatible -- both directions of the "
                 "min_compatible check must hold, not just one");

    TEST_CHECK(safety_link_versions_compatible(5, 3, 5, 3) == true, "identical versions are compatible");
    TEST_CHECK(safety_link_versions_compatible(10, 5, 3, 1) == false,
               "peer older than OUR min_compatible is rejected (peer_protocol=3 >= self_min_compatible=5 fails)");
    TEST_CHECK(safety_link_versions_compatible(3, 1, 10, 5) == false,
               "WE are older than the PEER's min_compatible is rejected too (self_protocol=3 >= peer_min_compatible=5 "
               "fails) -- a one-sided check would have missed this");
    TEST_CHECK(safety_link_versions_compatible(5, 3, 10, 3) == true,
               "a newer peer that still accepts our min_compatible remains compatible");
}

int g_test_failures = 0;
int g_test_count = 0;

int main(void)
{
    TEST_SECTION("safety_link.c host build -- safety_apply_status() / safety_link_versions_compatible()");

    test_apply_status_accepts_v1_and_v2_lengths();
    test_apply_status_temp_valid_flag_is_sole_authority();
    test_apply_status_ignores_peer_link_up_and_fault_bits();
    test_versions_compatible_is_two_sided();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
