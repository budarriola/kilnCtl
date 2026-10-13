// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// so backup_http.c's host test can reach ota_http.h -> kiln_io.h -> SX1509.h,
// which embeds an i2c_owner_t BY VALUE in SX1509Class. Only a complete type
// is needed (SX1509Class is never instantiated by this test, only its
// definition is parsed), so a dummy field is enough.
#ifndef TEST_STUB_I2C_OWNER_H
#define TEST_STUB_I2C_OWNER_H

typedef struct {
    int _unused;
    void *task_handle;
} i2c_owner_t;

/* Campaign 7: prototypes so SX1509.c compiles; bodies live in
 * test_kiln_io_sx_fake.c (never called: the fake leaves owner_initialized false). */
#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
esp_err_t i2c_owner_transfer(i2c_owner_t *o, i2c_master_dev_handle_t dev, const uint8_t *tx, size_t tx_len,
                             uint8_t *rx, size_t rx_len, int timeout_ms);
esp_err_t i2c_owner_init(i2c_owner_t *o, i2c_master_bus_handle_t bus, int a, int b, int c, int d);
esp_err_t i2c_owner_deinit(i2c_owner_t *o);

#endif // TEST_STUB_I2C_OWNER_H
