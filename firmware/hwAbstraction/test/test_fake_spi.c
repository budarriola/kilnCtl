/* test_fake_spi.c -- standalone MSVC host test for hwAbstraction/host/fake_spi.c.
 * Compiled and run by test_host_fakes.ps1.
 */
#include <stdio.h>
#include <string.h>

#include "fake_spi.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond) \
    do { \
        if (cond) { g_pass++; } \
        else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static int g_cb_calls = 0;
static void *g_cb_last_ctx = NULL;
static hal_status_t g_cb_last_result = HAL_OK;

static void test_cb(void *ctx, hal_status_t result)
{
    g_cb_calls++;
    g_cb_last_ctx = ctx;
    g_cb_last_result = result;
}

static hal_spi_bus_cfg_t default_bus_cfg(void)
{
    hal_spi_bus_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sck_pin = 1; cfg.mosi_pin = 2; cfg.miso_pin = 3;
    cfg.queue_len = 8; cfg.task_priority = 5; cfg.stack_depth = 4096;
    cfg.core_id = -1;
    return cfg;
}

static hal_spi_device_cfg_t default_dev_cfg(void)
{
    hal_spi_device_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.clock_hz = 4000000;
    cfg.mode = HAL_SPI_MODE_1;
    cfg.queue_size = 1;
    cfg.hw_cs = HAL_CS_NONE;
    cfg.cs_pin = 5;
    return cfg;
}

int main(void)
{
    fake_spi_reset_all();

    hal_spi_bus_cfg_t bus_cfg = default_bus_cfg();
    hal_spi_device_cfg_t dev_cfg = default_dev_cfg();

    /* --- init: NULL args -> HAL_INVALID_ARG --- */
    hal_spi_bus_t bus;
    memset(&bus, 0, sizeof(bus));
    CHECK(hal_spi_bus_init(NULL, 0, &bus_cfg) == HAL_INVALID_ARG);
    CHECK(hal_spi_bus_init(&bus, 0, NULL) == HAL_INVALID_ARG);

    /* --- uninitialized handles: every call -> HAL_NOT_READY --- */
    hal_spi_bus_t garbage_bus;
    memset(&garbage_bus, 0xCD, sizeof(garbage_bus));
    hal_spi_device_t garbage_dev;
    memset(&garbage_dev, 0xCD, sizeof(garbage_dev));
    CHECK(fake_spi_bus_is_live(&garbage_bus) == false);
    CHECK(fake_spi_device_is_live(&garbage_dev) == false);
    CHECK(hal_spi_bus_deinit(&garbage_bus) == HAL_NOT_READY);
    CHECK(hal_spi_bus_is_wedged(&garbage_bus) == false);
    CHECK(hal_spi_device_attach(&garbage_bus, &garbage_dev, &dev_cfg) == HAL_NOT_READY);
    uint8_t txb[4] = {1,2,3,4}, rxb[4];
    CHECK(hal_spi_transfer(&garbage_dev, txb, 4, rxb, 4, 100) == HAL_NOT_READY);
    CHECK(hal_spi_transfer_polling(&garbage_dev, txb, 4, rxb, 4, 100) == HAL_NOT_READY);
    CHECK(hal_spi_transfer_async(&garbage_dev, txb, 4, 100, test_cb, NULL) == HAL_NOT_READY);
    CHECK(fake_spi_script_rx(&garbage_dev, txb, 4) == HAL_NOT_READY);

    /* --- init succeeds; bus and device become live --- */
    CHECK(hal_spi_bus_init(&bus, 0, &bus_cfg) == HAL_OK);
    CHECK(fake_spi_bus_is_live(&bus) == true);
    CHECK(hal_spi_bus_is_wedged(&bus) == false);
    CHECK(hal_spi_get_task_handle(&bus) == NULL); /* host fake has no owner task */

    hal_spi_device_t dev;
    memset(&dev, 0, sizeof(dev));
    /* attach with NULL cfg -> invalid arg */
    CHECK(hal_spi_device_attach(&bus, &dev, NULL) == HAL_INVALID_ARG);
    CHECK(hal_spi_device_attach(&bus, &dev, &dev_cfg) == HAL_OK);
    CHECK(fake_spi_device_is_live(&dev) == true);

    /* --- basic transfer: tx captured, no scripted rx -> zero-filled --- */
    memset(rxb, 0xFF, sizeof(rxb));
    CHECK(hal_spi_transfer(&dev, txb, 4, rxb, 4, 100) == HAL_OK);
    CHECK(rxb[0] == 0 && rxb[1] == 0 && rxb[2] == 0 && rxb[3] == 0);
    CHECK(fake_spi_transfer_count() == 1);
    const fake_spi_transfer_record_t *rec = fake_spi_transfer(0);
    CHECK(rec != NULL);
    CHECK(rec != NULL && rec->kind == FAKE_SPI_XFER_NORMAL);
    CHECK(rec != NULL && rec->tx_len == 4 && memcmp(rec->tx, txb, 4) == 0);
    CHECK(fake_spi_transfer(999) == NULL);

    /* --- scripted rx: consumed FIFO, one response per call --- */
    uint8_t script1[2] = {0xAA, 0xBB};
    uint8_t script2[3] = {0x11, 0x22, 0x33};
    CHECK(fake_spi_script_rx(&dev, script1, 2) == HAL_OK);
    CHECK(fake_spi_script_rx(&dev, script2, 3) == HAL_OK);
    memset(rxb, 0, sizeof(rxb));
    CHECK(hal_spi_transfer(&dev, txb, 1, rxb, 2, 10) == HAL_OK);
    CHECK(rxb[0] == 0xAA && rxb[1] == 0xBB);
    memset(rxb, 0, sizeof(rxb));
    CHECK(hal_spi_transfer(&dev, txb, 1, rxb, 4, 10) == HAL_OK); /* rx_len 4 > scripted 3 */
    CHECK(rxb[0] == 0x11 && rxb[1] == 0x22 && rxb[2] == 0x33 && rxb[3] == 0);
    /* queue now empty -> back to zero-fill */
    memset(rxb, 0xFF, sizeof(rxb));
    CHECK(hal_spi_transfer(&dev, txb, 1, rxb, 2, 10) == HAL_OK);
    CHECK(rxb[0] == 0 && rxb[1] == 0);

    /* --- polling variant logged distinctly --- */
    CHECK(hal_spi_transfer_polling(&dev, txb, 2, NULL, 0, 10) == HAL_OK);
    CHECK(fake_spi_transfer(fake_spi_transfer_count() - 1)->kind == FAKE_SPI_XFER_POLLING);

    /* --- enqueue-timeout injection: request never accepted, not logged --- */
    size_t count_before = fake_spi_transfer_count();
    fake_spi_inject_enqueue_timeout(&bus, 2);
    CHECK(hal_spi_transfer(&dev, txb, 1, rxb, 1, 10) == HAL_TIMEOUT);
    CHECK(hal_spi_transfer_polling(&dev, txb, 1, rxb, 1, 10) == HAL_TIMEOUT);
    CHECK(fake_spi_transfer_count() == count_before); /* neither logged */
    /* budget exhausted -> back to normal */
    CHECK(hal_spi_transfer(&dev, txb, 1, rxb, 1, 10) == HAL_OK);
    CHECK(fake_spi_transfer_count() == count_before + 1);

    /* --- async: does not fire synchronously; fake_spi_pump() drives it --- */
    g_cb_calls = 0;
    int ctx_val = 42;
    CHECK(hal_spi_transfer_async(&dev, txb, 2, 100, test_cb, &ctx_val) == HAL_OK);
    CHECK(g_cb_calls == 0); /* must NOT fire before pump -- see fake_spi.h */
    CHECK(fake_spi_pending_async_count(&bus) == 1);
    CHECK(fake_spi_pump(&bus) == true);
    CHECK(g_cb_calls == 1);
    CHECK(g_cb_last_ctx == &ctx_val);
    CHECK(g_cb_last_result == HAL_OK);
    CHECK(fake_spi_pending_async_count(&bus) == 0);
    CHECK(fake_spi_pump(&bus) == false); /* nothing left to pump */

    /* --- async FIFO order across two enqueues --- */
    int ctxA = 1, ctxB = 2;
    CHECK(hal_spi_transfer_async(&dev, txb, 1, 10, test_cb, &ctxA) == HAL_OK);
    CHECK(hal_spi_transfer_async(&dev, txb, 1, 10, test_cb, &ctxB) == HAL_OK);
    CHECK(fake_spi_pending_async_count(&bus) == 2);
    g_cb_calls = 0;
    CHECK(fake_spi_pump(&bus) == true);
    CHECK(g_cb_last_ctx == &ctxA); /* oldest first */
    CHECK(fake_spi_pump(&bus) == true);
    CHECK(g_cb_last_ctx == &ctxB);
    CHECK(g_cb_calls == 2);

    /* --- completion-timeout injection: fires via pump with HAL_TIMEOUT,
     * does NOT wedge --- */
    fake_spi_inject_completion_timeout(&bus, 1);
    CHECK(hal_spi_transfer_async(&dev, txb, 1, 10, test_cb, NULL) == HAL_OK);
    CHECK(fake_spi_pump(&bus) == true);
    CHECK(g_cb_last_result == HAL_TIMEOUT);
    CHECK(hal_spi_bus_is_wedged(&bus) == false);

    /* --- async pool exhaustion: latches the wedge, distinct from timeout --- */
    {
        hal_spi_bus_t bus2;
        memset(&bus2, 0, sizeof(bus2));
        CHECK(hal_spi_bus_init(&bus2, 1, &bus_cfg) == HAL_OK);
        hal_spi_device_t dev2;
        memset(&dev2, 0, sizeof(dev2));
        CHECK(hal_spi_device_attach(&bus2, &dev2, &dev_cfg) == HAL_OK);
        for (int i = 0; i < FAKE_SPI_MAX_PENDING_ASYNC; i++) {
            CHECK(hal_spi_transfer_async(&dev2, txb, 1, 10, test_cb, NULL) == HAL_OK);
        }
        CHECK(hal_spi_bus_is_wedged(&bus2) == false); /* not wedged until pool is full */
        CHECK(hal_spi_transfer_async(&dev2, txb, 1, 10, test_cb, NULL) == HAL_NO_MEM);
        CHECK(hal_spi_bus_is_wedged(&bus2) == true);
        /* draining does not clear the latch -- it requires re-init */
        while (fake_spi_pump(&bus2)) { /* drain */ }
        CHECK(hal_spi_bus_is_wedged(&bus2) == true);
        CHECK(hal_spi_bus_deinit(&bus2) == HAL_OK);
        CHECK(hal_spi_bus_is_wedged(&bus2) == false); /* re-init clears it */
    }

    /* --- overflow: tx/rx larger than the fake's fixed buffers --- */
    {
        uint8_t huge_tx[FAKE_SPI_MAX_TX_BYTES + 1];
        memset(huge_tx, 0, sizeof(huge_tx));
        CHECK(hal_spi_transfer(&dev, huge_tx, sizeof(huge_tx), NULL, 0, 10) == HAL_INVALID_SIZE);
        uint8_t huge_rx[FAKE_SPI_MAX_RX_BYTES + 1];
        CHECK(hal_spi_transfer(&dev, txb, 1, huge_rx, sizeof(huge_rx), 10) == HAL_INVALID_SIZE);
    }

    /* --- fake_spi_script_rx error paths / pool exhaustion --- */
    CHECK(fake_spi_script_rx(&dev, NULL, 3) == HAL_INVALID_ARG);
    {
        uint8_t one[1] = {0x00};
        int ok = 0;
        for (int i = 0; i < FAKE_SPI_RX_SCRIPT_QUEUE_CAP; i++) {
            if (fake_spi_script_rx(&dev, one, 1) == HAL_OK) ok++;
        }
        CHECK(ok == FAKE_SPI_RX_SCRIPT_QUEUE_CAP);
        CHECK(fake_spi_script_rx(&dev, one, 1) == HAL_NO_MEM);
    }

    /* --- device pool exhaustion --- */
    fake_spi_reset_all();
    {
        hal_spi_bus_t b3;
        memset(&b3, 0, sizeof(b3));
        CHECK(hal_spi_bus_init(&b3, 0, &bus_cfg) == HAL_OK);
        hal_spi_device_t devs[FAKE_SPI_MAX_DEVICES];
        for (int i = 0; i < FAKE_SPI_MAX_DEVICES; i++) {
            memset(&devs[i], 0, sizeof(devs[i]));
            CHECK(hal_spi_device_attach(&b3, &devs[i], &dev_cfg) == HAL_OK);
        }
        hal_spi_device_t one_too_many;
        memset(&one_too_many, 0, sizeof(one_too_many));
        CHECK(hal_spi_device_attach(&b3, &one_too_many, &dev_cfg) == HAL_NO_MEM);
    }

    /* --- bus pool exhaustion --- */
    fake_spi_reset_all();
    {
        hal_spi_bus_t buses[FAKE_SPI_MAX_BUSES];
        for (int i = 0; i < FAKE_SPI_MAX_BUSES; i++) {
            memset(&buses[i], 0, sizeof(buses[i]));
            CHECK(hal_spi_bus_init(&buses[i], i, &bus_cfg) == HAL_OK);
        }
        hal_spi_bus_t one_too_many_bus;
        memset(&one_too_many_bus, 0, sizeof(one_too_many_bus));
        CHECK(hal_spi_bus_init(&one_too_many_bus, 99, &bus_cfg) == HAL_NO_MEM);
        CHECK(hal_spi_bus_deinit(&buses[0]) == HAL_OK);
        CHECK(hal_spi_bus_init(&one_too_many_bus, 99, &bus_cfg) == HAL_OK);
    }

    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
