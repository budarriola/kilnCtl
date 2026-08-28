// Host-test stub -- see stubs/esp_err.h for why these exist. Added
// 2026-08-27 for ota_http.c's host tests (test_ota_http.c).
#ifndef TEST_STUB_ESP_RANDOM_H
#define TEST_STUB_ESP_RANDOM_H

#include <stddef.h>
#include <stdint.h>

// Declared only, defined once in test_ota_http.c -- not real randomness
// (host tests must be deterministic), just deterministic filler so
// ota_challenge_get_handler() (never called by these tests) can compile.
void esp_fill_random(void *buf, size_t len);

// Added for safety_link.c's host build -- declared only, defined once in
// whichever test file links it (deterministic filler, not real randomness,
// same reasoning as esp_fill_random above).
uint32_t esp_random(void);

#endif // TEST_STUB_ESP_RANDOM_H
