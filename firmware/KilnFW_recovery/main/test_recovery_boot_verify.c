// test_recovery_boot_verify.c -- host test for recovery_boot_verify.c: the
// read-back after esp_ota_set_boot_partition(). Stubbed ESP-IDF calls
// (host_stubs/host_esp_stub.h), built and run by check_recovery_boot_verify.ps1.
// Prints "RESULT pass=N fail=N".
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>

#include "recovery_boot_verify.h"

static int g_pass, g_fail;

#define CHECK(cond, name)                                                   \
    do {                                                                    \
        if ((cond)) {                                                       \
            g_pass++;                                                       \
        } else {                                                            \
            g_fail++;                                                       \
            fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);        \
        }                                                                   \
    } while (0)

static esp_err_t s_set_rc;
static const esp_partition_t *s_boot_part; // what get_boot_partition reports
static const esp_partition_t *s_follow;    // if set, a successful set() makes get() report the request
static int s_set_calls, s_get_calls;

esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p)
{
    s_set_calls++;
    if (s_set_rc == ESP_OK && s_follow) {
        s_boot_part = p;
    }
    return s_set_rc;
}
const esp_partition_t *esp_ota_get_boot_partition(void)
{
    s_get_calls++;
    return s_boot_part;
}

static void reset(void)
{
    s_set_rc = ESP_OK;
    s_boot_part = NULL;
    s_follow = NULL;
    s_set_calls = s_get_calls = 0;
}

int main(void)
{
    esp_partition_t app = {1, 0x20000, 0x10, "app"};
    esp_partition_t app_copy = {2, 0x20000, 0x10, "app"}; // distinct object, same flash location
    esp_partition_t factory = {3, 0x10000, 0x00, "recovery"};
    esp_partition_t same_addr_other_subtype = {4, 0x20000, 0x11, "x"};
    esp_partition_t same_subtype_other_addr = {5, 0x30000, 0x10, "y"};

    CHECK(recovery_boot_partition_matches(&app, &app_copy), "same address+subtype matches");
    CHECK(!recovery_boot_partition_matches(&app, &factory), "different partition does not match");
    CHECK(!recovery_boot_partition_matches(&app, &same_addr_other_subtype), "subtype is compared");
    CHECK(!recovery_boot_partition_matches(&app, &same_subtype_other_addr), "address is compared");
    CHECK(!recovery_boot_partition_matches(NULL, &app) && !recovery_boot_partition_matches(&app, NULL) &&
              !recovery_boot_partition_matches(NULL, NULL),
          "NULL never matches");

    reset();
    s_follow = &app;
    CHECK(recovery_boot_partition_set_and_verify(&app) == ESP_OK, "set + matching read-back succeeds");
    CHECK(s_set_calls == 1 && s_get_calls == 1, "one set, one read-back");

    reset(); // set reports OK but otadata did not take: boot target still recovery
    s_boot_part = &factory;
    CHECK(recovery_boot_partition_set_and_verify(&app) == ESP_ERR_INVALID_STATE,
          "read-back mismatch fails loud");

    reset(); // read-back NULL
    CHECK(recovery_boot_partition_set_and_verify(&app) == ESP_ERR_INVALID_STATE, "NULL read-back fails loud");

    reset();
    s_set_rc = 0x1500 + 3; // e.g. ESP_ERR_OTA_VALIDATE_FAILED: passed through, no read-back
    s_boot_part = &app;
    CHECK(recovery_boot_partition_set_and_verify(&app) == s_set_rc, "a failed set propagates its own error");
    CHECK(s_get_calls == 0, "no read-back after a failed set");

    reset();
    CHECK(recovery_boot_partition_set_and_verify(NULL) == ESP_ERR_INVALID_ARG && s_set_calls == 0,
          "NULL request refused before any set");

    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
