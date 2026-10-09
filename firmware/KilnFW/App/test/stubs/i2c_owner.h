// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// so backup_http.c's host test can reach ota_http.h -> kiln_io.h -> SX1509.h,
// which embeds an i2c_owner_t BY VALUE in SX1509Class. Only a complete type
// is needed (SX1509Class is never instantiated by this test, only its
// definition is parsed), so a dummy field is enough.
#ifndef TEST_STUB_I2C_OWNER_H
#define TEST_STUB_I2C_OWNER_H

typedef struct {
    int _unused;
} i2c_owner_t;

#endif // TEST_STUB_I2C_OWNER_H
