// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// for wifi_prov.c's host tests (reached via the real wifi_provision_http.h,
// which wifi_prov.c #includes). Nothing here is exercised by the tests.
#ifndef TEST_STUB_ESP_HTTP_SERVER_H
#define TEST_STUB_ESP_HTTP_SERVER_H

typedef struct httpd_handle_obj *httpd_handle_t;
typedef struct httpd_req {
    int dummy;
} httpd_req_t;

#endif // TEST_STUB_ESP_HTTP_SERVER_H
