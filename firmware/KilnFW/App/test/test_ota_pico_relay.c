// test_ota_pico_relay.c -- host test for ota_pico_relay.c's state machine.
//
// docs/PICO_AUTO_UPDATE_PLAN.md used to flag this: "ota_pico_relay.c's own
// state machine has no host test today (only its terminal-state recognition
// is exercised indirectly)." This closes that gap.
//
// Convention: #include the real .c file directly (same as test_ota_http.c,
// test_dashboard_status_http.c, test_safety_link_compile.c), so this
// executable gets its own private stub directory
// (test/stubs_ota_pico_relay/) whose /I precedes @hostTestsRsp's own /I
// list -- see build_host_tests.ps1's registration block for this exe and
// that directory's freertos/task.h for why a private, controllable fake
// clock is required (the shared stub's xTaskGetTickCount() is hardcoded to
// 0, which would make relay_wait_for_states()'s timeout branch literally
// unreachable and hang this executable on any unmatched wait).
//
// Driving the state machine: ota_pico_relay_start() is called for real (to
// exercise its own guard/populate logic), then relay_task_fn(NULL) is
// invoked directly -- the private xTaskCreate() stub never runs the task
// function itself, same convention as every other host test that
// #includes a FreeRTOS-task-shaped .c file.
//
// Scope note: pico_update_attempts_next_slot() (persist/pico_update_attempts.c)
// is a SEPARATE module ota_pico_relay.c never calls or references -- it is
// invoked only from pico_auto_update_boot.c, and already has its own host
// test (test_pico_update_attempts.c). "Retry/slot-alternation" coverage
// here is therefore scoped to what ota_pico_relay.c actually owns: its own
// RELAY_MAX_RETRANSMIT_ROUNDS gap-retry loop.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// --- Fakes for everything ota_pico_relay.c depends on outside itself ------

// esp_partition_t / esp_partition_find_first / esp_partition_read
#include "esp_partition.h"

#define FAKE_PART_SIZE (900u * 1024u)
static uint8_t g_fake_flash[FAKE_PART_SIZE];
static esp_partition_t g_fake_partition = {
    .address = 0,
    .size = FAKE_PART_SIZE,
    .label = "pico_img",
    .type = ESP_PARTITION_TYPE_DATA,
    .subtype = ESP_PARTITION_SUBTYPE_DATA_UNDEFINED,
    .encrypted = false,
};
static bool g_fake_partition_present = true;
static bool g_fake_partition_read_fail = false;

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                                 const char *label)
{
    (void)type;
    (void)subtype;
    if (!g_fake_partition_present) {
        return NULL;
    }
    if (label && strcmp(label, "pico_img") == 0) {
        return &g_fake_partition;
    }
    return NULL;
}

esp_err_t esp_partition_read(const esp_partition_t *partition, size_t src_offset, void *dst, size_t size)
{
    (void)partition;
    if (g_fake_partition_read_fail) {
        return ESP_FAIL;
    }
    if (src_offset + size > sizeof(g_fake_flash)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(dst, g_fake_flash + src_offset, size);
    return ESP_OK;
}

// Never called by this module, but esp_partition.h declares them --
// definitions needed to link.
esp_err_t esp_partition_write(const esp_partition_t *partition, size_t dst_offset, const void *src, size_t size)
{
    (void)partition;
    (void)dst_offset;
    (void)src;
    (void)size;
    return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *partition, size_t offset, size_t size)
{
    (void)partition;
    (void)offset;
    (void)size;
    return ESP_OK;
}
uint32_t esp_partition_get_main_flash_sector_size(void) { return 4096u; }
esp_partition_iterator_t esp_partition_find(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                             const char *label)
{
    (void)type;
    (void)subtype;
    (void)label;
    return NULL;
}
const esp_partition_t *esp_partition_get(esp_partition_iterator_t iterator)
{
    (void)iterator;
    return NULL;
}
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t iterator)
{
    (void)iterator;
    return NULL;
}
esp_err_t esp_partition_iterator_release(esp_partition_iterator_t iterator)
{
    (void)iterator;
    return ESP_OK;
}

// --- SafetyLinkClass / safety_link_* fakes ---------------------------------
#include "safety_link.h"

static SafetyLinkClass g_fake_link; // zero-initialized, never dereferenced -- every
                                     // safety_link_* call below is faked.

// safety_link_set_update_in_progress() call counter/last value.
static int g_set_update_in_progress_calls = 0;
static bool g_set_update_in_progress_last = false;

esp_err_t safety_link_set_update_in_progress(SafetyLinkClass *link, bool in_progress)
{
    (void)link;
    g_set_update_in_progress_calls++;
    g_set_update_in_progress_last = in_progress;
    return ESP_OK;
}

// Every frame ever sent, recorded so tests can assert on command bytes
// (e.g. UPDATE_ABORT was sent on a failure path, or a specific gap chunk
// offset was resent).
#define FAKE_SENT_FRAMES_MAX 256
typedef struct {
    uint8_t cmd;
    uint32_t len;
    uint8_t payload[1 + 4 + 248];
} fake_sent_frame_t;
static fake_sent_frame_t g_sent_frames[FAKE_SENT_FRAMES_MAX];
static int g_sent_frames_count = 0;

esp_err_t safety_link_send_update_frame(SafetyLinkClass *link, const uint8_t *payload, size_t length)
{
    (void)link;
    if (g_sent_frames_count < FAKE_SENT_FRAMES_MAX && length > 0 && length <= sizeof(g_sent_frames[0].payload)) {
        g_sent_frames[g_sent_frames_count].cmd = payload[0];
        g_sent_frames[g_sent_frames_count].len = (uint32_t)length;
        memcpy(g_sent_frames[g_sent_frames_count].payload, payload, length);
        g_sent_frames_count++;
    }
    return ESP_OK;
}

static int fake_sent_frames_count_cmd(uint8_t cmd)
{
    int n = 0;
    for (int i = 0; i < g_sent_frames_count; i++) {
        if (g_sent_frames[i].cmd == cmd) {
            n++;
        }
    }
    return n;
}

// FIFO queue of scripted safety_link_get_update_status() replies. Each
// entry's age_ms is always reported as 0 (so the freshness check in
// relay_wait_for_states() -- age_ms <= elapsed -- always passes immediately,
// since elapsed >= 0). Once exhausted, every subsequent call advances the
// fake clock by a large fixed amount and returns a non-ESP_OK error --
// guaranteeing any relay_wait_for_states() call relying on an exhausted
// queue times out for real on its very next loop iteration, regardless of
// its specific deadline.
#define FAKE_STATUS_QUEUE_MAX 32
static safety_link_update_status_t g_status_queue[FAKE_STATUS_QUEUE_MAX];
// Parallel "stale" marker per queued entry -- see fake_status_push_stale()'s
// own comment for why this exists (opus review 2026-09-23: distinguishing a
// genuinely-timed-out round from one that merely consumed the next real
// entry early).
static uint8_t g_status_stale[FAKE_STATUS_QUEUE_MAX];
static int g_status_queue_len = 0;
static int g_status_queue_pos = 0;

static void fake_status_reset(void)
{
    memset(g_status_queue, 0, sizeof(g_status_queue));
    memset(g_status_stale, 0, sizeof(g_status_stale));
    g_status_queue_len = 0;
    g_status_queue_pos = 0;
}

static void fake_status_push(uint8_t state, uint8_t last_error, uint8_t gap_count, const uint16_t *gaps)
{
    if (g_status_queue_len >= FAKE_STATUS_QUEUE_MAX) {
        return;
    }
    safety_link_update_status_t *st = &g_status_queue[g_status_queue_len];
    g_status_stale[g_status_queue_len] = 0;
    g_status_queue_len++;
    memset(st, 0, sizeof(*st));
    st->state = state;
    st->last_error = last_error;
    st->gap_count = gap_count;
    for (uint8_t i = 0; i < gap_count && i < SAFETY_LINK_UPDATE_STATUS_MAX_GAPS; i++) {
        st->gap_chunk_indices[i] = gaps[i];
    }
}

// Pushes a queue slot that IS consumed (advances g_status_queue_pos, so a
// later push still lands after it in FIFO order) but is reported with an
// enormous out_age_ms, so relay_wait_for_states()'s own freshness check
// (age_ms <= elapsed) rejects it every time -- modelling a real "nothing
// fresh this round" condition rather than relying on queue underrun, which
// (per the opus review this responds to) can be silently satisfied by the
// NEXT round's real entry instead of ever exercising the timeout path.
static void fake_status_push_stale(void)
{
    fake_status_push(0, 0, 0, NULL);
    g_status_stale[g_status_queue_len - 1] = 1;
}

uint32_t g_ota_pico_relay_fake_ticks = 0; // definition; declared extern by stubs_ota_pico_relay/freertos/task.h

esp_err_t safety_link_get_update_status(SafetyLinkClass *link, safety_link_update_status_t *out, uint32_t *out_age_ms)
{
    (void)link;
    if (g_status_queue_pos < g_status_queue_len) {
        int idx = g_status_queue_pos++;
        *out = g_status_queue[idx];
        if (out_age_ms) {
            // 0xFFFFFFFFu is never <= any realistic `elapsed` value computed
            // from the fake tick counter, so this entry never passes
            // relay_wait_for_states()'s freshness check -- it is consumed
            // (advances the queue) but never accepted.
            *out_age_ms = g_status_stale[idx] ? 0xFFFFFFFFu : 0u;
        }
        return ESP_OK;
    }
    // Exhausted -- force any waiter relying on this to time out for real.
    g_ota_pico_relay_fake_ticks += 100000000u;
    return ESP_ERR_NOT_FOUND;
}

// --- ota_state.h / ota_record.h fakes --------------------------------------
#include "ota_state.h"
#include "ota_record.h"

static int g_ota_http_update_end_calls = 0;
void ota_http_update_end(void) { g_ota_http_update_end_calls++; }

static int g_ota_record_append_calls = 0;
static ota_record_t g_last_ota_record;

void ota_record_fill(ota_record_t *out, uint32_t uptime_s, const char *processor, const char *version_before,
                      const char *version_after, bool success, const char *reason,
                      const char *image_sha256_hex_or_null)
{
    memset(out, 0, sizeof(*out));
    out->version = OTA_RECORD_VERSION;
    out->uptime_s = uptime_s;
    strncpy(out->processor, processor ? processor : "", sizeof(out->processor) - 1);
    strncpy(out->version_before, version_before ? version_before : "", sizeof(out->version_before) - 1);
    strncpy(out->version_after, version_after ? version_after : "", sizeof(out->version_after) - 1);
    out->success = success ? 1 : 0;
    strncpy(out->reason, reason ? reason : "", sizeof(out->reason) - 1);
    strncpy(out->image_sha256_hex, image_sha256_hex_or_null ? image_sha256_hex_or_null : "",
            sizeof(out->image_sha256_hex) - 1);
}

esp_err_t ota_record_append(const ota_record_t *rec)
{
    g_ota_record_append_calls++;
    g_last_ota_record = *rec;
    return ESP_OK;
}

esp_err_t ota_record_load(ota_record_t *out)
{
    (void)out;
    return ESP_ERR_NVS_NOT_FOUND;
}

// hal_time_now_us() -- real definition linked from hwAbstraction/host/fake_time.c
#include "hal_time.h"

// --- The module under test -------------------------------------------------
#include "../drivers/net/ota_pico_relay.c"

// --- Test scaffolding --------------------------------------------------

static int g_failures = 0;
static int g_tests = 0;

#define CHECK(cond)                                                                                                  \
    do {                                                                                                             \
        g_tests++;                                                                                                   \
        if (!(cond)) {                                                                                               \
            g_failures++;                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
        }                                                                                                            \
    } while (0)

// Resets every static this module owns, plus every fake's state, between
// test cases -- same "local helper in the test file itself" convention
// test_zones_http.c's reconcile_test_reset() uses (no production-code hook
// needed: this file already has direct access to every static after
// #include-ing the real .c file).
static void test_reset_relay_state(void)
{
    s_pico_img_partition = NULL; // re-lookup every test (partition presence may change between cases)
    s_phase = OTA_PICO_RELAY_PHASE_IDLE;
    s_percent = 0;
    memset(s_last_error, 0, sizeof(s_last_error));
    s_relay_running = false;
    s_relay_task = NULL;
    memset(&s_relay_args, 0, sizeof(s_relay_args));

    g_ota_pico_relay_fake_ticks = 0;
    g_set_update_in_progress_calls = 0;
    g_set_update_in_progress_last = false;
    g_sent_frames_count = 0;
    g_ota_http_update_end_calls = 0;
    g_ota_record_append_calls = 0;
    memset(&g_last_ota_record, 0, sizeof(g_last_ota_record));
    fake_status_reset();

    g_fake_partition_present = true;
    g_fake_partition_read_fail = false;
    memset(g_fake_flash, 0xAA, sizeof(g_fake_flash));
}

// Runs one full relay attempt (start + task body) for a small image, after
// the caller has pre-loaded g_status_queue via fake_status_push(). Returns
// the final status via ota_pico_relay_get_status().
static void run_relay(uint32_t image_length)
{
    bool started = ota_pico_relay_start(&g_fake_link, image_length, 0x12345678u, NULL, NULL);
    CHECK(started);
    relay_task_fn(NULL); // xTaskCreate() stub never invokes this itself
}

// --- Happy path: BEGIN -> ERASING -> RECEIVING -> SENDING -> RETRANSMIT
// (0 gaps) -> FINISHING -> COMPLETE -> DONE ---------------------------------
static void test_happy_path_with_erase(void)
{
    test_reset_relay_state();
    fake_status_push(SAFETY_LINK_UPDATE_STATE_ERASING, 0, 0, NULL);
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL);
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL); // first retransmit-round poll: 0 gaps
    fake_status_push(SAFETY_LINK_UPDATE_STATE_COMPLETE, 0, 0, NULL);  // UPDATE_END reply

    run_relay(1000);

    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_DONE);
    CHECK(st.percent == 100);
    CHECK(strcmp(st.last_error, "ok") == 0);
    CHECK(fake_sent_frames_count_cmd(SAFETY_CMD_UPDATE_BEGIN) == 1);
    CHECK(fake_sent_frames_count_cmd(SAFETY_CMD_UPDATE_DATA) >= 1);
    CHECK(fake_sent_frames_count_cmd(SAFETY_CMD_UPDATE_END) == 1);
    CHECK(fake_sent_frames_count_cmd(SAFETY_CMD_UPDATE_ABORT) == 0);
    CHECK(g_ota_http_update_end_calls == 1);
    CHECK(g_ota_record_append_calls == 1);
    CHECK(g_last_ota_record.success == 1);
    CHECK(g_set_update_in_progress_calls == 2); // true then false
    CHECK(g_set_update_in_progress_last == false);
    CHECK(s_relay_running == false);
}

// Happy path without an erase step (Pico already erased -- BEGIN reply is
// RECEIVING directly).
static void test_happy_path_no_erase(void)
{
    test_reset_relay_state();
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL); // BEGIN reply
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL); // retransmit round: 0 gaps
    fake_status_push(SAFETY_LINK_UPDATE_STATE_COMPLETE, 0, 0, NULL);  // UPDATE_END reply

    run_relay(500);

    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_DONE);
    CHECK(g_ota_http_update_end_calls == 1);
}

// --- UPDATE_BEGIN failure transitions ---------------------------------

static void test_begin_timeout(void)
{
    test_reset_relay_state(); // empty queue -- every wait times out
    run_relay(500);
    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_FAILED);
    CHECK(strstr(st.last_error, "no reply to UPDATE_BEGIN") != NULL);
    CHECK(fake_sent_frames_count_cmd(SAFETY_CMD_UPDATE_ABORT) == 1);
    CHECK(g_ota_http_update_end_calls == 1);
    CHECK(g_set_update_in_progress_last == false);
}

static void test_begin_refused_crc_mismatch(void)
{
    test_reset_relay_state();
    fake_status_push(SAFETY_LINK_UPDATE_STATE_REFUSED, SAFETY_LINK_UPDATE_ERR_CRC_MISMATCH, 0, NULL);
    run_relay(500);
    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_FAILED);
    CHECK(strstr(st.last_error, "Pico refused UPDATE_BEGIN") != NULL);
    CHECK(strstr(st.last_error, "CRC mismatch") != NULL);
    CHECK(fake_sent_frames_count_cmd(SAFETY_CMD_UPDATE_ABORT) == 1);
    CHECK(g_ota_http_update_end_calls == 1);
}

static void test_begin_failed(void)
{
    test_reset_relay_state();
    fake_status_push(SAFETY_LINK_UPDATE_STATE_FAILED, SAFETY_LINK_UPDATE_ERR_INTERNAL, 0, NULL);
    run_relay(500);
    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_FAILED);
    CHECK(strstr(st.last_error, "FAILED") != NULL);
    CHECK(g_ota_http_update_end_calls == 1);
}

static void test_begin_aborted(void)
{
    test_reset_relay_state();
    fake_status_push(SAFETY_LINK_UPDATE_STATE_ABORTED, 0, 0, NULL);
    run_relay(500);
    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_FAILED);
    CHECK(strstr(st.last_error, "ABORTED") != NULL);
    CHECK(g_ota_http_update_end_calls == 1);
}

static void test_begin_overlap(void)
{
    test_reset_relay_state();
    fake_status_push(SAFETY_LINK_UPDATE_STATE_REFUSED_RUNNING_IMAGE_OVERLAP, 0, 0, NULL);
    run_relay(500);
    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_FAILED);
    CHECK(strstr(st.last_error, "overwrite its running flat image") != NULL);
    CHECK(g_ota_http_update_end_calls == 1);
}

// --- ERASING phase failure transitions -------------------------------

static void test_erase_timeout(void)
{
    test_reset_relay_state();
    fake_status_push(SAFETY_LINK_UPDATE_STATE_ERASING, 0, 0, NULL); // BEGIN reply
    // queue exhausted -- the RECEIVING wait after ERASING times out
    run_relay(500);
    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_FAILED);
    CHECK(strstr(st.last_error, "did not confirm RECEIVING") != NULL);
    CHECK(fake_sent_frames_count_cmd(SAFETY_CMD_UPDATE_ABORT) == 1);
}

static void test_erase_then_overlap(void)
{
    test_reset_relay_state();
    fake_status_push(SAFETY_LINK_UPDATE_STATE_ERASING, 0, 0, NULL);
    fake_status_push(SAFETY_LINK_UPDATE_STATE_REFUSED_RUNNING_IMAGE_OVERLAP, 0, 0, NULL);
    run_relay(500);
    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_FAILED);
    CHECK(strstr(st.last_error, "overwrite its running flat image") != NULL);
}

static void test_erase_then_wrong_state(void)
{
    test_reset_relay_state();
    fake_status_push(SAFETY_LINK_UPDATE_STATE_ERASING, 0, 0, NULL);
    // VERIFYING is neither in the erase-wait's accept mask (RECEIVING only)
    // nor one of relay_wait_for_states()'s terminal states, so it would never
    // make that wait return early -- REJECTED_SLOT_LINKAGE IS terminal and is
    // not specially handled in the erase-wait block, so it falls into the
    // "did not reach RECEIVING" else branch instead.
    fake_status_push(SAFETY_LINK_UPDATE_STATE_REJECTED_SLOT_LINKAGE, SAFETY_LINK_UPDATE_ERR_INTERNAL, 0, NULL);
    run_relay(500);
    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_FAILED);
    CHECK(strstr(st.last_error, "did not reach RECEIVING") != NULL);
}

// --- Retransmission rounds --------------------------------------------

// A gap is named once, then reported clear -- verifies the gap chunk is
// actually resent (checkable via the fake send-frame history) before
// proceeding to UPDATE_END.
static void test_retransmit_resends_named_gap(void)
{
    test_reset_relay_state();
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL); // BEGIN reply, no erase
    uint16_t gaps[1] = { 0 };                                          // chunk index 0 missing
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 1, gaps);  // round 1: 1 gap
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL);  // round 2: 0 gaps -- proceed
    fake_status_push(SAFETY_LINK_UPDATE_STATE_COMPLETE, 0, 0, NULL);   // UPDATE_END reply

    int sent_before = fake_sent_frames_count_cmd(SAFETY_CMD_UPDATE_DATA);
    run_relay(100); // one chunk (100 < UPDATE_CHUNK_LEN=248), so sequential pass sends exactly 1 UPDATE_DATA
    int sent_after = fake_sent_frames_count_cmd(SAFETY_CMD_UPDATE_DATA);

    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_DONE);
    // 1 sequential-pass send + 1 gap-retransmit resend of chunk 0 == 2 total.
    CHECK((sent_after - sent_before) == 2);
}

// No reply during a retransmit round is NOT fatal -- the loop continues to
// the next round rather than aborting on one missed cycle.
static void test_retransmit_missed_round_not_fatal(void)
{
    test_reset_relay_state();
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL); // BEGIN reply

    // relay_wait_for_states() polls once, then vTaskDelay(RELAY_STATUS_POLL_MS)
    // if it misses, until now_ms() >= deadline. Each poll -- hit or miss --
    // consumes one queued entry (via the fake FIFO), so round 1's ENTIRE
    // RELAY_GAP_ROUND_WAIT_MS window must be fed stale (rejected-by-age)
    // entries, or a later poll would simply pick up round 2's real entry
    // early -- which is what the previous (vacuous) version of this test
    // actually did: it queued only one dummy slot, round 1 consumed it,
    // found the queue "empty" only by luck of ordering, and round 2's own
    // entry got grabbed by round 1 on the very next poll instead, so
    // `!got` (ota_pico_relay.c's `if (!got) { ... continue; }`) was never
    // reached. The number of polls before timeout is
    // ceil(RELAY_GAP_ROUND_WAIT_MS / RELAY_STATUS_POLL_MS) + 1 (the last
    // poll's own status check still runs, and still must miss, before the
    // deadline check after it returns false) -- push exactly that many
    // stale entries so round 1 is stale-fed for its whole window and
    // nothing is left over for round 2 to consume early.
    uint32_t stale_needed =
        (RELAY_GAP_ROUND_WAIT_MS + RELAY_STATUS_POLL_MS - 1u) / RELAY_STATUS_POLL_MS + 1u;
    for (uint32_t i = 0; i < stale_needed; i++) {
        fake_status_push_stale();
    }
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL); // round 2: 0 gaps
    fake_status_push(SAFETY_LINK_UPDATE_STATE_COMPLETE, 0, 0, NULL);  // UPDATE_END reply

    run_relay(500);

    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_DONE);
    // The whole queue (BEGIN + every stale entry + round-2 reply +
    // UPDATE_END reply) was consumed with nothing left over -- proves
    // round 1 genuinely timed out consuming ONLY stale entries (reaching
    // `!got -> continue`), rather than round 2's real entry being consumed
    // by round 1 itself, and that round 2 (not round 1) is what actually
    // supplied the 0-gaps reply that let the relay proceed to UPDATE_END.
    CHECK(g_status_queue_pos == g_status_queue_len);
    CHECK(g_status_queue_len == (int)(1u + stale_needed + 2u));
}

static void test_retransmit_failed_mid_update(void)
{
    // Models a Pico reboot mid-update: the link starts replying FAILED
    // partway through retransmission.
    test_reset_relay_state();
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL); // BEGIN reply
    uint16_t gaps[1] = { 0 };
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 1, gaps);       // round 1: 1 gap
    fake_status_push(SAFETY_LINK_UPDATE_STATE_FAILED, SAFETY_LINK_UPDATE_ERR_INTERNAL, 0, NULL); // round 2: FAILED

    run_relay(500);
    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_FAILED);
    CHECK(strstr(st.last_error, "FAILED during retransmission") != NULL);
    CHECK(fake_sent_frames_count_cmd(SAFETY_CMD_UPDATE_ABORT) == 1);
}

static void test_retransmit_overlap(void)
{
    test_reset_relay_state();
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL);
    fake_status_push(SAFETY_LINK_UPDATE_STATE_REFUSED_RUNNING_IMAGE_OVERLAP, 0, 0, NULL);
    run_relay(500);
    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_FAILED);
    CHECK(strstr(st.last_error, "overwrite its running flat image") != NULL);
}

// Exhausting RELAY_MAX_RETRANSMIT_ROUNDS without ever reaching 0 gaps:
// every round is queued with a nonzero gap count, so relay proceeds straight
// to UPDATE_END with the image still incomplete.
static void test_retransmit_exhausted_rounds(void)
{
    test_reset_relay_state();
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL); // BEGIN reply
    uint16_t gaps[1] = { 0 };
    // Push one extra gap entry beyond RELAY_MAX_RETRANSMIT_ROUNDS. With a
    // correct loop bound (`round < RELAY_MAX_RETRANSMIT_ROUNDS`) the loop
    // runs exactly RELAY_MAX_RETRANSMIT_ROUNDS times and this extra entry is
    // left over for UPDATE_END's own wait to consume (and reject, since it
    // only accepts COMPLETE) -- no extra resend results. An off-by-one-high
    // loop bound (e.g. `round <= RELAY_MAX_RETRANSMIT_ROUNDS`) instead
    // consumes this entry as one more retransmit round, resending chunk 0
    // again and changing the resend count asserted below -- without this
    // extra entry, that mutation was indistinguishable, since the extra
    // round would just find the queue empty and `continue` without
    // resending (the gap this test used to have).
    for (uint32_t r = 0; r < RELAY_MAX_RETRANSMIT_ROUNDS + 1u; r++) {
        fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 1, gaps); // never reaches 0
    }
    // After RELAY_MAX_RETRANSMIT_ROUNDS rounds, the loop falls through to
    // UPDATE_END regardless of outcome. UPDATE_END's own wait only accepts
    // COMPLETE (RECEIVING/VERIFYING are neither in its accept mask nor one of
    // relay_wait_for_states()'s terminal states), so an image still-incomplete
    // after every round is observed here as UPDATE_END consuming the one
    // leftover queue entry above and still timing out for real ("no reply to
    // UPDATE_END"), not the separate "gave up" message (which is reachable
    // only if relay_wait_for_states() could return RECEIVING/VERIFYING as a
    // match, which it cannot).

    int sent_before = fake_sent_frames_count_cmd(SAFETY_CMD_UPDATE_DATA);
    run_relay(500); // 500 / UPDATE_CHUNK_LEN(248) => ceil = 3 chunks in the sequential pass
    int sent_after = fake_sent_frames_count_cmd(SAFETY_CMD_UPDATE_DATA);

    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_FAILED);
    CHECK(strstr(st.last_error, "no reply to UPDATE_END") != NULL);
    CHECK(fake_sent_frames_count_cmd(SAFETY_CMD_UPDATE_ABORT) == 1);
    // Pins the actual round count: 3 sequential-pass chunks + one
    // gap-resend of chunk 0 per round, for exactly RELAY_MAX_RETRANSMIT_ROUNDS
    // rounds. A sabotaged loop bound (e.g. `round < 3u` instead of
    // `round < RELAY_MAX_RETRANSMIT_ROUNDS`) changes this count even though
    // the final phase/message stay identical either way -- that insensitivity
    // is exactly what made this test vacuous before this assertion existed.
    CHECK((sent_after - sent_before) == (int)(3 + RELAY_MAX_RETRANSMIT_ROUNDS));
    // BEGIN + RELAY_MAX_RETRANSMIT_ROUNDS per-round entries were consumed by
    // the retransmit loop itself, and the one extra entry pushed above was
    // consumed by UPDATE_END's own wait (and rejected there) -- nothing is
    // left unconsumed.
    CHECK(g_status_queue_pos == g_status_queue_len);
    CHECK(g_status_queue_len == (int)(2 + RELAY_MAX_RETRANSMIT_ROUNDS));
}

// --- UPDATE_END failure transitions -----------------------------------

static void test_end_timeout(void)
{
    // Models link loss mid-transfer: everything up to and including the
    // retransmit rounds succeeds, but nothing ever answers UPDATE_END.
    test_reset_relay_state();
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL); // BEGIN reply
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL); // retransmit round: 0 gaps
    // queue exhausted -- UPDATE_END's wait times out
    run_relay(500);
    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_FAILED);
    CHECK(strstr(st.last_error, "no reply to UPDATE_END") != NULL);
    CHECK(fake_sent_frames_count_cmd(SAFETY_CMD_UPDATE_ABORT) == 1);
    CHECK(g_ota_http_update_end_calls == 1);
}

static void test_end_rejected_slot_linkage(void)
{
    test_reset_relay_state();
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL);
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL);
    fake_status_push(SAFETY_LINK_UPDATE_STATE_REJECTED_SLOT_LINKAGE, 0, 0, NULL);
    run_relay(500);
    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_FAILED);
    CHECK(strstr(st.last_error, "vector table not linked") != NULL);
}

static void test_end_overlap(void)
{
    test_reset_relay_state();
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL);
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL);
    fake_status_push(SAFETY_LINK_UPDATE_STATE_REFUSED_RUNNING_IMAGE_OVERLAP, 0, 0, NULL);
    run_relay(500);
    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_FAILED);
    CHECK(strstr(st.last_error, "overwrite its running flat image") != NULL);
}

static void test_end_failed(void)
{
    // Models a Pico reboot right at the finish line.
    test_reset_relay_state();
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL);
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL);
    fake_status_push(SAFETY_LINK_UPDATE_STATE_FAILED, SAFETY_LINK_UPDATE_ERR_INTERNAL, 0, NULL);
    run_relay(500);
    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_FAILED);
    CHECK(strstr(st.last_error, "FAILED") != NULL);
    CHECK(fake_sent_frames_count_cmd(SAFETY_CMD_UPDATE_ABORT) == 1);
}

// --- Partition-lookup / size-validation failure paths (before any frame
// is sent at all) --------------------------------------------------------

static void test_partition_missing(void)
{
    test_reset_relay_state();
    g_fake_partition_present = false;
    run_relay(500);
    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_FAILED);
    CHECK(strstr(st.last_error, "pico_img partition not found") != NULL);
    CHECK(fake_sent_frames_count_cmd(SAFETY_CMD_UPDATE_BEGIN) == 0); // never reached
    CHECK(g_ota_http_update_end_calls == 1);                        // done: still runs
}

static void test_image_too_large(void)
{
    test_reset_relay_state();
    run_relay(FAKE_PART_SIZE + 1u);
    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);
    CHECK(st.phase == OTA_PICO_RELAY_PHASE_FAILED);
    CHECK(strstr(st.last_error, "invalid for pico_img partition") != NULL);
}

// --- Idempotence: re-entering from a terminal state -------------------

static void test_idempotent_reentry_after_done(void)
{
    test_reset_relay_state();
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL);
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL);
    fake_status_push(SAFETY_LINK_UPDATE_STATE_COMPLETE, 0, 0, NULL);
    run_relay(500);
    ota_pico_relay_status_t st1;
    ota_pico_relay_get_status(&st1);
    CHECK(st1.phase == OTA_PICO_RELAY_PHASE_DONE);
    CHECK(s_relay_running == false); // cleared -- a second attempt is not refused

    // Second attempt, fresh queue, should run cleanly from scratch.
    fake_status_reset();
    g_sent_frames_count = 0;
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL);
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL);
    fake_status_push(SAFETY_LINK_UPDATE_STATE_COMPLETE, 0, 0, NULL);
    run_relay(500);
    ota_pico_relay_status_t st2;
    ota_pico_relay_get_status(&st2);
    CHECK(st2.phase == OTA_PICO_RELAY_PHASE_DONE);
    CHECK(g_ota_http_update_end_calls == 2); // exactly once per attempt, both attempts counted
}

static void test_idempotent_reentry_after_failed(void)
{
    test_reset_relay_state();
    // First attempt fails at UPDATE_BEGIN (timeout).
    run_relay(500);
    ota_pico_relay_status_t st1;
    ota_pico_relay_get_status(&st1);
    CHECK(st1.phase == OTA_PICO_RELAY_PHASE_FAILED);
    CHECK(s_relay_running == false);

    // A second attempt right after a FAILED terminal state must not be
    // refused by ota_pico_relay_start()'s own concurrency guard.
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL);
    fake_status_push(SAFETY_LINK_UPDATE_STATE_RECEIVING, 0, 0, NULL);
    fake_status_push(SAFETY_LINK_UPDATE_STATE_COMPLETE, 0, 0, NULL);
    run_relay(500);
    ota_pico_relay_status_t st2;
    ota_pico_relay_get_status(&st2);
    CHECK(st2.phase == OTA_PICO_RELAY_PHASE_DONE);
}

// ota_pico_relay_start()'s own guard: a call while s_relay_running is
// already true (simulated directly, since the host stub never actually
// runs a concurrent task) is refused.
static void test_start_refuses_while_running(void)
{
    test_reset_relay_state();
    s_relay_running = true;
    bool started = ota_pico_relay_start(&g_fake_link, 500, 0x1u, NULL, NULL);
    CHECK(started == false);
}

int main(void)
{
    test_happy_path_with_erase();
    test_happy_path_no_erase();
    test_begin_timeout();
    test_begin_refused_crc_mismatch();
    test_begin_failed();
    test_begin_aborted();
    test_begin_overlap();
    test_erase_timeout();
    test_erase_then_overlap();
    test_erase_then_wrong_state();
    test_retransmit_resends_named_gap();
    test_retransmit_missed_round_not_fatal();
    test_retransmit_failed_mid_update();
    test_retransmit_overlap();
    test_retransmit_exhausted_rounds();
    test_end_timeout();
    test_end_rejected_slot_linkage();
    test_end_overlap();
    test_end_failed();
    test_partition_missing();
    test_image_too_large();
    test_idempotent_reentry_after_done();
    test_idempotent_reentry_after_failed();
    test_start_refuses_while_running();

    printf("test_ota_pico_relay: %d/%d checks passed\n", g_tests - g_failures, g_tests);
    return g_failures == 0 ? 0 : 1;
}
