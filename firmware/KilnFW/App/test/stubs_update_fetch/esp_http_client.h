// Host-test stub for the update_fetch test: the slice of esp_http_client update_fetch.c uses. The fake in
// fake_support.c serves scripted routes (see fake_support.h).
#ifndef UPDATE_FETCH_STUB_ESP_HTTP_CLIENT_H
#define UPDATE_FETCH_STUB_ESP_HTTP_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifndef ESP_ERR_HTTP_EAGAIN
#define ESP_ERR_HTTP_EAGAIN 0x7008
#endif

typedef struct esp_http_client *esp_http_client_handle_t;

typedef enum {
    HTTP_EVENT_ERROR = 0,
    HTTP_EVENT_ON_CONNECTED,
    HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_ON_HEADER,
    HTTP_EVENT_ON_DATA,
    HTTP_EVENT_ON_FINISH,
    HTTP_EVENT_DISCONNECTED,
} esp_http_client_event_id_t;

typedef struct {
    esp_http_client_event_id_t event_id;
    void *data;
    int data_len;
    void *user_data;
    char *header_key;
    char *header_value;
} esp_http_client_event_t;

typedef struct {
    const char *url;
    int timeout_ms;
    esp_err_t (*event_handler)(esp_http_client_event_t *evt);
    void *user_data;
    int buffer_size;
    int buffer_size_tx;
    bool disable_auto_redirect;
    esp_err_t (*crt_bundle_attach)(void *conf);
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value);
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c);
int esp_http_client_get_status_code(esp_http_client_handle_t c);
bool esp_http_client_is_chunked_response(esp_http_client_handle_t c);
int esp_http_client_read(esp_http_client_handle_t c, char *buffer, int len);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c);
esp_err_t esp_http_client_close(esp_http_client_handle_t c);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c);

#endif
