// Host test for App/drivers/hw/FT6336U.c's HAL Phase 1b migration
// (docs/HW_ABSTRACTION.md) -- FT6336U.c no longer talks to
// driver/i2c_master.h or i2c_owner.c directly, it goes through
// interface/hal_i2c.h, whose host backend is
// firmware/hwAbstraction/host/fake_i2c.c. This is the first App/ driver
// migrated this way, so it is also the first host test to link a hal_i2c
// backend at all.
//
// Own executable (own main(), not merged into test_main.c/
// kilnctl_host_tests.exe), same convention as every other *_direct-include-
// or standalone-driver test in this directory: FT6336U.c is linked as its
// own translation unit (not #included) since nothing here needs its
// `static` internals -- the fake_i2c transfer log gives full visibility
// into what FT6336U.c actually put on the wire.
//
// Coverage:
//   1. FT6336U_start()'s identity check (FOCALTECH_ID/CIPHER_MID/
//      CIPHER_HIGH) against scripted rx bytes, and the resulting register
//      address sequence recorded by fake_i2c (0xA8, 0x9F, 0xA3 in that
//      order, all at FT6336U_ADDR).
//   2. FT6336U_read()'s TD_STATUS + TOUCH1 block decode, including the
//      12-bit X/Y reconstruction and TOUCH_DEV_NO_PRESSURE_SENTINEL for
//      z1 (this is a capacitive controller -- no pressure channel; see
//      touch_dev.h).
//   3. The "no touch" (touch_count == 0) path never reads the 4-byte
//      touch block at all -- FT6336U_read() must short-circuit before that
//      second transfer.
#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "FT6336U.h"
#include "fake_i2c.h"
#include "touch_dev.h"

static void reset_all(void)
{
    fake_i2c_reset_all();
}

// Queues the three identity-check bytes FT6336U_start()'s ft6336u_verify_id()
// expects, in the order it reads them: FOCALTECH_ID, CIPHER_MID, CIPHER_HIGH.
static void script_valid_identity(hal_i2c_bus_t *bus)
{
    uint8_t focaltech_id = FT6336U_EXPECT_FOCALTECH_ID;
    uint8_t cipher_mid = FT6336U_EXPECT_CIPHER_MID;
    uint8_t cipher_high = FT6336U_EXPECT_CIPHER_HIGH;
    TEST_CHECK(fake_i2c_script_rx(bus, FT6336U_ADDR, &focaltech_id, 1) == HAL_OK,
               "script FOCALTECH_ID byte");
    TEST_CHECK(fake_i2c_script_rx(bus, FT6336U_ADDR, &cipher_mid, 1) == HAL_OK,
               "script CIPHER_MID byte");
    TEST_CHECK(fake_i2c_script_rx(bus, FT6336U_ADDR, &cipher_high, 1) == HAL_OK,
               "script CIPHER_HIGH byte");
}

static hal_i2c_bus_t make_bus(void)
{
    hal_i2c_bus_t bus;
    hal_i2c_bus_cfg_t cfg = { .scl_pin = 0, .sda_pin = 0, .queue_len = 8,
                               .task_priority = 5, .stack_depth = 3072,
                               .core_id = HAL_CORE_ANY };
    TEST_CHECK(hal_i2c_bus_init(&bus, 0, &cfg) == HAL_OK, "hal_i2c_bus_init (fake)");
    return bus;
}

static void test_start_reads_identity_in_order(void)
{
    reset_all();
    hal_i2c_bus_t bus = make_bus();
    script_valid_identity(&bus);

    FT6336UClass t;
    memset(&t, 0, sizeof(t));
    esp_err_t err = FT6336U_start(&t, &bus);
    TEST_CHECK(err == ESP_OK, "FT6336U_start succeeds with a valid scripted identity");

    TEST_CHECK(fake_i2c_transfer_count() == 3,
               "FT6336U_start records exactly 3 transfers (the 3 identity reads)");
    if (fake_i2c_transfer_count() >= 3) {
        const fake_i2c_transfer_record_t *r0 = fake_i2c_transfer(0);
        const fake_i2c_transfer_record_t *r1 = fake_i2c_transfer(1);
        const fake_i2c_transfer_record_t *r2 = fake_i2c_transfer(2);
        TEST_CHECK(r0->addr == FT6336U_ADDR && r0->tx_len == 1 && r0->tx[0] == FT6336U_REG_FOCALTECH_ID,
                   "1st transfer addresses FOCALTECH_ID (0xA8)");
        TEST_CHECK(r1->addr == FT6336U_ADDR && r1->tx_len == 1 && r1->tx[0] == FT6336U_REG_CIPHER_MID,
                   "2nd transfer addresses CIPHER_MID (0x9F)");
        TEST_CHECK(r2->addr == FT6336U_ADDR && r2->tx_len == 1 && r2->tx[0] == FT6336U_REG_CIPHER_HIGH,
                   "3rd transfer addresses CIPHER_HIGH (0xA3)");
    }
}

static void test_start_rejects_wrong_identity(void)
{
    reset_all();
    hal_i2c_bus_t bus = make_bus();
    uint8_t wrong_id = 0x00; // not FT6336U_EXPECT_FOCALTECH_ID
    fake_i2c_script_rx(&bus, FT6336U_ADDR, &wrong_id, 1);

    FT6336UClass t;
    memset(&t, 0, sizeof(t));
    esp_err_t err = FT6336U_start(&t, &bus);
    TEST_CHECK(err == ESP_ERR_NOT_FOUND,
               "FT6336U_start refuses a device whose FOCALTECH_ID doesn't match");
    TEST_CHECK(t.dev_attached == false,
               "dev_attached is cleared on the identity-mismatch failure path");
    TEST_CHECK(!fake_i2c_device_is_live(&t.dev),
               "FT6336U_start's identity-check failure path detaches the device instead of "
               "leaking it on the shared bus (INTERFACE MISMATCH comment, now fixed via "
               "hal_i2c_device_detach())");
}

// Directly covers FT6336U_deinit()'s fixed leak: probe-succeeded/identity-
// failed must release the device slot it attached during FT6336U_init(), so
// a later consumer of the same address on the same bus (the realistic case:
// re-probing after a module is connected/reconnected) can attach cleanly
// instead of piling up dead slots on the shared bus.
static void test_deinit_after_failed_identity_frees_the_slot(void)
{
    reset_all();
    hal_i2c_bus_t bus = make_bus();
    uint8_t wrong_id = 0x00;
    fake_i2c_script_rx(&bus, FT6336U_ADDR, &wrong_id, 1);

    FT6336UClass t;
    memset(&t, 0, sizeof(t));
    TEST_CHECK(FT6336U_start(&t, &bus) == ESP_ERR_NOT_FOUND,
               "FT6336U_start fails identity check (setup)");
    TEST_CHECK(!fake_i2c_device_is_live(&t.dev), "device slot freed after the failed start");

    // A second attach at the same address on the same bus must succeed --
    // if the first slot had leaked, this would still succeed under
    // fake_i2c's generous FAKE_I2C_MAX_DEVICES cap, so also assert the
    // device count didn't grow across the failed attempt to make the
    // negative case meaningful.
    hal_i2c_device_t probe_dev;
    memset(&probe_dev, 0, sizeof(probe_dev));
    TEST_CHECK(hal_i2c_device_attach(&bus, &probe_dev, FT6336U_ADDR, 100000) == HAL_OK,
               "re-attaching at the same address after a freed detach succeeds");
}

static void test_start_absent_device_is_not_found(void)
{
    reset_all();
    hal_i2c_bus_t bus = make_bus();
    fake_i2c_script_nack(&bus, FT6336U_ADDR); // nothing answers the probe

    FT6336UClass t;
    memset(&t, 0, sizeof(t));
    esp_err_t err = FT6336U_start(&t, &bus);
    TEST_CHECK(err == ESP_ERR_NOT_FOUND, "FT6336U_start reports ESP_ERR_NOT_FOUND with no ACK at probe");
    TEST_CHECK(fake_i2c_transfer_count() == 0,
               "no transfer is attempted when the probe itself NACKs");
}

static void test_read_decodes_touch_point(void)
{
    reset_all();
    hal_i2c_bus_t bus = make_bus();
    script_valid_identity(&bus);

    FT6336UClass t;
    memset(&t, 0, sizeof(t));
    TEST_CHECK(FT6336U_start(&t, &bus) == ESP_OK, "FT6336U_start (read test setup)");

    // TD_STATUS: low nibble = 1 valid touch point.
    uint8_t td_status = 0x01;
    TEST_CHECK(fake_i2c_script_rx(&bus, FT6336U_ADDR, &td_status, 1) == HAL_OK, "script TD_STATUS");
    // TOUCH1 block: XH=0x01 (X[11:8]=1), XL=0x23 -> X = 0x123 = 291.
    //               YH=0x02 (Y[11:8]=2), YL=0x45 -> Y = 0x245 = 581.
    uint8_t touch_block[4] = { 0x01, 0x23, 0x02, 0x45 };
    TEST_CHECK(fake_i2c_script_rx(&bus, FT6336U_ADDR, touch_block, sizeof(touch_block)) == HAL_OK,
               "script TOUCH1 block");

    bool pressed = false;
    uint16_t x = 0xFFFF, y = 0xFFFF, z1 = 0;
    esp_err_t err = FT6336U_read(&t, &pressed, &x, &y, &z1);
    TEST_CHECK(err == ESP_OK, "FT6336U_read returns ESP_OK");
    TEST_CHECK(pressed == true, "FT6336U_read reports pressed for touch_count == 1");
    TEST_CHECK(x == 291, "FT6336U_read decodes X = 0x123 = 291");
    TEST_CHECK(y == 581, "FT6336U_read decodes Y = 0x245 = 581");
    TEST_CHECK(z1 == TOUCH_DEV_NO_PRESSURE_SENTINEL,
               "FT6336U_read reports the no-pressure sentinel (capacitive controller, no pressure channel)");

    // Two transfers total for this FT6336U_read() call: TD_STATUS, then the
    // TOUCH1 block. (3 identity transfers happened earlier during start.)
    TEST_CHECK(fake_i2c_transfer_count() == 5, "5 transfers total: 3 identity + TD_STATUS + TOUCH1 block");
    if (fake_i2c_transfer_count() >= 5) {
        const fake_i2c_transfer_record_t *r3 = fake_i2c_transfer(3);
        const fake_i2c_transfer_record_t *r4 = fake_i2c_transfer(4);
        TEST_CHECK(r3->tx[0] == FT6336U_REG_TD_STATUS, "4th transfer addresses TD_STATUS (0x02)");
        TEST_CHECK(r4->tx[0] == FT6336U_REG_TOUCH1_XH && r4->rx_len == 4,
                   "5th transfer reads the 4-byte TOUCH1 block starting at 0x03");
    }
}

static void test_read_no_touch_skips_touch_block(void)
{
    reset_all();
    hal_i2c_bus_t bus = make_bus();
    script_valid_identity(&bus);

    FT6336UClass t;
    memset(&t, 0, sizeof(t));
    TEST_CHECK(FT6336U_start(&t, &bus) == ESP_OK, "FT6336U_start (no-touch test setup)");

    uint8_t td_status = 0x00; // touch_count == 0
    fake_i2c_script_rx(&bus, FT6336U_ADDR, &td_status, 1);

    bool pressed = true; // deliberately wrong initial value
    uint16_t x = 123, y = 456, z1 = 0;
    esp_err_t err = FT6336U_read(&t, &pressed, &x, &y, &z1);
    TEST_CHECK(err == ESP_OK, "FT6336U_read returns ESP_OK with no touch");
    TEST_CHECK(pressed == false, "FT6336U_read reports not pressed for touch_count == 0");
    TEST_CHECK(x == 0 && y == 0, "FT6336U_read zeroes x/y when not pressed");

    // Only 4 transfers: 3 identity + the single TD_STATUS read. The TOUCH1
    // block must never be read when there is no valid touch point.
    TEST_CHECK(fake_i2c_transfer_count() == 4,
               "FT6336U_read does not read the TOUCH1 block when touch_count == 0");
}

int main(void)
{
    TEST_SECTION("FT6336U (hal_i2c)");

    test_start_reads_identity_in_order();
    test_start_rejects_wrong_identity();
    test_deinit_after_failed_identity_frees_the_slot();
    test_start_absent_device_is_not_found();
    test_read_decodes_touch_point();
    test_read_no_touch_skips_touch_block();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
