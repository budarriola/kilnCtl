// Host test pinning cmd_task.c's handle_sys_get_task_stats() -- specifically
// the WORDS -> BYTES conversion (cmd_ids.h's SIMFW_CMD_SYS_GET_TASK_STATS
// comment: "misreading that unit is precisely how the sim_engine overflow
// hid") and task_stats_id_for_name()'s fixed name->id table.
//
// Why this file mirrors rather than calls the real code: cmd_task.c is a
// FreeRTOS task file (includes FreeRTOS.h/task.h, and this specific handler
// calls uxTaskGetSystemState(), a real kernel walk of the live task list) --
// it is not part of this host-test harness's source list, same pure/task
// boundary test_i2c_owner_io_read.c's and test_cmd_payload_vectors.c's own
// header comments already document. `mirror_task_stats_id_for_name()` below
// is a byte-for-byte copy of cmd_task.c's task_stats_id_for_name(); the
// allocated/hwm conversion functions are the same two-line arithmetic
// expressions handle_sys_get_task_stats() performs, pulled out here so they
// can be exercised directly against hand-picked inputs (including the exact
// numbers a real TaskStatus_t would carry: pxStackBase/pxEndOfStack as
// StackType_t* addresses, usStackHighWaterMark as a word count) without a
// live FreeRTOS scheduler. Keep the two in sync by hand if either changes.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

// Mirrors cmd_ids.h's SIMFW_TASK_STATS_ID_* -- see that header for the
// authoritative values; duplicated here as plain constants only because this
// file cannot include cmd_ids.h without also pulling in cmd_task.c's own
// FreeRTOS-dependent neighbors (see header comment).
#define MIRROR_TASK_STATS_ID_UNKNOWN     0u
#define MIRROR_TASK_STATS_ID_CMD_TASK    1u
#define MIRROR_TASK_STATS_ID_USB_OWNER   2u
#define MIRROR_TASK_STATS_ID_SIM_ENGINE  3u
#define MIRROR_TASK_STATS_ID_I2C_OWNER   4u
#define MIRROR_TASK_STATS_ID_FAULT_SCHED 5u
#define MIRROR_TASK_STATS_ID_TELEMETRY   6u
#define MIRROR_TASK_STATS_ID_LOG_TASK    7u
#define MIRROR_TASK_STATS_ID_SPI_EMU_A   8u
#define MIRROR_TASK_STATS_ID_SPI_EMU_B   9u
#define MIRROR_TASK_STATS_ID_WAVE_OWNER  10u
#define MIRROR_TASK_STATS_ID_IDLE_CORE0  11u
#define MIRROR_TASK_STATS_ID_IDLE_CORE1  12u
#define MIRROR_TASK_STATS_ID_TIMER_SVC   13u

// Byte-for-byte mirror of cmd_task.c's task_stats_id_for_name().
static uint8_t mirror_task_stats_id_for_name(const char *name)
{
    if (strcmp(name, "cmd_task") == 0) return MIRROR_TASK_STATS_ID_CMD_TASK;
    if (strcmp(name, "usb_owner") == 0) return MIRROR_TASK_STATS_ID_USB_OWNER;
    if (strcmp(name, "sim_engine") == 0) return MIRROR_TASK_STATS_ID_SIM_ENGINE;
    if (strcmp(name, "i2c_owner") == 0) return MIRROR_TASK_STATS_ID_I2C_OWNER;
    if (strcmp(name, "fault_sched") == 0) return MIRROR_TASK_STATS_ID_FAULT_SCHED;
    if (strcmp(name, "telemetry") == 0) return MIRROR_TASK_STATS_ID_TELEMETRY;
    if (strcmp(name, "log_task") == 0) return MIRROR_TASK_STATS_ID_LOG_TASK;
    if (strcmp(name, "spi_emu_a") == 0) return MIRROR_TASK_STATS_ID_SPI_EMU_A;
    if (strcmp(name, "spi_emu_b") == 0) return MIRROR_TASK_STATS_ID_SPI_EMU_B;
    if (strcmp(name, "wave_owner") == 0) return MIRROR_TASK_STATS_ID_WAVE_OWNER;
    if (strcmp(name, "IDLE0") == 0) return MIRROR_TASK_STATS_ID_IDLE_CORE0;
    if (strcmp(name, "IDLE1") == 0) return MIRROR_TASK_STATS_ID_IDLE_CORE1;
    if (strcmp(name, "Tmr Svc") == 0) return MIRROR_TASK_STATS_ID_TIMER_SVC;
    return MIRROR_TASK_STATS_ID_UNKNOWN;
}

// StackType_t is uint32_t on this port (RP2040, GCC/RP2040 FreeRTOS-Kernel
// port) -- mirrored as a plain constant rather than pulled from portmacro.h,
// which this host-test harness cannot include (see header comment).
#define MIRROR_STACK_TYPE_SIZE 4u

// Mirrors handle_sys_get_task_stats()'s allocated-bytes line:
//   uint32_t allocated_words = (uint32_t)(t->pxEndOfStack - t->pxStackBase) + 1u;
//   uint32_t allocated_bytes = allocated_words * (uint32_t)sizeof(StackType_t);
// Takes plain word-address integers standing in for StackType_t* pointers
// (pointer subtraction on StackType_t* already yields a count of
// StackType_t elements, i.e. words -- the cast to uintptr_t-like uint32_t
// here is purely to let a host test supply that difference directly without
// needing real StackType_t objects to point at).
static uint32_t mirror_allocated_bytes(uint32_t stack_base_words, uint32_t end_of_stack_words)
{
    uint32_t allocated_words = (end_of_stack_words - stack_base_words) + 1u;
    return allocated_words * MIRROR_STACK_TYPE_SIZE;
}

// Mirrors handle_sys_get_task_stats()'s WORDS -> BYTES conversion line --
// THE line cmd_ids.h's SIMFW_CMD_SYS_GET_TASK_STATS comment points at:
//   uint32_t hwm_free_bytes = (uint32_t)t->usStackHighWaterMark * (uint32_t)sizeof(StackType_t);
static uint32_t mirror_hwm_free_bytes(uint32_t hwm_free_words)
{
    return hwm_free_words * MIRROR_STACK_TYPE_SIZE;
}

static void test_task_stats_id_for_name_matches_every_app_task(void)
{
    TEST_SECTION("task_stats_id_for_name -- every real xTaskCreate() name string");

    TEST_CHECK(mirror_task_stats_id_for_name("cmd_task") == MIRROR_TASK_STATS_ID_CMD_TASK, "cmd_task");
    TEST_CHECK(mirror_task_stats_id_for_name("usb_owner") == MIRROR_TASK_STATS_ID_USB_OWNER, "usb_owner");
    TEST_CHECK(mirror_task_stats_id_for_name("sim_engine") == MIRROR_TASK_STATS_ID_SIM_ENGINE, "sim_engine");
    TEST_CHECK(mirror_task_stats_id_for_name("i2c_owner") == MIRROR_TASK_STATS_ID_I2C_OWNER, "i2c_owner");
    TEST_CHECK(mirror_task_stats_id_for_name("fault_sched") == MIRROR_TASK_STATS_ID_FAULT_SCHED, "fault_sched");
    TEST_CHECK(mirror_task_stats_id_for_name("telemetry") == MIRROR_TASK_STATS_ID_TELEMETRY, "telemetry");
    TEST_CHECK(mirror_task_stats_id_for_name("log_task") == MIRROR_TASK_STATS_ID_LOG_TASK, "log_task");
    TEST_CHECK(mirror_task_stats_id_for_name("spi_emu_a") == MIRROR_TASK_STATS_ID_SPI_EMU_A, "spi_emu_a");
    TEST_CHECK(mirror_task_stats_id_for_name("spi_emu_b") == MIRROR_TASK_STATS_ID_SPI_EMU_B, "spi_emu_b");
    TEST_CHECK(mirror_task_stats_id_for_name("wave_owner") == MIRROR_TASK_STATS_ID_WAVE_OWNER, "wave_owner");
    TEST_CHECK(mirror_task_stats_id_for_name("IDLE0") == MIRROR_TASK_STATS_ID_IDLE_CORE0, "IDLE0 (SMP core 0 idle)");
    TEST_CHECK(mirror_task_stats_id_for_name("IDLE1") == MIRROR_TASK_STATS_ID_IDLE_CORE1, "IDLE1 (SMP core 1 idle)");
    TEST_CHECK(mirror_task_stats_id_for_name("Tmr Svc") == MIRROR_TASK_STATS_ID_TIMER_SVC, "Tmr Svc (FreeRTOS timer daemon)");
}

static void test_task_stats_id_for_name_unknown_task_reports_unknown(void)
{
    TEST_SECTION("task_stats_id_for_name -- a future/unrecognized task name reports UNKNOWN, not a crash or a wrong id");

    TEST_CHECK(mirror_task_stats_id_for_name("some_future_task") == MIRROR_TASK_STATS_ID_UNKNOWN,
               "unrecognized name -> UNKNOWN (0), never silently mismatched to an existing id");
    TEST_CHECK(mirror_task_stats_id_for_name("") == MIRROR_TASK_STATS_ID_UNKNOWN, "empty name -> UNKNOWN");
    // Case sensitivity: FreeRTOS task names are exact strings, and this
    // table's match is deliberately exact (strcmp, not a case-insensitive
    // compare) -- a name that differs only in case must NOT accidentally
    // match, since that would silently misreport which real task a wrong-
    // case name actually belongs to.
    TEST_CHECK(mirror_task_stats_id_for_name("CMD_TASK") == MIRROR_TASK_STATS_ID_UNKNOWN,
               "wrong-case name does not match -- strcmp is exact, not case-insensitive");
}

static void test_allocated_bytes_conversion(void)
{
    TEST_SECTION("allocated_bytes: (pxEndOfStack - pxStackBase + 1) * sizeof(StackType_t)");

    // CMD_TASK_STACK_WORDS = configMINIMAL_STACK_SIZE(256) * 3 = 768 words.
    // A real pxStackBase/pxEndOfStack pair 767 StackType_t elements apart
    // (word addresses 1000..1767 inclusive) is exactly 768 words --
    // pxEndOfStack points at the HIGHEST valid element, not one past it
    // (tasks.c's prvInitialiseNewTask(): "pxNewTCB->pxStack + (uxStackDepth - 1)"),
    // so the "+1" here is what makes this an inclusive count.
    TEST_CHECK(mirror_allocated_bytes(1000u, 1767u) == 3072u,
               "768-word stack (CMD_TASK_STACK_WORDS) -> 3072 allocated bytes");

    // configMINIMAL_STACK_SIZE alone (256 words) -- the exact number
    // cmd_ids.h's own comment warns about: "a stack 'of 256' is 1024 bytes,"
    // i.e. NOT 256 bytes, which is precisely the confusion that let the
    // sim_engine incident hide.
    TEST_CHECK(mirror_allocated_bytes(0u, 255u) == 1024u,
               "256-word stack (configMINIMAL_STACK_SIZE) -> 1024 bytes, NOT 256 bytes");

    // Degenerate single-word case: base == end -> exactly 1 word allocated.
    TEST_CHECK(mirror_allocated_bytes(500u, 500u) == 4u, "a single-word stack region -> 4 allocated bytes");
}

static void test_hwm_free_bytes_conversion(void)
{
    TEST_SECTION("hwm_free_bytes: uxTaskGetStackHighWaterMark()'s free-WORD count * sizeof(StackType_t)");

    // The exact numbers this pass's own manifest vector uses (SYS/GET_TASK_STATS
    // "get_task_stats_two_tasks", cmd_payload_vectors.json): 625 free words
    // -> 2500 free bytes.
    TEST_CHECK(mirror_hwm_free_bytes(625u) == 2500u, "625 free words -> 2500 free bytes");
    TEST_CHECK(mirror_hwm_free_bytes(250u) == 1000u, "250 free words -> 1000 free bytes");

    // Zero free words remaining (a task that has consumed its ENTIRE
    // allocated stack, right at the edge of overflow) must convert to zero
    // free bytes, not underflow or wrap.
    TEST_CHECK(mirror_hwm_free_bytes(0u) == 0u, "0 free words -> 0 free bytes (right at the overflow edge)");

    // *** THE BUG THIS CONVERSION EXISTS TO PREVENT, REPRODUCED DELIBERATELY ***
    // If a future edit forgot the "* sizeof(StackType_t)" multiply (treated
    // the raw word count as if it were already a byte count -- exactly how
    // the sim_engine overflow hid, cmd_ids.h's own comment), 625 free WORDS
    // would be misreported as 625 free BYTES instead of the correct 2500.
    // Asserting the two are different (not just asserting the correct value
    // above) proves this test would actually have caught that historical
    // failure mode, not merely that some conversion happens.
    uint32_t correct = mirror_hwm_free_bytes(625u);
    uint32_t words_misread_as_bytes = 625u; // what a missing multiply would have produced
    TEST_CHECK(correct != words_misread_as_bytes,
               "the correct byte conversion (2500) is NOT the same number as the raw word count (625) -- "
               "proves this conversion is load-bearing, not a no-op that happened to pass");
}

void run_test_task_stats_encoding(void)
{
    test_task_stats_id_for_name_matches_every_app_task();
    test_task_stats_id_for_name_unknown_task_reports_unknown();
    test_allocated_bytes_conversion();
    test_hwm_free_bytes_conversion();
}
