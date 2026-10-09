// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// so backup_http.c's host test can reach ota_http.h -> kiln_io.h -> SX1509.h
// without pulling the real ESP-IDF i2c driver. Opaque handles only: nothing
// backup_http.c's tests ever performs a real I2C transfer.
#ifndef TEST_STUB_DRIVER_I2C_MASTER_H
#define TEST_STUB_DRIVER_I2C_MASTER_H

typedef struct i2c_master_bus_s *i2c_master_bus_handle_t;
typedef struct i2c_master_dev_s *i2c_master_dev_handle_t;

#endif // TEST_STUB_DRIVER_I2C_MASTER_H
