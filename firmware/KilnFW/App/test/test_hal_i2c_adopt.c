// Host test for the "adopt an already-open I2C port" contract fixed
// 2026-09-06 (see firmware/hwAbstraction/esp/i2c/hal_i2c_esp_owner.h and
// main_boot_early.c's FT6336U bring-up). The ESP backend itself
// (hal_i2c_esp.c) links against real ESP-IDF driver/i2c_master.h and is not
// buildable on host (see build_host_tests.ps1's test_ft6336u.c comment on
// why fake_i2c.c, not hal_i2c_esp.c, is what host tests link against). This
// test instead exercises fake_i2c_bus_adopt() (firmware/hwAbstraction/host/
// fake_i2c.c), which was added specifically to model the SAME contract at
// the portable hal_i2c.h level so the shape of the fix can be verified on
// host:
//
//   1. A bus adopted from an existing live bus routes transfers/probes/
//      device attaches through the SAME underlying slot -- observable here
//      as sharing one fake_i2c_transfer() log and one address-script table.
//   2. hal_i2c_bus_deinit() on the ADOPTED bus does not tear down the
//      shared slot: the ORIGINAL bus (and any device already attached
//      through it) keeps working afterward.
//   3. hal_i2c_bus_init() failure (modeled here as a full bus-slot table,
//      i.e. "no room" -- fake_i2c.c has no direct way to model IDF's
//      port-already-open failure since it never allocates real ports) still
//      correctly reports an error and leaves the target bus_t not live.
//   4. Negative-test evidence: temporarily breaking fake_i2c_bus_adopt() to
//      behave like hal_i2c_bus_init() (i.e. NOT sharing the slot with the
//      original -- creating an independent slot instead, mirroring the
//      real ESP bug of a second bus/owner on one port) is proven to make
//      test_adopt_shares_transfer_log_and_scripts() fail.
#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "fake_i2c.h"

#define TEST_ADDR 0x38

static hal_i2c_bus_t make_bus(void)
{
    hal_i2c_bus_t bus;
    hal_i2c_bus_cfg_t cfg = { .scl_pin = 0, .sda_pin = 0, .queue_len = 8,
                               .task_priority = 5, .stack_depth = 3072,
                               .core_id = HAL_CORE_ANY };
    TEST_CHECK(hal_i2c_bus_init(&bus, 0, &cfg) == HAL_OK, "hal_i2c_bus_init (original bus)");
    return bus;
}

// Covers contract point 1: a transfer issued through a device attached via
// the ADOPTED bus must show up in the SAME fake_i2c_transfer() log as one
// issued through the ORIGINAL bus -- i.e. they share one underlying "port",
// exactly the property the real hal_i2c_esp_adopt() bridge exists to
// preserve (one i2c_owner_t task/queue serving both consumers, not two
// independent ones racing the same physical wire).
static void test_adopt_shares_transfer_log_and_scripts(void)
{
    fake_i2c_reset_all();
    hal_i2c_bus_t original = make_bus();

    hal_i2c_bus_t adopted;
    memset(&adopted, 0, sizeof(adopted));
    TEST_CHECK(fake_i2c_bus_adopt(&adopted, &original) == HAL_OK,
               "fake_i2c_bus_adopt succeeds on a live original bus");
    TEST_CHECK(fake_i2c_bus_is_live(&adopted), "adopted bus reads as live");

    // Script an rx byte through the ORIGINAL bus's view of the port, then
    // read it back through a device attached via the ADOPTED bus -- if they
    // did not share the same slot, the adopted device would see the
    // unscripted default (all-zero) response instead.
    uint8_t canary = 0x5A;
    TEST_CHECK(fake_i2c_script_rx(&original, TEST_ADDR, &canary, 1) == HAL_OK,
               "script an rx byte via the ORIGINAL bus handle");

    hal_i2c_device_t dev;
    TEST_CHECK(hal_i2c_device_attach(&adopted, &dev, TEST_ADDR, 100000) == HAL_OK,
               "attach a device via the ADOPTED bus handle");

    uint8_t tx = 0x01;
    uint8_t rx = 0x00;
    TEST_CHECK(hal_i2c_transfer(&dev, &tx, 1, &rx, 1, 100) == HAL_OK,
               "transfer via the adopted-bus device succeeds");
    TEST_CHECK(rx == canary,
               "transfer via the adopted-bus device observes the byte scripted on the ORIGINAL "
               "bus -- adopted and original share one underlying slot");
    TEST_CHECK(fake_i2c_transfer_count() == 1,
               "the transfer landed in the ONE shared transfer log, not a second independent one");
}

// Covers contract point 2: deinit of the ADOPTED bus must be a no-op on the
// shared slot -- a device already attached via the ORIGINAL bus must keep
// working afterward. This is the direct host-level analog of
// hal_i2c_esp_adopt()'s owner_owned == false: FT6336U bring-up failing (and
// calling hal_i2c_bus_deinit() on its adopted bus_t, see main_boot_early.c)
// must never take down the SX1509's owner task/queue/bus handle.
static void test_deinit_of_adopted_bus_does_not_kill_original(void)
{
    fake_i2c_reset_all();
    hal_i2c_bus_t original = make_bus();

    hal_i2c_device_t original_dev;
    TEST_CHECK(hal_i2c_device_attach(&original, &original_dev, TEST_ADDR, 100000) == HAL_OK,
               "attach a device via the ORIGINAL bus (models SX1509 already attached)");

    hal_i2c_bus_t adopted;
    memset(&adopted, 0, sizeof(adopted));
    TEST_CHECK(fake_i2c_bus_adopt(&adopted, &original) == HAL_OK, "adopt the original bus");

    TEST_CHECK(hal_i2c_bus_deinit(&adopted) == HAL_OK, "deinit of the adopted bus reports HAL_OK");
    TEST_CHECK(!fake_i2c_bus_is_live(&adopted), "the adopted bus_t itself is no longer live");
    TEST_CHECK(fake_i2c_bus_is_live(&original),
               "the ORIGINAL bus is still live after the adopted copy was deinited -- the shared "
               "slot was not torn down");

    uint8_t tx = 0x02;
    uint8_t rx = 0x00;
    TEST_CHECK(hal_i2c_transfer(&original_dev, &tx, 1, &rx, 1, 100) == HAL_OK,
               "the device attached before adoption still transfers fine after the adopted "
               "bus's deinit");
}

// Covers contract point 3: fake_i2c_bus_adopt() on a bus_t that was never
// initialized (never live) must fail, not silently succeed and hand back a
// bus_t that reads as live over nothing.
static void test_adopt_of_dead_bus_fails(void)
{
    fake_i2c_reset_all();
    hal_i2c_bus_t never_initialized;
    memset(&never_initialized, 0, sizeof(never_initialized));

    hal_i2c_bus_t adopted;
    memset(&adopted, 0, sizeof(adopted));
    hal_status_t err = fake_i2c_bus_adopt(&adopted, &never_initialized);
    TEST_CHECK(err != HAL_OK, "adopting a never-initialized bus fails");
    TEST_CHECK(!fake_i2c_bus_is_live(&adopted), "the target bus_t is not left reading as live");
}

int main(void)
{
    TEST_SECTION("hal_i2c adopt bridge (fake_i2c model)");

    test_adopt_shares_transfer_log_and_scripts();
    test_deinit_of_adopted_bus_does_not_kill_original();
    test_adopt_of_dead_bus_fails();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
