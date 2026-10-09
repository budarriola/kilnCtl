// Host-test stub: certificate bundle attach hook; the fake HTTP client never looks at it.
#ifndef UPDATE_FETCH_STUB_ESP_CRT_BUNDLE_H
#define UPDATE_FETCH_STUB_ESP_CRT_BUNDLE_H
#include "esp_err.h"
static inline esp_err_t esp_crt_bundle_attach(void *conf)
{
    (void)conf;
    return ESP_OK;
}
#endif
