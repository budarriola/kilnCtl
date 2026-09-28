// Host tests for App/drivers/http/http_async_job.c -- the shared one-at-a-time
// async-job helper (docs/HTTP_POST_OWNER_MIGRATION_PLAN.md slice A1).
// #includes http_async_job.c directly to reach its static run_job() (needed
// because the host xTaskCreate() stub never actually invokes the function it
// is given -- see stubs/freertos/task.h's own comment), same convention as
// every other test file in this executable that needs a static seam.
#include "test_common.h"

#include "esp_http_server.h"

#include "../drivers/http/http_async_job.c"

// ---------------------------------------------------------------------------
// Fakes shared across this file's tests
// ---------------------------------------------------------------------------

static int s_fn_calls = 0;
static httpd_req_t *s_fn_last_req = NULL;

// httpd_req_to_sockfd() -- declared in esp_http_server.h's stub, "declared
// once, defined per test file" convention; this is the only test file in
// this executable that calls into anything referencing it (A4 review
// follow-up C's httpd_sess_trigger_close() fix, 2026-09-28). Controllable so
// a test can exercise both the normal (>=0) and unresolvable (-1) paths.
static int s_stub_sockfd_result = 7; // any nonnegative placeholder
int httpd_req_to_sockfd(httpd_req_t *r)
{
    (void)r;
    return s_stub_sockfd_result;
}

static void fake_job_fn(httpd_req_t *async_req, void *ctx)
{
    s_fn_calls++;
    s_fn_last_req = async_req;
    (void)ctx;
    // Real job fns (e.g. ct_auto_zero_job()) always send a response before
    // returning; this fake's "early return" stands in for that -- what
    // matters here is only that http_async_job.c calls complete() exactly
    // once regardless of what fn itself did.
}

static void reset_stubs(void)
{
    s_fn_calls = 0;
    s_fn_last_req = NULL;
    g_test_stub_async_begin_should_fail = 0;
    g_test_stub_async_complete_calls = 0;
    g_test_stub_xtaskcreate_result = 1; // pdPASS
    s_busy = false;
    s_task_handle = NULL;
    s_stub_sockfd_result = 7;
    g_test_stub_sess_trigger_close_calls = 0;
    g_test_stub_sess_trigger_close_last_handle = NULL;
    g_test_stub_sess_trigger_close_last_sockfd = -1;
}

// ---------------------------------------------------------------------------
// http_async_job_try_start()
// ---------------------------------------------------------------------------

static void test_admits_when_idle(void)
{
    reset_stubs();
    httpd_req_t req = {0};

    http_async_job_start_result_t result = http_async_job_try_start(&req, "http_async_job", 4096, fake_job_fn, NULL);

    TEST_CHECK(result == HTTP_ASYNC_JOB_STARTED, "an idle helper must admit the job");
    TEST_CHECK(http_async_job_busy(), "busy must be set the moment the job is admitted");
}

static void test_second_call_refused_while_busy(void)
{
    reset_stubs();
    httpd_req_t req1 = {0};
    httpd_req_t req2 = {0};

    http_async_job_start_result_t first = http_async_job_try_start(&req1, "http_async_job", 4096, fake_job_fn, NULL);
    TEST_CHECK(first == HTTP_ASYNC_JOB_STARTED, "first call must be admitted");

    int complete_calls_before_second = g_test_stub_async_complete_calls;
    http_async_job_start_result_t second = http_async_job_try_start(&req2, "http_async_job", 4096, fake_job_fn, NULL);

    TEST_CHECK(second == HTTP_ASYNC_JOB_BUSY, "a second call while busy must be refused with BUSY, not RESOURCE_FAILURE");
    TEST_CHECK(g_test_stub_async_complete_calls == complete_calls_before_second,
               "a busy refusal must not touch req2 at all -- no begin(), so no complete() either");
    TEST_CHECK(http_async_job_busy(), "the first job's busy flag must be untouched by the refused second call");
}

static void test_begin_failure_refuses_and_clears_busy(void)
{
    reset_stubs();
    g_test_stub_async_begin_should_fail = 1;
    httpd_req_t req = {0};

    http_async_job_start_result_t result = http_async_job_try_start(&req, "http_async_job", 4096, fake_job_fn, NULL);

    TEST_CHECK(result == HTTP_ASYNC_JOB_RESOURCE_FAILURE, "a failed handoff begin() must refuse with RESOURCE_FAILURE, not BUSY");
    TEST_CHECK(!http_async_job_busy(), "busy must be cleared again after a begin() failure");
    TEST_CHECK(g_test_stub_async_complete_calls == 0,
               "no async copy was ever created, so complete() must not be called");
}

static void test_task_create_failure_undoes_begin_and_clears_busy(void)
{
    reset_stubs();
    g_test_stub_xtaskcreate_result = 0; // pdFAIL
    httpd_req_t req = {0};
    req.handle = (httpd_handle_t)0x1234;

    http_async_job_start_result_t result = http_async_job_try_start(&req, "http_async_job", 4096, fake_job_fn, NULL);

    TEST_CHECK(result == HTTP_ASYNC_JOB_RESOURCE_FAILURE, "a failed xTaskCreate() must refuse with RESOURCE_FAILURE, not BUSY");
    TEST_CHECK(!http_async_job_busy(), "busy must be cleared again after a task-create failure");
    TEST_CHECK(g_test_stub_async_complete_calls == 1,
               "the async copy from begin() must be undone via exactly one complete() call");
    TEST_CHECK(s_fn_calls == 0, "fn must never run when the job task itself could not be created");

    // A4 review follow-up C (2026-09-28): req's body was never read by
    // anyone on this path (fn never ran, and httpd_worker itself never reads
    // the body before handing off) -- the fix force-closes the session
    // rather than leaving a keep-alive client to desync on the unread bytes.
    //
    // NEGATIVE TEST (performed 2026-09-28, RED confirmed, restored by hand,
    // forced full rebuild): commenting out this fix's
    // `httpd_sess_trigger_close(handle, sockfd);` call in http_async_job.c
    // fails this exact assertion (g_test_stub_sess_trigger_close_calls stays
    // 0) while every other assertion in this file still passes -- proving
    // this test is not vacuous and genuinely exercises the new call.
    TEST_CHECK(g_test_stub_sess_trigger_close_calls == 1,
               "a failed job-task create must force-close the underlying session exactly once");
    TEST_CHECK(g_test_stub_sess_trigger_close_last_handle == req.handle,
               "the session close must be issued against req's own handle");
    TEST_CHECK(g_test_stub_sess_trigger_close_last_sockfd == s_stub_sockfd_result,
               "the session close must target the sockfd resolved off the async copy");
}

// Companion to the test above: when the sockfd can't be resolved at all
// (host stand-in for httpd_req_to_sockfd() failing on a real board), this
// path must not call httpd_sess_trigger_close() with a garbage fd -- it logs
// a warning and otherwise behaves exactly as before this fix (RESOURCE_FAILURE,
// busy cleared, exactly one complete() call).
static void test_task_create_failure_skips_close_when_sockfd_unresolvable(void)
{
    reset_stubs();
    g_test_stub_xtaskcreate_result = 0; // pdFAIL
    s_stub_sockfd_result = -1;
    httpd_req_t req = {0};
    req.handle = (httpd_handle_t)0x5678;

    http_async_job_start_result_t result = http_async_job_try_start(&req, "http_async_job", 4096, fake_job_fn, NULL);

    TEST_CHECK(result == HTTP_ASYNC_JOB_RESOURCE_FAILURE, "still refuses with RESOURCE_FAILURE");
    TEST_CHECK(!http_async_job_busy(), "busy must still be cleared");
    TEST_CHECK(g_test_stub_async_complete_calls == 1, "the async copy must still be completed exactly once");
    TEST_CHECK(g_test_stub_sess_trigger_close_calls == 0,
               "an unresolvable sockfd must never be passed to httpd_sess_trigger_close()");
}

// ---------------------------------------------------------------------------
// run_job() -- the body the FreeRTOS trampoline calls, exercised directly
// since the host xTaskCreate() stub never invokes it for real.
// ---------------------------------------------------------------------------

static void test_run_job_calls_fn_then_completes_exactly_once_and_clears_busy(void)
{
    reset_stubs();
    httpd_req_t req = {0};
    httpd_req_t *async_req = NULL;
    TEST_CHECK(httpd_req_async_handler_begin(&req, &async_req) == ESP_OK, "test setup: begin must succeed");
    s_busy = true; // matches the state http_async_job_try_start() would have left it in

    http_async_job_run_ctx_t rc = { .fn = fake_job_fn, .ctx = NULL, .async_req = async_req };
    run_job(&rc);

    TEST_CHECK(s_fn_calls == 1, "run_job() must call fn exactly once");
    TEST_CHECK(s_fn_last_req == async_req, "fn must receive the async copy, not the original req");
    TEST_CHECK(g_test_stub_async_complete_calls == 1,
               "run_job() must call complete() exactly once after fn returns, on every path");
    TEST_CHECK(!s_busy, "run_job() must clear busy once fn and complete() are done");
}

// 2026-09-25 fix-then-push review: run_job() used to clear s_busy in one
// critical section and leave s_task_handle for the FreeRTOS trampoline to
// null out afterward in a SEPARATE critical section -- a newly-admitted job
// could run xTaskCreate() and write a fresh s_task_handle in between those
// two, and the old task's trailing cleanup would then null out the NEW
// job's handle instead of its own ("reset one side of a pair", CLAUDE.md).
// This test proves run_job() alone now clears s_task_handle, atomically
// with s_busy, with no separate trampoline step required.
static void test_run_job_clears_task_handle_atomically_with_busy(void)
{
    reset_stubs();
    httpd_req_t req = {0};
    httpd_req_t *async_req = NULL;
    TEST_CHECK(httpd_req_async_handler_begin(&req, &async_req) == ESP_OK, "test setup: begin must succeed");
    s_busy = true;
    s_task_handle = (TaskHandle_t)0x1; // any non-NULL sentinel -- run_job() must clear it

    http_async_job_run_ctx_t rc = { .fn = fake_job_fn, .ctx = NULL, .async_req = async_req };
    run_job(&rc);

    TEST_CHECK(s_task_handle == NULL, "run_job() must clear s_task_handle itself, "
                                       "not leave it for a separate trampoline step");
    TEST_CHECK(!s_busy, "run_job() must clear busy in the same pass as the handle");
}

void run_test_http_async_job(void)
{
    test_admits_when_idle();
    test_second_call_refused_while_busy();
    test_begin_failure_refuses_and_clears_busy();
    test_task_create_failure_undoes_begin_and_clears_busy();
    test_task_create_failure_skips_close_when_sockfd_unresolvable();
    test_run_job_calls_fn_then_completes_exactly_once_and_clears_busy();
    test_run_job_clears_task_handle_atomically_with_busy();
}
