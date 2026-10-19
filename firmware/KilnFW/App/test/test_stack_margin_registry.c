// Host test for App/drivers/common/stack_margin.c's registry itself --
// specifically stack_margin_register()'s de-dup behavior, added after an
// opus review found the three OTA background-task launchers
// (ota_http_esp.c/ota_http_pico.c/ota_http_recovery.c) each register the
// SAME name against the SAME file-scope TaskHandle_t* slot on every POST to
// their route, and the registry (fixed-size, no removal path) had no
// protection against that: enough repeat POSTs would silently walk it to
// STACK_MARGIN_MAX_TASKS, after which every OTHER task's registration
// anywhere in the firmware starts failing too.
//
// This links against the REAL stack_margin.c (already a normal entry in
// build_host_tests.ps1's $sources, compiled once for the whole host-test
// executable) rather than #including it, unlike most single-purpose test
// files in this directory -- stack_margin.c has no static file-scope state
// this test needs to reach directly, only the public registry API in
// stack_margin.h, and multiple test files linking the same real .c object
// is exactly how test_display_power_wiring.c/test_stack_margin.c already
// share it.
//
// stack_margin.h documents the registry as having no internal locking,
// relying on every call site being effectively single-threaded with it --
// this test exercises that documented contract's OTHER half, the de-dup
// itself, which needs no threading to reach.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

#include "../drivers/common/stack_margin.h"

// stack_margin.h types task_handle_slot as `void *` specifically so callers
// outside App/drivers don't need freertos/task.h's TaskHandle_t -- this test
// takes advantage of that and uses plain pointer-sized locals instead.
//
// Deliberately only ONE unique name is ever registered by this whole file
// (below). test_main.c links every other test file's own real .c sources
// into this same executable/process, and several of them (test_boot_button.c,
// test_display_power_wiring.c, test_kiln_io_owner.c, ...) call the real
// *_start() functions that register real tasks into this same process-wide
// s_entries[] singleton before this test ever runs -- there is no reset
// between test files. A version of this test that registered two distinct
// new names was observed to fail on a full registry (STACK_MARGIN_MAX_TASKS
// reached by the time this file's second registration ran), which is a
// pre-existing shared-process constraint, not a de-dup bug -- see this
// file's own commit message. Needing only one spare slot keeps this test
// meaningful without asserting anything about how much registry headroom
// happens to be left when it runs.
static void *s_slot_a;
static void *s_slot_b;

static void test_repeat_register_same_name_and_slot_is_a_noop(void)
{
    size_t before = stack_margin_count();

    bool first = stack_margin_register("registry_test_task", &s_slot_a, 4096);
    TEST_CHECK(first, "first registration of a fresh name must succeed (registry has room for one more)");
    size_t after_first = stack_margin_count();
    TEST_CHECK(after_first == before + 1, "first registration must add exactly one row");

    // Same name, same slot pointer, repeated several times -- the shape of
    // a POST handler's background task re-registering on every request.
    for (int i = 0; i < 5; i++) {
        bool repeat = stack_margin_register("registry_test_task", &s_slot_a, 4096);
        TEST_CHECK(repeat, "a repeat registration with the same name+slot must still report success");
    }
    TEST_CHECK(stack_margin_count() == after_first,
               "repeat registrations with the same name+slot must never append a new row");
}

static void test_same_name_different_slot_is_refused_not_appended(void)
{
    // Reuses the SAME name registered above (no new slot needed): a second
    // registration under that name but a DIFFERENT handle-slot pointer is a
    // real name collision, not a repeat call, and must be refused rather
    // than silently accepted or silently appended as a second row.
    size_t before = stack_margin_count();
    bool collided = stack_margin_register("registry_test_task", &s_slot_b, 4096);
    TEST_CHECK(!collided, "same name with a DIFFERENT slot must be refused, not silently accepted");
    TEST_CHECK(stack_margin_count() == before,
               "a refused same-name/different-slot call must not append a row either");
}

static void test_registry_reads_back_the_dedup_row(void)
{
    // Sanity: the one row this file added really is readable back by name,
    // still pointing at slot A (not clobbered by the refused slot-B call
    // above) -- guards against a de-dup bug that returns true/false
    // correctly but mishandles what it actually stores.
    bool found = false;
    size_t n = stack_margin_count();
    for (size_t i = 0; i < n; i++) {
        const char *name = NULL;
        TEST_CHECK(stack_margin_read(i, &name, NULL, NULL, NULL, NULL), "stack_margin_read() must succeed for every index < count()");
        if (name && strcmp(name, "registry_test_task") == 0) {
            found = true;
        }
    }
    TEST_CHECK(found, "registry_test_task must be readable back after the repeat and collision registrations above");
}

// BM-01: stack_margin_register() takes the ADDRESS of a TaskHandle_t slot and reads through it on every
// get_stack_margin. main_network_http.c passed uart_proto->rx_task_handle by value after RAM-03 turned the
// protocol struct into a pointer, so the registry read the TCB's first word as a task handle and the INFO
// 0x04 reply never came. Every call in the boot file must pass an address.
static void test_main_network_http_registrations_pass_slot_addresses(void)
{
    TEST_SECTION("stack_margin: main_network_http.c registers slot addresses, not handle values (BM-01)");
    static const char *const candidates[] = {
        "../main_network_http.c",
        "App/main_network_http.c",
        "firmware/KilnFW/App/main_network_http.c",
    };
    char *text = test_read_source_anchored(__FILE__, "../main_network_http.c", candidates,
                                           sizeof(candidates) / sizeof(candidates[0]));
    if (!text) {
        TEST_CHECK(false, "could not locate App/main_network_http.c");
        return;
    }
    int calls = 0, bad = 0;
    const char *p = text;
    while ((p = strstr(p, "stack_margin_register(\"")) != NULL) {
        const char *q = strchr(p + strlen("stack_margin_register(\""), '"');
        p += strlen("stack_margin_register(\"");
        if (!q) {
            break;
        }
        q++; /* past the closing quote */
        while (*q == ',' || *q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') {
            q++;
        }
        calls++;
        if (*q != '&') {
            bad++;
        }
    }
    TEST_CHECK(calls >= 3, "main_network_http.c still has its stack_margin_register() calls");
    TEST_CHECK(bad == 0, "every stack_margin_register() slot argument is an address (&...), never a handle value");
    TEST_CHECK(strstr(text, "&ctx->uart_proto->rx_task_handle") != NULL, "uart_proto_rx registers the rx_task_handle address");
    free(text);
}

static void test_retain_exit_keeps_minimum(void)
{
    // Measurement hook: a task that exits retains its min free stack; the registry keeps the smallest.
    TEST_CHECK(!stack_margin_retain_exit("no_such_task", 100), "retain for an unregistered name is refused");
    TEST_CHECK(stack_margin_retain_exit("registry_test_task", 900), "retain for a registered name succeeds");
    TEST_CHECK(stack_margin_retain_exit("registry_test_task", 1500), "second retain succeeds");
    TEST_CHECK(stack_margin_retain_exit("registry_test_task", 700), "third retain succeeds");
    int seen = 0;
    for (size_t i = 0; i < stack_margin_count(); i++) {
        const char *name = NULL;
        bool alive = true;
        uint32_t ret = 0;
        TEST_CHECK(stack_margin_read(i, &name, NULL, NULL, NULL, &alive), "read ok");
        if (name && strcmp(name, "registry_test_task") == 0) {
            seen++;
            TEST_CHECK(!alive, "NULL-handle task reads as not alive");
            TEST_CHECK(stack_margin_retained(i, &ret), "retained value present");
            TEST_CHECK(ret == 700, "retained value is the minimum across exits (700)");
        } else if (name && strcmp(name, "update_fetch_wr") != 0) {
            TEST_CHECK(!stack_margin_retained(i, &ret), "tasks that never retained report none");
        }
    }
    TEST_CHECK(seen == 1, "registry_test_task row found once");
}

// UB-01: a self-deleting task clears its slot through stack_margin_task_exit(); the read path then
// reports the task dead and never calls uxTaskGetStackHighWaterMark() on the stale handle.
static void test_task_exit_clears_slot_and_reads_dead(void)
{
    TEST_SECTION("stack_margin: stack_margin_task_exit() clears the slot, read reports alive=false (UB-01)");
    static int fake_tcb;
    s_slot_a = &fake_tcb;
    size_t idx = (size_t)-1;
    size_t n = stack_margin_count();
    for (size_t i = 0; i < n; i++) {
        const char *name = NULL;
        if (stack_margin_read(i, &name, NULL, NULL, NULL, NULL) && name && strcmp(name, "registry_test_task") == 0) {
            idx = i;
        }
    }
    TEST_CHECK(idx != (size_t)-1, "registry_test_task row present");
    bool alive = false;
    TEST_CHECK(stack_margin_read(idx, NULL, NULL, NULL, NULL, &alive) && alive, "non-NULL slot reads alive");
    stack_margin_task_exit(&s_slot_a);
    TEST_CHECK(s_slot_a == NULL, "stack_margin_task_exit() NULLs the caller's slot");
    uint32_t hwm = 123;
    alive = true;
    TEST_CHECK(stack_margin_read(idx, NULL, NULL, &hwm, NULL, &alive), "read after exit succeeds");
    TEST_CHECK(!alive && hwm == 0, "read after exit reports alive=false, hwm 0");
    stack_margin_task_exit(NULL); /* must be a no-op, not a crash */
}

static char *ub01_read(const char *rel_from_test)
{
    char alt1[160], alt2[200];
    snprintf(alt1, sizeof(alt1), "App/%s", rel_from_test + 3);
    snprintf(alt2, sizeof(alt2), "firmware/KilnFW/App/%s", rel_from_test + 3);
    const char *const candidates[] = {rel_from_test, alt1, alt2};
    char *t = test_read_source_anchored(__FILE__, rel_from_test, candidates, 3);
    if (t) { /* normalise CRLF so multi-line patterns match either ending */
        char *w = t;
        for (char *r = t; *r; r++) {
            if (*r != '\r') {
                *w++ = *r;
            }
        }
        *w = '\0';
    }
    return t;
}

// UB-01 source scan: the read path holds s_read_lock across the slot load AND the stack walk, and every
// registered self-deleting task clears its slot through stack_margin_task_exit(), never a bare store.
static void test_ub01_read_lock_and_exit_sites(void)
{
    TEST_SECTION("stack_margin: read holds the lock across load + HWM; self-deleting tasks use task_exit (UB-01)");
    char *c = ub01_read("../drivers/common/stack_margin.c");
    TEST_CHECK(c != NULL, "stack_margin.c readable");
    if (c) {
        const char *rd = strstr(c, "bool stack_margin_read(");
        const char *lk = rd ? strstr(rd, "taskENTER_CRITICAL(&s_read_lock)") : NULL;
        const char *ld0 = rd ? strstr(rd, "*e->handle_slot") : NULL;
        const char *hw = ld0 ? strstr(ld0, "uxTaskGetStackHighWaterMark(handle)") : NULL;
        const char *ul = hw ? strstr(hw, "taskEXIT_CRITICAL(&s_read_lock)") : NULL;
        TEST_CHECK(rd && lk && ld0 && hw && ul && lk < ld0, "stack_margin_read(): ENTER(s_read_lock) < slot load < HWM < EXIT");
        const char *ex = strstr(c, "void stack_margin_task_exit(");
        const char *exl = ex ? strstr(ex, "taskENTER_CRITICAL(&s_read_lock)") : NULL;
        const char *exs = exl ? strstr(exl, "= NULL;") : NULL;
        const char *exu = exs ? strstr(exs, "taskEXIT_CRITICAL(&s_read_lock)") : NULL;
        TEST_CHECK(ex && exl && exs && exu, "stack_margin_task_exit(): slot cleared under s_read_lock");
        const char *re = strstr(c, "bool stack_margin_retain_exit(");
        const char *rel = re ? strstr(re, "taskENTER_CRITICAL(&s_read_lock)") : NULL;
        const char *rew = rel ? strstr(rel, "e->retained_valid = true;") : NULL;
        const char *reu = rew ? strstr(rew, "taskEXIT_CRITICAL(&s_read_lock)") : NULL;
        TEST_CHECK(re && rel && rew && reu, "stack_margin_retain_exit(): writes under s_read_lock (DF-02)");
        const char *rd2 = strstr(c, "bool stack_margin_retained(");
        const char *rdl = rd2 ? strstr(rd2, "taskENTER_CRITICAL(&s_read_lock)") : NULL;
        const char *rdv = rdl ? strstr(rdl, "retained_bytes") : NULL;
        const char *rdu = rdv ? strstr(rdv, "taskEXIT_CRITICAL(&s_read_lock)") : NULL;
        TEST_CHECK(rd2 && rdl && rdv && rdu, "stack_margin_retained(): reads under s_read_lock (DF-02)");
        free(c);
    }
    static const struct {
        const char *file;
        const char *call;
        const char *bare;
    } sites[] = {
        {"../drivers/http/ota_http_recovery.c", "stack_margin_task_exit(&s_recovery_exit_reboot_task)", "s_recovery_exit_reboot_task = NULL;"},
        {"../drivers/http/ota_http_recovery.c", "stack_margin_task_exit(&s_recovery_boot_reboot_task)", "s_recovery_boot_reboot_task = NULL;"},
        {"../drivers/net/pico_auto_update_boot.c", "stack_margin_task_exit(&s_task)", "done:\n    s_task = NULL;"},
        {"../drivers/update/update_fetch.c", "stack_margin_task_exit(&s_c->tls_task)", "s_c->tls_task = NULL;\n    s_c->busy = 0;"},
        {"../main_network_http.c", "stack_margin_task_exit(&s_ota_confirm_task)", "s_ota_confirm_task = NULL;\n            vTaskDelete"},
        {"../drivers/control/zones_current_sweep_task.c", "stack_margin_task_exit(&s_sweep.task)", "s_sweep.task = NULL;\n    /* Release"},
        {"../drivers/http/http_async_job.c", "stack_margin_task_exit(&s_task_handle)", "    s_task_handle = NULL;\n    portEXIT_CRITICAL"},
    };
    for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); i++) {
        char *t = ub01_read(sites[i].file);
        if (!t) {
            TEST_CHECK(false, sites[i].file);
            continue;
        }
        TEST_CHECK(strstr(t, sites[i].call) != NULL, sites[i].call);
        TEST_CHECK(strstr(t, sites[i].bare) == NULL, sites[i].bare);
        free(t);
    }
    {
        // UW-03: the writer is persistent; "go idle" must not unregister its stack_margin slot.
        char *t = ub01_read("../drivers/update/update_fetch.c");
        TEST_CHECK(t != NULL && strstr(t, "stack_margin_task_exit(&s_c->wr_task)") == NULL,
                   "UW-03: update_fetch_wr never calls stack_margin_task_exit (persistent writer)");
        free(t);
    }
    {
        // OR-08: the Pico rollback worker is persistent and static; it never exits or deletes itself.
        char *t = ub01_read("../drivers/http/ota_http_pico.c");
        TEST_CHECK(t != NULL && strstr(t, "stack_margin_task_exit(&s_ota_pico_rollback_task)") == NULL &&
                       strstr(t, "vTaskDelete(NULL)") == NULL && strstr(t, "xTaskCreateStaticPinnedToCore(") != NULL,
                   "OR-08: ota_pico_rollback is a static persistent task (no exit, no vTaskDelete)");
        free(t);
    }
}

void run_test_stack_margin_registry(void)
{
    TEST_SECTION("stack_margin registry: register() de-dup by (name, slot)");
    test_repeat_register_same_name_and_slot_is_a_noop();
    test_same_name_different_slot_is_refused_not_appended();
    test_registry_reads_back_the_dedup_row();
    test_main_network_http_registrations_pass_slot_addresses();
    test_retain_exit_keeps_minimum();
    test_task_exit_clears_slot_and_reads_dead();
    test_ub01_read_lock_and_exit_sites();
}
