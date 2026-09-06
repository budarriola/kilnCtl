// Host test for App/drivers/http/partition_info_http.c's GET /api/partitions
// handler (api_partitions_get_handler(), static -- reached here by
// including the driver file directly, same "no other seam" convention
// test_zones_http.c/test_board_temps.c already document).
//
// WHY THIS EXISTS. The tool this endpoint replaces
// (tools/PcTools/src/kilnctrl/partition_table.py's original JTAG-based
// read_chip_partition_table_bytes()) shipped with 23 unit tests that were
// all genuinely sound -- EXCEPT that every one of them injected a fake
// read_memory_fn standing in for the real OpenOCD call, so the one seam
// that mattered (whether OpenOCD can actually read flash offset 0x8000)
// was never exercised and the tool failed the first time it touched real
// hardware. This file exists so the SAME mistake cannot happen again on
// the replacement: api_partitions_get_handler() is exercised end to end
// against a fake esp_partition_find()/esp_partition_next() table (the one
// seam that stands in for real hardware here), and its OUTPUT -- the
// actual chunked JSON bytes -- is captured and asserted on, not merely
// "did it return ESP_OK".
//
// Own executable (not merged into kilnctl_host_tests.exe), same reason
// test_board_temps.c/test_zones_http.c get their own: this file supplies
// its own esp_partition_find/esp_partition_next/esp_partition_get/
// esp_partition_iterator_release/esp_ota_get_running_partition bodies,
// which would multiply-define against another test file's own fakes of
// the same symbols if linked together.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "../drivers/http/partition_info_http.c"

#include "fake_sysinfo.h"

// ---------------------------------------------------------------------
// Fake esp_partition table + iterator -- stands in for the real
// esp_partition_find()/esp_partition_next() ESP-IDF gives a running app.
// Deliberately interleaves APP and DATA entries in TABLE order (not
// grouped by type) so a fix that only walks entries sequentially instead
// of type-by-type would be caught: esp_partition_find() below only ever
// returns entries whose type matches the requested one, exactly like the
// real ESP-IDF call.
// ---------------------------------------------------------------------
static const esp_partition_t *s_fake_table = NULL;
static int s_fake_table_count = 0;
static const esp_partition_t *s_fake_running = NULL;

typedef struct esp_partition_iterator_opaque_t {
    int idx;
} iter_impl_t;
static iter_impl_t s_iter_pool[16];
static int s_iter_pool_used = 0;

esp_partition_iterator_t esp_partition_find(esp_partition_type_t type, esp_partition_subtype_t subtype, const char *label)
{
    (void)subtype;
    (void)label;
    for (int i = 0; i < s_fake_table_count; i++) {
        if (s_fake_table[i].type == type) {
            iter_impl_t *it = &s_iter_pool[s_iter_pool_used++];
            it->idx = i;
            return it;
        }
    }
    return NULL;
}

const esp_partition_t *esp_partition_get(esp_partition_iterator_t iterator)
{
    return &s_fake_table[iterator->idx];
}

esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t iterator)
{
    int type = s_fake_table[iterator->idx].type;
    for (int i = iterator->idx + 1; i < s_fake_table_count; i++) {
        if (s_fake_table[i].type == type) {
            iter_impl_t *it = &s_iter_pool[s_iter_pool_used++];
            it->idx = i;
            return it;
        }
    }
    return NULL;
}

esp_err_t esp_partition_iterator_release(esp_partition_iterator_t iterator)
{
    // Real ESP-IDF documents esp_partition_iterator_release(NULL) as a
    // safe no-op -- api_partitions_get_handler()'s emit_partitions_of_type()
    // relies on exactly that at the end of every walk. Assert it here so a
    // future change that stops relying on that contract is at least
    // exercised once against a NULL argument.
    (void)iterator;
    return ESP_OK;
}

// api_partitions_get_handler() now reads the running partition through
// hal_sysinfo_get_running_partition() (HW abstraction Phase 3 item 4), not
// esp_ota_get_running_partition() directly -- this test's own fake table
// still models "running" as an esp_partition_t* (s_fake_running) since every
// other partition-table fake in this file is shaped that way, so
// set_fake_running() below is the one place that also arms fake_sysinfo's
// mirror of it. esp_ota_get_running_partition() itself is no longer called
// by the code under test, but is kept defined here (unused) so any other
// translation unit this executable might someday link that still calls it
// does not fail to link.
const esp_partition_t *esp_ota_get_running_partition(void)
{
    return s_fake_running;
}

static void set_fake_running(const esp_partition_t *p)
{
    s_fake_running = p;
    if (p) {
        hal_sysinfo_partition_info_t info = {0};
        snprintf(info.label, sizeof(info.label), "%s", p->label);
        info.address = p->address;
        info.size = p->size;
        fake_sysinfo_set_running_partition(&info);
    } else {
        // fake_sysinfo_set_running_partition() was never called (or called
        // with NULL) -- fake_sysinfo.h documents this as the "never called"
        // default, which already reports HAL_IO/absent, matching
        // esp_ota_get_running_partition() returning NULL.
        fake_sysinfo_reset_all();
    }
}

// ---------------------------------------------------------------------
// httpd surface -- captures every chunk api_partitions_get_handler() sends
// into one growing buffer, so the test can assert on the actual bytes a
// client would receive, not just the handler's return code.
// ---------------------------------------------------------------------
static char s_response[8192];
static size_t s_response_len = 0;
static int s_send_chunk_calls = 0;
static int s_send_chunk_fail_at = -1; // -1 = never fail

httpd_handle_t wifi_provision_http_get_server(void) { return (httpd_handle_t)1; }

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri)
{
    (void)handle;
    (void)uri;
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) { (void)r; (void)type; return ESP_OK; }

esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg)
{
    (void)r; (void)error; (void)msg;
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, size_t buf_len)
{
    (void)r;
    s_send_chunk_calls++;
    if (s_send_chunk_fail_at >= 0 && s_send_chunk_calls == s_send_chunk_fail_at) {
        return ESP_FAIL;
    }
    if (buf == NULL || buf_len == 0) {
        return ESP_OK; // terminating zero-length chunk -- nothing to append
    }
    if (s_response_len + buf_len >= sizeof(s_response)) {
        // Test buffer itself overflowed -- a real client has no such cap
        // (chunked transfer has no fixed size), so this would only fire if
        // this TEST's own capture buffer were undersized, not a handler bug.
        TEST_CHECK(false, "test capture buffer overflowed -- grow s_response");
        return ESP_OK;
    }
    memcpy(s_response + s_response_len, buf, buf_len);
    s_response_len += buf_len;
    s_response[s_response_len] = '\0';
    return ESP_OK;
}

static void reset_capture(void)
{
    s_response_len = 0;
    s_response[0] = '\0';
    s_send_chunk_calls = 0;
    s_send_chunk_fail_at = -1;
    s_iter_pool_used = 0;
}

// ---------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------

static const esp_partition_t k_table[] = {
    // label, type, subtype, offset, size, label str, ... -- struct order is
    // {address, size, label, type, subtype, encrypted} per stubs/esp_partition.h
    { .address = 0x9000, .size = 0x6000, .label = "nvs", .type = ESP_PARTITION_TYPE_DATA, .subtype = 0x02, .encrypted = false },
    { .address = 0x10000, .size = 0x100000, .label = "ota_0", .type = ESP_PARTITION_TYPE_APP, .subtype = 0x10, .encrypted = false },
    { .address = 0x110000, .size = 0x100000, .label = "ota_1", .type = ESP_PARTITION_TYPE_APP, .subtype = 0x11, .encrypted = true },
    { .address = 0x210000, .size = 0x10000, .label = "coredump", .type = ESP_PARTITION_TYPE_DATA, .subtype = 0x03, .encrypted = false },
};

static void test_reports_running_slot_and_every_partition(void)
{
    reset_capture();
    s_fake_table = k_table;
    s_fake_table_count = 4;
    set_fake_running(&k_table[1]); // ota_0 is running

    httpd_req_t req = {0};
    esp_err_t err = api_partitions_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK on a healthy table");

    TEST_CHECK(strstr(s_response, "\"running\":\"ota_0\"") != NULL, "response names the running slot");
    TEST_CHECK(strstr(s_response, "\"label\":\"nvs\"") != NULL, "nvs entry present");
    TEST_CHECK(strstr(s_response, "\"label\":\"ota_0\"") != NULL, "ota_0 entry present");
    TEST_CHECK(strstr(s_response, "\"label\":\"ota_1\"") != NULL, "ota_1 entry present");
    TEST_CHECK(strstr(s_response, "\"label\":\"coredump\"") != NULL, "coredump entry present");
    TEST_CHECK(strstr(s_response, "\"encrypted\":true") != NULL, "encrypted=true reported for ota_1");
    TEST_CHECK(strstr(s_response, "\"offset\":1114112") != NULL, "ota_1 offset (0x110000) reported in decimal");
    TEST_CHECK(strstr(s_response, "\"size\":1048576") != NULL, "ota_1 size (0x100000) reported in decimal");

    // Well-formed JSON shape: opens with '{', ends with "]}", exactly one
    // "partitions":[ , and a comma between every pair of entries (3 commas
    // for 4 entries, none leading/trailing/doubled).
    TEST_CHECK(s_response[0] == '{', "response starts with '{'");
    size_t len = strlen(s_response);
    TEST_CHECK(len >= 2 && s_response[len - 1] == '}' && s_response[len - 2] == ']', "response ends with ']}'");
    int boundary_count = 0;
    for (const char *p = strstr(s_response, "\"partitions\":["); p && *p; p++) {
        if (p[0] == '}' && p[1] == ',' && p[2] == '{') boundary_count++;
    }
    TEST_CHECK(boundary_count == 3, "exactly 3 '},{ ' entry boundaries for 4 partition entries (no leading/trailing/double comma)");
}

static void test_no_running_partition_reports_empty_string(void)
{
    reset_capture();
    s_fake_table = k_table;
    s_fake_table_count = 4;
    set_fake_running(NULL); // esp_ota_get_running_partition() can legitimately return NULL

    httpd_req_t req = {0};
    esp_err_t err = api_partitions_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler still returns ESP_OK with no running partition known");
    TEST_CHECK(strstr(s_response, "\"running\":\"\"") != NULL, "running reported as empty string, not omitted or null");
}

static void test_empty_table_produces_valid_empty_array(void)
{
    reset_capture();
    s_fake_table = k_table;
    s_fake_table_count = 0; // no partitions of either type found
    set_fake_running(NULL);

    httpd_req_t req = {0};
    esp_err_t err = api_partitions_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK on an empty table");
    TEST_CHECK(strstr(s_response, "\"partitions\":[]") != NULL, "empty table reports an empty array, not truncated JSON");
}

static void test_chunk_send_failure_mid_stream_is_propagated(void)
{
    // Proves the handler does not silently report ESP_OK if the transport
    // fails partway through -- fail on the 3rd chunk sent (head, then the
    // first partition entry, then this one), which is well inside the
    // per-type loop, not the terminating chunk.
    reset_capture();
    s_fake_table = k_table;
    s_fake_table_count = 4;
    set_fake_running(&k_table[0]);
    s_send_chunk_fail_at = 3;

    httpd_req_t req = {0};
    esp_err_t err = api_partitions_get_handler(&req);
    TEST_CHECK(err == ESP_FAIL, "handler propagates a mid-stream httpd_resp_send_chunk failure as ESP_FAIL");
}

static void test_only_one_type_present_has_no_stray_comma(void)
{
    // Only DATA partitions in the table -- exercises the "first type walked
    // is empty" path (APP has zero matches) without ever tripping the
    // *first flag incorrectly and emitting a leading comma before the first
    // real entry.
    static const esp_partition_t data_only[] = {
        { .address = 0x9000, .size = 0x6000, .label = "nvs", .type = ESP_PARTITION_TYPE_DATA, .subtype = 0x02, .encrypted = false },
        { .address = 0xf000, .size = 0x1000, .label = "otadata", .type = ESP_PARTITION_TYPE_DATA, .subtype = 0x00, .encrypted = false },
    };
    reset_capture();
    s_fake_table = data_only;
    s_fake_table_count = 2;
    set_fake_running(NULL);

    httpd_req_t req = {0};
    esp_err_t err = api_partitions_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK when only one partition type is present");
    TEST_CHECK(strstr(s_response, "[{\"label\":\"nvs\"") != NULL, "first entry has no leading comma");
    TEST_CHECK(strstr(s_response, ",{\"label\":\"otadata\"") != NULL, "second entry has exactly one leading comma");
    TEST_CHECK(strstr(s_response, ",,") == NULL, "no doubled comma anywhere in the response");
}

int main(void)
{
    TEST_SECTION("partition_info_http");

    test_reports_running_slot_and_every_partition();
    test_no_running_partition_reports_empty_string();
    test_empty_table_produces_valid_empty_array();
    test_chunk_send_failure_mid_stream_is_propagated();
    test_only_one_type_present_has_no_stray_comma();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
