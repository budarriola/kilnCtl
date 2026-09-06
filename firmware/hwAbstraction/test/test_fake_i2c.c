/* test_fake_i2c.c -- standalone MSVC host test for hwAbstraction/host/fake_i2c.c.
 * Compiled and run by test_host_fakes.ps1.
 */
#include <stdio.h>
#include <string.h>

#include "fake_i2c.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond) \
    do { \
        if (cond) { g_pass++; } \
        else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

int main(void)
{
    fake_i2c_reset_all();

    /* --- init: NULL args -> HAL_INVALID_ARG --- */
    hal_i2c_bus_t bus;
    memset(&bus, 0, sizeof(bus));
    hal_i2c_bus_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.scl_pin = 21; cfg.sda_pin = 22;
    CHECK(hal_i2c_bus_init(NULL, 0, &cfg) == HAL_INVALID_ARG);
    CHECK(hal_i2c_bus_init(&bus, 0, NULL) == HAL_INVALID_ARG);

    /* --- uninitialized handles: every call -> HAL_NOT_READY --- */
    hal_i2c_bus_t garbage_bus;
    memset(&garbage_bus, 0xCD, sizeof(garbage_bus));
    hal_i2c_device_t garbage_dev;
    memset(&garbage_dev, 0xCD, sizeof(garbage_dev));
    CHECK(fake_i2c_bus_is_live(&garbage_bus) == false);
    CHECK(fake_i2c_device_is_live(&garbage_dev) == false);
    CHECK(hal_i2c_bus_deinit(&garbage_bus) == HAL_NOT_READY);
    CHECK(hal_i2c_device_attach(&garbage_bus, &garbage_dev, 0x40, 100000) == HAL_NOT_READY);
    uint8_t txb[4] = {1,2,3,4}, rxb[4];
    CHECK(hal_i2c_transfer(&garbage_dev, txb, 4, rxb, 4, 100) == HAL_NOT_READY);
    CHECK(hal_i2c_probe(&garbage_bus, 0x40, 200) == HAL_NOT_READY);
    CHECK(fake_i2c_script_rx(&garbage_bus, 0x40, txb, 4) == HAL_NOT_READY);
    CHECK(hal_i2c_device_detach(&garbage_dev) == HAL_NOT_READY);

    /* --- init succeeds --- */
    CHECK(hal_i2c_bus_init(&bus, 0, &cfg) == HAL_OK);
    CHECK(fake_i2c_bus_is_live(&bus) == true);
    CHECK(hal_i2c_get_task_handle(&bus) == NULL); /* host fake has no owner task */

    /* --- probe: default (unscripted) address ACKs --- */
    CHECK(hal_i2c_probe(&bus, 0x3E, 200) == HAL_OK);

    /* --- probe: scripted NACK -> HAL_NOT_FOUND (the documented reason
     * HAL_NOT_FOUND exists, per hal_i2c.h) --- */
    fake_i2c_script_nack(&bus, 0x40);
    CHECK(hal_i2c_probe(&bus, 0x40, 200) == HAL_NOT_FOUND);
    /* un-scripting restores ack */
    fake_i2c_script_ack(&bus, 0x40);
    CHECK(hal_i2c_probe(&bus, 0x40, 200) == HAL_OK);

    /* --- attach a device, transfer with no script -> zero-filled rx --- */
    hal_i2c_device_t dev;
    memset(&dev, 0, sizeof(dev));
    CHECK(hal_i2c_device_attach(&bus, &dev, 0x40, 400000) == HAL_OK);
    CHECK(fake_i2c_device_is_live(&dev) == true);
    memset(rxb, 0xFF, sizeof(rxb));
    CHECK(hal_i2c_transfer(&dev, txb, 2, rxb, 2, 200) == HAL_OK);
    CHECK(rxb[0] == 0 && rxb[1] == 0);
    CHECK(fake_i2c_transfer_count() == 1);
    const fake_i2c_transfer_record_t *rec = fake_i2c_transfer(0);
    CHECK(rec != NULL);
    CHECK(rec != NULL && rec->addr == 0x40 && rec->tx_len == 2 && memcmp(rec->tx, txb, 2) == 0);
    CHECK(fake_i2c_transfer(999) == NULL);

    /* --- scripted rx, FIFO, consumed one response per transfer --- */
    uint8_t s1[2] = {0xDE, 0xAD};
    uint8_t s2[1] = {0x99};
    CHECK(fake_i2c_script_rx(&bus, 0x40, s1, 2) == HAL_OK);
    CHECK(fake_i2c_script_rx(&bus, 0x40, s2, 1) == HAL_OK);
    memset(rxb, 0, sizeof(rxb));
    CHECK(hal_i2c_transfer(&dev, NULL, 0, rxb, 2, 200) == HAL_OK);
    CHECK(rxb[0] == 0xDE && rxb[1] == 0xAD);
    memset(rxb, 0, sizeof(rxb));
    CHECK(hal_i2c_transfer(&dev, NULL, 0, rxb, 2, 200) == HAL_OK); /* rx_len 2 > scripted 1 */
    CHECK(rxb[0] == 0x99 && rxb[1] == 0);

    /* --- NACK scripted on the address: transfer -> HAL_IO (distinct from
     * probe's HAL_NOT_FOUND, per hal_i2c.h "NACK on a transfer") --- */
    fake_i2c_script_nack(&bus, 0x40);
    CHECK(hal_i2c_transfer(&dev, txb, 1, rxb, 1, 200) == HAL_IO);
    fake_i2c_script_ack(&bus, 0x40);
    CHECK(hal_i2c_transfer(&dev, txb, 1, rxb, 1, 200) == HAL_OK);

    /* --- one-shot transfer timeout: fires once, then reverts to ack --- */
    fake_i2c_script_transfer_timeout(&bus, 0x40);
    CHECK(hal_i2c_transfer(&dev, txb, 1, rxb, 1, 200) == HAL_TIMEOUT);
    CHECK(hal_i2c_transfer(&dev, txb, 1, rxb, 1, 200) == HAL_OK); /* reverted */

    /* --- two addresses on one bus have independent scripts --- */
    hal_i2c_device_t dev2;
    memset(&dev2, 0, sizeof(dev2));
    CHECK(hal_i2c_device_attach(&bus, &dev2, 0x50, 400000) == HAL_OK);
    fake_i2c_script_nack(&bus, 0x50);
    CHECK(hal_i2c_probe(&bus, 0x40, 200) == HAL_OK);    /* still ack */
    CHECK(hal_i2c_probe(&bus, 0x50, 200) == HAL_NOT_FOUND);
    CHECK(hal_i2c_transfer(&dev2, txb, 1, rxb, 1, 200) == HAL_IO);

    /* --- overflow: tx/rx larger than the fake's fixed buffers --- */
    {
        uint8_t huge_tx[FAKE_I2C_MAX_TX_BYTES + 1];
        memset(huge_tx, 0, sizeof(huge_tx));
        CHECK(hal_i2c_transfer(&dev, huge_tx, sizeof(huge_tx), NULL, 0, 10) == HAL_INVALID_SIZE);
    }

    /* --- fake_i2c_script_rx error paths / queue exhaustion --- */
    CHECK(fake_i2c_script_rx(&bus, 0x40, NULL, 3) == HAL_INVALID_ARG);
    {
        uint8_t one[1] = {0x00};
        int ok = 0;
        hal_status_t last = HAL_OK;
        for (int i = 0; i < 10; i++) {
            last = fake_i2c_script_rx(&bus, 0x60, one, 1);
            if (last == HAL_OK) ok++;
        }
        CHECK(ok > 0);
        CHECK(last == HAL_NO_MEM); /* eventually exhausts the per-addr queue */
    }

    /* --- device pool exhaustion --- */
    fake_i2c_reset_all();
    {
        hal_i2c_bus_t b2;
        memset(&b2, 0, sizeof(b2));
        CHECK(hal_i2c_bus_init(&b2, 0, &cfg) == HAL_OK);
        hal_i2c_device_t devs[FAKE_I2C_MAX_DEVICES];
        for (int i = 0; i < FAKE_I2C_MAX_DEVICES; i++) {
            memset(&devs[i], 0, sizeof(devs[i]));
            CHECK(hal_i2c_device_attach(&b2, &devs[i], (uint8_t)(0x10 + i), 100000) == HAL_OK);
        }
        hal_i2c_device_t one_too_many;
        memset(&one_too_many, 0, sizeof(one_too_many));
        CHECK(hal_i2c_device_attach(&b2, &one_too_many, 0x7F, 100000) == HAL_NO_MEM);

        /* --- hal_i2c_device_detach: frees the slot so a fresh attach can
         * reuse it (this is the primitive FT6336U_deinit() now calls on its
         * identity-check failure path instead of leaking the slot) --- */
        CHECK(hal_i2c_device_detach(&devs[0]) == HAL_OK);
        CHECK(fake_i2c_device_is_live(&devs[0]) == false);
        CHECK(hal_i2c_device_attach(&b2, &one_too_many, 0x7F, 100000) == HAL_OK);
        CHECK(fake_i2c_device_is_live(&one_too_many) == true);
        /* detaching an already-detached (zeroed) handle is HAL_NOT_READY,
         * not a crash */
        CHECK(hal_i2c_device_detach(&devs[0]) == HAL_NOT_READY);
        /* detaching does not disturb a sibling device on the same bus */
        CHECK(fake_i2c_device_is_live(&devs[1]) == true);
    }

    /* --- bus pool exhaustion --- */
    fake_i2c_reset_all();
    {
        hal_i2c_bus_t buses[FAKE_I2C_MAX_BUSES];
        for (int i = 0; i < FAKE_I2C_MAX_BUSES; i++) {
            memset(&buses[i], 0, sizeof(buses[i]));
            CHECK(hal_i2c_bus_init(&buses[i], i, &cfg) == HAL_OK);
        }
        hal_i2c_bus_t one_too_many_bus;
        memset(&one_too_many_bus, 0, sizeof(one_too_many_bus));
        CHECK(hal_i2c_bus_init(&one_too_many_bus, 99, &cfg) == HAL_NO_MEM);
        CHECK(hal_i2c_bus_deinit(&buses[0]) == HAL_OK);
        CHECK(hal_i2c_bus_init(&one_too_many_bus, 99, &cfg) == HAL_OK);
    }

    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
