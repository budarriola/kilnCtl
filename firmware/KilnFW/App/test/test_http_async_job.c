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
}

// ---------------------------------------------------------------------------
// http_async_job_try_start()
// ---------------------------------------------------------------------------

static void test_admits_when_idle(void)
{
    reset_stubs();
    httpd_req_t req = {0};

    bool started = http_async_job_try_start(&req, "http_async_job", 4096, fake_job_fn, NULL);

    TEST_CHECK(started, "an idle helper must admit the job");
    TEST_CHECK(http_async_job_busy(), "busy must be set the moment the job is admitted");
}

static void test_second_call_refused_while_busy(void)
{
    reset_stubs();
    httpd_req_t req1 = {0};
    httpd_req_t req2 = {0};

    bool first = http_async_job_try_start(&req1, "http_async_job", 4096, fake_job_fn, NULL);
    TEST_CHECK(first, "first call must be admitted");

    int complete_calls_before_second = g_test_stub_async_complete_calls;
    bool second = http_async_job_try_start(&req2, "http_async_job", 4096, fake_job_fn, NULL);

    TEST_CHECK(!second, "a second call while busy must be refused");
    TEST_CHECK(g_test_stub_async_complete_calls == complete_calls_before_second,
               "a busy refusal must not touch req2 at all -- no begin(), so no complete() either");
    TEST_CHECK(http_async_job_busy(), "the first job's busy flag must be untouched by the refused second call");
}

static void test_begin_failure_refuses_and_clears_busy(void)
{
    reset_stubs();
    g_test_stub_async_begin_should_fail = 1;
    httpd_req_t req = {0};

    bool started = http_async_job_try_start(&req, "http_async_job", 4096, fake_job_fn, NULL);

    TEST_CHECK(!started, "a failed handoff begin() must refuse the job");
    TEST_CHECK(!http_async_job_busy(), "busy must be cleared again after a begin() failure");
    TEST_CHECK(g_test_stub_async_complete_calls == 0,
               "no async copy was ever created, so complete() must not be called");
}

static void test_task_create_failure_undoes_begin_and_clears_busy(void)
{
    reset_stubs();
    g_test_stub_xtaskcreate_result = 0; // pdFAIL
    httpd_req_t req = {0};

    bool started = http_async_job_try_start(&req, "http_async_job", 4096, fake_job_fn, NULL);

    TEST_CHECK(!started, "a failed xTaskCreate() must refuse the job");
    TEST_CHECK(!http_async_job_busy(), "busy must be cleared again after a task-create failure");
    TEST_CHECK(g_test_stub_async_complete_calls == 1,
               "the async copy from begin() must be undone via exactly one complete() call");
    TEST_CHECK(s_fn_calls == 0, "fn must never run when the job task itself could not be created");
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

void run_test_http_async_job(void)
{
    test_admits_when_idle();
    test_second_call_refused_while_busy();
    test_begin_failure_refuses_and_clears_busy();
    test_task_create_failure_undoes_begin_and_clears_busy();
    test_run_job_calls_fn_then_completes_exactly_once_and_clears_busy();
}
