// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// so backup_http.c's host test can reach ota_http.h -> kiln_io.h -> SX1509.h
// without pulling the real ESP-IDF i2c driver. Opaque handles only: nothing
// backup_http.c's tests ever performs a real I2C transfer.
#ifndef TEST_STUB_DRIVER_I2C_MASTER_H
#define TEST_STUB_DRIVER_I2C_MASTER_H

typedef struct i2c_master_bus_s *i2c_master_bus_handle_t;
typedef struct i2c_master_dev_s *i2c_master_dev_handle_t;

/* Campaign 7 (2026-10-09): declarations SX1509.c needs to COMPILE on the host.
 * Only test_kiln_io_sx_fake.c defines the bodies (a register-level fake
 * SX1509); no other test links SX1509.c, so nothing else needs them. */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum { I2C_ADDR_BIT_LEN_7 = 0 } i2c_addr_bit_len_t;
typedef struct {
    i2c_addr_bit_len_t dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
} i2c_device_config_t;

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                    i2c_master_dev_handle_t *out);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev);
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t addr, int timeout_ms);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *tx, size_t tx_len, int timeout_ms);
esp_err_t i2c_master_receive(i2c_master_dev_handle_t dev, uint8_t *rx, size_t rx_len, int timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *tx, size_t tx_len,
                                      uint8_t *rx, size_t rx_len, int timeout_ms);

#endif // TEST_STUB_DRIVER_I2C_MASTER_H
