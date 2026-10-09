// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// for wifi_prov.c's host tests. wifi_prov.c never actually fires an event
// through esp_event on the host build (wifi_prov_start(), the only place
// that registers handlers, is never called by the tests) -- this only needs
// to let the file compile and link.
#ifndef TEST_STUB_ESP_EVENT_H
#define TEST_STUB_ESP_EVENT_H

#include <stdint.h>

#include "esp_err.h"

typedef const char *esp_event_base_t;
typedef void (*esp_event_handler_t)(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id,
                                    void *event_data);
typedef struct esp_event_handler_instance *esp_event_handler_instance_t;

#define ESP_EVENT_ANY_ID (-1)

static const esp_event_base_t WIFI_EVENT = "WIFI_EVENT";
static const esp_event_base_t IP_EVENT = "IP_EVENT";

static inline esp_err_t esp_event_loop_create_default(void)
{
    return ESP_OK;
}

static inline esp_err_t esp_event_handler_instance_register(esp_event_base_t event_base, int32_t event_id,
                                                              esp_event_handler_t event_handler, void *event_handler_arg,
                                                              esp_event_handler_instance_t *instance)
{
    (void)event_base;
    (void)event_id;
    (void)event_handler;
    (void)event_handler_arg;
    (void)instance;
    return ESP_OK;
}

#endif // TEST_STUB_ESP_EVENT_H
