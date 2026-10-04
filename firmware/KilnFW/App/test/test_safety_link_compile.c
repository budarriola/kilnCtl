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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"
#include "fake_gpio.h" /* fault-hold tests read the physical fault pin level */
#include "fake_time.h" /* hal_time.h's host fake -- safety_link_inbox.c's safety_exchange() now
                        * calls hal_time_now_us() for link_reply_us (HW_ABSTRACTION.md "Still
                        * open") -- see fake_time.c already linked into this executable's build
                        * command (build_host_tests.ps1) for the safety_cfg_store hal_time
                        * migration, same fake now doubles for this. */

#include "MAX31856.h"
#include "kiln_io.h"
#include "profile_executor.h"
#include "thermo_owner.h"
#include "zones_config_accessors.h"
#include "safety_cfg_store.h"

MAX31856Class *MAX31856_bus_channel(MAX31856BusClass *bus, uint8_t channel)
{ (void)bus; (void)channel; return NULL; }
esp_err_t MAX31856_get_config(MAX31856Class *ch, MAX31856Config *out_cfg)
{ (void)ch; (void)out_cfg; return ESP_FAIL; }
esp_err_t MAX31856_read_all(MAX31856BusClass *bus, MAX31856Reading *out, size_t max_readings, size_t *out_count)
{ (void)bus; (void)out; (void)max_readings; (void)out_count; return ESP_FAIL; }
uint8_t kiln_io_get_relay_shadow(const kiln_io_t *io) { (void)io; return 0; }
uint8_t aux_outputs_cfg_enabled_mask(void) { return 0; } /* WP-9: safety_pico_relay_mask() */
void profile_executor_get_status(profile_exec_status_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
}
bool safety_cfg_store_maybe_refetch(SafetyLinkClass *link, uint16_t live_config_crc)
{ (void)link; (void)live_config_crc; return false; }
bool safety_cfg_store_refetch(SafetyLinkClass *link, uint16_t config_crc)
{ (void)link; (void)config_crc; return false; }
esp_err_t thermo_owner_command_read_all(MAX31856Reading *out, size_t max_readings, size_t *out_count)
{ (void)out; (void)max_readings; (void)out_count; return ESP_FAIL; }
bool zones_config_get_safety_tc_type(uint8_t *out_tc_type) { (void)out_tc_type; return false; }
bool zones_config_is_valid(void) { return true; }

/* 2026-09-10 opus review: safety_link_poll.c (#included below) now calls
 * safety_ceiling_sync_reconcile_on_link_up() on every tick the link is up,
 * to close the "link-down bypass" gap in the Pico-ceiling invariant (see
 * safety_ceiling_sync.h's own comment). The real implementation
 * (safety_ceiling_sync.c) pulls in zones_config_accessors.c's real
 * getters and safety_cfg_http.c's UART exchange machinery -- outside what
 * this executable links (it exists to compile-check safety_link.c's own
 * logic, same "everything else is faked" convention as every other stub on
 * this page). Faked as a no-op; the zones_config_get_safety_tc_type()/
 * zones_config_is_valid() stubs just above now serve only backup_export.c's
 * read-back use of the field (the removed safety_sync_tc_type()'s own use
 * of them is gone) -- this file is not the place ceiling-sync behaviour is
 * tested (that is test_safety_ceiling_policy.c at the pure-logic layer and
 * test_zones_http.c's test_reconcile_on_link_up_*() at the ESP-glue layer). */
void safety_ceiling_sync_reconcile_on_link_up(SafetyLinkClass *link) { (void)link; }
/* 2026-09-22: safety_link_poll.c's tick call site moved to the non-blocking
 * entry point (advisory adopted from the opus review that also fixed the
 * s_reconcile_lock TOCTOU race) -- faked here the same no-op way as the
 * blocking form just above, for the same reason. */
void safety_ceiling_sync_reconcile_on_link_up_nonblocking(SafetyLinkClass *link) { (void)link; }

/* 2026-09-15 (item HIGH1 of review_divergence_wiring_60d6552f_2026-09-15.md):
 * safety_link_poll.c (#included below) now also owns the deferred Pico-half
 * recapture poll that used to live in ui_page_home_refresh.c. That pulls in
 * the persist/ config store and the flash-worker dispatch, neither of which
 * this compile-check executable links -- same "everything else is faked"
 * convention as the stubs above. The poll's real behaviour is not tested
 * here; this file only needs safety_poll_task() to link. */
bool safety_ceiling_sync_is_standing_diverged(char *reason_out, size_t reason_cap)
{
    if (reason_out && reason_cap > 0) { reason_out[0] = 0; }
    return false;
}
/* 2026-09-16 (HIGH 1 of the adversarial review of 60d6552f): these three
 * stubs are no longer inert. safety_poll_task is the SOLE sender of the
 * ESP->Pico GET_STATUS heartbeat the Pico's S6b LINK_DEAD guard watches, and
 * the recapture service call sits immediately before that send. If the
 * recapture blocks the poll task for longer than link_timeout_s (10.0 s
 * default) the Pico trips S6b and ruins a firing. The stubs below model a
 * SLOW flash job by advancing the fake clock from inside the dispatch --
 * simulating a stuck NVS write rather than waiting on a real one -- so the
 * test at the bottom of this file can measure how much time the poll task
 * itself loses to it. */
static bool s_stub_recapture_pending = false;
static int s_stub_autosave_calls = 0;

/* How long a pathologically slow flash job takes, in fake microseconds.
 * Deliberately longer than the 10.0 s link_timeout_s default and a
 * non-round number (fake_time.h: idealized round steps hide bugs). */
#define STUB_SLOW_FLASH_JOB_US 11987003ull

bool kiln_cfg_store_pico_half_recapture_pending(void) { return s_stub_recapture_pending; }
void *g_stub_autosave_dispatcher = (void *)-1;
bool kiln_cfg_store_autosave_from_live_for_dispatcher(void *dispatcher_task, char *reason_out,
                                                      size_t reason_cap)
{
    if (reason_out && reason_cap > 0) { reason_out[0] = 0; }
    g_stub_autosave_dispatcher = dispatcher_task;
    s_stub_autosave_calls++;
    return true;
}

/* Await-the-job dispatch. The real one ends in xSemaphoreTake(s_bx_done,
 * portMAX_DELAY) -- `timeout_ms` bounds ONLY acquiring the worker, never the
 * job itself (flash_worker.h). So the caller really does pay the whole job
 * duration, which is what advancing the clock here models. */
esp_err_t uart_bridge_ext_run_on_flash_worker_timeout(void (*fn)(void *arg), void *arg,
                                                       uint32_t timeout_ms)
{
    (void)timeout_ms;
    fake_time_advance_us(STUB_SLOW_FLASH_JOB_US);
    if (fn) { fn(arg); }
    return ESP_OK;
}

/* NON-BLOCKING post. Returns immediately; the job is held for the worker to
 * run on one of its own later loop iterations. */
static void (*s_stub_posted_fn)(void *arg) = NULL;
esp_err_t uart_bridge_ext_post_on_flash_worker(void (*fn)(void *arg))
{
    if (!fn) { return ESP_ERR_INVALID_ARG; }
    if (s_stub_posted_fn) { return ESP_ERR_INVALID_STATE; }
    s_stub_posted_fn = fn;
    return ESP_OK;
}

/* Runs whatever is posted the way bx_worker_task would -- on the WORKER's
 * time, not the poster's. The clock advance is inside here precisely to show
 * the cost landed on the worker instead of on safety_poll_task. */
static void stub_run_posted_job_as_the_worker_would(void)
{
    void (*fn)(void *arg) = s_stub_posted_fn;
    s_stub_posted_fn = NULL;
    if (fn) {
        fake_time_advance_us(STUB_SLOW_FLASH_JOB_US);
        fn(NULL);
    }
}

// heat_enable_service_pending_release() -- 2026-09-15 fix
// (docs/audits/profile_executor_coredump_2026-09-15.md): safety_link_poll.c's
// safety_poll_task() now drains a pending heat_enable release once per loop,
// same "cross-module dependency, don't drag in its own state machine"
// convention as the fakes above -- heat_enable.c's own behaviour is tested by
// test_heat_enable.c, not here; this file only needs safety_poll_task() to
// link and call something.
void heat_enable_service_pending_release(void) { /* not exercised by this file's tests */ }

// relay_cycles_note_safety_edge() -- RELAY_LIFE_BUDGET.md.
// safety_apply_status() (safety_link_frames.c) now calls this once per
// OBSERVED K4 (SAFETY_FLAG_RELAY) transition. Faked as a plain counter, same
// "cross-module dependency, don't drag in its own NVS machinery" convention
// as safety_cfg_store_refetch()'s fake just above -- relay_cycles.c's own
// persistence is tested by test_relay_cycles.c, not here; this file only
// needs to prove safety_link_frames.c calls it the right NUMBER of times.
static int s_stub_relay_cycles_safety_edge_calls = 0;
void relay_cycles_note_safety_edge(void) { s_stub_relay_cycles_safety_edge_calls++; }
/* H9 CT alarm service (safety_poll_task calls it each pass); not exercised here. */
void ct_leak_alarm_service(SafetyLinkClass *link) { (void)link; }

esp_err_t uart_owner_init(uart_owner_t *owner, uart_port_t port, int tx_io, int rx_io,
                           int baud_rate, unsigned queue_len, unsigned task_priority,
                           uint32_t stack_depth, int core_id)
{ (void)owner; (void)port; (void)tx_io; (void)rx_io; (void)baud_rate; (void)queue_len;
  (void)task_priority; (void)stack_depth; (void)core_id; return ESP_FAIL; }
esp_err_t uart_owner_deinit(uart_owner_t *owner) { (void)owner; return ESP_OK; }
uint32_t uart_owner_get_rx_error_count(const uart_owner_t *owner) { (void)owner; return 0; }

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

// Models a reply that genuinely has not arrived yet at the moment a drain
// call's internal while loop first empties the queue -- e.g. a real
// SAFETY_CMD_PARAM reply still in flight on the wire after an unrelated
// frame (STATUS/DIAG/POWER) was dequeued first and degraded that call's own
// wait to zero (safety_drain_still_waiting(), safety_link.h). When
// s_fake_inbox_auto_push_after_first_timeout is set, the FIRST time this
// stub's queue goes empty it still reports ESP_ERR_TIMEOUT for that call (it
// really wasn't there yet) but then enqueues s_fake_inbox_delayed_msg so a
// LATER call -- the retry a looped safety_drain_inbox() caller makes with
// its own remaining budget -- finds it. Used by
// test_get_param_survives_a_leading_unrelated_frame() to distinguish the
// fixed call-site loop from the old single-call behavior it replaces.
// NOT the same as "armed at test setup": this only goes true once
// uart_protocol_send_broadcast()'s leading-push branch has actually run (see
// below), so the unrelated pre-send stale-stash drain
// (safety_link_get_param()'s own `safety_drain_inbox(link, 0)` before it
// sends anything) never counts as the "first timeout" this models -- only a
// timeout observed AFTER the leading frame was queued does.
static bool s_fake_inbox_auto_push_armed = false;
static bool s_fake_inbox_auto_push_done = false;
static uart_proto_message_t s_fake_inbox_delayed_msg;

// Opt-in only: advances the fake clock on every empty uart_protocol_receive()
// poll. Left OFF by default -- the link_reply_us tests (test_link_reply_us_*
// above) time an exchange by the EXACT number of microseconds their own stub
// advances via s_stub_broadcast_advance_us, and safety_drain_inbox()'s
// internal loop calls this stub again (finding the queue now empty) right
// after consuming the one reply message pushed at send time; advancing the
// clock unconditionally on every empty poll would inflate those exact-match
// timings. Only test_get_param_no_reply_reports_timeout() (a genuine,
// permanently-empty-queue case with no exact timing to pin) needs this on,
// so it explicitly arms it, and every reset function turns it back off.
static bool s_fake_inbox_advance_time_on_empty = false;

// Spin guard: counts consecutive empty uart_protocol_receive() polls. With
// s_fake_inbox_advance_time_on_empty left at its default (off), the fake
// clock never moves, so a call-site loop that re-derives its remaining
// budget from hal_time_now_us() (safety_link_get_param()/safety_link_
// get_stack_margin()) spins forever instead of ever observing
// remaining_ms == 0 -- and build_host_tests.ps1 has no per-executable
// timeout, so that hang would silently hold a build-gate slot rather than
// failing fast. Reset whenever the queue produces a message (by index or by
// push) or on fake_inbox_reset(); once consecutive empty polls pass
// SPIN_GUARD_LIMIT, print one line naming the problem and abort() so the
// hang surfaces as a fast, loud test failure instead.
#define SPIN_GUARD_LIMIT 100000
static uint32_t s_fake_inbox_empty_poll_count = 0;

static void fake_inbox_reset(void)
{
    s_fake_inbox_count = 0;
    s_fake_inbox_pos = 0;
    s_fake_inbox_auto_push_armed = false;
    s_fake_inbox_auto_push_done = false;
    s_fake_inbox_advance_time_on_empty = false;
    s_fake_inbox_empty_poll_count = 0;
}

static void fake_inbox_push(const uart_proto_message_t *msg)
{
    assert(s_fake_inbox_count < FAKE_INBOX_CAP && "grow FAKE_INBOX_CAP");
    s_fake_inbox[s_fake_inbox_count++] = *msg;
    s_fake_inbox_empty_poll_count = 0;
}

esp_err_t uart_protocol_receive(QueueHandle_t inbox, uart_proto_message_t *out_msg, TickType_t wait_ticks)
{
    (void)inbox;
    if (s_fake_inbox_pos < s_fake_inbox_count) {
        *out_msg = s_fake_inbox[s_fake_inbox_pos++];
        s_fake_inbox_empty_poll_count = 0;
        return ESP_OK;
    }
    if (++s_fake_inbox_empty_poll_count > SPIN_GUARD_LIMIT) {
        fprintf(stderr,
                "uart_protocol_receive stub: queue empty for %u polls with fake clock frozen"
                " -- a no-reply test must set s_fake_inbox_advance_time_on_empty\n",
                (unsigned)s_fake_inbox_empty_poll_count);
        abort();
    }
    if (s_fake_inbox_auto_push_armed && !s_fake_inbox_auto_push_done) {
        s_fake_inbox_auto_push_done = true;
        fake_inbox_push(&s_fake_inbox_delayed_msg);
    }
    /* An empty receive never actually blocked on the fake clock -- without
     * advancing it here, a call-site loop that re-derives its remaining
     * budget from hal_time_now_us() (safety_link_get_param()/safety_link_
     * get_stack_margin()) would spin forever on a genuinely-no-reply case
     * instead of eventually observing remaining_ms == 0. Advance by
     * (roughly) the requested wait plus a small margin so the loop always
     * makes forward progress even when wait_ticks == 0. Opt-in only -- see
     * s_fake_inbox_advance_time_on_empty's own comment above for why this
     * must not run unconditionally. */
    if (s_fake_inbox_advance_time_on_empty) {
        fake_time_advance_us((uint64_t)wait_ticks * portTICK_PERIOD_MS * 1000u + 1000u);
    }
    return ESP_ERR_TIMEOUT;
}
esp_err_t uart_protocol_send(uart_protocol_t *proto, uart_proto_device_t dst_device,
                              uint8_t dst_task, uint8_t src_task, const uint8_t *payload,
                              size_t length, uint32_t ack_timeout_ms)
{ (void)proto; (void)dst_device; (void)dst_task; (void)src_task; (void)payload; (void)length;
  (void)ack_timeout_ms; return ESP_FAIL; }
// Every existing test in this file relies on the always-ESP_FAIL default
// (this file's own top comment: safety_exchange()'s blocking request/reply
// cycle is explicitly out of scope for those) -- so the link_reply_us tests
// below opt IN via this flag rather than changing the default for everyone
// else. Reset to false at the top of every test that touches it, same
// "explicit per-test state, no leftover from a previous test" convention as
// fake_inbox_reset()/s_stub_relay_cycles_safety_edge_calls.
static bool s_stub_broadcast_send_succeeds = false;
// link_reply_us tests drive the fake clock from INSIDE this stub, not the
// test body: safety_exchange() captures reply_start_us immediately before
// this call and reply_stop_us immediately after safety_drain_inbox_for_
// status() returns, with the whole drain (including the fake
// uart_protocol_receive() above) happening synchronously inside THIS
// function call -- there is no other point at which "N us elapsed between
// send and reply" can be injected. When s_stub_broadcast_reply_push is
// true, the stub advances the fake clock by s_stub_broadcast_advance_us and
// enqueues s_stub_broadcast_reply_msg, simulating a real reply landing
// exactly that long after the request was handed to the UART.
static bool s_stub_broadcast_reply_push = false;
static uint64_t s_stub_broadcast_advance_us = 0;
static uart_proto_message_t s_stub_broadcast_reply_msg;
// Enqueued (if set) immediately before s_stub_broadcast_reply_msg, on the
// same send call -- models an unrelated frame (a periodic STATUS/DIAG/POWER
// push) landing in the real inbox ahead of the reply this call actually
// wants. Both land "at send time" from fake_inbox's point of view, same as
// s_stub_broadcast_reply_msg alone always has -- there is no other point in
// this synchronous stub to inject a second, earlier frame. Used by
// test_get_param_survives_a_leading_unrelated_frame() to prove the drain
// loop keeps waiting past it instead of giving up after the first frame.
static bool s_stub_broadcast_leading_push = false;
static uart_proto_message_t s_stub_broadcast_leading_msg;
// Every broadcast payload's first byte (command id), length and a copy of
// the most recent one -- tests assert on what went out, not just that
// something did. Recorded before the success gate so a send that the stub
// then fails is still visible.
#define STUB_BROADCAST_LOG_MAX 8
static unsigned s_stub_broadcast_count = 0;
static uint8_t s_stub_broadcast_cmd[STUB_BROADCAST_LOG_MAX];
static uint8_t s_stub_broadcast_last_payload[64];
static size_t s_stub_broadcast_last_len = 0;
esp_err_t uart_protocol_send_broadcast(uart_protocol_t *proto, uart_proto_device_t dst_device,
                                        uint8_t dst_task, uint8_t src_task,
                                        const uint8_t *payload, size_t length)
{ (void)proto; (void)dst_device; (void)dst_task; (void)src_task;
  if (s_stub_broadcast_count < STUB_BROADCAST_LOG_MAX) {
      s_stub_broadcast_cmd[s_stub_broadcast_count] = (payload && length) ? payload[0] : 0u;
  }
  s_stub_broadcast_count++;
  s_stub_broadcast_last_len = length;
  if (payload && length <= sizeof(s_stub_broadcast_last_payload)) {
      memcpy(s_stub_broadcast_last_payload, payload, length);
  }
  if (!s_stub_broadcast_send_succeeds) {
      return ESP_FAIL;
  }
  if (s_stub_broadcast_leading_push) {
      fake_inbox_push(&s_stub_broadcast_leading_msg);
      /* Arm the delayed-reply auto-push only from here on -- the leading
       * frame is now genuinely queued, so the NEXT time the fake receive()
       * stub finds the queue empty is the drain call actually racing the
       * real reply, not safety_link_get_param()'s own pre-send stale-stash
       * clear (which runs before this send and must not count). */
      s_fake_inbox_auto_push_armed = true;
  }
  if (s_stub_broadcast_reply_push) {
      fake_time_advance_us(s_stub_broadcast_advance_us);
      fake_inbox_push(&s_stub_broadcast_reply_msg);
  }
  return ESP_OK; }

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

// stubs/freertos/task.h's xTaskGetTickCount() is hardcoded to always return 0
// (added purely so profile_executor.c/autotune_engine.c's host tests link --
// see that stub's own header comment). safety_link_frames.c's boot-clear
// retry logic (2026-09-28 fix) needs to observe elapsed time -- the 30 s
// boot-clean window and the inter-attempt retry gap -- so this file needs a
// controllable fake tick count, same "local macro shadow, #undef after this
// #include block" precedent as xSemaphoreTake above. portTICK_PERIOD_MS is
// 1u in the stub (freertos/FreeRTOS.h), so a tick IS a millisecond here.
static TickType_t s_fake_tick_count = 0;
static inline TickType_t safety_link_test_xTaskGetTickCount(void) { return s_fake_tick_count; }
#define xTaskGetTickCount safety_link_test_xTaskGetTickCount

#include "../drivers/safety/safety_link.c"

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
#include "../drivers/safety/safety_link_frames.c"
#undef TAG
#define TAG TAG_inbox
#include "../drivers/safety/safety_link_inbox.c"
#undef TAG
#define TAG TAG_poll
#include "../drivers/safety/safety_link_poll.c"
#undef TAG
#define TAG TAG_commands
#include "../drivers/safety/safety_link_commands.c"
#undef TAG
#include "../drivers/safety/safety_link_payload.c" // no TAG of its own -- see that file's header comment

#undef xSemaphoreTake
#undef xTaskGetTickCount

// relay_authority.c -- the REAL, compiled chokepoint (not a stub -- no other
// host test in this suite links the real relay_authority_on_blocked(),
// every other caller of it fakes its own body instead, see e.g.
// test_autotune_engine_prestart.c). Included here, alongside the real,
// already-linked safety_link.c above, specifically for ROADMAP.md's
// 2026-09-04 link-loss-heating-block-during-a-Pico-update item: the property
// under test is that safety_link_set_update_in_progress() (which ota_pico_
// relay.c's relay task calls around a Pico update) does NOT relax what
// relay_authority_on_blocked() decides -- and that claim is worthless if
// on_blocked() is a hand-written stand-in rather than the function that
// actually ships. Needs freertos/portmacro.h's portMUX_TYPE/portENTER_
// CRITICAL stand-ins (App/test/stubs/freertos/portmacro.h, added alongside
// this) for its heat-claim spinlock -- host tests are single-threaded, so
// those expand to nothing.
#include "../drivers/owners/relay_authority.c"

// ---------------------------------------------------------------------

static void set_status_frame(uint8_t *p, uint8_t flags, float tc_c, float cj_c,
                              uint8_t tc_fault, float ia, float ib, float ic)
{
    memset(p, 0, SAFETY_LINK_STATUS_FRAME_LEN_V3);
    p[0] = SAFETY_CMD_GET_STATUS;
    p[1] = flags;
    memcpy(&p[2], &tc_c, sizeof(float));
    memcpy(&p[6], &cj_c, sizeof(float));
    p[10] = tc_fault;
    memcpy(&p[11], &ia, sizeof(float));
    memcpy(&p[15], &ib, sizeof(float));
    memcpy(&p[19], &ic, sizeof(float));
}

// Builds a SAFETY_CMD_DIAG (Frame B) payload -- byte layout per
// safety_link_frames.c's safety_apply_diag() header comment. Only the fields
// the boot-clear retry tests care about are parameterized; the rest are
// zeroed (harmless -- safety_apply_diag() doesn't gate on them).
static void set_diag_frame(uint8_t *p, uint8_t trip_reason, uint8_t state)
{
    memset(p, 0, SAFETY_LINK_DIAG_FRAME_LEN);
    p[0] = SAFETY_CMD_DIAG;
    p[1] = trip_reason;
    p[24] = state;
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
    TEST_CHECK(safety_apply_status(&link, &msg) == false, "a length that is neither V1, V2, nor V3 is rejected");
    TEST_CHECK(link.stats.frame_errors == 1, "the rejection is counted as a frame error");
}

// 2026-09-06, CURRENT_SENSE.md sec 4's "Tooling gap": SAFETY_CMD_POWER
// (Frame E) grew a 61-byte V2 layout carrying counts_avg[3] (raw ADC counts,
// independent of calibration) -- same "accept both fixed lengths, gate the
// new field on an explicit flag rather than length alone" shape as Frame A's
// own V1/V2 test just above. Fills the payload by hand (not via kilnlink_
// power_encode(), which this test file has no link to -- CommonFW's codec
// and KilnFW's hand-parser are deliberately two independent implementations
// of the same wire contract, per safety_link.h's own "mirrored here" notes)
// so this test genuinely exercises safety_apply_power()'s own byte offsets
// rather than round-tripping through the same code twice.
static void set_power_frame_v2(uint8_t *p, uint8_t flags, uint16_t c0, uint16_t c1, uint16_t c2)
{
    memset(p, 0, SAFETY_LINK_POWER_FRAME_LEN_V2);
    p[0] = SAFETY_CMD_POWER;
    p[1] = 120; // power_window_s
    p[2] = flags;
    float mains_v = 240.0f;
    memcpy(&p[3], &mains_v, sizeof(float));
    // bytes 7..42 (i_conducting_a/conduction_fraction/p_avg_w per channel)
    // deliberately left zeroed -- not under test here.
    float p_total = 100.0f;
    memcpy(&p[43], &p_total, sizeof(float));
    double energy = 1.0;
    memcpy(&p[47], &energy, sizeof(double));
    memcpy(&p[55], &c0, sizeof(uint16_t));
    memcpy(&p[57], &c1, sizeof(uint16_t));
    memcpy(&p[59], &c2, sizeof(uint16_t));
}

static void test_apply_power_accepts_v1_and_v2_lengths_and_gates_counts_on_flag(void)
{
    TEST_SECTION("safety_apply_power -- V1 (55B, legacy) and V2 (61B, current) frames BOTH "
                 "decode; counts_avg is gated on COUNTS_VALID (bit3), never on length alone "
                 "(CommonFW/docs/LINK_PROTOCOL.md Frame E, 2026-09-06)");

    SafetyLinkClass link = make_link();
    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));

    // V1 (55-byte) frame from a legacy Pico: no bytes 55..60 exist at all.
    // power_calibrated bit set, COUNTS_VALID bit clear (a real legacy peer
    // never sets a bit for a field it has never heard of).
    set_power_frame_v2(msg.payload, SAFETY_LINK_POWER_FLAG_CALIBRATED, 0, 0, 0);
    msg.length = SAFETY_LINK_POWER_FRAME_LEN_V1;
    TEST_CHECK(safety_apply_power(&link, &msg) == true, "V1 (55-byte) frame is accepted");
    TEST_CHECK(link.cached.power_counts_valid == false, "V1 frame leaves power_counts_valid false");
    TEST_CHECK(link.cached.power_channel_counts_avg[0] == 0 &&
               link.cached.power_channel_counts_avg[1] == 0 &&
               link.cached.power_channel_counts_avg[2] == 0,
               "V1 frame leaves counts_avg zeroed (nothing on the wire to read)");

    // V2 (61-byte) frame, COUNTS_VALID set, real nonzero counts.
    set_power_frame_v2(msg.payload,
                        (uint8_t)(SAFETY_LINK_POWER_FLAG_CALIBRATED | SAFETY_LINK_POWER_FLAG_COUNTS_VALID),
                        10, 2000, 4095);
    msg.length = SAFETY_LINK_POWER_FRAME_LEN_V2;
    TEST_CHECK(safety_apply_power(&link, &msg) == true, "V2 (61-byte) frame is accepted");
    TEST_CHECK(link.cached.power_counts_valid == true, "V2 frame with the flag set sets power_counts_valid true");
    TEST_CHECK(link.cached.power_channel_counts_avg[0] == 10 &&
               link.cached.power_channel_counts_avg[1] == 2000 &&
               link.cached.power_channel_counts_avg[2] == 4095,
               "V2 frame's counts_avg bytes decode correctly, per channel");

    // NEGATIVE case proving the flag governs, not length: a 61-byte frame
    // with real-looking nonzero bytes at the counts offset but the flag
    // CLEAR must still read back as invalid/zeroed -- exactly the untrusted-
    // wire-input discipline kilnlink_power_decode()'s own equivalent test
    // asserts (test_v2_length_but_flag_not_set_is_treated_as_invalid,
    // CommonFW/test/test_power.c).
    set_power_frame_v2(msg.payload, SAFETY_LINK_POWER_FLAG_CALIBRATED, 111, 222, 333);
    msg.length = SAFETY_LINK_POWER_FRAME_LEN_V2;
    TEST_CHECK(safety_apply_power(&link, &msg) == true, "V2-length frame with COUNTS_VALID clear still decodes OK");
    TEST_CHECK(link.cached.power_counts_valid == false,
               "power_counts_valid stays false when the flag is clear, even at V2 length");
    TEST_CHECK(link.cached.power_channel_counts_avg[0] == 0 &&
               link.cached.power_channel_counts_avg[1] == 0 &&
               link.cached.power_channel_counts_avg[2] == 0,
               "counts_avg is zeroed, not the raw wire bytes, when the flag is clear -- "
               "the assertion that would catch a decoder trusting length over the flag");

    // A length that is neither V1 nor V2 must be rejected outright.
    msg.length = 56;
    link.stats.frame_errors = 0;
    TEST_CHECK(safety_apply_power(&link, &msg) == false, "a length that is neither V1 nor V2 is rejected");
    TEST_CHECK(link.stats.frame_errors == 1, "the rejection is counted as a frame error");
}

static void test_apply_status_v3_borrowed(void)
{
    TEST_SECTION("safety_apply_status -- V3 (26B) BORROWED status frame: absent-byte contract "
                 "reads as UNKNOWN (never a false 'not borrowed'), present byte decodes correctly, "
                 "and a V3->V1/V2 regression clears the stale known-flag (2026-09-03, TASK 1)");

    SafetyLinkClass link = make_link();
    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));

    // V1 (23 bytes): no byte 24/25 at all -- must read as borrowed_known ==
    // false, the "not yet known" state, never a confident "not borrowed".
    set_status_frame(msg.payload, (uint8_t)(SAFETY_FLAG_TEMP_VALID), 123.5f, 24.0f, 0, 1.0f, 2.0f, 3.0f);
    msg.length = SAFETY_LINK_STATUS_FRAME_LEN_V1;
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "V1 frame is accepted");
    TEST_CHECK(link.cached.borrowed_known == false, "V1 frame leaves borrowed_known false (no byte 24/25 to read)");

    // V2 (24 bytes): tx_dropped_sat is present, but STILL no byte 24/25 --
    // borrowed_known must still be false. This is the case that would look
    // like an off-by-one if the length check were written as ">= V2" instead
    // of an exact per-version match.
    msg.payload[23] = 9;
    msg.length = SAFETY_LINK_STATUS_FRAME_LEN_V2;
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "V2 frame is accepted");
    TEST_CHECK(link.cached.borrowed_known == false, "V2 frame ALSO leaves borrowed_known false (byte 24/25 still absent)");

    // V3 (26 bytes), is_borrowed=true, a real committed zone index.
    msg.payload[24] = SAFETY_LINK_STATUS_FLAG2_BORROWED;
    msg.payload[25] = 1u;
    msg.length = SAFETY_LINK_STATUS_FRAME_LEN_V3;
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "V3 (26-byte) frame is accepted");
    TEST_CHECK(link.cached.borrowed_known == true, "V3 frame sets borrowed_known true");
    TEST_CHECK(link.cached.borrowed == true, "V3 frame's flags2 bit0 becomes cached.borrowed");
    TEST_CHECK(link.cached.borrowed_zone_index == 1u, "V3 frame's byte 25 becomes borrowed_zone_index");

    // Same V3 frame but is_borrowed=false and the zone-unknown sentinel --
    // proves the bit/byte are read from the wire, not hard-coded true by
    // this decode path.
    msg.payload[24] = 0u;
    msg.payload[25] = SAFETY_LINK_BORROWED_ZONE_UNKNOWN;
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "second V3 frame decodes");
    TEST_CHECK(link.cached.borrowed_known == true, "still borrowed_known == true (a V3 frame was received)");
    TEST_CHECK(link.cached.borrowed == false, "flags2 bit0 clear -> cached.borrowed == false");
    TEST_CHECK(link.cached.borrowed_zone_index == SAFETY_LINK_BORROWED_ZONE_UNKNOWN,
               "sentinel round-trips when the Pico itself doesn't know the zone");

    // Regression V3 -> V1: a peer that stops sending V3 (rollback, or an ESP
    // that stops confirming V3 support) must not leave a stale
    // borrowed_known=true pointing at the last V3 frame's now-stale bytes.
    set_status_frame(msg.payload, (uint8_t)(SAFETY_FLAG_TEMP_VALID), 123.5f, 24.0f, 0, 1.0f, 2.0f, 3.0f);
    msg.length = SAFETY_LINK_STATUS_FRAME_LEN_V1;
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "V1 frame after a V3 frame is still accepted");
    TEST_CHECK(link.cached.borrowed_known == false,
               "a peer regressing from V3 to V1 mid-session clears the stale borrowed_known flag");
}

static void test_apply_status_v3_tc_config_reasserted(void)
{
    TEST_SECTION("safety_apply_status -- V3 (26B) TC_CONFIG_REASSERTED flags2 bit 2 (2026-09-23): "
                 "absent-byte contract reads as UNKNOWN, present byte decodes correctly and is "
                 "independent of bits 0/1, and a V3->V1 regression clears the stale known-flag");

    SafetyLinkClass link = make_link();
    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));

    // V1 (23 bytes): no byte 24/25 at all -- must read as
    // tc_config_reasserted_known == false, never a confident "not
    // reasserted".
    set_status_frame(msg.payload, (uint8_t)(SAFETY_FLAG_TEMP_VALID), 123.5f, 24.0f, 0, 1.0f, 2.0f, 3.0f);
    msg.length = SAFETY_LINK_STATUS_FRAME_LEN_V1;
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "V1 frame is accepted");
    TEST_CHECK(link.cached.tc_config_reasserted_known == false,
               "V1 frame leaves tc_config_reasserted_known false (no byte 24/25 to read)");

    // V3 (26 bytes), bit 2 set alongside bits 0/1 -- proves the new bit is
    // decoded independently, not aliased onto BORROWED/CJ_VALID.
    msg.payload[24] = (uint8_t)(SAFETY_LINK_STATUS_FLAG2_BORROWED | SAFETY_LINK_STATUS_FLAG2_CJ_VALID |
                                SAFETY_LINK_STATUS_FLAG2_TC_CONFIG_REASSERTED);
    msg.payload[25] = 1u;
    msg.length = SAFETY_LINK_STATUS_FRAME_LEN_V3;
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "V3 (26-byte) frame is accepted");
    TEST_CHECK(link.cached.tc_config_reasserted_known == true, "V3 frame sets tc_config_reasserted_known true");
    TEST_CHECK(link.cached.tc_config_reasserted == true,
               "V3 frame's flags2 bit2 becomes cached.tc_config_reasserted");
    TEST_CHECK(link.cached.borrowed == true, "bit0 still decodes correctly alongside bit2");
    TEST_CHECK(link.cached.cj_valid == true, "bit1 still decodes correctly alongside bit2");

    // bit2 alone (0x04), bits 0/1 clear -- proves bit2 is decoded off its own
    // mask, not aliased onto BORROWED (0x01) or CJ_VALID (0x02): a decode
    // that mistakenly used either of those masks would read false here.
    msg.payload[24] = (uint8_t)(SAFETY_LINK_STATUS_FLAG2_TC_CONFIG_REASSERTED);
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "bit2-only V3 frame decodes");
    TEST_CHECK(link.cached.tc_config_reasserted == true, "bit2 alone still sets tc_config_reasserted true");
    TEST_CHECK(link.cached.borrowed == false, "bit0 clear alongside a lone bit2");
    TEST_CHECK(link.cached.cj_valid == false, "bit1 clear alongside a lone bit2");

    // bits 0/1 set (0x03), bit2 clear -- proves bit2 is decoded off its own
    // mask in the other direction: a decode that mistakenly used the
    // BORROWED or CJ_VALID mask in place of bit2 would read true here.
    msg.payload[24] = (uint8_t)(SAFETY_LINK_STATUS_FLAG2_BORROWED | SAFETY_LINK_STATUS_FLAG2_CJ_VALID);
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "bits0/1-only V3 frame decodes");
    TEST_CHECK(link.cached.tc_config_reasserted == false, "bit2 clear -> tc_config_reasserted false even with bits 0/1 set");
    TEST_CHECK(link.cached.borrowed == true, "bit0 still true");
    TEST_CHECK(link.cached.cj_valid == true, "bit1 still true");

    // Same V3 frame but bit 2 clear -- proves the bit is read from the wire,
    // not hard-coded true by this decode path.
    msg.payload[24] = 0u;
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "second V3 frame decodes");
    TEST_CHECK(link.cached.tc_config_reasserted_known == true, "still known == true (a V3 frame was received)");
    TEST_CHECK(link.cached.tc_config_reasserted == false, "flags2 bit2 clear -> cached.tc_config_reasserted == false");

    // Regression V3 -> V1: a peer that stops sending V3 must not leave a
    // stale tc_config_reasserted_known=true pointing at the last V3 frame's
    // now-stale bytes.
    set_status_frame(msg.payload, (uint8_t)(SAFETY_FLAG_TEMP_VALID), 123.5f, 24.0f, 0, 1.0f, 2.0f, 3.0f);
    msg.length = SAFETY_LINK_STATUS_FRAME_LEN_V1;
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "V1 frame after a V3 frame is still accepted");
    TEST_CHECK(link.cached.tc_config_reasserted_known == false,
               "a peer regressing from V3 to V1 mid-session clears the stale tc_config_reasserted_known flag");
}

static void test_apply_status_v3_active_slot(void)
{
    TEST_SECTION("safety_apply_status -- V3 (26B) ACTIVE_SLOT_KNOWN/ACTIVE_SLOT_B flags2 bits 3/4 "
                 "(2026-09-23, docs/PICO_AUTO_UPDATE_PLAN.md:64): absent-byte contract reads as "
                 "UNKNOWN, present byte decodes A/B correctly and is independent of bits 0/1/2, and "
                 "a V3->V1 regression clears the stale known-flag");

    // Pinned so this test cannot pass by tautology against the very macros
    // the decoder itself uses to interpret payload[24]: the slot-A/slot-B
    // cases below deliberately use raw hex literals for the slot bits, not
    // these macros, so a swap of the macro values in safety_link.h is
    // caught rather than silently moving both sides together.
    _Static_assert(SAFETY_LINK_STATUS_FLAG2_ACTIVE_SLOT_KNOWN == 0x08u,
                   "test_apply_status_v3_active_slot's raw literals (0x08u/0x18u) assume this value");
    _Static_assert(SAFETY_LINK_STATUS_FLAG2_ACTIVE_SLOT_B == 0x10u,
                   "test_apply_status_v3_active_slot's raw literals (0x08u/0x18u) assume this value");

    SafetyLinkClass link = make_link();
    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));

    // V1 (23 bytes): no byte 24/25 at all -- must read as
    // pico_active_slot_known == false, never a confident slot A.
    set_status_frame(msg.payload, (uint8_t)(SAFETY_FLAG_TEMP_VALID), 123.5f, 24.0f, 0, 1.0f, 2.0f, 3.0f);
    msg.length = SAFETY_LINK_STATUS_FRAME_LEN_V1;
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "V1 frame is accepted");
    TEST_CHECK(link.cached.pico_active_slot_known == false,
               "V1 frame leaves pico_active_slot_known false (no byte 24/25 to read)");

    // V3 (26 bytes), bits 3+4 set (KNOWN + slot B, raw literal 0x18u) --
    // proves the new bits are decoded independently, not aliased onto any
    // existing flag. Bits 0/1 still use their own macros here since this
    // case isn't testing THEM against a swap.
    msg.payload[24] = (uint8_t)(SAFETY_LINK_STATUS_FLAG2_BORROWED | SAFETY_LINK_STATUS_FLAG2_CJ_VALID |
                                SAFETY_LINK_STATUS_FLAG2_TC_CONFIG_REASSERTED | 0x18u);
    msg.payload[25] = 1u;
    msg.length = SAFETY_LINK_STATUS_FRAME_LEN_V3;
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "V3 (26-byte) frame is accepted");
    TEST_CHECK(link.cached.pico_active_slot_known == true, "V3 frame sets pico_active_slot_known true");
    TEST_CHECK(link.cached.pico_active_slot_is_b == true,
               "V3 frame's flags2 bit4 becomes cached.pico_active_slot_is_b (slot B)");
    TEST_CHECK(link.cached.borrowed == true, "bit0 still decodes correctly alongside bits3/4");
    TEST_CHECK(link.cached.cj_valid == true, "bit1 still decodes correctly alongside bits3/4");
    TEST_CHECK(link.cached.tc_config_reasserted == true, "bit2 still decodes correctly alongside bits3/4");

    // KNOWN set, B clear -- slot A -- proves bit4 is read off its own mask,
    // not hard-coded true whenever bit3 is set. Raw literal 0x08u, not the
    // macro: see the _Static_assert block above.
    msg.payload[24] = 0x08u;
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "KNOWN-only V3 frame decodes");
    TEST_CHECK(link.cached.pico_active_slot_known == true, "KNOWN bit alone sets pico_active_slot_known true");
    TEST_CHECK(link.cached.pico_active_slot_is_b == false, "B bit clear -> pico_active_slot_is_b false (slot A)");

    // B set, KNOWN clear -- a wire value that should never actually occur
    // (link_frame_pack_status() never sets B without KNOWN), but the decode
    // must still read it off its own mask rather than inferring KNOWN from
    // B, matching the sender-side contract precisely.
    msg.payload[24] = (uint8_t)SAFETY_LINK_STATUS_FLAG2_ACTIVE_SLOT_B;
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "B-only V3 frame decodes");
    TEST_CHECK(link.cached.pico_active_slot_known == false, "KNOWN clear even with B set");
    TEST_CHECK(link.cached.pico_active_slot_is_b == true, "B bit decodes true off its own mask regardless");

    // Regression V3 -> V1: a peer that stops sending V3 must not leave a
    // stale pico_active_slot_known=true pointing at the last V3 frame's
    // now-stale bytes.
    set_status_frame(msg.payload, (uint8_t)(SAFETY_FLAG_TEMP_VALID), 123.5f, 24.0f, 0, 1.0f, 2.0f, 3.0f);
    msg.length = SAFETY_LINK_STATUS_FRAME_LEN_V1;
    TEST_CHECK(safety_apply_status(&link, &msg) == true, "V1 frame after a V3 frame is still accepted");
    TEST_CHECK(link.cached.pico_active_slot_known == false,
               "a peer regressing from V3 to V1 mid-session clears the stale pico_active_slot_known flag");
}

static void test_safety_tc_is_separate_physical_sensor_predicate(void)
{
    TEST_SECTION("safety_tc_is_separate_physical_sensor() -- ROADMAP.md 'Safety TC display "
                 "audit, 2026-09-05' shared predicate: fail-to-shown. Suppressed ONLY for link "
                 "down or CONFIRMED borrowed (borrowed_known + borrowed); an older Pico that has "
                 "never confirmed V3 (borrowed_known false) must still show.");

    safety_link_status_t st;
    memset(&st, 0, sizeof(st));

    // Link down: suppressed regardless of the borrowed bits.
    st.link_up = false;
    st.borrowed_known = true;
    st.borrowed = false;
    TEST_CHECK(safety_tc_is_separate_physical_sensor(&st) == false,
               "link down suppresses even with borrowed_known/borrowed looking favorable");

    // Link up, V3 confirmed, BORROWED_ZONE/BOTH (borrowed == true): suppressed.
    st.link_up = true;
    st.borrowed_known = true;
    st.borrowed = true;
    TEST_CHECK(safety_tc_is_separate_physical_sensor(&st) == false,
               "confirmed BORROWED_ZONE/BOTH suppresses");

    // Link up, V3 confirmed, not borrowed (tc_source == SAFETY_TC_SOURCE_OWN_J7): shows.
    st.link_up = true;
    st.borrowed_known = true;
    st.borrowed = false;
    TEST_CHECK(safety_tc_is_separate_physical_sensor(&st) == true,
               "link up + borrowed_known + !borrowed shows");

    // Link up, but an older Pico (or one that hasn't confirmed V3 yet):
    // borrowed_known false is UNKNOWN, not confirmed borrowed, so it must
    // fail to shown rather than fail to hidden.
    st.link_up = true;
    st.borrowed_known = false;
    st.borrowed = false;
    TEST_CHECK(safety_tc_is_separate_physical_sensor(&st) == true,
               "borrowed_known false (pre-V3 Pico) shows -- unknown is not the same as "
               "confirmed borrowed, and this predicate fails to shown, not to hidden");

    // NULL is a safe "nothing to show", not a crash.
    TEST_CHECK(safety_tc_is_separate_physical_sensor(NULL) == false, "NULL status suppresses");
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

// ---------------------------------------------------------------------
// RELAY_LIFE_BUDGET.md -- K4 edge counting off consecutive
// GET_STATUS frames. relay_cycles_note_safety_edge() is faked as a plain
// counter above; these tests pin how many times safety_apply_status() (and
// safety_apply_fw_version()'s boot_id_changed branch) call it.
// ---------------------------------------------------------------------

static void send_status_with_relay_bit(SafetyLinkClass *link, uart_proto_message_t *msg, bool relay_on)
{
    uint8_t flags = (uint8_t)(SAFETY_FLAG_TEMP_VALID | (relay_on ? SAFETY_FLAG_RELAY : 0));
    set_status_frame(msg->payload, flags, 100.0f, 20.0f, 0, 0, 0, 0);
    msg->length = SAFETY_LINK_STATUS_FRAME_LEN_V1;
    TEST_CHECK(safety_apply_status(link, msg) == true, "status frame decodes");
}

static void test_k4_edge_counting_off_on_on_off_counts_two(void)
{
    TEST_SECTION("safety_apply_status -- K4 edge counting: off, on, on, off counts exactly "
                 "TWO edges (one real transition each way; the repeated 'on' frame is not a "
                 "second edge)");

    s_stub_relay_cycles_safety_edge_calls = 0;
    SafetyLinkClass link = make_link();
    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));

    send_status_with_relay_bit(&link, &msg, false); // off
    TEST_CHECK(s_stub_relay_cycles_safety_edge_calls == 0, "off -> off (relative to the unknown start) is not an edge");

    send_status_with_relay_bit(&link, &msg, true); // off -> on
    TEST_CHECK(s_stub_relay_cycles_safety_edge_calls == 1, "off -> on is the first edge");

    send_status_with_relay_bit(&link, &msg, true); // on -> on (repeat)
    TEST_CHECK(s_stub_relay_cycles_safety_edge_calls == 1, "on -> on (same state repeated) counts nothing new");

    send_status_with_relay_bit(&link, &msg, false); // on -> off
    TEST_CHECK(s_stub_relay_cycles_safety_edge_calls == 2, "on -> off is the second edge");
}

static void test_k4_edge_counting_first_frame_counts_zero(void)
{
    TEST_SECTION("safety_apply_status -- K4 edge counting: the very FIRST status frame this "
                 "link ever sees counts ZERO edges, however K4 reads -- there is no prior "
                 "observed state to compare against (RELAY_LIFE_BUDGET.md's "
                 "'unknown, not off' starting state)");

    s_stub_relay_cycles_safety_edge_calls = 0;
    SafetyLinkClass link = make_link();
    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));

    // First observation reads K4 ON. If the tracked "previous" state
    // defaulted to false/off instead of genuinely unknown, this would be
    // wrongly counted as an off->on edge.
    send_status_with_relay_bit(&link, &msg, true);
    TEST_CHECK(s_stub_relay_cycles_safety_edge_calls == 0,
               "first-ever frame resyncs silently, counting zero edges even though K4 reads ON");
    TEST_CHECK(link.safety_relay_state_known == true, "state is now known, for the NEXT frame to compare against");
    TEST_CHECK(link.safety_relay_state == true, "and it correctly remembers ON");
}

static void test_k4_edge_counting_boot_id_change_counts_zero(void)
{
    TEST_SECTION("safety_apply_fw_version -- a Pico boot_id change forgets the tracked K4 "
                 "state (same 'reset one side of a producer/consumer pair' hazard as "
                 "trip_last_seq): the first status frame after the reboot must not be compared "
                 "against a stale pre-reboot memory, so it counts zero edges regardless of "
                 "whether K4's reported state actually changed across the reboot");

    s_stub_relay_cycles_safety_edge_calls = 0;
    SafetyLinkClass link = make_link();
    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));

    // Establish a known state: K4 OFF.
    send_status_with_relay_bit(&link, &msg, false);
    TEST_CHECK(s_stub_relay_cycles_safety_edge_calls == 0, "setup: first frame, zero edges");
    TEST_CHECK(link.safety_relay_state_known == true, "setup: state now known (off)");

    // The Pico reboots -- a FW_VERSION frame with a NEW boot_id arrives.
    uart_proto_message_t fw_msg;
    memset(&fw_msg, 0, sizeof(fw_msg));
    fw_msg.length = set_fw_version_frame(fw_msg.payload, false, NULL, 0, NULL, 0, /*boot_id=*/42,
                                          /*config_version=*/0, /*config_crc=*/0);
    safety_apply_fw_version(&link, &fw_msg);
    TEST_CHECK(link.pico_boot_id_known == true && link.pico_boot_id == 42, "setup: boot_id learned");
    TEST_CHECK(link.safety_relay_state_known == false,
               "the FIRST FW_VERSION frame (pico_boot_id was not known before) already counts as "
               "a 'boot_id changed' event and forgets the tracked K4 state -- correct, since this "
               "ESP has no baseline to trust either way yet");

    // Next status frame reads K4 ON -- must resync silently (zero edges),
    // exactly like the very-first-frame case, not report an off->on edge
    // just because the last REMEMBERED state (from before the reboot) was off.
    send_status_with_relay_bit(&link, &msg, true);
    TEST_CHECK(s_stub_relay_cycles_safety_edge_calls == 0,
               "post-reboot resync frame counts zero edges, even though K4 now reads ON and the "
               "pre-reboot memory was OFF");

    // Now establish this boot's baseline and change boot_id AGAIN, to prove
    // the reset fires on a genuine SUBSEQUENT boot_id change too, not only
    // the first-ever one.
    send_status_with_relay_bit(&link, &msg, true); // on -> on, still resynced, no new edge
    TEST_CHECK(s_stub_relay_cycles_safety_edge_calls == 0, "second frame this boot: still just a repeat, no edge");

    fw_msg.length = set_fw_version_frame(fw_msg.payload, false, NULL, 0, NULL, 0, /*boot_id=*/43,
                                          /*config_version=*/0, /*config_crc=*/0); // a genuinely different boot_id
    safety_apply_fw_version(&link, &fw_msg);
    TEST_CHECK(link.safety_relay_state_known == false, "a later, genuine boot_id change also resets tracking");

    send_status_with_relay_bit(&link, &msg, false); // reads OFF this time
    TEST_CHECK(s_stub_relay_cycles_safety_edge_calls == 0,
               "resync after the SECOND reboot also counts zero edges, despite differing from the "
               "immediately-prior remembered state (on)");
}

static void test_fw_version_frame_too_short_for_min_compatible_leaves_peer_unknown(void)
{
    TEST_SECTION("safety_apply_fw_version -- a frame from a peer built BEFORE the "
                 "min_compatible field existed (payload shorter than 5 bytes, so bytes "
                 "3..4 do not exist on the wire at all) must NOT be read as "
                 "min_compatible=0/'compatible with everything'. The safe interpretation "
                 "is 'unknown', same as no FW_VERSION frame having arrived yet -- "
                 "peer_version_known must stay false so the fail-closed "
                 "version_mismatch = !peer_version_known || !peer_version_compatible "
                 "check (safety_link_poll.c) still asserts the link fault.");

    SafetyLinkClass link = make_link();
    link.initialized = true;
    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));

    // A pre-min_compatible ANNOUNCE/FW_VERSION frame carried only cmd(1) +
    // protocol(2) = 3 bytes total -- one short of the 5 needed to reach
    // offset 3..4 where min_compatible now lives.
    msg.payload[0] = 0x0B; // GET_FW_VERSION reply cmd id (irrelevant to the parser)
    msg.payload[1] = 5;    // protocol_version low byte
    msg.payload[2] = 0;    // protocol_version high byte
    msg.length = 3;

    safety_apply_fw_version(&link, &msg);

    TEST_CHECK(link.peer_version_known == false,
               "too-short frame (no room for min_compatible) leaves peer_version_known "
               "false, NOT true-with-min_compatible-defaulted-to-0 -- an all-zero struct "
               "field must never be misread as 'peer accepts everything'");
    TEST_CHECK(link.peer_version_compatible == false,
               "the compatibility verdict itself is also left at its safe (false) default, "
               "never flipped to true by a frame that never supplied enough bytes to judge it");

    // A frame with EXACTLY 5 bytes (the boundary) DOES carry min_compatible
    // and must be accepted -- this is not "any short frame is refused",
    // it is specifically "a frame that cannot possibly contain the field
    // is not misread as though it did".
    SafetyLinkClass link2 = make_link();
    link2.initialized = true;
    uart_proto_message_t msg2;
    memset(&msg2, 0, sizeof(msg2));
    msg2.payload[0] = 0x0B;
    msg2.payload[1] = 5; msg2.payload[2] = 0; // protocol_version = 5
    msg2.payload[3] = 3; msg2.payload[4] = 0; // min_compatible = 3
    msg2.length = 5;

    safety_apply_fw_version(&link2, &msg2);

    TEST_CHECK(link2.peer_version_known == true,
               "a frame that reaches exactly byte offset 4 (5 bytes total) DOES carry "
               "min_compatible and IS accepted as known -- the boundary is exclusive on "
               "the short side only");
    TEST_CHECK(link2.peer_protocol_version == 5 && link2.peer_min_compatible == 3,
               "min_compatible decodes correctly right at the minimum viable frame length");
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
// SAFETY_CMD_PARAM (0x1E) / SAFETY_CMD_GET_PARAM (0x23), KILNLINK_PROTOCOL_
// VERSION 7 -- inbox stash coverage (5-byte found=0, 9-byte found=1/f32,
// 4-byte too-short and 10-byte too-long rejection) plus an end-to-end
// safety_link_get_param() param_id-mismatch test via the stubbed broadcast
// send/reply mechanism (s_stub_broadcast_*, used above by the link_reply_us
// tests). Modeled on make_rollback_result_frame() / test_rollback_result_
// late_frame_is_stashed_not_dropped() above.
// --------------------------------------------------------------------------

static uint8_t make_param_frame(uint8_t *payload, uint16_t param_id, uint8_t found, uint8_t type,
                                 const uint8_t *value, uint8_t value_len)
{
    kilnlink_param_t p;
    memset(&p, 0, sizeof(p));
    p.param_id = param_id;
    p.found = found;
    p.type = type;
    if (value && value_len) {
        memcpy(&p.value, value, value_len);
    }
    kilnlink_param_status_t enc_status = KILNLINK_PARAM_OK;
    size_t enc_len = kilnlink_param_encode(&p, payload, KILNLINK_PARAM_MAX_LEN, &enc_status);
    assert(enc_len >= KILNLINK_PARAM_HDR_LEN && "test setup: encode must succeed");
    return (uint8_t)enc_len;
}

static void test_param_frame_5_byte_found_zero_is_stashed(void)
{
    TEST_SECTION("safety_drain_inbox_ex -- a 5-byte PARAM frame (found=0, no value bytes) is "
                 "captured into the stash");

    SafetyLinkClass link = make_link();
    link.initialized = true;

    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.length = make_param_frame(msg.payload, /*param_id=*/0x0505, /*found=*/0,
                                   /*type=*/KILNLINK_PARAM_TYPE_U8, NULL, 0);
    TEST_CHECK(msg.length == KILNLINK_PARAM_HDR_LEN, "a found=0 reply is exactly the header, 5 bytes");

    fake_inbox_reset();
    fake_inbox_push(&msg);
    (void)safety_drain_inbox_ex(&link, 0, false, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

    TEST_CHECK(link.has_stashed_param == true, "a 5-byte PARAM frame is stashed, not dropped");
    TEST_CHECK(link.stashed_param.length == KILNLINK_PARAM_HDR_LEN, "the stashed copy keeps its length");

    uart_proto_message_t taken;
    TEST_CHECK(safety_take_stashed_param(&link, &taken) == true, "the stash is consumable");
    TEST_CHECK(link.has_stashed_param == false, "taking it clears the stash");
    kilnlink_param_t decoded;
    TEST_CHECK(kilnlink_param_decode(taken.payload, taken.length, &decoded) == KILNLINK_PARAM_OK,
               "a found=0 frame decodes cleanly");
    TEST_CHECK(decoded.param_id == 0x0505 && decoded.found == 0, "decoded fields match what was sent");
}

static void test_param_frame_9_byte_found_one_f32_is_stashed(void)
{
    TEST_SECTION("safety_drain_inbox_ex -- a 9-byte PARAM frame (found=1, type=F32, 4 value bytes) "
                 "is captured into the stash");

    SafetyLinkClass link = make_link();
    link.initialized = true;

    float f = 3.5f;
    uint8_t value[4];
    memcpy(value, &f, sizeof(value));

    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.length = make_param_frame(msg.payload, /*param_id=*/0x0102, /*found=*/1,
                                   /*type=*/KILNLINK_PARAM_TYPE_F32, value, sizeof(value));
    TEST_CHECK(msg.length == KILNLINK_PARAM_MAX_LEN, "a found=1/F32 reply is the widest shape, 9 bytes");

    fake_inbox_reset();
    fake_inbox_push(&msg);
    (void)safety_drain_inbox_ex(&link, 0, false, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

    TEST_CHECK(link.has_stashed_param == true, "a 9-byte PARAM frame is stashed, not dropped");
    TEST_CHECK(link.stashed_param.length == KILNLINK_PARAM_MAX_LEN, "the stashed copy keeps its length");

    uart_proto_message_t taken;
    TEST_CHECK(safety_take_stashed_param(&link, &taken) == true, "the stash is consumable");
    kilnlink_param_t decoded;
    TEST_CHECK(kilnlink_param_decode(taken.payload, taken.length, &decoded) == KILNLINK_PARAM_OK,
               "a found=1/F32 frame decodes cleanly");
    TEST_CHECK(decoded.param_id == 0x0102 && decoded.found == 1 && decoded.type == KILNLINK_PARAM_TYPE_F32,
               "decoded fields match what was sent");
    TEST_CHECK(decoded.value.f32_val == 3.5f, "the decoded value survives the round trip");
}

static void test_param_frame_4_byte_too_short_is_not_stashed(void)
{
    TEST_SECTION("safety_drain_inbox_ex -- a 4-byte frame with cmd byte KILNLINK_PARAM_CMD is too "
                 "short to be a real PARAM reply (below KILNLINK_PARAM_HDR_LEN) and must NOT be stashed");

    SafetyLinkClass link = make_link();
    link.initialized = true;

    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.length = KILNLINK_PARAM_HDR_LEN - 1;
    msg.payload[0] = KILNLINK_PARAM_CMD;

    fake_inbox_reset();
    fake_inbox_push(&msg);
    (void)safety_drain_inbox_ex(&link, 0, false, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

    TEST_CHECK(link.has_stashed_param == false, "a too-short frame is not stashed");
}

static void test_param_frame_10_byte_too_long_is_not_stashed(void)
{
    TEST_SECTION("safety_drain_inbox_ex -- a 10-byte frame with cmd byte KILNLINK_PARAM_CMD is too "
                 "long to be a real PARAM reply (above KILNLINK_PARAM_MAX_LEN) and must NOT be stashed");

    SafetyLinkClass link = make_link();
    link.initialized = true;

    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.length = KILNLINK_PARAM_MAX_LEN + 1;
    msg.payload[0] = KILNLINK_PARAM_CMD;

    fake_inbox_reset();
    fake_inbox_push(&msg);
    (void)safety_drain_inbox_ex(&link, 0, false, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

    TEST_CHECK(link.has_stashed_param == false, "a too-long frame is not stashed");
}

static void test_get_param_drops_reply_with_mismatched_param_id(void)
{
    TEST_SECTION("safety_link_get_param -- a PARAM reply carrying a DIFFERENT param_id than requested "
                 "(a very late reply to a prior call) is dropped as an error, never handed back as "
                 "this call's answer");

    SafetyLinkClass link = make_link();
    link.initialized = true;

    s_stub_broadcast_send_succeeds = true;
    s_stub_broadcast_reply_push = true;
    s_stub_broadcast_advance_us = 500;
    memset(&s_stub_broadcast_reply_msg, 0, sizeof(s_stub_broadcast_reply_msg));
    s_stub_broadcast_reply_msg.length =
        make_param_frame(s_stub_broadcast_reply_msg.payload, /*param_id=*/0x9999, /*found=*/1,
                          KILNLINK_PARAM_TYPE_U8, (const uint8_t[]){ 7 }, 1);

    uint8_t out[KILNLINK_PARAM_MAX_LEN];
    size_t out_len = 0;
    esp_err_t err = safety_link_get_param(&link, /*param_id=*/0x0505, out, sizeof(out), &out_len);

    TEST_CHECK(err == ESP_FAIL, "a param_id mismatch must never be reported as success");
    TEST_CHECK(link.has_stashed_param == false, "the mismatched reply was consumed (taken), not left "
                                                 "sitting in the stash for a later caller to misread");
}

static void test_get_param_succeeds_with_matching_param_id(void)
{
    TEST_SECTION("safety_link_get_param -- a PARAM reply carrying the SAME param_id as requested "
                 "succeeds and hands back the raw frame bytes");

    SafetyLinkClass link = make_link();
    link.initialized = true;

    s_stub_broadcast_send_succeeds = true;
    s_stub_broadcast_reply_push = true;
    s_stub_broadcast_advance_us = 500;
    memset(&s_stub_broadcast_reply_msg, 0, sizeof(s_stub_broadcast_reply_msg));
    s_stub_broadcast_reply_msg.length =
        make_param_frame(s_stub_broadcast_reply_msg.payload, /*param_id=*/0x0505, /*found=*/1,
                          KILNLINK_PARAM_TYPE_U8, (const uint8_t[]){ 7 }, 1);

    uint8_t out[KILNLINK_PARAM_MAX_LEN];
    size_t out_len = 0;
    esp_err_t err = safety_link_get_param(&link, /*param_id=*/0x0505, out, sizeof(out), &out_len);

    TEST_CHECK(err == ESP_OK, "a matching param_id succeeds");
    TEST_CHECK(out_len == s_stub_broadcast_reply_msg.length, "the raw reply length is handed back unchanged");
    kilnlink_param_t decoded;
    TEST_CHECK(kilnlink_param_decode(out, out_len, &decoded) == KILNLINK_PARAM_OK,
               "the raw bytes handed back decode cleanly");
    TEST_CHECK(decoded.found == 1 && decoded.value.u8_val == 7, "the relayed value matches what the peer sent");
}

static void test_get_param_survives_a_leading_unrelated_frame(void)
{
    TEST_SECTION("safety_link_get_param -- an unrelated frame (a periodic STATUS/DIAG/POWER push) "
                 "dequeued ahead of our own PARAM reply must not end the wait early; the reply, which "
                 "genuinely has not arrived yet at that point, must still be picked up on a later drain "
                 "within the same call's budget. Reproduces the safety_drain_still_waiting() bug class "
                 "(safety_link.h) for STACK_MARGIN/PARAM, which had no want_*/got_* wiring.");

    SafetyLinkClass link = make_link();
    link.initialized = true;

    fake_inbox_reset();
    s_stub_broadcast_send_succeeds = true;
    /* The reply is NOT pushed at send time -- it "arrives" only after the
     * call's first internal drain empties the queue and times out once,
     * exactly like the real bug this reproduces. */
    s_stub_broadcast_reply_push = false;
    s_stub_broadcast_advance_us = 500;

    s_stub_broadcast_leading_push = true;
    memset(&s_stub_broadcast_leading_msg, 0, sizeof(s_stub_broadcast_leading_msg));
    s_stub_broadcast_leading_msg.length = 1;
    s_stub_broadcast_leading_msg.payload[0] = SAFETY_CMD_GET_STATUS;

    memset(&s_fake_inbox_delayed_msg, 0, sizeof(s_fake_inbox_delayed_msg));
    s_fake_inbox_delayed_msg.length =
        make_param_frame(s_fake_inbox_delayed_msg.payload, /*param_id=*/0x0505, /*found=*/1,
                          KILNLINK_PARAM_TYPE_U8, (const uint8_t[]){ 9 }, 1);

    uint8_t out[KILNLINK_PARAM_MAX_LEN];
    size_t out_len = 0;
    esp_err_t err = safety_link_get_param(&link, /*param_id=*/0x0505, out, sizeof(out), &out_len);

    TEST_CHECK(err == ESP_OK, "a reply that arrives after a leading unrelated frame must still be "
                              "picked up within the call's own timeout budget, not dropped as ESP_ERR_TIMEOUT");
    kilnlink_param_t decoded;
    TEST_CHECK(kilnlink_param_decode(out, out_len, &decoded) == KILNLINK_PARAM_OK,
               "the delayed reply's raw bytes decode cleanly");
    TEST_CHECK(decoded.found == 1 && decoded.value.u8_val == 9, "the relayed value matches the delayed reply, "
                                                                 "not the leading unrelated frame");

    /* Leave global stub state clean for every test after this one -- same
     * "explicit per-test state, no leftover" convention this file's own
     * comments call out elsewhere (fake_inbox_reset()'s doc comment above). */
    s_stub_broadcast_leading_push = false;
    fake_inbox_reset();
}

// Negative-test companion for the loop fix itself (6bc4fc23's opus review,
// follow-up item 1): a genuine no-reply case must still report ESP_ERR_
// TIMEOUT once the loop's remaining budget is exhausted, not spin forever.
// Before the fake uart_protocol_receive() stub above was made to advance
// the fake clock on every empty poll, this test would hang: the call-site
// loop re-derives its remaining budget from hal_time_now_us(), which never
// advanced on an all-ESP_ERR_TIMEOUT stub, so remaining_ms never reached 0.
static void test_get_param_no_reply_reports_timeout(void)
{
    TEST_SECTION("safety_link_get_param -- a request that is sent but never answered still "
                 "reports ESP_ERR_TIMEOUT once the loop's budget is spent, rather than spinning "
                 "forever (the fake clock now advances on every empty uart_protocol_receive() poll)");

    SafetyLinkClass link = make_link();
    link.initialized = true;

    fake_inbox_reset();
    fake_time_reset_all();
    s_stub_broadcast_send_succeeds = true;
    s_stub_broadcast_reply_push = false; // nothing ever lands in the inbox
    s_fake_inbox_advance_time_on_empty = true;

    uint8_t out[KILNLINK_PARAM_MAX_LEN];
    size_t out_len = 0;
    esp_err_t err = safety_link_get_param(&link, /*param_id=*/0x0505, out, sizeof(out), &out_len);

    TEST_CHECK(err == ESP_ERR_TIMEOUT, "no reply ever arrives -- the call reports a timeout, not a hang");

    fake_inbox_reset();
}

// Loop-fix coverage for safety_link_get_stack_margin(), mirroring test_get_
// param_survives_a_leading_unrelated_frame() above exactly: a periodic
// STATUS/DIAG/POWER push dequeued ahead of our own STACK_MARGIN reply must
// not end the wait early -- the reply, which genuinely has not arrived yet
// at that point, must still be picked up on a later drain within the same
// call's budget.
static void test_get_stack_margin_survives_a_leading_unrelated_frame(void)
{
    TEST_SECTION("safety_link_get_stack_margin -- an unrelated frame (a periodic STATUS/DIAG/POWER "
                 "push) dequeued ahead of our own STACK_MARGIN reply must not end the wait early; the "
                 "reply, which genuinely has not arrived yet at that point, must still be picked up on "
                 "a later drain within the same call's budget.");

    SafetyLinkClass link = make_link();
    link.initialized = true;

    fake_inbox_reset();
    fake_time_reset_all();
    s_stub_broadcast_send_succeeds = true;
    /* The reply is NOT pushed at send time -- it "arrives" only after the
     * call's first internal drain empties the queue and times out once,
     * exactly like the real bug this reproduces. */
    s_stub_broadcast_reply_push = false;
    s_stub_broadcast_advance_us = 500;

    s_stub_broadcast_leading_push = true;
    memset(&s_stub_broadcast_leading_msg, 0, sizeof(s_stub_broadcast_leading_msg));
    s_stub_broadcast_leading_msg.length = 1;
    s_stub_broadcast_leading_msg.payload[0] = SAFETY_CMD_GET_STATUS;

    kilnlink_stack_margin_t margin_in;
    memset(&margin_in, 0, sizeof(margin_in));
    margin_in.rounds_completed = 3;
    margin_in.last_tick_ms = 12345;
    for (unsigned i = 0; i < KILNLINK_STACK_MARGIN_NUM_TASKS; i++) {
        margin_in.entries[i].task_id = (uint8_t)i;
        margin_in.entries[i].high_water_words = (uint16_t)(100 + i);
        margin_in.entries[i].stack_total_words = (uint16_t)(512 + i);
    }

    memset(&s_fake_inbox_delayed_msg, 0, sizeof(s_fake_inbox_delayed_msg));
    kilnlink_stack_margin_status_t enc_status = KILNLINK_STACK_MARGIN_OK;
    s_fake_inbox_delayed_msg.length =
        (uint16_t)kilnlink_stack_margin_encode(&margin_in, s_fake_inbox_delayed_msg.payload,
                                                sizeof(s_fake_inbox_delayed_msg.payload), &enc_status);
    TEST_CHECK(enc_status == KILNLINK_STACK_MARGIN_OK, "the delayed reply frame encodes cleanly");

    kilnlink_stack_margin_t out;
    memset(&out, 0, sizeof(out));
    esp_err_t err = safety_link_get_stack_margin(&link, &out);

    TEST_CHECK(err == ESP_OK, "a reply that arrives after a leading unrelated frame must still be "
                              "picked up within the call's own timeout budget, not dropped as ESP_ERR_TIMEOUT");
    TEST_CHECK(out.rounds_completed == 3 && out.last_tick_ms == 12345,
               "the decoded reply matches the delayed reply, not the leading unrelated frame");
    TEST_CHECK(out.entries[0].high_water_words == 100 && out.entries[8].high_water_words == 108,
               "per-task entries roundtrip through the delayed reply");

    /* Leave global stub state clean for every test after this one. */
    s_stub_broadcast_leading_push = false;
    fake_inbox_reset();
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

// --------------------------------------------------------------------------
// ROADMAP.md M15 TASK B3 -- synthetic-old-peer compatibility test. The
// prose judgement kilnlink_version.h's own comments re-argue at every bump
// ("does an old peer break loudly or silently?") is made build-checkable
// here: for every protocol version from KILNLINK_MIN_COMPATIBLE to the
// current KILNLINK_PROTOCOL_VERSION, this table enumerates which Pico->ESP
// frames a peer built at that version can actually send (derived from
// kilnlink_version.h's own per-bump history comments, cited per row below),
// and proves safety_drain_inbox_ex()'s real dispatch switch
// (safety_link_inbox.c) has an explicit case for every one of them -- a
// frame that would be silently dropped (payload[0] falls through to the
// switch's `default:` and increments stats.unmatched_cmd_count, see that
// branch's own comment) fails this test instead of merely reading as a
// hung link on the bench.
//
// Scope: this is the KilnFW (ESP) side ONLY -- the dispatch table that
// decides what happens to a frame the Pico sends. The mirror question (does
// SaftyFW's link_task.c dispatch have a case for every frame the ESP can
// send it) cannot be answered the same way from THIS tree: SaftyFW is a
// separate Pico-target firmware (firmware/SaftyFW), its link_task.c pulls
// in real RP2040 hardware/pico-sdk headers, and no host-test harness in
// this repo #includes it off-target the way test_safety_link_compile.c
// does for safety_link.c (see this file's own top-of-file header comment
// for the "compiles and links off-target" precedent this file follows --
// nothing equivalent has been built for the Pico side). Enumerating ITS
// dispatch table without linking real hardware code is therefore out of
// reach from here; closing that gap is a SaftyFW-side task, not this one.
//
// How each row was derived (kilnlink_version.h history, this file's own
// #include of kilnlink_version.h via safety_link.c above):
//   - GET_STATUS/FW_VERSION/UPDATE_STATUS/POWER/DIAG/TRIP_EVENT/CT_CAL/
//     CONFIG_PAGE/COMMIT_CONFIG_REJECTED all predate KILNLINK_MIN_COMPATIBLE
//     itself (7) -- none of their version-history entries describe them as
//     NEWLY introduced at 7 or later; the 6->7 bump only renumbered the
//     REQUEST ids (0x22/0x23/0x24, ESP->Pico, not part of this Pico->ESP
//     dispatch table) and left the reply ids (0x1A/0x1E/0x1F) unchanged,
//     and the 7->8 bump only added a bit WITHIN the existing 0x1F payload.
//     So every one of these is sendable by a peer at MIN_COMPATIBLE (7).
//   - ROLLBACK_RESULT (0x25): "8 -> 9 (2026-08-30): new Pico -> ESP frame,
//     SAFETY_CMD_ROLLBACK_RESULT (0x25) ... An ESP still on protocol 8 or
//     older simply never sees this frame" -- introduced at protocol 9,
//     gated by KILNLINK_ROLLBACK_RESULT_MIN_PROTOCOL (kilnlink_rollback_
//     result.h). KILNLINK_MIN_COMPATIBLE is explicitly NOT raised alongside
//     this bump (kilnlink_version.h: "NOT bumped alongside the 8 -> 9 step
//     above ... a peer built against 7 or 8 remains fully compatible"), so
//     a MIN_COMPATIBLE(7) peer genuinely may never send this frame -- this
//     is the one row where "gated above a version" has to hold.
//   - The 9 -> 10 bump (BORROWED status bytes) grew an EXISTING frame
//     (GET_STATUS) rather than introducing a new dispatched frame id, so it
//     adds no new row here -- safety_apply_status()'s own V1/V2/V3 length
//     handling (already covered by test_apply_status_v3_borrowed() above)
//     is the build-checkable half of THAT bump.
struct kilnlink_compat_frame {
    const char *name;
    uint8_t cmd;
    uint16_t min_version; /* first KILNLINK_PROTOCOL_VERSION this frame is sendable at */
    const char *citation;
};

static const struct kilnlink_compat_frame s_compat_frames[] = {
    {"GET_STATUS (Frame A)", SAFETY_CMD_GET_STATUS, 7, "predates MIN_COMPATIBLE=7"},
    {"FW_VERSION (Frame C)", SAFETY_CMD_FW_VERSION, 7, "predates MIN_COMPATIBLE=7"},
    {"UPDATE_STATUS", SAFETY_CMD_UPDATE_STATUS, 7, "predates MIN_COMPATIBLE=7"},
    {"POWER (Frame E)", SAFETY_CMD_POWER, 7, "predates MIN_COMPATIBLE=7"},
    {"DIAG (Frame B)", SAFETY_CMD_DIAG, 7, "predates MIN_COMPATIBLE=7"},
    {"TRIP_EVENT (Frame D)", SAFETY_CMD_TRIP_EVENT, 7, "predates MIN_COMPATIBLE=7"},
    {"CT_CAL reply (0x1A)", KILNLINK_CT_CAL_CMD, 7,
     "6->7: only the GET_* REQUEST id moved (0x22); the 0x1A reply id is unchanged"},
    {"CONFIG_PAGE reply (0x1F)", KILNLINK_CONFIG_PAGE_CMD, 7,
     "6->7: only the GET_* REQUEST id moved (0x24); the 0x1F reply id is unchanged "
     "(7->8 added the `set` bit inside the same 0x1F payload, no new id)"},
    {"COMMIT_CONFIG_REJECTED (0x20)", KILNLINK_COMMIT_CONFIG_REJECTED_CMD, 7, "predates MIN_COMPATIBLE=7"},
    {"ROLLBACK_RESULT (0x25)", KILNLINK_ROLLBACK_RESULT_CMD, KILNLINK_ROLLBACK_RESULT_MIN_PROTOCOL,
     "8->9: \"new Pico -> ESP frame, SAFETY_CMD_ROLLBACK_RESULT (0x25)\" -- "
     "KILNLINK_MIN_COMPATIBLE NOT raised alongside this bump"},
    {"STACK_MARGIN (0x2C)", KILNLINK_STACK_MARGIN_CMD, 13,
     "12->13: new Pico -> ESP frame, SAFETY_CMD_STACK_MARGIN (0x2C), reply to "
     "SAFETY_CMD_GET_STACK_MARGIN (0x2B) -- KILNLINK_MIN_COMPATIBLE NOT raised "
     "alongside this bump, a pre-13 Pico simply never sends 0x2C"},
};
#define COMPAT_FRAME_COUNT (sizeof(s_compat_frames) / sizeof(s_compat_frames[0]))

// Sends one frame with the given cmd byte through the REAL dispatch
// (safety_drain_inbox_ex(), safety_link_inbox.c) and returns true if it hit
// an explicit case (stats.unmatched_cmd_count did not move), false if it
// fell through to the switch's default: branch -- i.e. would be silently
// dropped on real hardware.
static bool compat_frame_has_dispatch_case(uint8_t cmd)
{
    SafetyLinkClass link = make_link();
    link.initialized = true;

    uart_proto_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.payload[0] = cmd;
    msg.length = 1; // enough to enter the switch; decode success is not what this proves

    fake_inbox_reset();
    fake_inbox_push(&msg);

    uint32_t before = link.stats.unmatched_cmd_count;
    (void)safety_drain_inbox_ex(&link, 0, false, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    return link.stats.unmatched_cmd_count == before;
}

static void test_dispatch_has_a_case_for_every_frame_each_compatible_version_can_send(void)
{
    TEST_SECTION("safety_drain_inbox_ex dispatch -- every frame a peer at any version from "
                 "KILNLINK_MIN_COMPATIBLE to KILNLINK_PROTOCOL_VERSION can send has an explicit "
                 "dispatch case (positive direction: nothing a compatible peer sends is silently dropped)");

    for (uint16_t version = KILNLINK_MIN_COMPATIBLE; version <= KILNLINK_PROTOCOL_VERSION; version++) {
        for (size_t i = 0; i < COMPAT_FRAME_COUNT; i++) {
            const struct kilnlink_compat_frame *f = &s_compat_frames[i];
            if (version < f->min_version) {
                continue; // this peer version predates the frame -- covered by the reverse test below
            }
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "protocol %u peer can send %s (introduced at %u, %s) -- current dispatch must "
                     "have an explicit case for cmd 0x%02X",
                     (unsigned)version, f->name, (unsigned)f->min_version, f->citation, (unsigned)f->cmd);
            TEST_CHECK(compat_frame_has_dispatch_case(f->cmd), msg);
        }
    }
}

static void test_frames_gated_above_a_version_are_not_expected_from_that_peer(void)
{
    TEST_SECTION("reverse direction -- a frame gated to protocol >= min_version must NOT be "
                 "counted as something an older peer (below that gate) can send, even though "
                 "the current build's dispatch happens to have a case for it (additive-safe)");

    for (size_t i = 0; i < COMPAT_FRAME_COUNT; i++) {
        const struct kilnlink_compat_frame *f = &s_compat_frames[i];
        for (uint16_t version = KILNLINK_MIN_COMPATIBLE; version < f->min_version; version++) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "protocol %u peer predates %s (introduced at %u, %s) -- must NOT be in that "
                     "version's expected-frame set",
                     (unsigned)version, f->name, (unsigned)f->min_version, f->citation);
            TEST_CHECK(version < f->min_version, msg); // table-construction tautology made explicit/checkable
        }
    }

    // The one real row this closes: ROLLBACK_RESULT's gate must sit STRICTLY
    // above KILNLINK_MIN_COMPATIBLE, or a "MIN_COMPATIBLE peer never sends
    // this" claim above would be false for the oldest peer this build still
    // accepts at all.
    TEST_CHECK(KILNLINK_ROLLBACK_RESULT_MIN_PROTOCOL > KILNLINK_MIN_COMPATIBLE,
               "ROLLBACK_RESULT's MIN_PROTOCOL gate (9) sits strictly above KILNLINK_MIN_COMPATIBLE (7) -- "
               "the oldest peer this build still talks to is never expected to send it");
}

// ---------------------------------------------------------------------
// ROADMAP.md (2026-09-04) -- "Link-loss heating block not bypassed during a
// Pico update". These call the REAL, compiled relay_authority_on_blocked()
// against a REAL SafetyLinkClass whose fault_sources this test drives
// through safety_link.c's own public safety_link_set_fault_source()/
// safety_link_set_update_in_progress() setters -- not a re-implementation of
// either function's rule (this repo has shipped that mistake before; see
// the durable memory's negative-test-every-check note). The property:
// update_in_progress_quiet (safety_link.h's own doc comment on that field)
// is documented to affect ONLY which log line safety_update_health() emits
// for an already-down link, never fault_sources itself -- these tests pin
// that in the one place that actually decides whether a relay may turn ON.

static void test_update_in_progress_does_not_relax_link_loss_block(void)
{
    TEST_SECTION("relay_authority_on_blocked() -- a link-loss fault stays blocked "
                 "while a Pico update is in progress (ROADMAP.md 2026-09-04): "
                 "safety_link_set_update_in_progress() must not relax it");

    SafetyLinkClass link = make_link();
    link.initialized = true;

    // No fault yet, no update in progress: heat is not blocked by this path.
    uint32_t sources = 0xFFFFFFFFu; // poisoned, so a no-op setter is visible below
    TEST_CHECK(relay_authority_on_blocked(&link, &sources) == false,
               "sanity: a freshly-initialized link with no asserted fault source is not blocked");
    TEST_CHECK(sources == 0u, "out_sources reads back 0 when nothing is asserted");

    // Simulate link loss the same way safety_update_health() actually does
    // on a real stale link: assert SAFETY_FAULT_SRC_SAFETY_LINK through the
    // real public setter, not by poking link.fault_sources directly.
    TEST_CHECK(safety_link_set_fault_source(&link, SAFETY_FAULT_SRC_SAFETY_LINK, true) == ESP_OK,
               "asserting the link-loss fault source succeeds");
    TEST_CHECK(relay_authority_on_blocked(&link, NULL) == true,
               "link-loss fault source alone already blocks relay-ON, before any update starts");

    // This is the exact call ota_pico_relay.c's relay task makes right
    // before the link legitimately goes quiet for a Pico update.
    TEST_CHECK(safety_link_set_update_in_progress(&link, true) == ESP_OK,
               "safety_link_set_update_in_progress(true) succeeds");

    uint32_t sources_during_update = 0;
    TEST_CHECK(relay_authority_on_blocked(&link, &sources_during_update) == true,
               "REGRESSION PIN: a link-loss fault STILL blocks relay-ON while an update is "
               "in progress -- an update must never become a path that relaxes the link-loss "
               "heating block");
    TEST_CHECK((sources_during_update & SAFETY_FAULT_SRC_SAFETY_LINK) != 0u,
               "the fault-source bitmask itself is untouched by update_in_progress -- "
               "update_in_progress_quiet is documented to affect log text only");
    TEST_CHECK(safety_link_get_fault_sources(&link) == SAFETY_FAULT_SRC_SAFETY_LINK,
               "safety_link_get_fault_sources() reports exactly the same fault source "
               "during the update as it did before -- set_update_in_progress changed nothing here");

    // Ending the update (the relay task's `done:` label, success or
    // failure alike) must not silently clear the still-asserted fault
    // either -- only an actual link recovery should do that.
    TEST_CHECK(safety_link_set_update_in_progress(&link, false) == ESP_OK,
               "safety_link_set_update_in_progress(false) succeeds");
    TEST_CHECK(relay_authority_on_blocked(&link, NULL) == true,
               "still blocked after the update ends -- the underlying fault was never actually "
               "cleared, only the update flag was");
}

static void test_link_loss_during_update_denies_heat_end_to_end(void)
{
    TEST_SECTION("relay_authority_on_blocked() -- a link that goes stale WHILE an update is "
                 "already in progress is denied heat exactly the same as any other link-loss, "
                 "not treated specially because an update happens to be running");

    SafetyLinkClass link = make_link();
    link.initialized = true;

    // Update starts on a healthy link: not blocked yet.
    TEST_CHECK(safety_link_set_update_in_progress(&link, true) == ESP_OK, "update begins");
    TEST_CHECK(relay_authority_on_blocked(&link, NULL) == false,
               "update-in-progress alone, with no fault asserted, does not spuriously block "
               "heat -- this flag really is inert outside the fault-source path");

    // The link then genuinely goes stale mid-update (UPDATE_PROTOCOL.md's
    // "the Pico update deliberately trips the liveness rule").
    TEST_CHECK(safety_link_set_fault_source(&link, SAFETY_FAULT_SRC_SAFETY_LINK, true) == ESP_OK,
               "link-loss fault asserts mid-update");
    uint32_t sources = 0;
    TEST_CHECK(relay_authority_on_blocked(&link, &sources) == true,
               "heat is denied the moment the link goes stale, update in progress or not");
    TEST_CHECK(sources == SAFETY_FAULT_SRC_SAFETY_LINK,
               "the reported source is exactly the link-loss bit, not something update-specific");
}

// --------------------------------------------------------------------------
// HW_ABSTRACTION.md "Still open", 2026-09-06: link_reply_us. safety_exchange()
// (safety_link_inbox.c) now times the request/reply exchange itself with
// hal_time_now_us() -- these tests are the first coverage of that function's
// blocking path at all (this file's own header comment used to list it as
// explicitly out of scope; the controllable uart_protocol_send_broadcast/
// uart_protocol_receive stubs above make it reachable now). Uses fake_time.c
// (App/test's existing hal_time.h host fake) as the clock; per that header's
// own "must not reintroduce the idealized-input bug class" contract, the two
// advances below are deliberately non-round (3001us, 987us), not 1000/2000.
// --------------------------------------------------------------------------

static void reset_link_reply_us_test_state(void)
{
    fake_inbox_reset();
    fake_time_reset_all();
    s_stub_broadcast_send_succeeds = false;
    s_stub_broadcast_reply_push = false;
    s_stub_broadcast_advance_us = 0;
}

static void set_stub_status_reply(void)
{
    memset(&s_stub_broadcast_reply_msg, 0, sizeof(s_stub_broadcast_reply_msg));
    set_status_frame(s_stub_broadcast_reply_msg.payload, (uint8_t)SAFETY_FLAG_TEMP_VALID,
                      123.5f, 24.0f, 0, 1.0f, 2.0f, 3.0f);
    s_stub_broadcast_reply_msg.length = SAFETY_LINK_STATUS_FRAME_LEN_V1;
}

static void test_link_reply_us_records_a_matched_exchange(void)
{
    TEST_SECTION("safety_exchange() -- link_reply_us records count/last/min/max/mean across "
                 "two real request/reply round trips, timed on the fake clock");

    reset_link_reply_us_test_state();
    SafetyLinkClass link = make_link();
    link.initialized = true;
    // make_link() is a bare memset(0), not full safety_link_start() bring-up
    // -- real bring-up seeds this to UINT32_MAX (safety_link.c) specifically
    // so the first real sample can ever beat it; without this line here a
    // memset-0 min would never update (elapsed < 0 is never true).
    link.stats.link_reply_us_min = UINT32_MAX;

    s_stub_broadcast_send_succeeds = true;
    s_stub_broadcast_reply_push = true;
    set_stub_status_reply();
    s_stub_broadcast_advance_us = 3001; // first round trip: 3001us

    const uint8_t request[] = { SAFETY_CMD_GET_STATUS };
    esp_err_t err = safety_exchange(&link, request, sizeof(request), true);

    TEST_CHECK(err == ESP_OK, "first exchange succeeds -- the reply was pushed and matched");
    TEST_CHECK(link.stats.link_reply_us_count == 1, "one matched exchange counted");
    TEST_CHECK(link.stats.link_reply_us_last == 3001, "last == the exact simulated round trip");
    TEST_CHECK(link.stats.link_reply_us_min == 3001, "min == the only sample so far");
    TEST_CHECK(link.stats.link_reply_us_max == 3001, "max == the only sample so far");
    TEST_CHECK(link.stats.link_reply_us_mean == 3001, "mean == the only sample so far");
    TEST_CHECK(link.stats.timeouts == 0, "a matched reply is not counted as a timeout");

    // Second round trip, a different (non-round) duration -- proves min/max/
    // mean are tracked across calls, not just latched from the first one.
    set_stub_status_reply();
    s_stub_broadcast_advance_us = 987;
    err = safety_exchange(&link, request, sizeof(request), true);

    TEST_CHECK(err == ESP_OK, "second exchange also succeeds");
    TEST_CHECK(link.stats.link_reply_us_count == 2, "two matched exchanges counted");
    TEST_CHECK(link.stats.link_reply_us_last == 987, "last updates to the second, faster reply");
    TEST_CHECK(link.stats.link_reply_us_min == 987, "min drops to the faster of the two");
    TEST_CHECK(link.stats.link_reply_us_max == 3001, "max stays at the slower of the two");
    TEST_CHECK(link.stats.link_reply_us_mean == (3001 + 987) / 2,
               "mean is the running average of both samples, not just the latest");
}

// Negative-test companion (project convention: prove a check CAN fail, not
// just that it passes on the happy path -- feedback_negative_test_every_
// check.md). This exercises the real "no matching reply" path -- the
// request is sent successfully but nothing answers -- and pins that
// link_reply_us must NOT advance in that case (the exchange has nothing to
// time).
//
// UPDATED 2026-09-10 (finding 5, the follow-up opus review on docs/audits/
// safety_link_get_status_timeout_counter_2026-09-10.md): this test used to
// also pin that a miss advances `stats.timeouts` by 1 directly from
// safety_exchange(). That is no longer true, deliberately -- stats.timeouts
// is now written exclusively by safety_poll_task()'s per-iteration push-gap
// check (safety_link_poll.c), which this file's own header comment
// documents as out of scope (it needs the real FreeRTOS-timed loop, not
// just this one safety_exchange() call). safety_exchange() itself no longer
// touches stats.timeouts at all -- see that field's own doc comment
// (safety_link.h) and safety_exchange()'s implementation (safety_link_
// inbox.c) for why.
static void test_link_reply_us_not_recorded_when_nothing_answers(void)
{
    TEST_SECTION("safety_exchange() -- REGRESSION PIN: a request that is sent but never "
                 "answered must NOT advance link_reply_us (there is no reply to time), and "
                 "must NOT touch stats.timeouts either (that counter moved to safety_poll_task())");

    reset_link_reply_us_test_state();
    SafetyLinkClass link = make_link();
    link.initialized = true;

    s_stub_broadcast_send_succeeds = true;
    s_stub_broadcast_reply_push = false; // nothing lands in the inbox -- fake_inbox stays empty

    const uint8_t request[] = { SAFETY_CMD_GET_STATUS };
    esp_err_t err = safety_exchange(&link, request, sizeof(request), true);

    TEST_CHECK(err == ESP_ERR_TIMEOUT, "no reply arrives -- the exchange reports a timeout");
    TEST_CHECK(link.stats.link_reply_us_count == 0,
               "link_reply_us_count is untouched -- no matched reply to time");
    TEST_CHECK(link.stats.link_reply_us_last == 0, "last is untouched (still its zeroed initial value)");
    TEST_CHECK(link.stats.timeouts == 0,
               "safety_exchange() no longer increments timeouts at all -- that accounting "
               "moved to safety_poll_task()'s elapsed-time push-gap check");
}

// --------------------------------------------------------------------------
// 2026-09-27 (starved-ack fix): SAFETY_FAULT_MIN_HOLD_MS (safety_link.h) --
// a fault source that has been asserted must stay asserted for at least this
// long before a deassert request may actually release it, so a fast ack from
// KilnFW (escalate_guard_trip() -> profile_executor_halt() -> clear_this_
// runs_faults(), all in the same task, milliseconds apart) cannot starve the
// RP2040 safety processor's own SAFTYFW_MAIN_FAULT_DEBOUNCE_MS-based S6a
// latch. Uses fake_time.h (hal_time_now_ms()'s host fake), not the FreeRTOS
// tick stub -- this file's own header comment documents xTaskGetTickCount()
// as frozen at 0 in this build, which would make any tick-based elapsed-time
// assertion vacuously true. Advances are deliberately non-round (299ms,
// 301ms rather than 300ms flat) per fake_time.h's own "must not reintroduce
// the idealized-input bug class" contract, except where the test is
// specifically pinning the exact boundary.
// --------------------------------------------------------------------------

static void reset_fault_hold_test_state(void)
{
    fake_time_reset_all();
}

static void test_fault_hold_immediate_deassert_stays_high_until_hold_elapses(void)
{
    TEST_SECTION("SAFETY_FAULT_MIN_HOLD_MS -- assert then immediate deassert: the line "
                 "stays high (fault_sources still set) until the hold time elapses");

    reset_fault_hold_test_state();
    SafetyLinkClass link = make_link();
    link.initialized = true;
    // Drive a real (fake) output pin so the checks below pin the PHYSICAL
    // line level safety_apply_fault_locked() writes, not only the mask.
    fake_gpio_reset();
    link.fault_io = 6;
    TEST_CHECK(hal_gpio_init_out(link.fault_io, false) == HAL_OK, "fault pin init");

    TEST_CHECK(safety_link_set_fault_source(&link, SAFETY_FAULT_SRC_APP, true) == ESP_OK,
               "assert succeeds");
    TEST_CHECK(link.fault_sources == SAFETY_FAULT_SRC_APP, "line is asserted");
    TEST_CHECK(fake_gpio_current_level(link.fault_io), "physical fault pin driven high");

    // Immediate deassert request, same instant (0ms elapsed) -- must be
    // deferred, not applied, per the whole point of this fix.
    TEST_CHECK(safety_link_set_fault_source(&link, SAFETY_FAULT_SRC_APP, false) == ESP_OK,
               "deassert request itself still returns ESP_OK -- it is accepted, just deferred");
    TEST_CHECK(link.fault_sources == SAFETY_FAULT_SRC_APP,
               "REGRESSION PIN: the line stays asserted immediately after a fast deassert -- "
               "this is the exact starved-ack scenario the fix exists for");
    TEST_CHECK((link.fault_pending_deassert_mask & SAFETY_FAULT_SRC_APP) != 0u,
               "the bit is parked pending, not silently dropped");
    TEST_CHECK(fake_gpio_current_level(link.fault_io),
               "REGRESSION PIN: physical fault pin still high after the fast deassert");

    // The poll-tick servicer must not release it early either.
    fake_time_advance_ms(299); // non-round, just under the 300ms hold
    safety_link_service_pending_fault_deassert(&link);
    TEST_CHECK(link.fault_sources == SAFETY_FAULT_SRC_APP,
               "still asserted 299ms in -- one ms short of the 300ms minimum hold");

    // Cross the threshold; the servicer (the poll tick, not the original
    // caller) is what actually releases it.
    fake_time_advance_ms(2); // now 301ms since the original assert
    safety_link_service_pending_fault_deassert(&link);
    TEST_CHECK(link.fault_sources == 0u,
               "released once the hold time has elapsed, via the poll-tick servicer");
    TEST_CHECK(link.fault_pending_deassert_mask == 0u, "pending mask cleared along with it");
    TEST_CHECK(!fake_gpio_current_level(link.fault_io),
               "physical fault pin released low by the servicer");
}

static void test_fault_hold_reassert_during_hold_cancels_pending_deassert(void)
{
    TEST_SECTION("SAFETY_FAULT_MIN_HOLD_MS -- a re-assert during the hold window cancels "
                 "the pending deassert and keeps the line high");

    reset_fault_hold_test_state();
    SafetyLinkClass link = make_link();
    link.initialized = true;

    TEST_CHECK(safety_link_set_fault_source(&link, SAFETY_FAULT_SRC_APP, true) == ESP_OK,
               "assert succeeds");
    fake_time_advance_ms(37); // non-round, well inside the hold window
    TEST_CHECK(safety_link_set_fault_source(&link, SAFETY_FAULT_SRC_APP, false) == ESP_OK,
               "deassert request deferred");
    TEST_CHECK((link.fault_pending_deassert_mask & SAFETY_FAULT_SRC_APP) != 0u,
               "pending, as expected");

    // Re-assert before the hold elapses: must cancel the pending release.
    TEST_CHECK(safety_link_set_fault_source(&link, SAFETY_FAULT_SRC_APP, true) == ESP_OK,
               "re-assert succeeds");
    TEST_CHECK(link.fault_sources == SAFETY_FAULT_SRC_APP, "still asserted");
    TEST_CHECK((link.fault_pending_deassert_mask & SAFETY_FAULT_SRC_APP) == 0u,
               "REGRESSION PIN: the re-assert cancels the pending deassert -- it must not "
               "still fire later just because it was queued before the re-assert");

    // Even well past the ORIGINAL assert's hold time, the line must stay up,
    // since the servicer has nothing pending to act on any more.
    fake_time_advance_ms(500);
    safety_link_service_pending_fault_deassert(&link);
    TEST_CHECK(link.fault_sources == SAFETY_FAULT_SRC_APP,
               "still asserted long after the original hold window -- the cancelled deassert "
               "never comes back on its own");
}

static void test_fault_hold_deassert_after_hold_elapsed_is_immediate(void)
{
    TEST_SECTION("SAFETY_FAULT_MIN_HOLD_MS -- a deassert requested AFTER the hold time has "
                 "already elapsed is applied immediately, with no deferral at all");

    reset_fault_hold_test_state();
    SafetyLinkClass link = make_link();
    link.initialized = true;

    TEST_CHECK(safety_link_set_fault_source(&link, SAFETY_FAULT_SRC_APP, true) == ESP_OK,
               "assert succeeds");
    fake_time_advance_ms(613); // non-round, well past the 300ms hold

    TEST_CHECK(safety_link_set_fault_source(&link, SAFETY_FAULT_SRC_APP, false) == ESP_OK,
               "deassert succeeds");
    TEST_CHECK(link.fault_sources == 0u,
               "released immediately -- the hold time had already elapsed, so there is nothing "
               "to defer");
    TEST_CHECK(link.fault_pending_deassert_mask == 0u,
               "nothing was ever parked pending for this bit");
}

static void test_fault_hold_is_independent_per_bit(void)
{
    TEST_SECTION("SAFETY_FAULT_MIN_HOLD_MS -- one source's hold timer does not affect "
                 "another source's own independent hold");

    reset_fault_hold_test_state();
    SafetyLinkClass link = make_link();
    link.initialized = true;

    TEST_CHECK(safety_link_set_fault_source(&link, SAFETY_FAULT_SRC_APP, true) == ESP_OK,
               "APP source asserts");
    fake_time_advance_ms(250); // non-round; APP is now 250ms into its hold
    TEST_CHECK(safety_link_set_fault_source(&link, SAFETY_FAULT_SRC_MANUAL, true) == ESP_OK,
               "MANUAL source asserts later -- its own hold starts from here, not from APP's");

    // Deassert both now: APP has not yet held 300ms (only 250ms), MANUAL has
    // held 0ms -- both must defer.
    TEST_CHECK(safety_link_set_fault_source(&link, SAFETY_FAULT_SRC_APP | SAFETY_FAULT_SRC_MANUAL,
                                             false) == ESP_OK,
               "combined deassert request accepted");
    TEST_CHECK(link.fault_sources == (SAFETY_FAULT_SRC_APP | SAFETY_FAULT_SRC_MANUAL),
               "both still asserted -- neither has held long enough yet");

    // Advance so APP crosses 300ms total (250 + 51 = 301) but MANUAL has only
    // been waiting 51ms of its own, independent hold.
    fake_time_advance_ms(51);
    safety_link_service_pending_fault_deassert(&link);
    TEST_CHECK((link.fault_sources & SAFETY_FAULT_SRC_APP) == 0u,
               "APP releases once ITS OWN 300ms hold (from its own assert time) elapses");
    TEST_CHECK((link.fault_sources & SAFETY_FAULT_SRC_MANUAL) != 0u,
               "MANUAL stays asserted -- its own hold, timed from its own later assert, has not "
               "elapsed yet, and APP's timer must not have been used for it");
}

// Negative-test companion, finding 4 (same follow-up review): an earlier
// version of this fix gated safety_exchange()'s returned `err` on safety_
// link_up_locked(), so a miss on a link that still read "up" by that ~1500 ms
// age check returned ESP_OK -- silently laundering safety_link_ping() into a
// restatement of the age check it exists to independently corroborate. This
// pins the fix: even with the link reading UP (ever_received=true,
// cached_tick=0 -- see this file's own "xTaskGetTickCount() always returns 0"
// header comment a few tests up), a miss must still report ESP_ERR_TIMEOUT.
// Manually confirmed this test fails if the miss branch is changed back to
// `if (!safety_link_up_locked(link)) { err = ESP_ERR_TIMEOUT; }`.
static void test_exchange_timeout_is_reported_even_when_link_reads_up(void)
{
    TEST_SECTION("safety_exchange() -- REGRESSION PIN (finding 4): a miss reports "
                 "ESP_ERR_TIMEOUT even when safety_link_up_locked() would say the link is up -- "
                 "safety_link_ping() must stay independent evidence, not a restatement of the age check");

    reset_link_reply_us_test_state();
    SafetyLinkClass link = make_link();
    link.initialized = true;
    link.ever_received = true;
    link.cached_tick = 0; // reads as age 0 under the stub tick clock -- link reads UP
    link.poll_period_ms = 500;

    TEST_CHECK(safety_link_up_locked(&link) == true,
               "sanity check: this link genuinely reads up by the age-based liveness check");

    s_stub_broadcast_send_succeeds = true;
    s_stub_broadcast_reply_push = false; // nothing lands in the inbox this exchange

    const uint8_t request[] = { SAFETY_CMD_GET_STATUS };
    esp_err_t err = safety_exchange(&link, request, sizeof(request), true);

    TEST_CHECK(err == ESP_ERR_TIMEOUT,
               "a miss is reported as a timeout regardless of the link's age-based liveness state");
}

// HIGH 1 of the adversarial review of 60d6552f: the deferred Pico-half
// recapture must not be able to stall the safety-link heartbeat.
//
// safety_poll_task is the SOLE sender of the ESP->Pico GET_STATUS heartbeat
// (safety_poll_task()'s "DO NOT DELETE THIS SEND" comment), and
// safety_poll_service_pico_half_recapture() is called immediately before that
// send. Whatever time the service call consumes is time the heartbeat is
// late. The Pico's S6b LINK_DEAD guard fires at link_timeout_s, 10.0 s by
// default, so any recapture path that can hold this task for longer than that
// can spuriously trip a firing.
//
// The stubs at the top of this file model a pathologically slow flash job
// (STUB_SLOW_FLASH_JOB_US, ~11.99 s of fake time) rather than waiting on a
// real NVS write. On the parent commit the service call dispatched with
// uart_bridge_ext_run_on_flash_worker_timeout() and AWAITED the job -- the
// 50 ms cap there bounds only acquiring the worker, never the job itself --
// so the whole 11.99 s landed on this task and the first check below fails.
// With the non-blocking post it lands on bx_flash_worker instead.
#define SAFETY_LINK_DEAD_BUDGET_US 10000000ull

static void test_recapture_cannot_stall_the_heartbeat_task(void)
{
    TEST_SECTION("safety_poll: deferred recapture must not stall the S6b heartbeat (HIGH 1)");

    fake_time_reset_all();
    s_stub_recapture_pending = true;
    s_stub_autosave_calls = 0;
    s_stub_posted_fn = NULL;

    uint64_t before = fake_time_now_us();
    safety_poll_service_pico_half_recapture();
    uint64_t elapsed = fake_time_now_us() - before;

    TEST_CHECK(elapsed < SAFETY_LINK_DEAD_BUDGET_US,
               "servicing the deferred Pico-half recapture must not hold safety_poll_task for "
               "anything approaching link_timeout_s (10.0 s) -- this task is the only sender of "
               "the GET_STATUS heartbeat, and the send is the very next thing it does, so time "
               "spent here is heartbeat latency and trips S6b LINK_DEAD");
    TEST_CHECK(elapsed == 0,
               "the recapture dispatch must be genuinely fire-and-forget: the poll task must pay "
               "NO part of the flash job's duration, not merely less than the S6b budget");
    TEST_CHECK(s_stub_autosave_calls == 0,
               "the autosave must NOT have run on safety_poll_task -- it is an NVS write, and this "
               "task's 8192 B stack is PSRAM-backed (PSRAM stack + NVS = panic)");

    // The work is deferred, not dropped: the worker runs it on its own time.
    TEST_CHECK(s_stub_posted_fn != NULL,
               "the recapture job must have been posted to the flash worker, not silently skipped");
    stub_run_posted_job_as_the_worker_would();
    TEST_CHECK(s_stub_autosave_calls == 1,
               "the posted job still performs exactly one recapture autosave, on the flash worker's "
               "own internal-SRAM stack");
    TEST_CHECK(g_stub_autosave_dispatcher == NULL,
               "the POSTED recapture job must pass a NULL dispatcher identity: it is posted through "
               "the no-arg uart_bridge_ext_post_on_flash_worker() path, so it is by construction "
               "not the task that set an autosave target override -- passing any non-NULL handle "
               "here could steer this unrelated autosave into an in-flight import's incoming slot");

    s_stub_recapture_pending = false;
}

// --------------------------------------------------------------------------
// 2026-09-24 fault-edge instrumentation (safety_link.h's safety_fault_edge_t
// comment): the ring/counters that let a latched S6a mainFault be traced
// back to which ESP source flipped, and when, after the fact. These drive
// the real, compiled safety_link_set_fault_source()/safety_link_get_fault_
// edges() -- not a re-implementation of the ring logic.
// --------------------------------------------------------------------------

static void test_fault_edge_records_a_single_transition(void)
{
    TEST_SECTION("safety_link_get_fault_edges -- one assert then one clear records two edges");

    fake_time_reset_all();
    SafetyLinkClass link = make_link();
    link.initialized = true;

    TEST_CHECK(safety_link_set_fault_source(&link, SAFETY_FAULT_SRC_THERMO, true) == ESP_OK,
               "asserting THERMO succeeds");
    // 2026-09-27 (SAFETY_FAULT_MIN_HOLD_MS): this test is exercising the
    // fault-edge ring/counters, not the hold-time deferral (covered by its
    // own tests above) -- advance well past the hold so this clear is
    // recorded as an immediate edge, same as before that fix.
    fake_time_advance_ms(301);
    TEST_CHECK(safety_link_set_fault_source(&link, SAFETY_FAULT_SRC_THERMO, false) == ESP_OK,
               "clearing THERMO succeeds");

    safety_fault_edge_snapshot_t snap;
    TEST_CHECK(safety_link_get_fault_edges(&link, &snap) == ESP_OK, "get_fault_edges succeeds");
    TEST_CHECK(snap.count == 2, "exactly two transitions recorded");
    TEST_CHECK(snap.total_recorded == 2, "total_recorded matches count when the ring has not wrapped");

    TEST_CHECK(snap.entries[0].source_mask_before == 0u, "edge 0 before == 0");
    TEST_CHECK(snap.entries[0].source_mask_after == SAFETY_FAULT_SRC_THERMO, "edge 0 after == THERMO");
    TEST_CHECK(snap.entries[0].first_set_bit == 2u,
               "edge 0's first_set_bit is bit index 2 (SAFETY_FAULT_SRC_THERMO == 0x04)");

    TEST_CHECK(snap.entries[1].source_mask_before == SAFETY_FAULT_SRC_THERMO, "edge 1 before == THERMO");
    TEST_CHECK(snap.entries[1].source_mask_after == 0u, "edge 1 after == 0");
    TEST_CHECK(snap.entries[1].first_set_bit == 0xFFu,
               "edge 1 is a pure clear -- first_set_bit reports 0xFF, not a stale bit index");

    TEST_CHECK(snap.counts.rising_count[2] == 1, "THERMO's rising-edge counter is 1 (one assert only)");
    TEST_CHECK(snap.counts.last_rising_valid[2] == true, "THERMO's last-rising timestamp is now valid");
    TEST_CHECK(snap.counts.rising_count[0] == 0, "an untouched source's rising counter stays 0");
    TEST_CHECK(snap.counts.last_rising_valid[0] == false,
               "an untouched source's last-rising-valid flag stays false");
}

static void test_fault_edge_no_op_set_does_not_record(void)
{
    TEST_SECTION("safety_link_get_fault_edges -- a no-op set (already at that state) records nothing");

    SafetyLinkClass link = make_link();
    link.initialized = true;

    TEST_CHECK(safety_link_set_fault_source(&link, SAFETY_FAULT_SRC_APP, false) == ESP_OK,
               "clearing an already-clear source succeeds (it's a no-op, not an error)");

    safety_fault_edge_snapshot_t snap;
    TEST_CHECK(safety_link_get_fault_edges(&link, &snap) == ESP_OK, "get_fault_edges succeeds");
    TEST_CHECK(snap.count == 0, "no transition recorded for a mask that did not actually change");
    TEST_CHECK(snap.total_recorded == 0, "total_recorded agrees");
}

static void test_fault_edge_multi_bit_assert_reports_lowest_first_set_bit(void)
{
    TEST_SECTION("safety_link_get_fault_edges -- asserting two sources in one call reports the "
                 "LOWEST bit as first_set_bit and counts BOTH rising edges");

    SafetyLinkClass link = make_link();
    link.initialized = true;

    TEST_CHECK(safety_link_set_fault_source(&link, SAFETY_FAULT_SRC_PC_LINK | SAFETY_FAULT_SRC_APP, true) ==
                   ESP_OK,
               "asserting two sources in one call succeeds");

    safety_fault_edge_snapshot_t snap;
    TEST_CHECK(safety_link_get_fault_edges(&link, &snap) == ESP_OK, "get_fault_edges succeeds");
    TEST_CHECK(snap.count == 1, "one edge recorded for one call, even though two bits rose");
    TEST_CHECK(snap.entries[0].first_set_bit == 1u,
               "first_set_bit is PC_LINK's bit index (1), the lower of the two that rose (APP is bit 4)");
    TEST_CHECK(snap.counts.rising_count[1] == 1, "PC_LINK's own rising counter incremented");
    TEST_CHECK(snap.counts.rising_count[4] == 1, "APP's own rising counter ALSO incremented");
}

static void test_fault_edge_ring_wraps_and_keeps_oldest_to_newest_order(void)
{
    TEST_SECTION("safety_link_get_fault_edges -- wraparound: more than RING_LEN transitions keeps "
                 "only the most recent RING_LEN, in oldest-to-newest order, while total_recorded "
                 "keeps counting past the ring's capacity");

    fake_time_reset_all();
    SafetyLinkClass link = make_link();
    link.initialized = true;

    // Alternate MANUAL on/off. Each call is one transition (one edge), so
    // driving this SAFETY_LINK_FAULT_EDGE_RING_LEN + 5 times wraps the ring
    // by exactly 5 entries. 2026-09-27 (SAFETY_FAULT_MIN_HOLD_MS): each
    // deassert must be requested well past the hold time from its own
    // assert, or it would be deferred rather than recorded as an immediate
    // edge -- this test is exercising the ring/counters, not the hold-time
    // deferral (covered by its own tests above).
    uint32_t total_transitions = SAFETY_LINK_FAULT_EDGE_RING_LEN + 5u;
    for (uint32_t i = 0; i < total_transitions; i++) {
        bool assert_now = (i % 2u) == 0u;
        fake_time_advance_ms(301);
        TEST_CHECK(safety_link_set_fault_source(&link, SAFETY_FAULT_SRC_MANUAL, assert_now) == ESP_OK,
                   "each alternating transition succeeds");
    }

    safety_fault_edge_snapshot_t snap;
    TEST_CHECK(safety_link_get_fault_edges(&link, &snap) == ESP_OK, "get_fault_edges succeeds");
    TEST_CHECK(snap.count == SAFETY_LINK_FAULT_EDGE_RING_LEN,
               "count caps at the ring length once more transitions than that have occurred");
    TEST_CHECK(snap.total_recorded == total_transitions,
               "total_recorded keeps the true lifetime count, proving 5 older entries were "
               "overwritten rather than the ring silently growing");

    // The oldest kept entry is transition index 5 (0-indexed: transitions
    // 0..4 were overwritten). Transition i asserts iff i is even, so
    // transition 5 is a CLEAR (before=MANUAL, after=0).
    TEST_CHECK(snap.entries[0].source_mask_before == SAFETY_FAULT_SRC_MANUAL,
               "oldest surviving entry is transition #5, a clear (before == MANUAL)");
    TEST_CHECK(snap.entries[0].source_mask_after == 0u, "oldest surviving entry's after == 0");
    // The newest entry is the very last transition driven above (index
    // total_transitions-1 == 20, which is EVEN -- an assert, since even
    // indices assert).
    TEST_CHECK(snap.entries[snap.count - 1].source_mask_after == SAFETY_FAULT_SRC_MANUAL,
               "newest entry is the last transition driven (an assert, since index 20 is even)");
    TEST_CHECK(snap.counts.rising_count[0] == (total_transitions + 1u) / 2u,
               "MANUAL's rising-edge counter reflects every assert across the whole run, "
               "including the ones the ring itself no longer holds -- the counters never wrap "
               "along with the ring");
}

// ---------------------------------------------------------------------
// 2026-09-28 review: boot-clear bounded-retry tests. The old s_boot_clear_
// attempted one-shot latched permanently on the first ESP_OK send, so a
// Pico refusal (its own S6a release debounce not yet elapsed) burned the
// only attempt and left a genuinely-releasable stale S6a latched for the
// rest of the boot. These drive safety_apply_diag()/safety_link_service_
// boot_clear_if_pending() directly, with s_fake_tick_count standing in for
// xTaskGetTickCount() (see this file's own macro-shadow comment above the
// #include block).

// Common setup: a fresh link with a cached STATUS frame (so safety_link_
// send_clear_trip()'s diag-age staleness check, measured off cached_tick,
// never refuses locally) and boot-clean armed. Resets every one-shot/retry
// static this feature owns so tests don't see a previous test's state --
// same "explicit per-test reset" convention as s_stub_broadcast_* above.
static SafetyLinkClass boot_clear_test_setup(void)
{
    SafetyLinkClass link = make_link();
    link.initialized = true;
    s_fake_tick_count = 0;
    s_stub_broadcast_send_succeeds = true;
    s_stub_broadcast_reply_push = false;
    s_stub_broadcast_leading_push = false;

    uart_proto_message_t status;
    memset(&status, 0, sizeof(status));
    set_status_frame(status.payload, 0, 20.0f, 20.0f, 0, 0, 0, 0);
    status.length = SAFETY_LINK_STATUS_FRAME_LEN_V1;
    TEST_CHECK(safety_apply_status(&link, &status) == true, "status frame primes cached_tick");

    safety_link_mark_boot_clean(); // resets s_boot_clear_attempts/last_attempt_tick too
    return link;
}

// Applies one TRIPPED/trip_reason DIAG frame and services whatever it
// queued. Returns the stub broadcast count immediately after. Refreshes the
// cached STATUS frame first (real STATUS traffic keeps arriving alongside
// DIAG on the real link) so safety_link_send_clear_trip()'s own diag-age
// staleness check (SAFETY_LINK_STALE_MS, 1500 ms) never refuses locally just
// because a test advanced s_fake_tick_count past it between attempts.
static unsigned drive_one_diag_and_service(SafetyLinkClass *link, uint8_t trip_reason)
{
    uart_proto_message_t status;
    memset(&status, 0, sizeof(status));
    set_status_frame(status.payload, 0, 20.0f, 20.0f, 0, 0, 0, 0);
    status.length = SAFETY_LINK_STATUS_FRAME_LEN_V1;
    safety_apply_status(link, &status);

    uart_proto_message_t diag;
    memset(&diag, 0, sizeof(diag));
    set_diag_frame(diag.payload, trip_reason, SAFETY_LINK_DIAG_STATE_TRIPPED);
    diag.length = SAFETY_LINK_DIAG_FRAME_LEN;
    TEST_CHECK(safety_apply_diag(link, &diag) == true, "DIAG frame decodes");
    safety_link_service_boot_clear_if_pending(link);
    return s_stub_broadcast_count;
}

static void test_boot_clear_refused_then_retried_succeeds(void)
{
    TEST_SECTION("safety_link_service_boot_clear_if_pending -- a Pico-refused clear "
                 "(DIAG still TRIPPED/MAIN_FAULT afterward) is retried after the spacing "
                 "gap rather than burned as a one-shot, and a later DIAG showing the trip "
                 "gone (Pico accepted it) needs no further send");

    SafetyLinkClass link = boot_clear_test_setup();
    unsigned base = s_stub_broadcast_count;

    // First DIAG: still tripped -- this is the boot-clear's very first
    // attempt, so it always sends regardless of spacing.
    TEST_CHECK(drive_one_diag_and_service(&link, SAFETY_LINK_TRIP_REASON_MAIN_FAULT) == base + 1,
               "first attempt sends CLEAR_TRIP");
    TEST_CHECK(s_boot_clear_attempts == 1, "one attempt is now counted");

    // "Refused": the Pico's DIAG still reports TRIPPED/MAIN_FAULT right
    // away, before the retry-spacing gap has elapsed. Must NOT resend.
    TEST_CHECK(drive_one_diag_and_service(&link, SAFETY_LINK_TRIP_REASON_MAIN_FAULT) == base + 1,
               "an immediate re-refusal within the spacing gap is not retried yet");
    TEST_CHECK(s_boot_clear_attempts == 1, "attempt count unchanged while inside the gap");

    // Advance past the spacing gap; still tripped -- now eligible to retry.
    s_fake_tick_count += SAFETY_LINK_BOOT_CLEAR_RETRY_GAP_MS;
    TEST_CHECK(drive_one_diag_and_service(&link, SAFETY_LINK_TRIP_REASON_MAIN_FAULT) == base + 2,
               "once the spacing gap has elapsed, a still-refused clear is retried");
    TEST_CHECK(s_boot_clear_attempts == 2, "second attempt now counted");

    // The Pico accepted this retry: the next DIAG no longer shows TRIPPED.
    // No further send should happen -- "confirmed acceptance" is exactly
    // this condition, not a separate flag.
    s_fake_tick_count += SAFETY_LINK_BOOT_CLEAR_RETRY_GAP_MS;
    uart_proto_message_t cleared;
    memset(&cleared, 0, sizeof(cleared));
    set_diag_frame(cleared.payload, 0, SAFETY_LINK_DIAG_STATE_ARMED);
    cleared.length = SAFETY_LINK_DIAG_FRAME_LEN;
    TEST_CHECK(safety_apply_diag(&link, &cleared) == true, "cleared DIAG frame decodes");
    safety_link_service_boot_clear_if_pending(&link);
    TEST_CHECK(s_stub_broadcast_count == base + 2, "no further send once the Pico's own DIAG shows "
                                                     "the trip is gone");
    TEST_CHECK(s_boot_clear_attempts == 2, "attempt count stays at 2 -- success needed no 3rd attempt");

    // Review fix: a NEW S6a arriving after the Pico accepted the clear, still
    // inside the 30 s window with a spare retry slot, must NOT be auto-cleared.
    s_fake_tick_count += SAFETY_LINK_BOOT_CLEAR_RETRY_GAP_MS;
    TEST_CHECK(drive_one_diag_and_service(&link, SAFETY_LINK_TRIP_REASON_MAIN_FAULT) == base + 2,
               "an S6a re-latched after an accepted clear is never auto-cleared by a leftover slot");
    TEST_CHECK(s_boot_clean == false, "accepted clear closes the boot-clean window for good");
}

static void test_boot_clear_stops_after_own_fault_source_rises(void)
{
    TEST_SECTION("safety_link_service_boot_clear_if_pending -- once a clear has gone out, "
                 "a rising edge on any of this board's own fault sources closes the retry "
                 "window, even after that source is released again (fault_sources back to 0)");

    fake_time_reset_all();
    SafetyLinkClass link = boot_clear_test_setup();
    unsigned base = s_stub_broadcast_count;

    TEST_CHECK(drive_one_diag_and_service(&link, SAFETY_LINK_TRIP_REASON_MAIN_FAULT) == base + 1,
               "first attempt sends CLEAR_TRIP");

    // This board genuinely asserts its fault line (e.g. a link blip or a PC
    // SET_FAULT), holds it past SAFETY_FAULT_MIN_HOLD_MS, then releases it.
    s_fake_tick_count += 500u;
    TEST_CHECK(safety_link_set_fault_source(&link, SAFETY_FAULT_SRC_MANUAL, true) == ESP_OK, "assert");
    fake_time_advance_us((uint64_t)(SAFETY_FAULT_MIN_HOLD_MS + 1u) * 1000u);
    TEST_CHECK(safety_link_set_fault_source(&link, SAFETY_FAULT_SRC_MANUAL, false) == ESP_OK, "release");
    TEST_CHECK(link.fault_sources == 0u, "fault_sources back to 0 -- the old gate alone would pass");

    s_fake_tick_count += SAFETY_LINK_BOOT_CLEAR_RETRY_GAP_MS;
    TEST_CHECK(drive_one_diag_and_service(&link, SAFETY_LINK_TRIP_REASON_MAIN_FAULT) == base + 1,
               "no retry once this board has raised a fault since the last attempt");
    TEST_CHECK(s_boot_clean == false, "window closed");
}

static void test_boot_clear_persistent_refusal_gives_up_after_bound(void)
{
    TEST_SECTION("safety_link_service_boot_clear_if_pending -- a persistently refused clear "
                 "(DIAG never stops showing TRIPPED/MAIN_FAULT) gives up after "
                 "SAFETY_LINK_BOOT_CLEAR_MAX_ATTEMPTS attempts and never floods the link");

    SafetyLinkClass link = boot_clear_test_setup();
    unsigned base = s_stub_broadcast_count;

    for (unsigned i = 0; i < SAFETY_LINK_BOOT_CLEAR_MAX_ATTEMPTS; i++) {
        unsigned expect = base + i + 1;
        TEST_CHECK(drive_one_diag_and_service(&link, SAFETY_LINK_TRIP_REASON_MAIN_FAULT) == expect,
                   "attempt N sends while under the bound");
        s_fake_tick_count += SAFETY_LINK_BOOT_CLEAR_RETRY_GAP_MS;
    }
    TEST_CHECK(s_boot_clear_attempts == SAFETY_LINK_BOOT_CLEAR_MAX_ATTEMPTS,
               "attempt count reaches exactly the bound");

    unsigned after_bound = s_stub_broadcast_count;
    // Keep offering a still-tripped DIAG, well past the spacing gap each
    // time -- a genuinely persistent trip must not be retried forever.
    for (unsigned i = 0; i < 5; i++) {
        s_fake_tick_count += SAFETY_LINK_BOOT_CLEAR_RETRY_GAP_MS;
        drive_one_diag_and_service(&link, SAFETY_LINK_TRIP_REASON_MAIN_FAULT);
    }
    TEST_CHECK(s_stub_broadcast_count == after_bound,
               "no further sends past the bound, no matter how long the trip persists");
    TEST_CHECK(s_boot_clear_attempts == SAFETY_LINK_BOOT_CLEAR_MAX_ATTEMPTS,
               "attempt count never exceeds the bound");
}

static void test_boot_clear_never_fires_for_a_non_s6a_trip(void)
{
    TEST_SECTION("safety_link_service_boot_clear_if_pending -- a trip reason other than "
                 "SAFETY_LINK_TRIP_REASON_MAIN_FAULT (S6a) is never cleared by the boot-clean "
                 "one-shot/retry mechanism, no matter how the DIAG otherwise looks");

    SafetyLinkClass link = boot_clear_test_setup();
    unsigned base = s_stub_broadcast_count;

    uint8_t non_s6a_reason = SAFETY_LINK_TRIP_REASON_MAIN_FAULT + 1u; // any other reason code
    for (unsigned i = 0; i < 3; i++) {
        drive_one_diag_and_service(&link, non_s6a_reason);
        s_fake_tick_count += SAFETY_LINK_BOOT_CLEAR_RETRY_GAP_MS;
    }
    TEST_CHECK(s_stub_broadcast_count == base, "no CLEAR_TRIP is ever sent for a non-S6a trip reason");
    TEST_CHECK(s_boot_clear_attempts == 0, "attempt count stays at 0 -- this trip is simply not "
                                            "this mechanism's to clear");
}

static void test_fault_edge_uninitialized_link_refuses(void)
{
    TEST_SECTION("safety_link_get_fault_edges -- an uninitialized link refuses rather than "
                 "returning garbage, and still zeroes *out");

    SafetyLinkClass link = make_link(); // initialized left false
    safety_fault_edge_snapshot_t snap;
    memset(&snap, 0xAA, sizeof(snap)); // poison, so a leftover garbage read is visible

    TEST_CHECK(safety_link_get_fault_edges(&link, &snap) == ESP_ERR_INVALID_STATE,
               "an uninitialized link is refused with ESP_ERR_INVALID_STATE");
    TEST_CHECK(snap.count == 0, "out is zeroed even on refusal -- never left holding poison/garbage");
}

int g_test_failures = 0;
int g_test_count = 0;

int main(void)
{
    TEST_SECTION("safety_link.c host build -- safety_apply_status() / safety_link_versions_compatible()");

    test_apply_status_accepts_v1_and_v2_lengths();
    test_apply_power_accepts_v1_and_v2_lengths_and_gates_counts_on_flag();
    test_apply_status_v3_borrowed();
    test_apply_status_v3_tc_config_reasserted();
    test_apply_status_v3_active_slot();
    test_safety_tc_is_separate_physical_sensor_predicate();
    test_apply_status_temp_valid_flag_is_sole_authority();
    test_apply_status_ignores_peer_link_up_and_fault_bits();
    test_k4_edge_counting_off_on_on_off_counts_two();
    test_k4_edge_counting_first_frame_counts_zero();
    test_k4_edge_counting_boot_id_change_counts_zero();
    test_fw_version_unknown_before_any_frame_arrives();
    test_fw_version_known_and_dirty_roundtrips();
    test_fw_version_frame_too_short_for_min_compatible_leaves_peer_unknown();
    test_fw_version_max_length_commit_and_datetime_no_truncation();
    test_fw_version_oversized_commit_len_is_capped();
    test_versions_compatible_is_two_sided();
    test_rollback_result_late_frame_is_stashed_not_dropped();
    test_rollback_result_frame_is_not_stashed_when_someone_is_waiting();
    test_param_frame_5_byte_found_zero_is_stashed();
    test_param_frame_9_byte_found_one_f32_is_stashed();
    test_param_frame_4_byte_too_short_is_not_stashed();
    test_param_frame_10_byte_too_long_is_not_stashed();
    test_get_param_drops_reply_with_mismatched_param_id();
    test_get_param_succeeds_with_matching_param_id();
    test_get_param_survives_a_leading_unrelated_frame();
    test_get_param_no_reply_reports_timeout();
    test_get_stack_margin_survives_a_leading_unrelated_frame();
    test_stale_reset_leaves_flags_alone_while_link_is_up();
    test_stale_reset_clears_flags_once_link_is_observed_down();
    test_stale_reset_never_received_is_also_down();
    test_stale_reset_then_reapply_recovers_after_reconnect();
    test_dispatch_has_a_case_for_every_frame_each_compatible_version_can_send();
    test_frames_gated_above_a_version_are_not_expected_from_that_peer();
    test_update_in_progress_does_not_relax_link_loss_block();
    test_link_loss_during_update_denies_heat_end_to_end();
    test_link_reply_us_records_a_matched_exchange();
    test_link_reply_us_not_recorded_when_nothing_answers();
    test_fault_hold_immediate_deassert_stays_high_until_hold_elapses();
    test_fault_hold_reassert_during_hold_cancels_pending_deassert();
    test_fault_hold_deassert_after_hold_elapsed_is_immediate();
    test_fault_hold_is_independent_per_bit();
    test_exchange_timeout_is_reported_even_when_link_reads_up();
    test_recapture_cannot_stall_the_heartbeat_task();

    test_fault_edge_records_a_single_transition();
    test_fault_edge_no_op_set_does_not_record();
    test_fault_edge_multi_bit_assert_reports_lowest_first_set_bit();
    test_fault_edge_ring_wraps_and_keeps_oldest_to_newest_order();
    test_fault_edge_uninitialized_link_refuses();

    test_boot_clear_refused_then_retried_succeeds();
    test_boot_clear_stops_after_own_fault_source_rises();
    test_boot_clear_persistent_refusal_gives_up_after_bound();
    test_boot_clear_never_fires_for_a_non_s6a_trip();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
