// Host tests for the REAL discrete_task.c (HOST_TEST_COVERAGE_GAPS round 2,
// campaign R2-2): the E-stop (S7) and mainFault (S6a) sampling loop.
// discrete_pin_policy and debounce_policy have pure tests; what had none is
// the task that wires pins -> polarity -> debounce -> published flags and
// checks in with the watchdog each period. Driven through the task harness
// (vTaskDelayUntil hook) with fake_gpio pin levels scripted per iteration.
//
// Debounce windows are derived from the task period: N_estop = ceil(50/10) = 5
// samples, N_mainfault = ceil(200/10) = 20.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"
#include "task_harness.h"

#include "board_pins.h"
#include "debounce_policy.h"
#include "discrete_pin_policy.h"
#include "fake_gpio.h"
#include "hal_gpio.h"
#include "task_priorities.h"
#include "tasks/discrete_task.h"
#include "tasks/watchdog_task.h"

int g_test_failures = 0;
int g_test_count = 0;

#define MAXIT 64
#define N_ESTOP SAFTYFW_DEBOUNCE_SAMPLES(SAFTYFW_ESTOP_DEBOUNCE_MS, SAFTYFW_PERIOD_DISCRETE_TASK_MS)
#define N_MF SAFTYFW_DEBOUNCE_SAMPLES(SAFTYFW_MAIN_FAULT_DEBOUNCE_MS, SAFTYFW_PERIOD_DISCRETE_TASK_MS)

/* ---- stubs for the two things discrete_task.c calls out to ---- */
static uint8_t s_active_level;
uint8_t config_store_get_estop_active_level(void) { return s_active_level; }

static int s_checkins;
static int s_checkin_id_wrong;
void watchdog_task_checkin(watchdog_checkin_id_t id)
{
    s_checkins++;
    if (id != WATCHDOG_CHECKIN_DISCRETE_TASK) {
        s_checkin_id_wrong++;
    }
}

/* ---- script: raw pin levels (true = GPIO high) for iteration i ---- */
static bool s_estop_hi[MAXIT];
static bool s_mf_hi[MAXIT];
static bool s_estop_out[MAXIT];
static bool s_mf_out[MAXIT];
static int s_n;
static int s_it;
static uint8_t s_level_at[MAXIT]; /* estop active level to present at iteration i */

static void delay_hook(void)
{
    if (s_it > 0) { /* state published by the previous iteration's sample */
        s_estop_out[s_it - 1] = discrete_task_estop_pressed();
        s_mf_out[s_it - 1] = discrete_task_main_fault();
    }
    if (s_it >= s_n) {
        th_abort();
        return;
    }
    s_active_level = s_level_at[s_it];
    fake_gpio_force_state(SAFTYFW_PIN_ESTOP, HAL_GPIO_DIR_IN, s_estop_hi[s_it]);
    fake_gpio_force_state(SAFTYFW_PIN_MAIN_FAULT, HAL_GPIO_DIR_IN, s_mf_hi[s_it]);
    s_it++;
}

static void fresh(void)
{
    th_reset();
    fake_gpio_reset();
    s_checkins = 0;
    s_checkin_id_wrong = 0;
    s_active_level = DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH;
    memset(s_estop_hi, 0, sizeof(s_estop_hi));
    memset(s_mf_hi, 1, sizeof(s_mf_hi)); /* mainFault idle = GPIO high (active low) */
    memset(s_level_at, 0, sizeof(s_level_at));
    memset(s_estop_out, 0, sizeof(s_estop_out));
    memset(s_mf_out, 0, sizeof(s_mf_out));
    s_it = 0;
    s_n = 0;
}

/* Start the task and run n iterations of the script. Returns discrete_task_start(). */
static bool run(int n)
{
    s_n = n;
    s_it = 0;
    th_set_delay_hook(delay_hook);
    bool ok = discrete_task_start();
    if (ok) {
        th_run_captured_task();
        /* the last iteration's result is recorded by the aborting hook call */
    }
    return ok;
}

static void test_start(void)
{
    TEST_SECTION("discrete_task_start() -- pins and initial state");
    fresh();
    TEST_CHECK(run(1), "start succeeds");
    TEST_CHECK(fake_gpio_current_dir(SAFTYFW_PIN_ESTOP) == HAL_GPIO_DIR_IN, "E-stop pin is an input");
    TEST_CHECK(fake_gpio_current_dir(SAFTYFW_PIN_MAIN_FAULT) == HAL_GPIO_DIR_IN, "mainFault pin is an input");
    TEST_CHECK(!discrete_task_estop_pressed(), "E-stop starts not pressed");
    TEST_CHECK(!discrete_task_main_fault(), "mainFault starts not asserted");
    TEST_CHECK(N_ESTOP == 5 && N_MF == 20, "debounce windows derive from the 10 ms period (5 / 20 samples)");
}

static void test_estop_debounce_active_high(void)
{
    TEST_SECTION("E-stop: default ACTIVE_HIGH, 5-sample assert and release debounce");
    fresh();
    for (int i = 2; i < 20; i++) s_estop_hi[i] = true;   /* press from iteration 2 */
    for (int i = 20; i < 40; i++) s_estop_hi[i] = false; /* release at 20 */
    TEST_CHECK(run(40), "start");
    TEST_CHECK(!s_estop_out[2 + (int)N_ESTOP - 2], "N-1 high samples: still not pressed");
    TEST_CHECK(s_estop_out[2 + (int)N_ESTOP - 1], "Nth consecutive high sample publishes pressed");
    TEST_CHECK(s_estop_out[19], "stays pressed while held");
    TEST_CHECK(s_estop_out[20 + (int)N_ESTOP - 2], "N-1 low samples after press: still pressed (release debounced)");
    TEST_CHECK(!s_estop_out[20 + (int)N_ESTOP - 1], "Nth low sample releases");
    TEST_CHECK(s_checkins == 40, "one watchdog check-in per period");
    TEST_CHECK(s_checkin_id_wrong == 0, "check-ins use the discrete_task id");
}

static void test_estop_glitch_rejected(void)
{
    TEST_SECTION("E-stop: a glitch shorter than the window never publishes");
    fresh();
    for (int rep = 0; rep < 6; rep++) {
        for (int k = 0; k < (int)N_ESTOP - 1; k++) s_estop_hi[rep * 6 + k] = true;
        /* sample rep*6+N-1 low breaks the streak */
    }
    TEST_CHECK(run(40), "start");
    bool ever = false;
    for (int i = 0; i < 40; i++) ever |= s_estop_out[i];
    TEST_CHECK(!ever, "repeated N-1 sample bursts never assert E-stop");
}

static void test_estop_active_low_runtime(void)
{
    TEST_SECTION("E-stop: polarity param re-read every sample (no reboot)");
    fresh();
    /* GPIO low throughout. ACTIVE_HIGH: low = healthy. Flip to ACTIVE_LOW at 10: low = pressed. */
    for (int i = 0; i < 30; i++) {
        s_estop_hi[i] = false;
        s_level_at[i] = (i < 10) ? DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH : DISCRETE_PIN_POLICY_ESTOP_ACTIVE_LOW;
    }
    TEST_CHECK(run(30), "start");
    TEST_CHECK(!s_estop_out[9], "ACTIVE_HIGH + low pin: not pressed");
    TEST_CHECK(!s_estop_out[10 + (int)N_ESTOP - 2], "after flip: debounce not yet satisfied");
    TEST_CHECK(s_estop_out[10 + (int)N_ESTOP - 1], "after flip to ACTIVE_LOW the low pin asserts E-stop");
}

static void test_main_fault(void)
{
    TEST_SECTION("mainFault: active LOW, 20-sample debounce");
    fresh();
    for (int i = 3; i < 40; i++) s_mf_hi[i] = false;  /* asserted from 3 */
    for (int i = 40; i < 80 && i < MAXIT; i++) s_mf_hi[i] = true;
    TEST_CHECK(run(MAXIT), "start");
    TEST_CHECK(!s_mf_out[3 + (int)N_MF - 2], "N-1 low samples: not yet");
    TEST_CHECK(s_mf_out[3 + (int)N_MF - 1], "Nth consecutive low sample publishes mainFault");
    TEST_CHECK(s_mf_out[39], "held");
    TEST_CHECK(s_mf_out[40 + (int)N_MF - 2], "release also debounced");
    TEST_CHECK(!s_mf_out[40 + (int)N_MF - 1], "Nth high sample clears");
    TEST_CHECK(!s_estop_out[30], "mainFault does not leak into E-stop");
}

static void test_independence(void)
{
    TEST_SECTION("E-stop and mainFault debounce independently");
    fresh();
    for (int i = 0; i < 30; i++) { s_estop_hi[i] = true; s_mf_hi[i] = false; }
    TEST_CHECK(run(30), "start");
    TEST_CHECK(s_estop_out[(int)N_ESTOP - 1] && !s_mf_out[(int)N_ESTOP - 1], "E-stop asserts at 5 while mainFault still debouncing");
    TEST_CHECK(s_mf_out[(int)N_MF - 1], "mainFault asserts at 20");
}

/* Nested-run guard (review B1): task_harness exits 2 with a message when a
 * hook calls th_run_captured_task() while a run is active. exit() cannot be
 * observed in-process, so the parent re-runs this exe with a child argument
 * and checks the exit code and stderr text. */
static void nested_hook(void)
{
    th_run_captured_task(); /* illegal: a run is already active */
    th_abort();
}

static int nested_child(void)
{
    fresh();
    th_set_delay_hook(nested_hook);
    if (discrete_task_start()) {
        th_run_captured_task();
    }
    return 0; /* reaching here means the guard did not fire */
}

static void test_harness_nested_run_guard(const char *self)
{
    TEST_SECTION("task_harness -- nested th_run_captured_task() exits 2");
    char cmd[1200];
    char errpath[1024]; /* beside the test executable (build output dir), never the caller's cwd */
    snprintf(errpath, sizeof(errpath), "%s.nested_child_stderr.txt", self);
    snprintf(cmd, sizeof(cmd), "\"\"%s\" --nested-child 2> \"%s\"\"", self, errpath);
    int rc = system(cmd);
    TEST_CHECK(rc == 2, "nested run exits with code 2");
    char buf[256] = {0};
    FILE *f = fopen(errpath, "rb");
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);
        remove(errpath);
    }
    TEST_CHECK(strstr(buf, "nested th_run_captured_task") != NULL, "nested run says why it exited");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--nested-child") == 0) {
        return nested_child();
    }
    test_start();
    test_estop_debounce_active_high();
    test_estop_glitch_rejected();
    test_estop_active_low_runtime();
    test_main_fault();
    test_independence();
    test_harness_nested_run_guard(argv[0]);

    printf("\n%d checks, %d failures\n", g_test_count, g_test_failures);
    return g_test_failures == 0 ? 0 : 1;
}
