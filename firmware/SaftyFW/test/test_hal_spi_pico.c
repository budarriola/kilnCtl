// test_hal_spi_pico.c -- host test for hal_spi_pico.c's OWN adapter logic
// (pin/clock/mode/cs validation, one-device-ever enforcement, tx_len/rx_len
// matching, spi_owner_* error pass-through), in isolation from spi_owner.c's
// real pico-sdk hardware/spi.h calls. Links the real hal_spi_pico.c against
// spi_owner_stub.c (stubs/spi_owner_stub/) -- see that stub's own header
// comment for why this is a DIFFERENT test from test_max31856_hal_spi.c
// (which fakes hal_spi.h itself via fake_spi.c to test a hal_spi CLIENT;
// this test fakes what hal_spi_pico.c calls INTO, to test the adapter body).
#include "test_common.h"

#include <string.h>

#include "board_pins.h"
#include "hal_spi.h"
#include "spi_owner_stub.h"

static void valid_bus_cfg(hal_spi_bus_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->sck_pin = SAFTYFW_PIN_SPI0_SCK;
    cfg->mosi_pin = SAFTYFW_PIN_SPI0_MOSI;
    cfg->miso_pin = SAFTYFW_PIN_SPI0_MISO;
    cfg->cs0_pin = SAFTYFW_PIN_SPI0_CS0;
}

static void valid_device_cfg(hal_spi_device_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->clock_hz = 4000000u;
    cfg->mode = HAL_SPI_MODE_1;
    cfg->hw_cs = HAL_CS_NONE;
    cfg->cs_pin = SAFTYFW_PIN_SPI0_CS0;
}

static void reset(void)
{
    spi_owner_stub_reset();
}

static void test_bus_init_missing_cs0_pin_rejected(void)
{
    // HAL Phase 1b, "close the upward include": hal_spi_pico.c no longer
    // knows the "right" pin values (board_pins.h moved out of this file
    // entirely) -- it forwards whatever cfg says straight into
    // spi_owner_init(). The one thing it still rejects locally is a caller
    // that provides no CS0 pin at all, since there is no default to fall
    // back to.
    TEST_SECTION("hal_spi_bus_init() -- cfg->cs0_pin == HAL_CS_NONE -> HAL_INVALID_ARG, spi_owner_init() never called");
    reset();
    hal_spi_bus_t bus;
    hal_spi_bus_cfg_t cfg;

    valid_bus_cfg(&cfg);
    cfg.cs0_pin = HAL_CS_NONE;
    TEST_CHECK(hal_spi_bus_init(&bus, 0, &cfg) == HAL_INVALID_ARG, "missing cs0_pin rejected");
    TEST_CHECK(spi_owner_stub_init_count() == 0, "spi_owner_init() never called on a rejected cfg");
}

static void test_bus_init_forwards_pins_to_owner(void)
{
    // Pins are no longer validated against a compile-time constant here --
    // they are forwarded to spi_owner_init() verbatim, whatever the caller
    // asks for (the caller, e.g. max31856.c, is now the one that reads
    // board_pins.h and is responsible for passing the right values), as
    // long as the four pins are distinct and in range (see the dedicated
    // rejection tests below).
    TEST_SECTION("hal_spi_bus_init() -- any distinct, in-range sck/mosi/miso/cs0 pin set is forwarded, not validated");
    reset();
    hal_spi_bus_t bus;
    hal_spi_bus_cfg_t cfg;

    valid_bus_cfg(&cfg);
    // Four distinct valid pins, none colliding with each other or with the
    // default cs0_pin (SAFTYFW_PIN_SPI0_CS0 == 1).
    cfg.sck_pin = SAFTYFW_PIN_SPI0_SCK + 5;   // 7
    cfg.mosi_pin = SAFTYFW_PIN_SPI0_MOSI + 5; // 8
    cfg.miso_pin = SAFTYFW_PIN_SPI0_MISO + 5; // 5
    TEST_CHECK(hal_spi_bus_init(&bus, 0, &cfg) == HAL_OK,
               "a non-default pin set is accepted -- this backend has no opinion about it anymore");
    TEST_CHECK(spi_owner_stub_init_count() == 1, "spi_owner_init() was called exactly once");
}

static void test_bus_init_zeroed_cfg_rejected(void)
{
    // An all-zero hal_spi_bus_cfg_t (e.g. a caller that forgot to fill it
    // in) would otherwise pass the old lone HAL_CS_NONE-only check with
    // sck_pin == mosi_pin == miso_pin == cs0_pin == 0, driving all four SPI0
    // roles off the same GPIO0.
    TEST_SECTION("hal_spi_bus_init() -- all-zero cfg (sck==mosi==miso==cs0==GPIO0) -> HAL_INVALID_ARG");
    reset();
    hal_spi_bus_t bus;
    hal_spi_bus_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    TEST_CHECK(hal_spi_bus_init(&bus, 0, &cfg) == HAL_INVALID_ARG,
               "a zeroed cfg with all pins colliding on GPIO0 is rejected");
    TEST_CHECK(spi_owner_stub_init_count() == 0, "spi_owner_init() never called on a rejected cfg");
}

static void test_bus_init_duplicate_pin_rejected(void)
{
    TEST_SECTION("hal_spi_bus_init() -- any two of sck/mosi/miso/cs0 sharing a GPIO -> HAL_INVALID_ARG");
    reset();
    hal_spi_bus_t bus;
    hal_spi_bus_cfg_t cfg;

    valid_bus_cfg(&cfg);
    cfg.mosi_pin = cfg.sck_pin; // collide sck/mosi, cs0/miso stay distinct
    TEST_CHECK(hal_spi_bus_init(&bus, 0, &cfg) == HAL_INVALID_ARG, "sck == mosi rejected");
    TEST_CHECK(spi_owner_stub_init_count() == 0, "spi_owner_init() never called");

    valid_bus_cfg(&cfg);
    cfg.cs0_pin = cfg.miso_pin; // collide cs0/miso
    TEST_CHECK(hal_spi_bus_init(&bus, 0, &cfg) == HAL_INVALID_ARG, "cs0 == miso rejected");
    TEST_CHECK(spi_owner_stub_init_count() == 0, "spi_owner_init() never called");
}

static void test_bus_init_out_of_range_pin_rejected(void)
{
    TEST_SECTION("hal_spi_bus_init() -- a pin outside 0..29 (RP2040 GPIO count) -> HAL_INVALID_ARG");
    reset();
    hal_spi_bus_t bus;
    hal_spi_bus_cfg_t cfg;

    valid_bus_cfg(&cfg);
    cfg.sck_pin = 30; // one past the last real RP2040 GPIO
    TEST_CHECK(hal_spi_bus_init(&bus, 0, &cfg) == HAL_INVALID_ARG, "sck_pin == 30 rejected");
    TEST_CHECK(spi_owner_stub_init_count() == 0, "spi_owner_init() never called");

    valid_bus_cfg(&cfg);
    cfg.mosi_pin = 254; // would truncate to a plausible-looking uint8_t if uncaught
    TEST_CHECK(hal_spi_bus_init(&bus, 0, &cfg) == HAL_INVALID_ARG, "mosi_pin == 254 rejected");
    TEST_CHECK(spi_owner_stub_init_count() == 0, "spi_owner_init() never called");
}

static void test_bus_init_owner_failure_propagates(void)
{
    TEST_SECTION("hal_spi_bus_init() -- spi_owner_init() returning false -> HAL_IO");
    reset();
    spi_owner_stub_set_init_result(false);
    hal_spi_bus_t bus;
    hal_spi_bus_cfg_t cfg;
    valid_bus_cfg(&cfg);
    TEST_CHECK(hal_spi_bus_init(&bus, 0, &cfg) == HAL_IO, "a false spi_owner_init() becomes HAL_IO");
}

static void test_device_attach_cfg_mismatches_rejected(void)
{
    TEST_SECTION("hal_spi_device_attach() -- clock/mode/cs mismatch against the one hardwired device -> HAL_INVALID_ARG");
    reset();
    hal_spi_bus_t bus;
    hal_spi_bus_cfg_t bus_cfg;
    valid_bus_cfg(&bus_cfg);
    TEST_CHECK(hal_spi_bus_init(&bus, 0, &bus_cfg) == HAL_OK, "bus_init succeeds (setup)");

    hal_spi_device_t dev;
    hal_spi_device_cfg_t dev_cfg;

    valid_device_cfg(&dev_cfg);
    dev_cfg.clock_hz = 1000000u;
    TEST_CHECK(hal_spi_device_attach(&bus, &dev, &dev_cfg) == HAL_INVALID_ARG,
               "wrong clock_hz rejected");

    valid_device_cfg(&dev_cfg);
    dev_cfg.mode = HAL_SPI_MODE_0;
    TEST_CHECK(hal_spi_device_attach(&bus, &dev, &dev_cfg) == HAL_INVALID_ARG, "wrong mode rejected");

    valid_device_cfg(&dev_cfg);
    dev_cfg.cs_pin = SAFTYFW_PIN_SPI0_CS0 + 1;
    TEST_CHECK(hal_spi_device_attach(&bus, &dev, &dev_cfg) == HAL_INVALID_ARG,
               "wrong cs_pin rejected");

    valid_device_cfg(&dev_cfg);
    dev_cfg.hw_cs = 5; /* anything other than HAL_CS_NONE -- CS is bit-banged, never hardware here */
    TEST_CHECK(hal_spi_device_attach(&bus, &dev, &dev_cfg) == HAL_INVALID_ARG,
               "non-HAL_CS_NONE hw_cs rejected");
}

static void test_device_attach_before_bus_init_not_ready(void)
{
    TEST_SECTION("hal_spi_device_attach() -- bus never initialized -> HAL_NOT_READY");
    reset();
    hal_spi_bus_t bus;
    memset(&bus, 0, sizeof(bus)); /* zeroed storage -- magic never stamped */
    hal_spi_device_t dev;
    hal_spi_device_cfg_t dev_cfg;
    valid_device_cfg(&dev_cfg);
    TEST_CHECK(hal_spi_device_attach(&bus, &dev, &dev_cfg) == HAL_NOT_READY,
               "attach against an uninitialized bus fails NOT_READY, not a crash");
}

static void test_second_attach_is_busy(void)
{
    TEST_SECTION("hal_spi_device_attach() -- a second attach on the same bus -> HAL_BUSY (one CS line, one device, ever)");
    reset();
    hal_spi_bus_t bus;
    hal_spi_bus_cfg_t bus_cfg;
    valid_bus_cfg(&bus_cfg);
    TEST_CHECK(hal_spi_bus_init(&bus, 0, &bus_cfg) == HAL_OK, "bus_init succeeds (setup)");

    hal_spi_device_t dev1, dev2;
    hal_spi_device_cfg_t dev_cfg;
    valid_device_cfg(&dev_cfg);
    TEST_CHECK(hal_spi_device_attach(&bus, &dev1, &dev_cfg) == HAL_OK, "first attach succeeds");
    TEST_CHECK(hal_spi_device_attach(&bus, &dev2, &dev_cfg) == HAL_BUSY,
               "second attach on the same bus is refused, even with an identical, otherwise-valid cfg");
}

static void test_transfer_before_attach_not_ready(void)
{
    TEST_SECTION("hal_spi_transfer() -- an unattached device -> HAL_NOT_READY, no spi_owner call");
    reset();
    hal_spi_device_t dev;
    memset(&dev, 0, sizeof(dev)); /* zeroed storage -- magic never stamped */
    uint8_t tx[2] = { 0x01u, 0x02u };
    TEST_CHECK(hal_spi_transfer(&dev, tx, sizeof(tx), NULL, 0, 0) == HAL_NOT_READY,
               "transfer against a never-attached device fails NOT_READY");
    TEST_CHECK(spi_owner_stub_transfer_count() == 0,
               "spi_owner_transfer() is never reached for an unattached device");
}

static void test_rx_len_mismatch_rejected(void)
{
    TEST_SECTION("hal_spi_transfer()/_polling() -- rx_len != tx_len when rx is non-NULL -> HAL_INVALID_ARG (spi_owner_transfer() has ONE length for both buffers)");
    reset();
    hal_spi_bus_t bus;
    hal_spi_bus_cfg_t bus_cfg;
    valid_bus_cfg(&bus_cfg);
    TEST_CHECK(hal_spi_bus_init(&bus, 0, &bus_cfg) == HAL_OK, "bus_init succeeds (setup)");
    hal_spi_device_t dev;
    hal_spi_device_cfg_t dev_cfg;
    valid_device_cfg(&dev_cfg);
    TEST_CHECK(hal_spi_device_attach(&bus, &dev, &dev_cfg) == HAL_OK, "attach succeeds (setup)");

    uint8_t tx[3] = { 0xAAu, 0xBBu, 0xCCu };
    uint8_t rx[2] = { 0 };
    TEST_CHECK(hal_spi_transfer(&dev, tx, sizeof(tx), rx, sizeof(rx), 0) == HAL_INVALID_ARG,
               "rx_len (2) != tx_len (3) with a real rx buffer is rejected");
    TEST_CHECK(hal_spi_transfer_polling(&dev, tx, sizeof(tx), rx, sizeof(rx), 0) == HAL_INVALID_ARG,
               "same rejection on the polling entry point (identical body on this backend)");
    TEST_CHECK(spi_owner_stub_transfer_count() == 0,
               "spi_owner_transfer() is never reached for a rejected length pair");

    // rx == NULL with any rx_len is fine (rx_len is meaningless when there is
    // no buffer to fill) -- a write-only transfer, matching max31856_write_u8().
    TEST_CHECK(hal_spi_transfer(&dev, tx, sizeof(tx), NULL, 0, 0) == HAL_OK,
               "rx == NULL bypasses the length check entirely");
    TEST_CHECK(spi_owner_stub_last_len_arg() == sizeof(tx),
               "spi_owner_transfer()'s one len argument is tx_len when rx is NULL");
    TEST_CHECK(spi_owner_stub_last_rx_was_null(), "rx was actually passed through as NULL");
}

static void test_transfer_success_and_owner_failure_propagate(void)
{
    TEST_SECTION("hal_spi_transfer() -- success and spi_owner_transfer() failure both propagate");
    reset();
    hal_spi_bus_t bus;
    hal_spi_bus_cfg_t bus_cfg;
    valid_bus_cfg(&bus_cfg);
    TEST_CHECK(hal_spi_bus_init(&bus, 0, &bus_cfg) == HAL_OK, "bus_init succeeds (setup)");
    hal_spi_device_t dev;
    hal_spi_device_cfg_t dev_cfg;
    valid_device_cfg(&dev_cfg);
    TEST_CHECK(hal_spi_device_attach(&bus, &dev, &dev_cfg) == HAL_OK, "attach succeeds (setup)");

    uint8_t tx[2] = { 0x01u, 0x23u };
    uint8_t rx[2] = { 0 };
    TEST_CHECK(hal_spi_transfer(&dev, tx, sizeof(tx), rx, sizeof(rx), 0) == HAL_OK,
               "a successful spi_owner_transfer() becomes HAL_OK");
    TEST_CHECK(spi_owner_stub_last_tx_len() == sizeof(tx) &&
                   memcmp(spi_owner_stub_last_tx(), tx, sizeof(tx)) == 0,
               "the exact tx bytes reached spi_owner_transfer()");

    spi_owner_stub_set_transfer_result(false);
    TEST_CHECK(hal_spi_transfer(&dev, tx, sizeof(tx), rx, sizeof(rx), 0) == HAL_IO,
               "a false spi_owner_transfer() becomes HAL_IO, not HAL_OK");
}

static void test_async_not_supported(void)
{
    TEST_SECTION("hal_spi_transfer_async() -- HAL_NOT_SUPPORTED, no spi_owner call (spi_owner.c has no async path at all)");
    reset();
    hal_spi_bus_t bus;
    hal_spi_bus_cfg_t bus_cfg;
    valid_bus_cfg(&bus_cfg);
    TEST_CHECK(hal_spi_bus_init(&bus, 0, &bus_cfg) == HAL_OK, "bus_init succeeds (setup)");
    hal_spi_device_t dev;
    hal_spi_device_cfg_t dev_cfg;
    valid_device_cfg(&dev_cfg);
    TEST_CHECK(hal_spi_device_attach(&bus, &dev, &dev_cfg) == HAL_OK, "attach succeeds (setup)");

    uint8_t tx[1] = { 0x00u };
    TEST_CHECK(hal_spi_transfer_async(&dev, tx, sizeof(tx), 0, NULL, NULL) == HAL_NOT_SUPPORTED,
               "async is unconditionally unsupported on this backend");
    TEST_CHECK(spi_owner_stub_transfer_count() == 0, "spi_owner_transfer() was never called");
}

static void test_bus_adopt_before_init_not_ready(void)
{
    TEST_SECTION("hal_spi_bus_adopt() -- existing bus never initialized -> HAL_NOT_READY");
    reset();
    hal_spi_bus_t existing;
    memset(&existing, 0, sizeof(existing)); /* zeroed storage -- magic never stamped */
    hal_spi_bus_t bus;
    TEST_CHECK(hal_spi_bus_adopt(&bus, 0, &existing) == HAL_NOT_READY,
               "adopting an uninitialized bus fails NOT_READY, not a crash");
}

static void test_bus_adopt_null_args_rejected(void)
{
    TEST_SECTION("hal_spi_bus_adopt() -- NULL bus/existing -> HAL_INVALID_ARG");
    reset();
    hal_spi_bus_t bus, existing;
    TEST_CHECK(hal_spi_bus_adopt(NULL, 0, &existing) == HAL_INVALID_ARG, "NULL bus rejected");
    TEST_CHECK(hal_spi_bus_adopt(&bus, 0, NULL) == HAL_INVALID_ARG, "NULL existing rejected");
}

static void test_bus_adopt_shares_singleton_without_reinit(void)
{
    TEST_SECTION("hal_spi_bus_adopt() -- shares the already-initialized singleton, never calls spi_owner_init() again, and never owns it");
    reset();
    hal_spi_bus_t existing;
    hal_spi_bus_cfg_t bus_cfg;
    valid_bus_cfg(&bus_cfg);
    TEST_CHECK(hal_spi_bus_init(&existing, 0, &bus_cfg) == HAL_OK, "real bus_init succeeds (setup)");
    TEST_CHECK(spi_owner_stub_init_count() == 1, "spi_owner_init() called exactly once so far");

    hal_spi_bus_t adopted;
    TEST_CHECK(hal_spi_bus_adopt(&adopted, 0, &existing) == HAL_OK, "adopt succeeds against a live bus");
    TEST_CHECK(spi_owner_stub_init_count() == 1,
               "adopt does NOT call spi_owner_init() a second time -- same singleton, not a second bring-up");

    // The adopted bus inherits the SAME cs0_pin the real bus recorded, so a
    // device attach through it validates against that value, not a
    // compile-time constant this file no longer has any notion of.
    hal_spi_device_t dev;
    hal_spi_device_cfg_t dev_cfg;
    valid_device_cfg(&dev_cfg);
    TEST_CHECK(hal_spi_device_attach(&adopted, &dev, &dev_cfg) == HAL_OK,
               "device_attach through the adopted bus succeeds against the inherited cs0_pin");

    // Deinit on the adopted bus must not report itself as tearing down the
    // shared singleton -- it only clears its own local bookkeeping.
    TEST_CHECK(hal_spi_bus_deinit(&adopted) == HAL_OK,
               "deinit on an adopted (non-owning) bus succeeds locally, unlike the owning bus's HAL_NOT_SUPPORTED");
}

static void test_adopted_attach_busy_when_owner_already_attached(void)
{
    // The one-device-ever guard is a fact about the single spi_owner.c
    // singleton bus, not about any one hal_spi_bus_t handle onto it: an
    // OWNED bus and a bus obtained via hal_spi_bus_adopt() both refer to the
    // same physical bus, so attaching through the adopted handle while the
    // owner's device is already attached must be refused exactly like a
    // second attach through the owning handle itself.
    TEST_SECTION("hal_spi_device_attach() -- adopted-bus attach refused HAL_BUSY when the owner already has a device attached");
    reset();
    hal_spi_bus_t owner;
    hal_spi_bus_cfg_t bus_cfg;
    valid_bus_cfg(&bus_cfg);
    TEST_CHECK(hal_spi_bus_init(&owner, 0, &bus_cfg) == HAL_OK, "owner bus_init succeeds (setup)");

    hal_spi_device_t owner_dev;
    hal_spi_device_cfg_t dev_cfg;
    valid_device_cfg(&dev_cfg);
    TEST_CHECK(hal_spi_device_attach(&owner, &owner_dev, &dev_cfg) == HAL_OK,
               "owner attaches its device first (setup)");

    hal_spi_bus_t adopted;
    TEST_CHECK(hal_spi_bus_adopt(&adopted, 0, &owner) == HAL_OK, "adopt succeeds against the owning bus");

    hal_spi_device_t adopted_dev;
    TEST_CHECK(hal_spi_device_attach(&adopted, &adopted_dev, &dev_cfg) == HAL_BUSY,
               "attach through the adopted handle is refused -- the owner's device already claims the one CS line");
}

void run_test_hal_spi_pico(void)
{
    test_bus_init_missing_cs0_pin_rejected();
    test_bus_init_forwards_pins_to_owner();
    test_bus_init_zeroed_cfg_rejected();
    test_bus_init_duplicate_pin_rejected();
    test_bus_init_out_of_range_pin_rejected();
    test_bus_init_owner_failure_propagates();
    test_device_attach_cfg_mismatches_rejected();
    test_device_attach_before_bus_init_not_ready();
    test_second_attach_is_busy();
    test_transfer_before_attach_not_ready();
    test_rx_len_mismatch_rejected();
    test_transfer_success_and_owner_failure_propagate();
    test_async_not_supported();
    test_bus_adopt_before_init_not_ready();
    test_bus_adopt_null_args_rejected();
    test_bus_adopt_shares_singleton_without_reinit();
    test_adopted_attach_busy_when_owner_already_attached();
}
