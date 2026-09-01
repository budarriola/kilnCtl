// ROADMAP.md M13's TASK 2: safety_link.c was ~3900 lines of the KilnFW <->
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
// Later split into six translation units (safety_link.c/_frames.c/_inbox.c/
// _poll.c/_commands.c/_payload.c -- the owner's 1500-line rule) once
// safety_link.c itself grew to 4183 lines; see safety_link_internal.h's own
// header comment for the seams and which functions' linkage changed. This
// file still proves the exact same thing it always did -- "the driver
// compiles and links off-target, and these two decode functions are pinned"
// -- it just now #includes all six real .c files (in dependency order)
// instead of one, since the functions under test now live in different
// translation units that link together into this same executable.
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
// A small canned queue, controllable per-test, rather than the previous
// always-ESP_ERR_TIMEOUT stub -- needed below to drive safety_drain_inbox_
// ex()'s receive loop directly (its ONE registered consumer of link->inbox,
// per that function's own comment) so the ROLLBACK_RESULT (0x25) stash
// branch can be exercised without needing a real uart_protocol_t/queue.
// Every OTHER test in this file still gets ESP_ERR_TIMEOUT (an empty queue),
// unchanged from before -- see fake_inbox_reset(), called at the top of
// every test that touches this.
#define FAKE_INBOX_CAP 4
static uart_proto_message_t s_fake_inbox[FAKE_INBOX_CAP];
static int s_fake_inbox_count = 0;
static int s_fake_inbox_pos = 0;

static void fake_inbox_reset(void)
{
    s_fake_inbox_count = 0;
    s_fake_inbox_pos = 0;
}

static void fake_inbox_push(const uart_proto_message_t *msg)
{
    assert(s_fake_inbox_count < FAKE_INBOX_CAP && "grow FAKE_INBOX_CAP");
    s_fake_inbox[s_fake_inbox_count++] = *msg;
}

esp_err_t uart_protocol_receive(QueueHandle_t inbox, uart_proto_message_t *out_msg, TickType_t wait_ticks)
{
    (void)inbox;
    (void)wait_ticks;
    if (s_fake_inbox_pos < s_fake_inbox_count) {
        *out_msg = s_fake_inbox[s_fake_inbox_pos++];
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}
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

// Each of the five files below is its own translation unit in the real
// firmware build, and each independently declares `static const char *TAG
// = "safety_link";` at file scope -- completely unremarkable there (five
// separate TUs, five separate file-scope statics, same convention every
// ESP-IDF source file in this codebase follows). Textually #include-ing
// all of them into this ONE test executable (same "compile the real .c"
// precedent as safety_link.c above) would redefine `TAG` five times in a
// row, which is a hard compile error only because of THIS FILE's unity-
// build strategy -- not a real defect in any of the five. Rather than touch
// five production files to work around a test-only artifact, rename TAG to
// a fresh identifier around each subsequent #include: every ESP_LOGx(TAG,
// ...) call inside the included file still macro-expands to *some* valid,
// distinct `static const char *`, so nothing about those files' own logic
// changes, only which token this test's single merged TU sees for each.
#define TAG TAG_frames
#include "../drivers/safety_link_frames.c"
#undef TAG
#define TAG TAG_inbox
#include "../drivers/safety_link_inbox.c"
#undef TAG
#define TAG TAG_poll
#include "../drivers/safety_link_poll.c"
#undef TAG
#define TAG TAG_commands
#include "../drivers/safety_link_commands.c"
#undef TAG
#include "../drivers/safety_link_payload.c" // no TAG of its own -- see that file's header comment

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

// ---------------------------------------------------------------------
// safety_apply_fw_version() / safety_link_get_peer_build_status() --
// TODO.md 9.6's "show the safety processor's build commit/date on the OTA
// page" item. safety_link.h's own doc comment on peer_build_commit_len/
// peer_build_datetime_len is explicit that the wire strings are NOT
// null-terminated (kilnlink_announce.h's convention) -- these tests exist
// because ota_page.html now renders exactly these bytes, and a reader that
// assumed a C string here would read past a 64-byte buffer with no
// terminator in it at all.

// SAFETY_CMD_FW_VERSION payload layout (LINK_PROTOCOL.md sec 4 Frame C,
// safety_parse_fw_version() above): byte0=cmd, [1:2]=protocol LE,
// [3:4]=min_compatible LE, byte5=dirty, byte6=commit_len, commit bytes,
// datetime_len byte, datetime bytes, boot_id, config_version,
// [config_crc LE]. Returns the total length written.
static uint8_t set_fw_version_frame(uint8_t *p, bool dirty, const uint8_t *commit, uint8_t commit_len,
                                     const uint8_t *datetime, uint8_t datetime_len,
                                     uint8_t boot_id, uint8_t config_version, uint16_t config_crc)
{
    size_t i = 0;
    p[i++] = SAFETY_CMD_FW_VERSION;
    p[i++] = (uint8_t)(KILNLINK_PROTOCOL_VERSION & 0xFFu);
    p[i++] = (uint8_t)((KILNLINK_PROTOCOL_VERSION >> 8) & 0xFFu);
    p[i++] = (uint8_t)(KILNLINK_MIN_COMPATIBLE & 0xFFu);
    p[i++] = (uint8_t)((KILNLINK_MIN_COMPATIBLE >> 8) & 0xFFu);
    p[i++] = dirty ? 1u : 0u;
    p[i++] = commit_len;
    memcpy(&p[i], commit, commit_len);
    i += commit_len;
    p[i++] = datetime_len;
    memcpy(&p[i], datetime, datetime_len);
    i += datetime_len;
    p[i++] = boot_id;
    p[i++] = config_version;
    p[i++] = (uint8_t)(config_crc & 0xFFu);
    p[i++] = (uint8_t)((config_crc >> 8) & 0xFFu);
    return (uint8_t)i;
}

static void test_fw_version_unknown_before_any_frame_arrives(void)
{
    TEST_SECTION("safety_link_get_peer_build_status -- known is false until a FW_VERSION "
                 "frame has actually parsed far enough to report a build; the caller must "
                 "not trust commit/dirty/datetime before then (safety_link.h's own gating rule)");

    SafetyLinkClass link = make_link();
    link.initialized = true; // safety_link_get_peer_build_status() refuses ESP_ERR_INVALID_STATE otherwise
    bool known = true, dirty = true;
    uint8_t commit[64], commit_len = 0xFF, datetime[32], datetime_len = 0xFF;
    uint8_t config_version = 0xFF;
    uint16_t config_crc = 0xFFFF;

    TEST_CHECK(safety_link_get_peer_build_status(&link, &known, &dirty, commit, &commit_len,
                                                  datetime, &datetime_len, &config_version,
                                                  &config_crc) == ESP_OK,
               "accessor succeeds even with nothing received yet");
    TEST_CHECK(known == false, "a link with no FW_VERSION frame yet reports known=false");
}

static void test_fw_version_known_and_dirty_roundtrips(void)
{
    TEST_SECTION("safety_apply_fw_version -- a well-formed frame with dirty=1 and a short "
                 "commit/datetime round-trips through safety_link_get_peer_build_status "
                 "exactly, including the dirty flag ota_page.html marks with a '-dirty' suffix");

    SafetyLinkClass link = make_link();
    link.initialized = true; // safety_link_get_peer_build_status() refuses ESP_ERR_INVALID_STATE otherwise
    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));

    const uint8_t commit_in[8] = "abcd123"; // 7 chars + NUL, but only commit_len (7) is meaningful
    const uint8_t datetime_in[5] = "12:00";
    msg.length = set_fw_version_frame(msg.payload, true, commit_in, 7, datetime_in, 5, 42, 3, 0xBEEF);

    safety_apply_fw_version(&link, &msg);

    bool known = false, dirty = false;
    uint8_t commit[64], commit_len = 0, datetime[32], datetime_len = 0;
    uint8_t config_version = 0;
    uint16_t config_crc = 0;
    TEST_CHECK(safety_link_get_peer_build_status(&link, &known, &dirty, commit, &commit_len,
                                                  datetime, &datetime_len, &config_version,
                                                  &config_crc) == ESP_OK, "accessor succeeds");
    TEST_CHECK(known == true, "known becomes true once a FW_VERSION frame parses far enough");
    TEST_CHECK(dirty == true, "the dirty bit set on the wire survives to the accessor");
    TEST_CHECK(commit_len == 7, "commit_len matches exactly what was sent, not a strlen guess");
    TEST_CHECK(memcmp(commit, commit_in, 7) == 0, "commit bytes match exactly");
    TEST_CHECK(datetime_len == 5, "datetime_len matches exactly what was sent");
    TEST_CHECK(memcmp(datetime, datetime_in, 5) == 0, "datetime bytes match exactly");
    TEST_CHECK(config_version == 3, "config_version passes through");
    TEST_CHECK(config_crc == 0xBEEF, "config_crc passes through");
}

static void test_fw_version_max_length_commit_and_datetime_no_truncation(void)
{
    TEST_SECTION("safety_apply_fw_version -- a MAXIMUM-length commit (64B) and datetime (32B), "
                 "neither NUL-terminated anywhere in the buffer, decode without truncation "
                 "or reading/writing past either destination");

    SafetyLinkClass link = make_link();
    link.initialized = true; // safety_link_get_peer_build_status() refuses ESP_ERR_INVALID_STATE otherwise
    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));

    // Deliberately no zero byte anywhere in either buffer -- a reader that
    // treated this as a C string would run off the end looking for a NUL
    // that is never there (safety_link.h's documented wire convention).
    uint8_t commit_in[64];
    for (int i = 0; i < 64; i++) commit_in[i] = (uint8_t)('A' + (i % 26));
    uint8_t datetime_in[32];
    for (int i = 0; i < 32; i++) datetime_in[i] = (uint8_t)('a' + (i % 26));

    msg.length = set_fw_version_frame(msg.payload, false, commit_in, 64, datetime_in, 32, 1, 1, 0x1234);

    safety_apply_fw_version(&link, &msg);

    bool known = false, dirty = true;
    uint8_t commit[64], commit_len = 0, datetime[32], datetime_len = 0;
    uint8_t config_version = 0;
    uint16_t config_crc = 0;
    TEST_CHECK(safety_link_get_peer_build_status(&link, &known, &dirty, commit, &commit_len,
                                                  datetime, &datetime_len, &config_version,
                                                  &config_crc) == ESP_OK, "accessor succeeds");
    TEST_CHECK(known == true, "a max-length frame still reports known=true");
    TEST_CHECK(commit_len == 64, "a full 64-byte commit is NOT truncated");
    TEST_CHECK(memcmp(commit, commit_in, 64) == 0, "all 64 commit bytes match exactly, no overrun into the datetime field");
    TEST_CHECK(datetime_len == 32, "a full 32-byte datetime is NOT truncated");
    TEST_CHECK(memcmp(datetime, datetime_in, 32) == 0, "all 32 datetime bytes match exactly, no overrun past the destination");
}

static void test_fw_version_oversized_commit_len_is_capped(void)
{
    TEST_SECTION("safety_apply_fw_version -- a wire commit_len GREATER than the 64-byte "
                 "destination is capped, not copied whole (2026-08-27 stack-overflow fix's "
                 "own regression case, re-proven here through the accessor this OTA-page "
                 "feature now depends on)");

    SafetyLinkClass link = make_link();
    link.initialized = true; // safety_link_get_peer_build_status() refuses ESP_ERR_INVALID_STATE otherwise
    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));

    uint8_t commit_in[100];
    for (int i = 0; i < 100; i++) commit_in[i] = (uint8_t)('X');
    const uint8_t datetime_in[4] = "2026";
    // commit_len byte says 100 (> KILNLINK_ANNOUNCE_MAX_COMMIT_LEN=64); the frame still
    // carries all 100 wire bytes so the offset math for datetime/boot_id/config stays
    // aligned -- see safety_parse_fw_version()'s comment on why the copy is capped but
    // the PARSE OFFSET still advances by the full wire length.
    msg.length = set_fw_version_frame(msg.payload, false, commit_in, 100, datetime_in, 4, 9, 2, 0x0042);

    safety_apply_fw_version(&link, &msg);

    bool known = false, dirty = false;
    uint8_t commit[64], commit_len = 0, datetime[32], datetime_len = 0;
    uint8_t config_version = 0;
    uint16_t config_crc = 0;
    TEST_CHECK(safety_link_get_peer_build_status(&link, &known, &dirty, commit, &commit_len,
                                                  datetime, &datetime_len, &config_version,
                                                  &config_crc) == ESP_OK, "accessor succeeds");
    TEST_CHECK(known == true, "an oversized-but-well-formed frame still reports known=true");
    TEST_CHECK(commit_len == 64, "an oversized commit_len (100) is capped to the 64-byte destination");
    TEST_CHECK(datetime_len == 4, "the datetime field decodes correctly despite the oversized commit -- "
               "the parse offset advanced by the FULL wire commit_len (100), not the capped copy (64)");
    TEST_CHECK(memcmp(datetime, datetime_in, 4) == 0, "datetime bytes are the real ones, not read from "
               "the middle of the over-long commit string");
    TEST_CHECK(config_version == 2, "fields after the oversized commit still decode correctly");
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

// --------------------------------------------------------------------------
// SAFETY_CMD_ROLLBACK_RESULT (0x25) stash -- opus review, "a late 0x25 must
// be surfaced, not dropped." Before this fix, safety_drain_inbox_ex()'s
// KILNLINK_ROLLBACK_RESULT_CMD case only ever captured the frame into an
// out-param; a frame arriving on any OTHER drain through this shared inbox
// (the ordinary GET_STATUS poll, or a later repeat of safety_link_send_
// rollback_ex()'s own send-burst) had no out-param wired up and was
// silently discarded, exactly the CONFIG_PAGE/COMMIT_CONFIG_REJECTED bug
// this file's own header comment already documents for those two frames.
// --------------------------------------------------------------------------

static uint8_t make_rollback_result_frame(uint8_t *payload, uint8_t accepted, uint8_t reason)
{
    kilnlink_rollback_result_t rr = { .accepted = accepted, .reason = reason };
    kilnlink_rollback_result_status_t enc_status = KILNLINK_ROLLBACK_RESULT_OK;
    size_t enc_len = kilnlink_rollback_result_encode(&rr, payload, KILNLINK_ROLLBACK_RESULT_LEN, &enc_status);
    assert(enc_len == KILNLINK_ROLLBACK_RESULT_LEN && "test setup: encode must succeed");
    return (uint8_t)enc_len;
}

static void test_rollback_result_late_frame_is_stashed_not_dropped(void)
{
    TEST_SECTION("safety_drain_inbox_ex -- a ROLLBACK_RESULT (0x25) frame arriving while nobody is "
                 "waiting for it is stashed, not silently dropped");

    SafetyLinkClass link = make_link();
    link.initialized = true;

    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.length = make_rollback_result_frame(msg.payload, /*accepted=*/0,
                                             KILNLINK_ROLLBACK_RESULT_REASON_SLOT_INVALID);

    fake_inbox_reset();
    fake_inbox_push(&msg);

    // Nobody is waiting for a rollback result -- both out-params NULL, the
    // exact shape safety_link_send_rollback_ex()'s inter-repeat drains and
    // the ordinary GET_STATUS poll both use.
    (void)safety_drain_inbox_ex(&link, 0, false, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

    TEST_CHECK(link.has_stashed_rollback_result == true,
               "the frame is captured into the stash instead of being thrown away");
    TEST_CHECK(link.stashed_rollback_result.length == KILNLINK_ROLLBACK_RESULT_LEN,
               "the stashed copy is the frame that actually arrived");

    // Prove it is SURFACED, not just stored: safety_take_stashed_rollback_
    // result() -- the accessor safety_link_send_rollback_ex()'s boot_id
    // watch consults on every poll -- returns it, and decoding it produces
    // the exact refusal the peer sent.
    uart_proto_message_t taken;
    TEST_CHECK(safety_take_stashed_rollback_result(&link, &taken) == true, "the stash is consumable");
    TEST_CHECK(link.has_stashed_rollback_result == false,
               "taking it clears the stash -- a second take must not hand out the same frame twice");
    kilnlink_rollback_result_t decoded;
    TEST_CHECK(kilnlink_rollback_result_decode(taken.payload, taken.length, &decoded) == KILNLINK_ROLLBACK_RESULT_OK,
               "the taken frame decodes cleanly");
    TEST_CHECK(decoded.accepted == 0 && decoded.reason == KILNLINK_ROLLBACK_RESULT_REASON_SLOT_INVALID,
               "the surfaced refusal carries the exact reason the peer sent");
    TEST_CHECK(safety_link_rollback_infer_outcome(/*refusal_received=*/true, /*refusal_decoded_ok=*/true,
                                                   /*boot_id_changed=*/false) == SAFETY_LINK_ROLLBACK_OUTCOME_REFUSED,
               "feeding that surfaced evidence through the same inference the boot_id watch uses "
               "produces REFUSED -- end-to-end proof the late frame changes the reported outcome, "
               "not just that a struct field got set");
}

static void test_rollback_result_frame_is_not_stashed_when_someone_is_waiting(void)
{
    TEST_SECTION("safety_drain_inbox_ex -- a ROLLBACK_RESULT frame IS captured directly (not stashed) "
                 "when a caller is actively waiting for one -- the stash exists only for the unclaimed "
                 "case, it must not also swallow the frame a live waiter should see immediately");

    SafetyLinkClass link = make_link();
    link.initialized = true;

    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.length = make_rollback_result_frame(msg.payload, /*accepted=*/0, KILNLINK_ROLLBACK_RESULT_REASON_ARMED);

    fake_inbox_reset();
    fake_inbox_push(&msg);

    uart_proto_message_t result_msg;
    bool got_result = false;
    (void)safety_drain_inbox_ex(&link, 0, false, NULL, NULL, NULL, NULL, NULL, NULL, &result_msg, &got_result);

    TEST_CHECK(got_result == true, "an active waiter receives the frame through its own out-param");
    TEST_CHECK(link.has_stashed_rollback_result == false,
               "a frame handed directly to a waiting caller must NOT also sit in the stash -- "
               "otherwise a second, unrelated caller could later consume the SAME frame a second time");
}

// --------------------------------------------------------------------------
// safety_reset_stale_peer_info_if_link_down() -- opus-review finding 1,
// "the boot_id evidence channel is single-shot and unrecoverable". Before
// this fix, peer_version_known/pico_boot_id_known/peer_build_known were set
// once by safety_apply_fw_version() and NEVER cleared, even once the link
// was later observed down -- so a lost boot-time FW_VERSION frame (or a
// Pico that rebooted while this side still held pre-drop state) left this
// side believing it already knew the peer's identity forever, and it never
// re-requested. The extracted function under test is exactly what the poll
// loop now calls every iteration; this file's own header comment lists the
// poll loop's timing as explicitly out of scope, but the pure decision this
// function makes does not need that loop running to be exercised.
//
// The stub xTaskGetTickCount() (App/test/stubs/freertos/task.h) always
// returns 0, so safety_elapsed_ms(since) computes as (0 - since) with
// unsigned wraparound: since=0 reads as elapsed=0 ("just received", link
// up); any nonzero `since` wraps to a huge elapsed value, comfortably past
// SAFETY_LINK_UP_PERIODS * the poll period, i.e. "link down". That is used
// deliberately below to drive link.cached_tick between the two states.
// --------------------------------------------------------------------------

static void test_stale_reset_leaves_flags_alone_while_link_is_up(void)
{
    TEST_SECTION("safety_reset_stale_peer_info_if_link_down -- while the link reads UP, "
                 "peer_version_known/pico_boot_id_known/peer_build_known are left untouched");

    SafetyLinkClass link = make_link();
    link.ever_received = true;
    link.cached_tick = 0; // see header comment above -- reads as age 0, link up
    link.peer_version_known = true;
    link.pico_boot_id_known = true;
    link.peer_build_known = true;

    safety_reset_stale_peer_info_if_link_down(&link);

    TEST_CHECK(link.peer_version_known == true, "peer_version_known untouched while link is up");
    TEST_CHECK(link.pico_boot_id_known == true, "pico_boot_id_known untouched while link is up");
    TEST_CHECK(link.peer_build_known == true, "peer_build_known untouched while link is up");
}

static void test_stale_reset_clears_flags_once_link_is_observed_down(void)
{
    TEST_SECTION("safety_reset_stale_peer_info_if_link_down -- the assertion that can fail: "
                 "once the link is observed down, all three 'known' latches from a PRIOR "
                 "connection are cleared, so the poll loop's own !peer_version_known branch "
                 "re-requests FW_VERSION instead of trusting stale pre-drop state forever");

    SafetyLinkClass link = make_link();
    link.ever_received = true;
    link.cached_tick = 1; // see header comment above -- wraps to a huge elapsed, link down
    link.peer_version_known = true;
    link.pico_boot_id_known = true;
    link.peer_build_known = true;
    link.pico_boot_id = 42; // a stale boot_id from before the link dropped

    safety_reset_stale_peer_info_if_link_down(&link);

    TEST_CHECK(link.peer_version_known == false,
               "peer_version_known cleared -- this is what makes the poll loop re-request FW_VERSION");
    TEST_CHECK(link.pico_boot_id_known == false,
               "pico_boot_id_known cleared -- a rollback's boot_id-watch baseline can no longer be "
               "compared against a boot_id from a connection that already ended");
    TEST_CHECK(link.peer_build_known == false,
               "peer_build_known cleared -- same reasoning for finding 2's build-identity evidence");
    // The cached boot_id VALUE itself is deliberately left alone (only the
    // "known" flag is cleared) -- there is no reason to zero it, and doing
    // so would be indistinguishable from a real boot_id of 0.
    TEST_CHECK(link.pico_boot_id == 42, "the stale cached boot_id value itself is untouched, only its "
                                         "'known' flag is -- nothing reads it while known is false");
}

static void test_stale_reset_never_received_is_also_down(void)
{
    TEST_SECTION("safety_reset_stale_peer_info_if_link_down -- a link that has NEVER received "
                 "anything (fresh boot, no GET_STATUS reply yet) is down too, and clears the "
                 "same three flags if they were somehow already set");

    SafetyLinkClass link = make_link();
    link.ever_received = false;
    link.peer_version_known = true;
    link.pico_boot_id_known = true;
    link.peer_build_known = true;

    safety_reset_stale_peer_info_if_link_down(&link);

    TEST_CHECK(link.peer_version_known == false, "never-received link -- cleared");
    TEST_CHECK(link.pico_boot_id_known == false, "never-received link -- cleared");
    TEST_CHECK(link.peer_build_known == false, "never-received link -- cleared");
}

// The full story finding 1 asks to be proven end-to-end: a lost boot
// FW_VERSION still yields a correct outcome after reconnect. Simulated here
// as three steps against the real functions under test (not the poll loop's
// timing, which this file's own header comment already says is out of
// scope): (1) a prior connection leaves the three flags known=true and a
// cached boot_id, (2) the link is observed down (the boot push was lost,
// or the Pico rebooted silently) and the reset function clears them, (3) a
// FRESH FW_VERSION frame arrives with a DIFFERENT boot_id, applied through
// the real safety_apply_fw_version() -- proving the flags are not just
// clearable but actually get re-populated with the NEW peer's identity,
// which is what lets a subsequent rollback attempt compare against the
// right baseline instead of a connection that already ended.
static void test_stale_reset_then_reapply_recovers_after_reconnect(void)
{
    TEST_SECTION("safety_reset_stale_peer_info_if_link_down + safety_apply_fw_version -- a lost "
                 "boot FW_VERSION still yields a correct outcome after reconnect: the stale flags "
                 "are cleared, then a fresh frame repopulates them with the NEW peer's identity");

    SafetyLinkClass link = make_link();
    link.ever_received = true;
    link.cached_tick = 0;
    link.peer_version_known = true;
    link.pico_boot_id_known = true;
    link.pico_boot_id = 42; // stale, from the connection that just ended
    link.peer_build_known = true;

    // Step 1: the link is observed down (the sole boot-time FW_VERSION
    // burst was entirely lost, or arrived and this side dropped anyway).
    link.cached_tick = 1; // wraps to "down", see the section header comment above
    safety_reset_stale_peer_info_if_link_down(&link);
    TEST_CHECK(link.peer_version_known == false && link.pico_boot_id_known == false &&
                   link.peer_build_known == false,
               "step 1: all three cleared once the link is seen down");

    // Step 2: the link comes back and a NEW FW_VERSION frame (a different
    // boot_id -- e.g. the Pico actually did reboot) finally arrives and is
    // applied through the real decode path.
    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));
    uint8_t commit[3] = {1, 2, 3};
    uint8_t datetime[2] = {4, 5};
    msg.length = set_fw_version_frame(msg.payload, /*dirty=*/false, commit, 3, datetime, 2,
                                       /*boot_id=*/99, /*config_version=*/1, /*config_crc=*/0);
    safety_apply_fw_version(&link, &msg);

    TEST_CHECK(link.peer_version_known == true, "step 2: peer_version_known repopulated by the fresh frame");
    TEST_CHECK(link.pico_boot_id_known == true, "step 2: pico_boot_id_known repopulated");
    TEST_CHECK(link.pico_boot_id == 99, "step 2: the NEW boot_id (99), not the stale 42, is now cached -- "
                                         "a rollback attempt from here compares against the right baseline");
    TEST_CHECK(link.peer_build_known == true, "step 2: peer_build_known repopulated too");
}

int g_test_failures = 0;
int g_test_count = 0;

int main(void)
{
    TEST_SECTION("safety_link.c host build -- safety_apply_status() / safety_link_versions_compatible()");

    test_apply_status_accepts_v1_and_v2_lengths();
    test_apply_status_temp_valid_flag_is_sole_authority();
    test_apply_status_ignores_peer_link_up_and_fault_bits();
    test_fw_version_unknown_before_any_frame_arrives();
    test_fw_version_known_and_dirty_roundtrips();
    test_fw_version_max_length_commit_and_datetime_no_truncation();
    test_fw_version_oversized_commit_len_is_capped();
    test_versions_compatible_is_two_sided();
    test_rollback_result_late_frame_is_stashed_not_dropped();
    test_rollback_result_frame_is_not_stashed_when_someone_is_waiting();
    test_stale_reset_leaves_flags_alone_while_link_is_up();
    test_stale_reset_clears_flags_once_link_is_observed_down();
    test_stale_reset_never_received_is_also_down();
    test_stale_reset_then_reapply_recovers_after_reconnect();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
