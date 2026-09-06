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

static void test_bus_init_pin_mismatch_rejected(void)
{
    TEST_SECTION("hal_spi_bus_init() -- pin mismatch against board_pins.h -> HAL_INVALID_ARG");
    reset();
    hal_spi_bus_t bus;
    hal_spi_bus_cfg_t cfg;

    valid_bus_cfg(&cfg);
    cfg.sck_pin = SAFTYFW_PIN_SPI0_SCK + 1;
    TEST_CHECK(hal_spi_bus_init(&bus, 0, &cfg) == HAL_INVALID_ARG, "wrong sck_pin rejected");
    TEST_CHECK(spi_owner_stub_init_count() == 0, "spi_owner_init() never called on a rejected cfg");

    valid_bus_cfg(&cfg);
    cfg.mosi_pin = SAFTYFW_PIN_SPI0_MOSI + 1;
    TEST_CHECK(hal_spi_bus_init(&bus, 0, &cfg) == HAL_INVALID_ARG, "wrong mosi_pin rejected");

    valid_bus_cfg(&cfg);
    cfg.miso_pin = SAFTYFW_PIN_SPI0_MISO + 1;
    TEST_CHECK(hal_spi_bus_init(&bus, 0, &cfg) == HAL_INVALID_ARG, "wrong miso_pin rejected");
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

void run_test_hal_spi_pico(void)
{
    test_bus_init_pin_mismatch_rejected();
    test_bus_init_owner_failure_propagates();
    test_device_attach_cfg_mismatches_rejected();
    test_device_attach_before_bus_init_not_ready();
    test_second_attach_is_busy();
    test_transfer_before_attach_not_ready();
    test_rx_len_mismatch_rejected();
    test_transfer_success_and_owner_failure_propagate();
    test_async_not_supported();
}
