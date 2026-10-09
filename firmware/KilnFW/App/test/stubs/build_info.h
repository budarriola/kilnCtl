// Host-test stub -- see stubs/esp_err.h for why these exist. Added
// 2026-08-27 for ota_http.c's host tests (test_ota_http.c). ota_http.c's own
// header comment names this as CommonFW's per-processor build-identity
// header (TODO.md 9.6) -- fixed placeholder values are fine here since none
// of these fields is ever asserted on by these tests, only referenced from
// ota_esp_status_get_handler(), which the tests never call.
#ifndef TEST_STUB_BUILD_INFO_H
#define TEST_STUB_BUILD_INFO_H

#define FW_GIT_COMMIT "stub"
#define FW_GIT_DIRTY 0
#define FW_BUILD_DATE "1970-01-01"
#define FW_BUILD_TIME "00:00:00"

#endif // TEST_STUB_BUILD_INFO_H
