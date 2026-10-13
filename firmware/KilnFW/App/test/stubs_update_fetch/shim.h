// Force-included (cl /FI) into every translation unit of the update_fetch host test. Papers over the
// GCC/newlib spellings update_fetch.c and update_http.c use that MSVC lacks, and satisfies update_fetch.c's
// build-config guard. Nothing here changes what the production files do.
#ifndef UPDATE_FETCH_HOST_SHIM_H
#define UPDATE_FETCH_HOST_SHIM_H

#include <stddef.h>
#include <string.h>

#define CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC 1
#define CONFIG_MBEDTLS_DYNAMIC_BUFFER 1

// g_update_image_id carries a GCC section/used/aligned attribute; MSVC has no such spelling.
#define __attribute__(x)

#define strncasecmp _strnicmp

// GCC __atomic builtin used for update_fetch.c's single-flight flag.
#include <intrin.h>
#define __ATOMIC_ACQ_REL 4
#define __atomic_exchange_n(p, v, o) _InterlockedExchange((volatile long *)(p), (long)(v))

// httpd_req_recv()'s timeout code (not in the shared esp_http_server.h stub).
#define HTTPD_SOCK_ERR_TIMEOUT (-3)

// Pull the private FreeRTOS/psa/app-desc stubs in before any shared stub header can pull in its sibling.
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "psa/crypto.h"
size_t strlcpy(char *dst, const char *src, size_t cap);

// Running identity the policy compares candidates against: a released build, so an upgrade to v1.2.3 is
// allowed and the typed-confirm / downgrade branches are reachable.
#define FW_RELEASE_VERSION "1.0.0"
#define FW_PARTITIONS_SHA256 "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"

#endif
