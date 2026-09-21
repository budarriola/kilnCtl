// Host tests for App/drivers/net/wifi_prov.c's static-IP AP-fallback logic
// (TODO.md section 1's 2026-08-21 fix). No ESP-IDF/FreeRTOS runtime --
// wifi_prov.c is genuinely coupled to esp_wifi/esp_netif/esp_timer/nvs/lwIP/
// FreeRTOS, far more than any other file under host test today, so this
// pulls in a much bigger stub surface (see App/test/stubs/esp_wifi.h,
// esp_netif.h, esp_event.h, esp_timer.h, nvs.h, nvs_flash.h,
// esp_http_server.h, lwip/*, freertos/queue.h, and the freertos/task.h +
// freertos/semphr.h additions) than test_profile_feasibility.c's type-only
// stand-ins.
//
// wifi_prov.c is #included directly (not compiled as a separate source and
// linked) so this file can reach s_wifi and the do_*() statics -- the only
// way to test this module's actual behavior without a production-code seam.
// The regression under test lives entirely in do_ev_got_ip()/
// do_confirm_static_reachable()/wifi_prov_note_possible_static_reachability(),
// all static or file-scope, so there is no public API that could stand in
// for direct access. This is a HOST-TEST-ONLY convention -- wifi_prov.c
// itself is unmodified, and this file is never compiled on-target.
//
// Two things are genuinely untestable on the host and are noted, not
// silently skipped:
//   - wifi_prov_start()/owner_task() themselves (real esp_wifi/esp_netif
//     bring-up, real task creation) -- the stubs make these no-ops on
//     purpose; testing them would just be testing the stubs.
//   - The actual xQueueSend/owner_task() dispatch path -- the queue stub
//     never delivers (see freertos/queue.h), so
//     wifi_prov_note_possible_static_reachability()'s "did it decide to
//     post CMD_CONFIRM_STATIC_REACHABLE" and do_confirm_static_reachable()'s
//     "what happens once that command is serviced" are tested as two
//     separate steps rather than end-to-end through a real queue -- which
//     matches the code's own caller-thread-validates /
//     owner-task-mutates split (see wifi_prov_note_possible_static_
//     reachability()'s doc comment in wifi_prov.h).
#include <string.h>

#include "test_common.h"

// Needed here (ahead of wifi_prov.c's own #includes below) purely for the
// TYPES of the stub globals this file defines -- esp_wifi.h/esp_err.h/
// esp_http_server.h are otherwise only reached transitively through
// wifi_prov.c's own #include list.
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_wifi.h"

// ---- Stub globals wifi_prov.c's stand-in headers read/write ----
// (extern-declared in the stub headers themselves; defined once here.)
wifi_mode_t g_stub_wifi_mode = WIFI_MODE_NULL;
int g_stub_wifi_set_mode_calls = 0;
// esp_wifi_set_storage()/esp_wifi_restore() counters (added 2026-09-21,
// docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md) -- wifi_prov.c
// now calls esp_wifi_set_storage(RAM) once during wifi_prov_start();
// test_wifi_prov_start_sets_ram_storage() below asserts on both counters.
int g_stub_wifi_set_storage_calls = 0;
wifi_storage_t g_stub_wifi_last_storage = WIFI_STORAGE_FLASH;
int g_stub_wifi_restore_calls = 0;
esp_err_t g_stub_ap_info_result = ESP_OK;
int8_t g_stub_ap_info_rssi = -50;
int g_stub_getsockname_result = 0;
char g_stub_local_ip[16] = "0.0.0.0";
int g_stub_queue_send_calls = 0;

// wifi_provision_http_start()/wifi_provision_http_get_server() are declared
// by the real wifi_provision_http.h (off-limits -- another agent owns
// wifi_provision_http.c) and called from wifi_prov_start(), which this file
// never calls. Bodies supplied here purely to satisfy the linker.
esp_err_t wifi_provision_http_start(void) { return ESP_OK; }
httpd_handle_t wifi_provision_http_get_server(void) { return NULL; }

// time_sync_notify_got_ip() is declared by the real time_sync.h and called
// from do_ev_got_ip() -- see wifi_prov.c's hook comment there. The real
// definition (time_sync.c) is off-limits to a host build (esp_netif_sntp.h/
// nvs.h, no stub written for it in this pass -- see time_sync_tz.h's header
// comment). This fake just counts calls, same "count it, don't simulate it"
// shape as g_stub_wifi_set_mode_calls above, so a test can assert the hook
// fired without needing SNTP itself to be host-testable.
int g_stub_time_sync_notify_got_ip_calls = 0;
void time_sync_notify_got_ip(void) { g_stub_time_sync_notify_got_ip_calls++; }

#include "../drivers/net/wifi_prov.c"
// wifi_prov.c split 2026-09-04 (ROADMAP.md M15 A3, "files over 1500 lines
// should be broken up where it makes sense") -- the new wifi_prov_*.c pieces
// are NOT added as separate compile units in build_host_tests.ps1; this file
// #includes all of them directly, same convention as
// test_profile_executor_prestart.c's own multi-#include block, so this file
// keeps reaching every split-out module's `static`/file-scope internals
// (do_ev_got_ip()/do_confirm_static_reachable() among them) the same way it
// did when this was all one translation unit.
#include "../drivers/net/wifi_prov_nvs.c"
#include "../drivers/net/wifi_prov_link.c"
#include "../drivers/net/wifi_prov_api.c"

// ---- Test scaffolding ----------------------------------------------------

// Resets every piece of state a test below might have touched, so tests
// don't leak into each other. Mirrors a fresh boot's s_wifi (all zero) plus
// the stub globals' defaults.
static void reset_state(void)
{
    memset(&s_wifi, 0, sizeof(s_wifi));
    s_wifi.sta_rssi = -127;
    // post_event()/wifi_prov_post_and_wait() both refuse (silently, by design -- see
    // wifi_prov.c's owner-task comment) when s_wifi_cmd_queue is NULL, which it is
    // until wifi_prov_start() runs (called only by
    // test_wifi_prov_start_sets_ram_storage()). Point it
    // at any non-NULL value so post_event() actually reaches xQueueSend(),
    // which is what g_stub_queue_send_calls counts -- the queue is never
    // really drained (xQueueSend always "fails" in the stub, see
    // freertos/queue.h), only whether wifi_prov.c DECIDED to post matters
    // here.
    s_wifi_cmd_queue = (QueueHandle_t)1;
    g_stub_wifi_mode = WIFI_MODE_APSTA; // "AP is up alongside the join" -- the pre-GOT_IP state
    g_stub_wifi_set_mode_calls = 0;
    g_stub_ap_info_result = ESP_OK;
    g_stub_ap_info_rssi = -50;
    g_stub_getsockname_result = 0;
    strcpy(g_stub_local_ip, "0.0.0.0");
    g_stub_queue_send_calls = 0;
    g_stub_wifi_set_storage_calls = 0;
    g_stub_wifi_last_storage = WIFI_STORAGE_FLASH;
}

// ---- Tests -----------------------------------------------------------

static void test_static_wrong_address_keeps_ap_up(void)
{
    TEST_SECTION("do_ev_got_ip -- WRONG static IP reaches CONNECTED but AP stays up (the regression)");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_STATIC;
    strcpy(s_wifi.static_ip, "192.168.1.50");
    s_wifi.static_ip_confirmed = false;
    s_wifi.state = WIFI_PROV_STATE_CONNECTING;

    do_ev_got_ip();

    TEST_CHECK(s_wifi.state == WIFI_PROV_STATE_CONNECTED,
               "join still reaches CONNECTED even though the static config is unconfirmed");
    TEST_CHECK(g_stub_wifi_set_mode_calls == 0,
               "esp_wifi_set_mode() must NOT be called -- the fallback AP is never torn down "
               "for an unconfirmed static join");
    TEST_CHECK(g_stub_wifi_mode == WIFI_MODE_APSTA, "radio mode is untouched, still APSTA (AP present)");

    // Now simulate the operator's phone hitting the STILL-UP fallback AP
    // (its own IP, not the bad static address) -- this must NOT be mistaken
    // for reachability confirmation.
    strcpy(g_stub_local_ip, "192.168.4.1"); // the AP's own well-known IP
    wifi_prov_note_possible_static_reachability(3 /* fake fd -- getsockname() is stubbed, never really used */);
    TEST_CHECK(g_stub_queue_send_calls == 0,
               "a request landing on the AP's own IP (not the static address) must not even attempt "
               "to post CMD_CONFIRM_STATIC_REACHABLE");
    TEST_CHECK(!s_wifi.static_ip_confirmed, "still unconfirmed -- nothing has proven the static config works");
}

static void test_static_correct_address_would_confirm(void)
{
    TEST_SECTION("wifi_prov_note_possible_static_reachability -- a request on the STATIC address itself posts");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_STATIC;
    strcpy(s_wifi.static_ip, "192.168.1.50");
    s_wifi.static_ip_confirmed = false;

    strcpy(g_stub_local_ip, "192.168.1.50"); // exact match
    wifi_prov_note_possible_static_reachability(3);
    TEST_CHECK(g_stub_queue_send_calls == 1,
               "a request that arrived on the configured static address does post "
               "CMD_CONFIRM_STATIC_REACHABLE");

    // Already confirmed: must not re-post (idempotent no-op, per the doc
    // comment).
    g_stub_queue_send_calls = 0;
    s_wifi.static_ip_confirmed = true;
    wifi_prov_note_possible_static_reachability(3);
    TEST_CHECK(g_stub_queue_send_calls == 0, "already-confirmed is a no-op, does not re-post");

    // DHCP mode: irrelevant, must not post either.
    g_stub_queue_send_calls = 0;
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    s_wifi.static_ip_confirmed = false;
    wifi_prov_note_possible_static_reachability(3);
    TEST_CHECK(g_stub_queue_send_calls == 0, "DHCP mode never posts -- nothing to confirm");

    // getsockname() failing: must not post.
    g_stub_queue_send_calls = 0;
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_STATIC;
    g_stub_getsockname_result = -1;
    wifi_prov_note_possible_static_reachability(3);
    TEST_CHECK(g_stub_queue_send_calls == 0, "a getsockname() failure must not post either");

    // Negative fd (as e.g. a caller with no real socket might pass): must
    // not even reach getsockname().
    g_stub_queue_send_calls = 0;
    g_stub_getsockname_result = 0;
    wifi_prov_note_possible_static_reachability(-1);
    TEST_CHECK(g_stub_queue_send_calls == 0, "a negative fd is refused before any getsockname() call");
}

static void test_confirm_static_reachable_drops_ap(void)
{
    TEST_SECTION("do_confirm_static_reachable -- confirming a static join finally drops the AP");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_STATIC;
    strcpy(s_wifi.static_ip, "192.168.1.50");
    s_wifi.static_ip_confirmed = false;
    s_wifi.state = WIFI_PROV_STATE_CONNECTED; // do_ev_got_ip() already ran, AP was left up

    do_confirm_static_reachable();

    TEST_CHECK(s_wifi.static_ip_confirmed, "static_ip_confirmed is now true");
    TEST_CHECK(g_stub_wifi_set_mode_calls == 1, "esp_wifi_set_mode() was called exactly once, to drop the AP");
    TEST_CHECK(g_stub_wifi_mode == WIFI_MODE_STA, "radio mode is now STA-only -- the AP is torn down");

    // Idempotent: calling again must not touch the radio a second time.
    do_confirm_static_reachable();
    TEST_CHECK(g_stub_wifi_set_mode_calls == 1, "a second confirmation is a harmless no-op, no extra radio call");
}

static void test_confirm_static_reachable_state_changed_since(void)
{
    TEST_SECTION("do_confirm_static_reachable -- state moved on since GOT_IP: confirm but nothing to tear down");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_STATIC;
    strcpy(s_wifi.static_ip, "192.168.1.50");
    s_wifi.static_ip_confirmed = false;
    s_wifi.state = WIFI_PROV_STATE_RECONNECTING; // link dropped again before the HTTP request arrived

    do_confirm_static_reachable();

    TEST_CHECK(s_wifi.static_ip_confirmed, "confirmation still records -- a future join won't need re-proving");
    TEST_CHECK(g_stub_wifi_set_mode_calls == 0, "nothing to tear down: state is no longer CONNECTED");
}

static void test_dhcp_got_ip_drops_ap_immediately(void)
{
    TEST_SECTION("do_ev_got_ip -- DHCP join drops the AP on GOT_IP alone, unaffected by static_ip_confirmed");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    s_wifi.static_ip_confirmed = false; // irrelevant in DHCP mode
    s_wifi.state = WIFI_PROV_STATE_CONNECTING;

    do_ev_got_ip();

    TEST_CHECK(s_wifi.state == WIFI_PROV_STATE_CONNECTED, "DHCP join reaches CONNECTED");
    TEST_CHECK(g_stub_wifi_set_mode_calls == 1, "DHCP GOT_IP drops the AP immediately, same as before the fix");
    TEST_CHECK(g_stub_wifi_mode == WIFI_MODE_STA, "radio mode is STA-only");
}

static void test_static_already_confirmed_got_ip_drops_ap(void)
{
    TEST_SECTION("do_ev_got_ip -- a static join that was ALREADY confirmed (re-join to the same good config) "
                 "drops the AP on GOT_IP without waiting for a new HTTP request");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_STATIC;
    strcpy(s_wifi.static_ip, "192.168.1.50");
    s_wifi.static_ip_confirmed = true; // proven on a previous join, e.g. after a disconnect/reconnect
    s_wifi.state = WIFI_PROV_STATE_RECONNECTING;

    do_ev_got_ip();

    TEST_CHECK(s_wifi.state == WIFI_PROV_STATE_CONNECTED, "rejoin reaches CONNECTED");
    TEST_CHECK(g_stub_wifi_set_mode_calls == 1, "an already-confirmed static config drops the AP on GOT_IP");
    TEST_CHECK(g_stub_wifi_mode == WIFI_MODE_STA, "radio mode is STA-only");
}

static void test_set_static_ip_resets_confirmation(void)
{
    TEST_SECTION("do_set_static_ip -- every (re)write resets static_ip_confirmed, never carries over a stale proof");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_STATIC;
    strcpy(s_wifi.static_ip, "192.168.1.50");
    s_wifi.static_ip_confirmed = true; // previously proven for THIS address

    // Re-typing the SAME address must still reset confirmation -- there is
    // no "unchanged, so skip the reset" special case in the production code,
    // and there shouldn't be: a re-submit could be the operator fixing a
    // netmask/gateway typo while leaving the IP itself alone.
    do_set_static_ip("192.168.1.50", "255.255.255.0", "192.168.1.1");
    TEST_CHECK(!s_wifi.static_ip_confirmed, "re-writing the static config resets confirmation, even to the same IP");

    s_wifi.static_ip_confirmed = true;
    do_set_static_ip("10.0.0.5", "255.255.255.0", "10.0.0.1");
    TEST_CHECK(!s_wifi.static_ip_confirmed, "writing a genuinely different static config resets confirmation");
    TEST_CHECK(strcmp(s_wifi.static_ip, "10.0.0.5") == 0, "the new address is stored");
}

static void test_set_dhcp_resets_confirmation(void)
{
    TEST_SECTION("do_set_dhcp -- switching back to DHCP resets static_ip_confirmed (hygiene, per the doc comment)");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_STATIC;
    strcpy(s_wifi.static_ip, "192.168.1.50");
    s_wifi.static_ip_confirmed = true;

    do_set_dhcp();

    TEST_CHECK(s_wifi.ip_mode == WIFI_PROV_IP_MODE_DHCP, "mode is now DHCP");
    TEST_CHECK(!s_wifi.static_ip_confirmed, "confirmation is reset -- a later switch back to STATIC starts unproven");
    TEST_CHECK(s_wifi.static_ip[0] == '\0', "the stale static IP string is cleared");
}

static void test_static_ip_confirmed_false_at_boot(void)
{
    TEST_SECTION("s_wifi.static_ip_confirmed -- false at boot (zero-initialized), same as a fresh reset_state()");

    memset(&s_wifi, 0, sizeof(s_wifi));
    TEST_CHECK(!s_wifi.static_ip_confirmed, "a zero-initialized s_wifi (boot state) has static_ip_confirmed == false");
}

static void test_wifi_prov_start_sets_ram_storage(void)
{
    TEST_SECTION("wifi_prov_start -- switches the IDF Wi-Fi driver to RAM storage "
                 "(docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md) so the driver "
                 "never keeps its own flash copy of STA/AP config");

    reset_state();

    esp_err_t err = wifi_prov_start();

    TEST_CHECK(err == ESP_OK, "wifi_prov_start() succeeds against the host stubs");
    TEST_CHECK(g_stub_wifi_set_storage_calls >= 1,
               "esp_wifi_set_storage() was called at least once during start-up");
    TEST_CHECK(g_stub_wifi_last_storage == WIFI_STORAGE_RAM,
               "the last esp_wifi_set_storage() call requested WIFI_STORAGE_RAM, not the flash-backed default");
}

void run_test_wifi_prov(void)
{
    test_static_ip_confirmed_false_at_boot();
    test_wifi_prov_start_sets_ram_storage();
    test_static_wrong_address_keeps_ap_up();
    test_static_correct_address_would_confirm();
    test_confirm_static_reachable_drops_ap();
    test_confirm_static_reachable_state_changed_since();
    test_dhcp_got_ip_drops_ap_immediately();
    test_static_already_confirmed_got_ip_drops_ap();
    test_set_static_ip_resets_confirmation();
    test_set_dhcp_resets_confirmation();
}
