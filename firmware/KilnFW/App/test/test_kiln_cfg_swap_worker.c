// Host tests for kiln_cfg_swap_worker.c's single-flight safety-config writer
// guard (docs/HTTP_POST_OWNER_MIGRATION.md A2 gap, closed 2026-10-02):
// the kiln config swap drives the same Pico SET_PARAM/COMMIT_CONFIG staged
// transaction an http_async_job, the zone current sweep and the poll-side
// ceiling reconcile do, so it must not overlap any of them.
//
// Own executable (build_host_tests.ps1): #includes kiln_cfg_swap_worker.c
// directly to reach its static run_swap_job() (the host xTaskCreate() stub
// never runs the worker task), with kiln_cfg_swap_apply()/_boot_recover()
// and stack_margin_register() faked here and the REAL
// safety_cfg_writer_guard.c linked.
//
// WHAT THIS FILE PROVES
//  1. submit() is refused (false, a reason, nothing published, nothing
//     queued) while any other writer owns the guard, and leaves that
//     writer's claim untouched.
//  2. An accepted submit claims SAFETY_CFG_WRITER_SWAP on the submitting
//     thread BEFORE the worker runs, so another writer is refused for the
//     whole job; the worker holds the guard across kiln_cfg_swap_apply()
//     and releases it on both the success and the failed/diverged outcome.
//  3. A failed queue send (the depth-1 queue lost a race) releases the
//     claim -- no leak that would wedge every later writer.
//
// NOT covered here: the worker task's boot-recovery claim (swap_worker_task()
// runs an infinite loop the host cannot drive); that path is a claim/release
// pair around one call and is covered by review and the target build only.
//
// NEGATIVE TEST (2026-10-02): removing the try_claim() in submit() fails
// checks 1 and 2 below; removing the release() in run_swap_job() fails the
// "released after success/failure" checks; removing the release() on the
// queue-send failure path fails check 3. Restored by hand, full rebuild.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

// Backing storage for the freertos queue stub's extern state -- see
// stubs/freertos/queue.h's own comment (each test .c defines it).
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
int g_stub_queue_ring_enabled = 1;
unsigned char g_stub_queue_ring[TEST_STUB_QUEUE_RING_MAX_CAPACITY][256];
unsigned long g_stub_queue_ring_item_len[TEST_STUB_QUEUE_RING_MAX_CAPACITY];
int g_stub_queue_ring_capacity = 0;
int g_stub_queue_ring_count = 0;
int g_stub_queue_ring_head = 0;
int g_stub_queue_send_calls = 0;
unsigned char g_stub_last_queue_item[256];

#include "../drivers/persist/kiln_cfg_swap_worker.c"
#include "../drivers/persist/kiln_cfg_swap.h"

// ---- fakes -----------------------------------------------------------------
bool stack_margin_register(const char *name, void *task_handle_slot, uint32_t configured_stack_bytes)
{
    (void)name;
    (void)task_handle_slot;
    (void)configured_stack_bytes;
    return true;
}

static int s_apply_calls = 0;
static bool s_apply_result = true;
static bool s_apply_diverged = false;
static safety_cfg_writer_t s_owner_seen_during_apply = SAFETY_CFG_WRITER_NONE;

bool kiln_cfg_swap_apply(int32_t target_id, bool ack_no_safety_processor, char *reason_out, size_t reason_cap,
                         bool *out_diverged)
{
    (void)target_id;
    (void)ack_no_safety_processor;
    s_apply_calls++;
    s_owner_seen_during_apply = safety_cfg_writer_owner();
    if (reason_out && reason_cap > 0) {
        snprintf(reason_out, reason_cap, "%s", s_apply_result ? "" : "fake refusal");
    }
    if (out_diverged) {
        *out_diverged = s_apply_diverged;
    }
    return s_apply_result;
}

void kiln_cfg_swap_boot_recover(void)
{
}

// ---- helpers ---------------------------------------------------------------
static void reset_all(void)
{
    safety_cfg_writer_t held = safety_cfg_writer_owner();
    if (held != SAFETY_CFG_WRITER_NONE) {
        (void)safety_cfg_writer_release(held);
    }
    s_apply_calls = 0;
    s_apply_result = true;
    s_apply_diverged = false;
    s_owner_seen_during_apply = SAFETY_CFG_WRITER_NONE;
    g_stub_queue_ring_enabled = 1;
    g_stub_queue_ring_count = 0;
    g_stub_queue_ring_head = 0;
    g_stub_queue_send_calls = 0;
    publish(KILN_CFG_SWAP_JOB_IDLE, -1, false, NULL);
}

static kiln_cfg_swap_job_state_t state_now(void)
{
    kiln_cfg_swap_job_state_t st = KILN_CFG_SWAP_JOB_IDLE;
    kiln_cfg_swap_worker_get_status(&st, NULL, NULL, NULL, 0);
    return st;
}

// ---- tests -----------------------------------------------------------------
static void test_submit_refused_while_other_writer_holds_guard(void)
{
    TEST_SECTION("submit() refused while another safety-config writer owns the guard");
    const safety_cfg_writer_t others[] = { SAFETY_CFG_WRITER_ASYNC_JOB, SAFETY_CFG_WRITER_SWEEP,
                                           SAFETY_CFG_WRITER_RECONCILE };
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        reset_all();
        TEST_CHECK(safety_cfg_writer_try_claim(others[i]), "test setup: other writer claims");
        char reason[KILN_CFG_SWAP_REASON_MAX];
        TEST_CHECK(!kiln_cfg_swap_worker_submit(5, false, reason, sizeof(reason)),
                   "submit must be refused while another writer owns the guard");
        TEST_CHECK(reason[0] != '\0', "a refusal must carry a reason for the 409 body");
        TEST_CHECK(state_now() == KILN_CFG_SWAP_JOB_IDLE, "a refused submit must not publish RUNNING");
        TEST_CHECK(g_stub_queue_send_calls == 0, "a refused submit must not queue a job");
        TEST_CHECK(safety_cfg_writer_owner() == others[i], "the refusal must leave the other writer's claim alone");
    }
}

static void test_accepted_submit_holds_guard_through_apply_and_releases(void)
{
    TEST_SECTION("accepted submit claims SWAP up front; the worker releases it on success");
    reset_all();
    char reason[KILN_CFG_SWAP_REASON_MAX];
    TEST_CHECK(kiln_cfg_swap_worker_submit(5, false, reason, sizeof(reason)), "idle guard: submit accepted");
    TEST_CHECK(safety_cfg_writer_owner() == SAFETY_CFG_WRITER_SWAP,
               "the guard is claimed at accept time, before the worker runs");
    TEST_CHECK(!safety_cfg_writer_try_claim(SAFETY_CFG_WRITER_ASYNC_JOB) &&
                   !safety_cfg_writer_try_claim(SAFETY_CFG_WRITER_SWEEP) &&
                   !safety_cfg_writer_try_claim(SAFETY_CFG_WRITER_RECONCILE),
               "every other writer is refused between accept and the worker's run");

    swap_job_t job;
    TEST_CHECK(xQueueReceive(s_queue, &job, 0) == pdTRUE, "the accepted job was queued");
    run_swap_job(&job);
    TEST_CHECK(s_apply_calls == 1, "apply ran once");
    TEST_CHECK(s_owner_seen_during_apply == SAFETY_CFG_WRITER_SWAP, "the guard was held across kiln_cfg_swap_apply()");
    TEST_CHECK(state_now() == KILN_CFG_SWAP_JOB_DONE_OK, "success published");
    TEST_CHECK(safety_cfg_writer_owner() == SAFETY_CFG_WRITER_NONE, "guard released after success");
    TEST_CHECK(safety_cfg_writer_try_claim(SAFETY_CFG_WRITER_SWEEP), "another writer is admitted afterwards");
    (void)safety_cfg_writer_release(SAFETY_CFG_WRITER_SWEEP);
}

static void test_failed_and_diverged_apply_release_guard(void)
{
    TEST_SECTION("failed and diverged applies release the guard too");
    for (int diverged = 0; diverged <= 1; diverged++) {
        reset_all();
        s_apply_result = false;
        s_apply_diverged = diverged != 0;
        char reason[KILN_CFG_SWAP_REASON_MAX];
        TEST_CHECK(kiln_cfg_swap_worker_submit(7, false, reason, sizeof(reason)), "submit accepted");
        swap_job_t job;
        TEST_CHECK(xQueueReceive(s_queue, &job, 0) == pdTRUE, "queued");
        run_swap_job(&job);
        TEST_CHECK(state_now() == KILN_CFG_SWAP_JOB_DONE_FAILED, "failure published");
        TEST_CHECK(safety_cfg_writer_owner() == SAFETY_CFG_WRITER_NONE, "guard released after a failed apply");
    }
}

static void test_queue_send_failure_releases_guard(void)
{
    TEST_SECTION("a failed queue send releases the claim");
    reset_all();
    g_stub_queue_ring_enabled = 0; // stub xQueueSend then reports pdFALSE
    char reason[KILN_CFG_SWAP_REASON_MAX];
    TEST_CHECK(!kiln_cfg_swap_worker_submit(9, false, reason, sizeof(reason)), "queue send failure refuses");
    TEST_CHECK(g_stub_queue_send_calls == 1, "the send was attempted");
    TEST_CHECK(safety_cfg_writer_owner() == SAFETY_CFG_WRITER_NONE,
               "the claim taken for the refused submit must not leak");
    TEST_CHECK(state_now() == KILN_CFG_SWAP_JOB_DONE_FAILED, "the lost race is published as a failure");
}

int main(void)
{
    g_stub_queue_ring_enabled = 1;
    TEST_CHECK(kiln_cfg_swap_worker_start() == ESP_OK, "worker start (creates mutex and depth-1 queue)");
    s_task = (TaskHandle_t)0x1; // the host xTaskCreate() stub leaves the handle NULL

    test_submit_refused_while_other_writer_holds_guard();
    test_accepted_submit_holds_guard_through_apply_and_releases();
    test_failed_and_diverged_apply_release_guard();
    test_queue_send_failure_releases_guard();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
