// Host test for ui_page_network_secret.c (REVIEW_LCDFX13 INFO-1): masking must leave no secret bytes in RAM.
#include "test_common.h"
#include "../drivers/ui/ui_page_network_secret.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void run_test_ui_page_network_secret(void)
{
    TEST_SECTION("ui_page_network_secret_wipe: both buffers are zeroed, every byte");
    {
        char uri[64];
        char last[64];
        memset(uri, 'X', sizeof(uri));
        memset(last, 'Y', sizeof(last));
        snprintf(uri, sizeof(uri), "%s", "WIFI:T:WPA;S:kiln;P:hunter2;;");
        snprintf(last, sizeof(last), "%s", "WIFI:T:WPA;S:kiln;P:hunter2;;");
        ui_page_network_secret_wipe(uri, sizeof(uri), last, sizeof(last));
        bool uri_clear = true, last_clear = true;
        for (size_t i = 0; i < sizeof(uri); i++) {
            if (uri[i] != '\0') uri_clear = false;
            if (last[i] != '\0') last_clear = false;
        }
        TEST_CHECK(uri_clear, "uri buffer fully zeroed (no tail of the password remains)");
        TEST_CHECK(last_clear, "last-encoded buffer fully zeroed");
        TEST_CHECK(strstr(uri, "hunter2") == NULL && strstr(last, "hunter2") == NULL, "password text gone");
    }

    TEST_SECTION("ui_page_network_secret_wipe: NULL / empty buffers are tolerated");
    {
        char last[8] = "abc";
        ui_page_network_secret_wipe(NULL, 0, last, sizeof(last));
        TEST_CHECK(last[0] == '\0', "NULL uri skipped, last still wiped");
        ui_page_network_secret_wipe(NULL, 0, NULL, 0);
        TEST_CHECK(1, "both NULL does not crash");
    }

    TEST_SECTION("blank QR payload carries no secret");
    {
        TEST_CHECK(strstr(UI_PAGE_NETWORK_AP_QR_BLANK, "P:") == NULL, "blank payload has no password field");
    }
}
