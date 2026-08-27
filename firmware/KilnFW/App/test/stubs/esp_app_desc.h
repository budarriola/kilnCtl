// Host-test stub -- see stubs/esp_err.h for why these exist. Added
// 2026-08-27 for ota_http.c's host tests (test_ota_http.c).
#ifndef TEST_STUB_ESP_APP_DESC_H
#define TEST_STUB_ESP_APP_DESC_H

#include <stdint.h>

typedef struct {
    uint32_t magic_word;
    uint32_t secure_version;
    uint32_t reserv1[2];
    char version[32];
    char project_name[32];
    char time[16];
    char date[16];
    char idf_ver[32];
    uint8_t app_elf_sha256[32];
    uint32_t min_efuse_blk_rev_full;
    uint32_t max_efuse_blk_rev_full;
    uint32_t reserv2[19];
} esp_app_desc_t;

// Test-controllable -- defaults to NULL (no running-app descriptor), which is
// exactly the "cannot be told this build's version" case ota_http.c already
// treats as an empty-string fallback. Set by a test to exercise the
// "real descriptor present" branch.
extern const esp_app_desc_t *g_stub_esp_app_desc;

static inline const esp_app_desc_t *esp_app_get_description(void) { return g_stub_esp_app_desc; }

#endif // TEST_STUB_ESP_APP_DESC_H
