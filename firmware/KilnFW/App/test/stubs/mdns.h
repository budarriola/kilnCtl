// Host-test stub of ESP-IDF mdns.h: only what http_auth_http.c's F4 Host
// allow-list needs. The real MDNS_NAME_BUF_LEN is 64.
#ifndef STUB_MDNS_H
#define STUB_MDNS_H
#include "esp_err.h"
#define MDNS_NAME_BUF_LEN 64
esp_err_t mdns_hostname_get(char *hostname);
#endif
