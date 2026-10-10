// Host test, campaign 9b (docs/audits/HOST_TEST_COVERAGE_GAPS_2026-10-09.md item 9, second half):
// refusal and failure-mapping paths of the OTA HTTP handlers (ota_http_esp.c, ota_http_pico.c,
// ota_http_recovery.c) and the route registration table. It #includes test_ota_http.c (and with it every
// handler .c plus all the fakes) and adds a second main. The base file's main is renamed away.
//
// Auth is the dispatcher's ADMIN tier (route_tier_table.h), not in-handler, so there is no in-handler auth
// refusal to test here. What is covered: the interlock gate (409/428), the single update claim (a refusal must
// not release another party's claim; every failure path must release its own), body/size/header validation,
// 500 mapping of flash/boot-partition failures, and "never claim success on an unverified write".
#define main ota_http_base_main
#include "test_ota_http.c"
#undef main

static esp_partition_t s_part_target = { .address = 0x110000, .size = 0x200000, .label = "app", .type = 0, .subtype = 0x10 };
static esp_partition_t s_part_running = { .address = 0x10000, .size = 0x200000, .label = "recovery", .type = 0, .subtype = 0x00 };

static unsigned char s_img[8192];

static void refusal_reset(void)
{
    static int dummy_io;
    s_io = (kiln_io_t *)&dummy_io;
    s_fake_io_read_result = ESP_OK;
    s_fake_io_relay_shadow = 0;
    g_stub_profile_state = PROFILE_EXEC_IDLE;
    g_stub_autotune_active = false;
    s_test_sweep_active = false;
    ota_http_safety = NULL;
    s_update_claim = OTA_UPDATE_NONE;
    stub_headers_reset();
    stub_header_set("X-Ota-Ack-No-Safety", "1");
    s_last_err_code = 0;
    s_last_err_msg[0] = '\0';
    s_last_resp_status[0] = '\0';
    s_last_resp_body[0] = '\0';
    s_fake_next_part = &s_part_target;
    s_fake_running_part = &s_part_running;
    s_fake_boot_part = &s_part_running;
    s_fake_set_ignored = false;
    s_fake_set_rc = ESP_OK;
    s_fake_ota_begin_rc = ESP_OK;
    s_fake_ota_write_rc = ESP_OK;
    s_fake_ota_write_ok_budget = -1;
    s_fake_ota_end_rc = ESP_OK;
    s_fake_ota_begin_calls = s_fake_ota_write_calls = s_fake_ota_end_calls = s_fake_ota_abort_calls = 0;
    s_fake_rollback_possible = true;
    s_fake_recv_body = NULL;
    s_fake_recv_off = 0;
    s_fake_recv_bin = s_img;
    s_fake_recv_bin_len = 0;
    s_fake_recv_bin_off = 0;
    s_fake_recv_max_chunk = 4096;
    s_fake_recv_fail_after = -1;
    s_fake_recv_calls = 0;
    s_fake_select_result = RECOVERY_SWITCH_OK;
    s_fake_select_calls = 0;
    s_fake_restore_calls = 0;
    s_fake_relay_start_rc = false;
    s_fake_relay_start_calls = 0;
    s_stub_recovery_mode = false;
    s_stub_mark_healthy_calls = 0;
    g_test_stub_xtaskcreate_result = 1;
    memset(s_img, 0, sizeof(s_img));
    s_img[0] = 0xE9;
    s_img[12] = 9; /* chip id ESP32-S3, little endian uint16 at offset 12 */
}

/* Feeds an image body of `len` bytes (valid header) to the fake recv, and a request of that content_len. */
static void refusal_body(httpd_req_t *req, size_t len)
{
    memset(req, 0, sizeof(*req));
    req->content_len = len;
    s_fake_recv_bin = s_img;
    s_fake_recv_bin_len = len;
    s_fake_recv_bin_off = 0;
}

static bool claim_free(void) { return s_update_claim == OTA_UPDATE_NONE; }

static void test_esp_push_interlock_and_claim(void)
{
    TEST_SECTION("ota_esp_post_handler -- interlock refusals and the single update claim");
    httpd_req_t req;

    refusal_reset();
    g_stub_profile_state = PROFILE_EXEC_RUNNING;
    refusal_body(&req, 4096);
    (void)ota_esp_post_handler(&req);
    TEST_CHECK(strncmp(s_last_resp_status, "409", 3) == 0, "firing: 409");
    TEST_CHECK(s_fake_ota_begin_calls == 0, "firing: esp_ota_begin never reached");
    TEST_CHECK(claim_free(), "firing: no claim taken");

    refusal_reset();
    g_stub_autotune_active = true;
    refusal_body(&req, 4096);
    (void)ota_esp_post_handler(&req);
    TEST_CHECK(strncmp(s_last_resp_status, "409", 3) == 0, "autotune: 409");
    TEST_CHECK(s_fake_ota_begin_calls == 0, "autotune: esp_ota_begin never reached");

    refusal_reset();
    stub_headers_reset(); /* no ack header, no safety link */
    refusal_body(&req, 4096);
    (void)ota_esp_post_handler(&req);
    TEST_CHECK(strncmp(s_last_resp_status, "428", 3) == 0, "no safety ack: 428");
    TEST_CHECK(s_fake_ota_begin_calls == 0, "no ack: esp_ota_begin never reached");
    TEST_CHECK(claim_free(), "no ack: no claim taken");

    refusal_reset();
    TEST_CHECK(ota_http_update_try_begin(OTA_HTTP_CONTEXT_PICO), "setup: another party holds the claim");
    refusal_body(&req, 4096);
    (void)ota_esp_post_handler(&req);
    TEST_CHECK(strncmp(s_last_resp_status, "409", 3) == 0, "claim held by another: 409");
    TEST_CHECK(s_update_claim == OTA_UPDATE_PICO, "a refused push must NOT release the other party's claim");
    TEST_CHECK(s_fake_ota_begin_calls == 0, "claim held: esp_ota_begin never reached");
    ota_http_update_end();
}

static void test_esp_push_validation_and_failures(void)
{
    TEST_SECTION("ota_esp_post_handler -- body/size validation, 500 mapping, abort and claim release");
    httpd_req_t req;

    refusal_reset();
    refusal_body(&req, 0);
    (void)ota_esp_post_handler(&req);
    TEST_CHECK(s_last_err_code == 400, "content-length 0: 400");
    TEST_CHECK(claim_free(), "content-length 0: claim released");

    refusal_reset();
    s_fake_next_part = NULL;
    refusal_body(&req, 4096);
    (void)ota_esp_post_handler(&req);
    TEST_CHECK(s_last_err_code == 500, "no OTA partition: 500");
    TEST_CHECK(claim_free(), "no OTA partition: claim released");

    refusal_reset();
    s_fake_running_part = &s_part_target; /* single-slot: next == running */
    refusal_body(&req, 4096);
    (void)ota_esp_post_handler(&req);
    TEST_CHECK(strncmp(s_last_resp_status, "409", 3) == 0, "target == running: 409");
    TEST_CHECK(s_fake_ota_begin_calls == 0, "target == running: esp_ota_begin never called");
    TEST_CHECK(claim_free(), "target == running: claim released");

    refusal_reset();
    refusal_body(&req, s_part_target.size + 1);
    (void)ota_esp_post_handler(&req);
    TEST_CHECK(s_last_err_code == 400, "image larger than partition: 400");
    TEST_CHECK(s_fake_ota_begin_calls == 0, "oversize: esp_ota_begin never called");
    TEST_CHECK(claim_free(), "oversize: claim released");

    refusal_reset();
    refusal_body(&req, 4096);
    s_fake_recv_fail_after = 0; /* header read fails */
    (void)ota_esp_post_handler(&req);
    TEST_CHECK(s_last_err_code == 400, "header read failure: 400");
    TEST_CHECK(s_fake_ota_begin_calls == 0, "header read failure: begin not called");
    TEST_CHECK(claim_free(), "header read failure: claim released");

    refusal_reset();
    s_img[0] = 0x00;
    refusal_body(&req, 4096);
    (void)ota_esp_post_handler(&req);
    TEST_CHECK(s_last_err_code == 400, "bad magic: 400");
    TEST_CHECK(s_fake_ota_begin_calls == 0, "bad magic: begin not called");
    TEST_CHECK(claim_free(), "bad magic: claim released");

    refusal_reset();
    s_img[12] = 5; /* wrong chip */
    refusal_body(&req, 4096);
    (void)ota_esp_post_handler(&req);
    TEST_CHECK(s_last_err_code == 400, "wrong chip id: 400");
    TEST_CHECK(s_fake_ota_begin_calls == 0, "wrong chip id: begin not called");

    refusal_reset();
    s_fake_ota_begin_rc = ESP_FAIL;
    refusal_body(&req, 4096);
    (void)ota_esp_post_handler(&req);
    TEST_CHECK(s_last_err_code == 500, "esp_ota_begin fails: 500");
    TEST_CHECK(s_fake_ota_abort_calls == 0, "begin failed: nothing to abort");
    TEST_CHECK(claim_free(), "begin failed: claim released");

    refusal_reset();
    s_fake_ota_write_rc = ESP_FAIL;
    s_fake_ota_write_ok_budget = 0;
    refusal_body(&req, 4096);
    (void)ota_esp_post_handler(&req);
    TEST_CHECK(s_last_err_code == 500, "header write fails: 500");
    TEST_CHECK(s_fake_ota_abort_calls == 1, "header write fails: esp_ota_abort called");
    TEST_CHECK(claim_free(), "header write fails: claim released");

    refusal_reset();
    refusal_body(&req, 8192);
    s_fake_recv_fail_after = 6000; /* dies mid-transfer */
    (void)ota_esp_post_handler(&req);
    TEST_CHECK(s_last_err_code == 400, "recv dies mid-transfer: 400");
    TEST_CHECK(s_fake_ota_abort_calls == 1, "mid-transfer failure: esp_ota_abort called");
    TEST_CHECK(s_fake_boot_part == &s_part_running, "mid-transfer failure: boot partition untouched");
    TEST_CHECK(claim_free(), "mid-transfer failure: claim released");

    refusal_reset();
    s_fake_ota_write_rc = ESP_FAIL;
    s_fake_ota_write_ok_budget = 1; /* header write ok, first chunk write fails */
    refusal_body(&req, 8192);
    (void)ota_esp_post_handler(&req);
    TEST_CHECK(s_last_err_code == 500, "chunk write fails: 500");
    TEST_CHECK(s_fake_ota_abort_calls == 1, "chunk write fails: esp_ota_abort called");
    TEST_CHECK(claim_free(), "chunk write fails: claim released");

    refusal_reset();
    s_fake_ota_end_rc = ESP_FAIL;
    refusal_body(&req, 4096);
    (void)ota_esp_post_handler(&req);
    TEST_CHECK(s_last_err_code == 400, "esp_ota_end rejects the image: 400");
    TEST_CHECK(s_fake_ota_abort_calls == 0, "esp_ota_end already freed the handle: no abort");
    TEST_CHECK(s_fake_boot_part == &s_part_running, "rejected image: boot partition untouched");
    TEST_CHECK(claim_free(), "rejected image: claim released");

    refusal_reset();
    s_fake_set_ignored = true; /* set reports OK but does not take effect */
    refusal_body(&req, 4096);
    (void)ota_esp_post_handler(&req);
    TEST_CHECK(s_last_err_code == 500, "boot set ignored: 500, never a success body");
    TEST_CHECK(strstr(s_last_resp_body, "\"ok\":true") == NULL, "boot set ignored: no ok:true body");
    TEST_CHECK(claim_free(), "boot set ignored: claim released");

    refusal_reset();
    refusal_body(&req, 4096);
    esp_err_t err = ota_esp_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "success: ESP_OK");
    TEST_CHECK(s_last_err_code == 0, "success: no error response");
    TEST_CHECK(strstr(s_last_resp_body, "\"ok\":true") != NULL, "success: ok:true body");
    TEST_CHECK(s_fake_boot_part == &s_part_target, "success: boot partition is the target");
    TEST_CHECK(s_fake_ota_end_calls == 1, "success: esp_ota_end called once");
    TEST_CHECK(claim_free(), "success: claim released");
}

static void test_esp_rollback_refusals(void)
{
    TEST_SECTION("ota_esp_rollback_post_handler -- interlock, claim, no previous image");
    httpd_req_t req;
    memset(&req, 0, sizeof(req));

    refusal_reset();
    g_stub_profile_state = PROFILE_EXEC_RUNNING;
    (void)ota_esp_rollback_post_handler(&req);
    TEST_CHECK(strncmp(s_last_resp_status, "409", 3) == 0, "firing: 409");
    TEST_CHECK(claim_free(), "firing: no claim taken");

    refusal_reset();
    TEST_CHECK(ota_http_update_try_begin(OTA_HTTP_CONTEXT_PICO), "setup: other party claim");
    (void)ota_esp_rollback_post_handler(&req);
    TEST_CHECK(strncmp(s_last_resp_status, "409", 3) == 0, "claim held: 409");
    TEST_CHECK(s_update_claim == OTA_UPDATE_PICO, "refusal must not release the other party's claim");
    ota_http_update_end();

    refusal_reset();
    s_fake_rollback_possible = false;
    (void)ota_esp_rollback_post_handler(&req);
    TEST_CHECK(strncmp(s_last_resp_status, "409", 3) == 0, "no previous image: 409");
    TEST_CHECK(strstr(s_last_resp_body, "\"ok\":true") == NULL, "no previous image: no ok:true");
    TEST_CHECK(claim_free(), "no previous image: claim released");

    refusal_reset();
    (void)ota_esp_rollback_post_handler(&req);
    TEST_CHECK(strstr(s_last_resp_body, "\"status\":\"rebooting\"") != NULL, "success: rebooting body");
    TEST_CHECK(s_update_claim == OTA_UPDATE_ESP, "success: claim intentionally held across the reboot");
    ota_http_update_end();
}

static void test_recovery_exit_and_boot_guard(void)
{
    TEST_SECTION("recovery_exit / boot_guard_reset -- mode check and result reporting");
    httpd_req_t req;
    memset(&req, 0, sizeof(req));

    refusal_reset();
    s_stub_recovery_mode = false;
    (void)ota_recovery_exit_post_handler(&req);
    TEST_CHECK(s_last_err_code == 403, "not in recovery mode: 403");
    TEST_CHECK(s_stub_mark_healthy_calls == 0, "not in recovery mode: boot counter untouched");

    refusal_reset();
    s_stub_recovery_mode = true;
    (void)ota_recovery_exit_post_handler(&req);
    TEST_CHECK(s_stub_mark_healthy_calls == 1, "recovery mode: counter clear attempted exactly once");

    refusal_reset();
    s_stub_boot_guard_reset_verified = false;
    (void)ota_boot_guard_reset_post_handler(&req);
    TEST_CHECK(strstr(s_last_resp_body, "\"ok\":false") != NULL, "unverified clear reported ok:false");
    TEST_CHECK(strstr(s_last_resp_body, "\"ok\":true") == NULL, "unverified clear never reports ok:true");
    s_stub_boot_guard_reset_verified = true;
    (void)ota_boot_guard_reset_post_handler(&req);
    TEST_CHECK(strstr(s_last_resp_body, "\"ok\":true") != NULL, "verified clear reports ok:true");
}

static void test_recovery_boot_refusals(void)
{
    TEST_SECTION("ota_recovery_boot_post_handler -- mode gate, interlock, claim, relay, select/restore ordering");
    httpd_req_t req;
    memset(&req, 0, sizeof(req));

    refusal_reset();
    g_stub_profile_state = PROFILE_EXEC_RUNNING;
    (void)ota_recovery_boot_post_handler(&req);
    TEST_CHECK(strncmp(s_last_resp_status, "409", 3) == 0, "firing: mode gate 409");
    TEST_CHECK(s_fake_select_calls == 0, "firing: boot target never touched");
    TEST_CHECK(claim_free(), "firing: no claim taken");

    refusal_reset();
    g_stub_autotune_active = true;
    (void)ota_recovery_boot_post_handler(&req);
    TEST_CHECK(strncmp(s_last_resp_status, "409", 3) == 0, "autotune: 409");
    TEST_CHECK(s_fake_select_calls == 0, "autotune: boot target never touched");

    refusal_reset();
    stub_headers_reset(); /* no ack, no safety link: interlock refuses */
    (void)ota_recovery_boot_post_handler(&req);
    TEST_CHECK(strncmp(s_last_resp_status, "428", 3) == 0, "interlock (no safety ack): 428");
    TEST_CHECK(s_fake_select_calls == 0, "interlock: boot target never touched");

    refusal_reset();
    TEST_CHECK(ota_http_update_try_begin(OTA_HTTP_CONTEXT_PICO), "setup: other party claim");
    (void)ota_recovery_boot_post_handler(&req);
    TEST_CHECK(strncmp(s_last_resp_status, "409", 3) == 0, "claim held: 409");
    TEST_CHECK(s_update_claim == OTA_UPDATE_PICO, "refusal must not release the other party's claim");
    TEST_CHECK(s_fake_select_calls == 0, "claim held: boot target never touched");
    ota_http_update_end();

    refusal_reset();
    s_fake_io_relay_shadow = 0x01; /* K1 energized */
    (void)ota_recovery_boot_post_handler(&req);
    TEST_CHECK(strncmp(s_last_resp_status, "409", 3) == 0, "relay energized: 409");
    TEST_CHECK(s_fake_select_calls == 0, "relay energized: boot target never touched");
    TEST_CHECK(claim_free(), "relay energized: claim released");

    refusal_reset();
    s_fake_io_read_result = ESP_FAIL; /* relay state unreadable counts as energized */
    (void)ota_recovery_boot_post_handler(&req);
    TEST_CHECK(strncmp(s_last_resp_status, "409", 3) == 0, "relay unreadable: 409");
    TEST_CHECK(s_fake_select_calls == 0, "relay unreadable: boot target never touched");

    refusal_reset();
    s_fake_select_result = RECOVERY_SWITCH_NOT_PRESENT;
    (void)ota_recovery_boot_post_handler(&req);
    TEST_CHECK(strncmp(s_last_resp_status, "409", 3) == 0, "recovery not present: 409");
    TEST_CHECK(s_fake_restore_calls == 0, "recovery not present: nothing written, nothing restored");
    TEST_CHECK(claim_free(), "recovery not present: claim released");

    refusal_reset();
    s_fake_select_result = RECOVERY_SWITCH_SET_FAILED;
    (void)ota_recovery_boot_post_handler(&req);
    TEST_CHECK(strncmp(s_last_resp_status, "500", 3) == 0, "set failed: 500");
    TEST_CHECK(s_fake_restore_calls == 1, "set failed: boot target restored once");
    TEST_CHECK(strstr(s_last_resp_body, "\"ok\":true") == NULL, "set failed: no success body");
    TEST_CHECK(claim_free(), "set failed: claim released");

    refusal_reset();
    g_test_stub_xtaskcreate_result = 0; /* pdFAIL */
    (void)ota_recovery_boot_post_handler(&req);
    TEST_CHECK(s_last_err_code == 500, "reboot task create failure: 500");
    TEST_CHECK(s_fake_restore_calls == 1, "reboot task create failure: boot target restored");
    TEST_CHECK(strstr(s_last_resp_body, "rebooting") == NULL, "reboot task create failure: no 'rebooting' body");
    TEST_CHECK(claim_free(), "reboot task create failure: claim released");

    refusal_reset();
    (void)ota_recovery_boot_post_handler(&req);
    TEST_CHECK(strstr(s_last_resp_body, "\"target\":\"recovery\"") != NULL, "success: recovery target body");
    TEST_CHECK(s_fake_restore_calls == 0, "success: no restore");
    TEST_CHECK(s_update_claim == OTA_UPDATE_ESP, "success: claim held across the reboot");
    ota_http_update_end();
}

static void test_pico_push_refusals(void)
{
    TEST_SECTION("ota_pico_post_handler -- interlock, claim, body and relay-start failures release the claim");
    httpd_req_t req;

    refusal_reset();
    refusal_body(&req, 4096); /* ota_http_safety is NULL */
    (void)ota_pico_post_handler(&req);
    TEST_CHECK(s_last_err_code == 500, "no safety link configured: 500");
    TEST_CHECK(s_fake_relay_start_calls == 0, "no safety link: relay never started");
    TEST_CHECK(claim_free(), "no safety link: claim released");

    refusal_reset();
    memset(&s_rollback_test_safety, 0, sizeof(s_rollback_test_safety));
    ota_http_safety = &s_rollback_test_safety;
    g_stub_profile_state = PROFILE_EXEC_RUNNING;
    refusal_body(&req, 4096);
    (void)ota_pico_post_handler(&req);
    TEST_CHECK(strncmp(s_last_resp_status, "409", 3) == 0, "firing: 409");
    TEST_CHECK(s_fake_relay_start_calls == 0, "firing: relay never started");
    TEST_CHECK(claim_free(), "firing: no claim taken");

    refusal_reset();
    memset(&s_rollback_test_safety, 0, sizeof(s_rollback_test_safety));
    ota_http_safety = &s_rollback_test_safety;
    TEST_CHECK(ota_http_update_try_begin(OTA_HTTP_CONTEXT_ESP), "setup: other party claim");
    refusal_body(&req, 4096);
    (void)ota_pico_post_handler(&req);
    TEST_CHECK(strncmp(s_last_resp_status, "409", 3) == 0, "claim held: 409");
    TEST_CHECK(s_update_claim == OTA_UPDATE_ESP, "refusal must not release the other party's claim");
    ota_http_update_end();

    refusal_reset();
    memset(&s_rollback_test_safety, 0, sizeof(s_rollback_test_safety));
    ota_http_safety = &s_rollback_test_safety;
    refusal_body(&req, 0);
    (void)ota_pico_post_handler(&req);
    TEST_CHECK(s_last_err_code == 400, "content-length 0: 400");
    TEST_CHECK(claim_free(), "content-length 0: claim released");

    refusal_reset();
    memset(&s_rollback_test_safety, 0, sizeof(s_rollback_test_safety));
    ota_http_safety = &s_rollback_test_safety;
    refusal_body(&req, 8192);
    s_fake_recv_fail_after = 4096;
    (void)ota_pico_post_handler(&req);
    TEST_CHECK(s_last_err_code == 400, "recv dies mid-transfer: 400");
    TEST_CHECK(s_fake_relay_start_calls == 0, "mid-transfer failure: relay never started");
    TEST_CHECK(claim_free(), "mid-transfer failure: claim released");

    refusal_reset();
    memset(&s_rollback_test_safety, 0, sizeof(s_rollback_test_safety));
    ota_http_safety = &s_rollback_test_safety;
    s_fake_relay_start_rc = false;
    refusal_body(&req, 4096);
    (void)ota_pico_post_handler(&req);
    TEST_CHECK(s_fake_relay_start_calls == 1, "relay start attempted once");
    TEST_CHECK(s_last_err_code == 500, "relay start fails: 500");
    TEST_CHECK(strstr(s_last_resp_body, "relay_started") == NULL, "relay start fails: no relay_started body");
    TEST_CHECK(claim_free(), "relay start fails: claim released (no relay task owns it)");

    refusal_reset();
    memset(&s_rollback_test_safety, 0, sizeof(s_rollback_test_safety));
    ota_http_safety = &s_rollback_test_safety;
    s_fake_relay_start_rc = true;
    refusal_body(&req, 4096);
    (void)ota_pico_post_handler(&req);
    TEST_CHECK(strncmp(s_last_resp_status, "202", 3) == 0, "relay started: 202");
    TEST_CHECK(s_update_claim == OTA_UPDATE_PICO, "relay started: claim handed to the relay task, still held");
    ota_http_update_end();
    ota_http_safety = NULL;
}

static void test_route_registration(void)
{
    TEST_SECTION("ota_http_start -- route registration count, no duplicates, no NULL handlers");
    static int dummy_server;
    s_reg_count = 0;
    s_fake_http_server = &dummy_server;
    esp_err_t err = ota_http_start(NULL, NULL, NULL);
    s_fake_http_server = NULL;
    TEST_CHECK(err == ESP_OK, "ota_http_start succeeds against a fake server");
    TEST_CHECK(s_reg_count == 13, "exactly 13 routes registered");
    bool dup = false, null_handler = false;
    for (int i = 0; i < s_reg_count; i++) {
        if (!s_reg[i].handler) null_handler = true;
        for (int j = i + 1; j < s_reg_count; j++) {
            if (s_reg[i].method == s_reg[j].method && strcmp(s_reg[i].uri, s_reg[j].uri) == 0) dup = true;
        }
    }
    TEST_CHECK(!dup, "no duplicate (uri, method) pair");
    TEST_CHECK(!null_handler, "no NULL handler");
}

int main(void)
{
    esp_err_t e = ota_http_start(NULL, NULL, NULL); /* initialises the locks; INVALID_STATE without a server */
    TEST_CHECK(e == ESP_ERR_INVALID_STATE || e == ESP_OK, "ota_http_start initialises locks");
    test_esp_push_interlock_and_claim();
    test_esp_push_validation_and_failures();
    test_esp_rollback_refusals();
    test_recovery_exit_and_boot_guard();
    test_recovery_boot_refusals();
    test_pico_push_refusals();
    test_route_registration();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
