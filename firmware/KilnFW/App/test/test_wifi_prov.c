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
#include "hal_time.h"
#include <stdbool.h>
#include <string.h>

#include "test_common.h"

// Needed here (ahead of wifi_prov.c's own #includes below) purely for the
// TYPES of the stub globals this file defines -- esp_wifi.h/esp_err.h/
// esp_http_server.h are otherwise only reached transitively through
// wifi_prov.c's own #include list.
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "lwip/sockets.h" // AF_INET6, for g_stub_getsockname_family's initializer below

// ---- Stub globals wifi_prov.c's stand-in headers read/write ----
// (extern-declared in the stub headers themselves; defined once here.)
wifi_mode_t g_stub_wifi_mode = WIFI_MODE_NULL;
int g_stub_wifi_set_mode_calls = 0;
// esp_wifi_set_storage()/esp_wifi_restore() counters (added 2026-09-21,
// docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md) -- wifi_prov.c
// now calls esp_wifi_set_storage(RAM) once during wifi_prov_start();
// test_wifi_prov_start_sets_ram_storage() below asserts on both counters.
int g_stub_wifi_set_storage_calls = 0;
// esp_wifi_connect() counter (2026-09-25 review fix) -- proves do_add_network()/
// do_set_mode() defer the actual join to start_sta_join() (run by owner_task()
// AFTER the reply) rather than connecting synchronously inside the command body.
int g_stub_wifi_connect_calls = 0;
wifi_storage_t g_stub_wifi_last_storage = WIFI_STORAGE_FLASH;
int g_stub_wifi_restore_calls = 0;
esp_err_t g_stub_ap_info_result = ESP_OK;
int8_t g_stub_ap_info_rssi = -50;
int g_stub_getsockname_result = 0;
// 2026-09-29 review-fix round 2: widened 16 -> 48 (>= INET6_ADDRSTRLEN, 46)
// -- an IPv4-mapped AF_INET6 string like "::ffff:192.168.4.1" is 19 chars +
// NUL, which silently overran the old 16-byte buffer (MSVC's own "array
// bounds overflow" warning on the strcpy() call sites caught this).
char g_stub_local_ip[48] = "0.0.0.0";
// 2026-09-29, see stubs/lwip/sockets.h's own comment: the address family
// getsockname() reports, needed now that wifi_prov_request_arrived_on_ap()
// branches on plain AF_INET vs. IPv4-mapped AF_INET6.
int g_stub_getsockname_family = AF_INET6;
int g_stub_queue_send_calls = 0;
unsigned char g_stub_last_queue_item[256];
// esp_wifi_ap_get_sta_list()'s reported station count (2026-09-28) -- drives
// wifi_prov_get_ap_client_count(), which ap_teardown_should_defer() (wifi_prov_link.c)
// consults when web auth is off. See stubs/esp_wifi.h.
int g_stub_ap_sta_count = 0;
// esp_netif_get_ip_info()'s reported IP address (2026-09-28, see
// stubs/esp_netif.h's comment) -- controllable so sta_link_is_live()/
// reconcile_sta_state() can be tested with a station that genuinely holds a
// lease, not just "associated." Any non-zero value stands in for a real IP.
uint32_t g_stub_netif_ip_addr = 0;
uint32_t g_stub_dns_main = 0;
uint32_t g_stub_dns_backup = 0;
int g_stub_dns_set_calls = 0;
int g_stub_lwip_backup_clears = 0;
extern void fake_kv_reset_all(void);

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

// http_auth_policy_web_enabled()/http_auth_any_ap_session_active() are
// declared by the real http_auth_policy_iface.h/http_session_iface.h
// (off-limits -- their real bodies pull in PSA crypto/hal_time/web_auth_store,
// none of which this narrow host-test build links) and called from
// wifi_prov_link.c's ap_teardown_should_defer() (2026-09-28 owner request:
// "reconnect to wifi ... when there are no users logged in to the website";
// 2026-09-29 owner follow-up: narrowed to "a session that arrived through the
// AP", not any admin session -- see ap_teardown_should_defer()'s own header
// comment). Fakes here are directly controllable (unlike
// time_sync_notify_got_ip() above, which only counts) so tests can drive
// every branch of ap_teardown_should_defer().
bool g_stub_web_auth_enabled = false;
bool http_auth_policy_web_enabled(void) { return g_stub_web_auth_enabled; }
// 2026-09-29: the production signal ap_teardown_should_defer() consults is
// the AP-scoped one (a LAN-only session must never defer by itself; see the
// header comment above).
bool g_stub_any_ap_session_active = false;
// (void)ap_station_present -- the WEB_AUTH_TIMEOUT_NEVER_S handling that
// parameter drives is exercised at the lower layer, test_http_session_iface.c's
// own tests against the real http_auth_any_ap_session_active(); this fake
// only needs to satisfy the (now two-argument) call site.
bool http_auth_any_ap_session_active(bool ap_station_present) {
    (void)ap_station_present;
    return g_stub_any_ap_session_active;
}
// Review fix (2026-09-29, round 2): restored as a *fake*, not dropped --
// the earlier version of this comment argued nothing calls
// http_auth_any_session_active() from this file's production call graph any
// more, so leaving it unfaked was fine. That made the LAN-only regression
// test below vacuous: reverting ap_teardown_should_defer() to the old,
// broader call would only produce a LINK ERROR against a host-test binary
// that no longer defines this symbol, never a real, specific test FAILURE --
// exactly the mistake the first negative-test attempt for this bug caught
// and abandoned (see this file's own history) but which the test itself
// didn't actually guard against. Restoring the fake, and setting it TRUE
// (with g_stub_any_ap_session_active FALSE and zero AP clients) in
// test_got_ip_drops_ap_for_lan_only_session_even_with_auth_on() below,
// means a revert to the old call now fails that test's own assertions
// (the old code would defer because *some* session is active) rather than
// merely failing to link -- confirmed by deliberately reverting the
// production call, rebuilding, and seeing that specific test fail (then
// restoring by hand and rebuilding clean again).
bool g_stub_any_session_active = false;
bool http_auth_any_session_active(void) { return g_stub_any_session_active; }

// stack_margin_register() is declared by the real stack_margin.h (safe to
// include -- that header is deliberately FreeRTOS-free, see its own top
// comment) but its .c is NOT host-tested (needs uxTaskGetStackHighWaterMark());
// wifi_prov_start() now calls it once at task-creation time (2026-09-28,
// registering wifi_prov_owner for stack-margin reporting). This fake just
// counts calls and records the last name/size, same "count it, don't
// simulate it" shape as time_sync_notify_got_ip() above.
#include "../drivers/common/stack_margin.h"
int g_stub_stack_margin_register_calls = 0;
char g_stub_stack_margin_register_last_name[32] = {0};
uint32_t g_stub_stack_margin_register_last_bytes = 0;
bool stack_margin_register(const char *name, void *task_handle_slot, uint32_t configured_stack_bytes)
{
    (void)task_handle_slot;
    g_stub_stack_margin_register_calls++;
    if (name) {
        strncpy(g_stub_stack_margin_register_last_name, name,
                sizeof(g_stub_stack_margin_register_last_name) - 1);
    }
    g_stub_stack_margin_register_last_bytes = configured_stack_bytes;
    return true;
}

// ---- R2-D (HOST_TEST_COVERAGE_GAPS round 2): controllable scan / netif fakes ----
// The shared stubs/esp_wifi.h and stubs/esp_netif.h scan and IP stubs always report
// zero access points and an empty dotted quad. Rather than edit those shared stubs
// (other host executables include them), the call sites inside the #included driver
// sources are redirected here with function-like macros, defined AFTER the stub
// headers and BEFORE wifi_prov.c. Defaults reproduce the old stub behaviour.
#include "esp_netif.h"
static uint16_t g_r2d_scan_n = 0;
static wifi_ap_record_t g_r2d_scan_recs[25];
static esp_err_t g_r2d_scan_start_rc = ESP_OK;
static esp_err_t g_r2d_scan_recs_rc = ESP_OK;
static int g_r2d_scan_start_calls = 0;
static uint16_t g_r2d_last_fetch_request = 0;
static uint32_t g_r2d_netmask = 0;
static esp_err_t r2d_scan_start(const wifi_scan_config_t *cfg, int block)
{
    (void)cfg; (void)block;
    g_r2d_scan_start_calls++;
    return g_r2d_scan_start_rc;
}
static esp_err_t r2d_scan_get_ap_num(uint16_t *num)
{
    if (num) *num = g_r2d_scan_n;
    return ESP_OK;
}
static esp_err_t r2d_scan_get_ap_records(uint16_t *num, wifi_ap_record_t *records)
{
    g_r2d_last_fetch_request = *num;
    if (g_r2d_scan_recs_rc != ESP_OK) return g_r2d_scan_recs_rc;
    uint16_t n = *num < g_r2d_scan_n ? *num : g_r2d_scan_n;
    for (uint16_t i = 0; i < n; i++) records[i] = g_r2d_scan_recs[i];
    *num = n;
    return ESP_OK;
}
static esp_err_t r2d_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info)
{
    (void)netif;
    info->ip.addr = g_stub_netif_ip_addr;
    info->netmask.addr = g_r2d_netmask;
    info->gw.addr = 0;
    return ESP_OK;
}
static char *r2d_ip4_ntoa(const esp_ip4_addr_t *addr, char *buf, uint32_t buflen)
{
    if (!buf || buflen == 0) return buf;
    if (addr->addr == 0) { buf[0] = '\0'; return buf; }
    snprintf(buf, buflen, "%u.%u.%u.%u", (unsigned)(addr->addr & 0xFFu), (unsigned)((addr->addr >> 8) & 0xFFu),
             (unsigned)((addr->addr >> 16) & 0xFFu), (unsigned)((addr->addr >> 24) & 0xFFu));
    return buf;
}
#define esp_wifi_scan_start(c, b) r2d_scan_start((c), (b))
#define esp_wifi_scan_get_ap_num(n) r2d_scan_get_ap_num(n)
#define esp_wifi_scan_get_ap_records(n, r) r2d_scan_get_ap_records((n), (r))
#define esp_netif_get_ip_info(n, i) r2d_get_ip_info((n), (i))
#define esp_ip4addr_ntoa(a, b, l) r2d_ip4_ntoa((a), (b), (l))

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
#include "../drivers/persist/legacy_default_nvs.h"

// Ring-mode backing storage for stubs/freertos/queue.h (see that file's
// 2026-08-24 comments) -- defined here, after the includes above, because
// TEST_STUB_QUEUE_RING_MAX_CAPACITY is only visible once freertos/queue.h has
// been pulled in transitively by wifi_prov.c's own #include chain. This file
// never enables ring mode (g_stub_queue_ring_enabled stays 0, same original
// always-pdFALSE semantics), so these definitions exist only to satisfy the
// linker now that this file builds as its own executable (2026-09-28) rather
// than sharing test_uart_log_bridge.c's definitions of the same globals.
int g_stub_queue_ring_enabled = 0;
unsigned char g_stub_queue_ring[TEST_STUB_QUEUE_RING_MAX_CAPACITY][256];
unsigned long g_stub_queue_ring_item_len[TEST_STUB_QUEUE_RING_MAX_CAPACITY];
int g_stub_queue_ring_capacity = 0;
int g_stub_queue_ring_count = 0;
int g_stub_queue_ring_head = 0;

// ---- Test scaffolding ----------------------------------------------------

// Resets every piece of state a test below might have touched, so tests
// don't leak into each other. Mirrors a fresh boot's s_wifi (all zero) plus
// the stub globals' defaults.
static void reset_state(void)
{
    memset(&s_wifi, 0, sizeof(s_wifi));
    s_saved_nets_refused = false;
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
    // 2026-09-28 ap_pending_teardown additions -- default to "nobody logged
    // in, no AP client", i.e. teardown never deferred, matching every
    // pre-existing test above that expects an immediate teardown.
    g_stub_web_auth_enabled = false;
    g_stub_any_ap_session_active = false;
    g_stub_any_session_active = false;
    g_stub_ap_sta_count = 0;
    g_stub_dns_main = g_stub_dns_backup = 0;
    g_stub_dns_set_calls = 0;
    g_stub_lwip_backup_clears = 0;
    g_stub_netif_ip_addr = 0; // no lease by default -- sta_link_is_live() reads false unless a test opts in
    g_stub_getsockname_family = AF_INET6; // matches this board's CONFIG_LWIP_IPV6=y shape by default
}

// ---- Tests -----------------------------------------------------------

static void test_static_wrong_address_keeps_ap_up(void)
{
    TEST_SECTION("do_ev_got_ip -- WRONG static IP reaches CONNECTED but AP stays up (the regression)");

    reset_state();
    // Plain AF_INET shape -- see get_local_ipv4_string()'s two branches in
    // wifi_prov_link.c. The mapped-AF_INET6 shape is covered separately by
    // test_static_reachability_v4_mapped_af_inet6_*() below.
    g_stub_getsockname_family = AF_INET;
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
    // Plain AF_INET shape here too -- see the sibling test above.
    g_stub_getsockname_family = AF_INET;
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
    do_set_static_ip("192.168.1.50", "255.255.255.0", "192.168.1.1", NULL, NULL);
    TEST_CHECK(!s_wifi.static_ip_confirmed, "re-writing the static config resets confirmation, even to the same IP");

    s_wifi.static_ip_confirmed = true;
    do_set_static_ip("10.0.0.5", "255.255.255.0", "10.0.0.1", NULL, NULL);
    TEST_CHECK(!s_wifi.static_ip_confirmed, "writing a genuinely different static config resets confirmation");
    TEST_CHECK(strcmp(s_wifi.static_ip, "10.0.0.5") == 0, "the new address is stored");
}

static void test_set_static_ip_rejects_ap_subnet(void)
{
    TEST_SECTION("wifi_prov_set_static_ip() -- refuses an address inside the fallback AP's own 192.168.4.0/24 subnet");

    reset_state();
    s_wifi.started = true;

    esp_err_t err = wifi_prov_set_static_ip("192.168.4.1", "255.255.255.0", "192.168.4.1", NULL, NULL);
    TEST_CHECK(err == ESP_ERR_INVALID_ARG, "the AP's own address is refused");
    TEST_CHECK(s_wifi.ip_mode != WIFI_PROV_IP_MODE_STATIC, "refused request never applies");

    err = wifi_prov_set_static_ip("192.168.4.200", "255.255.255.0", "192.168.4.1", NULL, NULL);
    TEST_CHECK(err == ESP_ERR_INVALID_ARG, "any other address in the AP subnet is refused too, not just .1");

    // A genuinely different subnet must still pass validation and reach the
    // post -- this is not a blanket refusal of the STATIC feature. The stub
    // xQueueSend() always "fails" (see this file's top-of-file comment), so
    // the real owner_task() dispatch never runs here and the call times out
    // rather than returning ESP_OK; ESP_ERR_TIMEOUT (not ESP_ERR_INVALID_ARG)
    // is exactly the signal that validation was passed.
    err = wifi_prov_set_static_ip("192.168.1.50", "255.255.255.0", "192.168.1.1", NULL, NULL);
    TEST_CHECK(err == ESP_ERR_TIMEOUT, "an address outside 192.168.4.0/24 passes validation (times out on the stubbed queue, not refused)");
}

// ---- static-IP DNS fields (ROADMAP M18) ----
static void test_set_static_ip_dns_validation(void)
{
    TEST_SECTION("wifi_prov_set_static_ip() -- optional dns/dns2: malformed/0.0.0.0/orphan dns2 refused, unset or valid accepted");

    reset_state();
    s_wifi.started = true;
    const char *ip = "192.168.1.50", *nm = "255.255.255.0", *gw = "192.168.1.1";

    TEST_CHECK(wifi_prov_set_static_ip(ip, nm, gw, "not-an-ip", NULL) == ESP_ERR_INVALID_ARG, "malformed dns refused");
    TEST_CHECK(wifi_prov_set_static_ip(ip, nm, gw, "1.1.1", NULL) == ESP_ERR_INVALID_ARG, "3-octet dns refused");
    TEST_CHECK(wifi_prov_set_static_ip(ip, nm, gw, "256.1.1.1", NULL) == ESP_ERR_INVALID_ARG, "octet > 255 refused");
    TEST_CHECK(wifi_prov_set_static_ip(ip, nm, gw, "0.0.0.0", NULL) == ESP_ERR_INVALID_ARG, "0.0.0.0 dns refused");
    TEST_CHECK(wifi_prov_set_static_ip(ip, nm, gw, "1.1.1.1", "bogus") == ESP_ERR_INVALID_ARG, "malformed dns2 refused");
    TEST_CHECK(wifi_prov_set_static_ip(ip, nm, gw, NULL, "8.8.8.8") == ESP_ERR_INVALID_ARG, "dns2 without dns refused (NULL)");
    TEST_CHECK(wifi_prov_set_static_ip(ip, nm, gw, "", "8.8.8.8") == ESP_ERR_INVALID_ARG, "dns2 without dns refused (empty)");
    TEST_CHECK(g_stub_queue_send_calls == 0, "refused requests post nothing");

    TEST_CHECK(wifi_prov_set_static_ip(ip, nm, gw, NULL, NULL) == ESP_ERR_TIMEOUT, "no dns passes validation");
    TEST_CHECK(wifi_prov_set_static_ip(ip, nm, gw, "", "") == ESP_ERR_TIMEOUT, "empty dns/dns2 pass validation");
    TEST_CHECK(wifi_prov_set_static_ip(ip, nm, gw, "1.1.1.1", NULL) == ESP_ERR_TIMEOUT, "dns alone passes validation");
    TEST_CHECK(wifi_prov_set_static_ip(ip, nm, gw, "1.1.1.1", "8.8.8.8") == ESP_ERR_TIMEOUT, "dns + dns2 pass validation");
}

static void test_set_static_ip_stores_and_clears_dns(void)
{
    TEST_SECTION("do_set_static_ip/do_set_dhcp -- dns/dns2 stored, overwritten (not merged) on re-post, cleared by DHCP");

    reset_state();
    do_set_static_ip("192.168.1.50", "255.255.255.0", "192.168.1.1", "1.1.1.1", "8.8.8.8");
    TEST_CHECK(strcmp(wifi_prov_get_static_dns(), "1.1.1.1") == 0, "dns stored");
    TEST_CHECK(strcmp(wifi_prov_get_static_dns2(), "8.8.8.8") == 0, "dns2 stored");

    do_set_static_ip("192.168.1.50", "255.255.255.0", "192.168.1.1", NULL, NULL);
    TEST_CHECK(wifi_prov_get_static_dns()[0] == '\0' && wifi_prov_get_static_dns2()[0] == '\0',
               "re-posting without dns clears both");

    do_set_static_ip("192.168.1.50", "255.255.255.0", "192.168.1.1", "9.9.9.9", NULL);
    do_set_dhcp();
    TEST_CHECK(wifi_prov_get_static_dns()[0] == '\0' && wifi_prov_get_static_dns2()[0] == '\0', "DHCP clears dns/dns2");
}

static void test_dns_nvs_round_trip(void)
{
    TEST_SECTION("static_dns/static_dns2 -- NVS round trip; a board that predates the keys loads empty (no version bump)");

    reset_state();
    fake_kv_reset_all();
    TEST_CHECK(hal_kv_init_partition(WIFI_NVS_PARTITION) == HAL_OK, "setup: init wifi_nvs partition");
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_STATIC;
    strcpy(s_wifi.static_ip, "192.168.1.50");
    strcpy(s_wifi.static_netmask, "255.255.255.0");
    strcpy(s_wifi.static_gateway, "192.168.1.1");
    strcpy(s_wifi.static_dns, "1.1.1.1");
    strcpy(s_wifi.static_dns2, "8.8.8.8");
    TEST_CHECK(nvs_save_ip_config() == ESP_OK, "save succeeds");

    memset(s_wifi.static_dns, 0, sizeof(s_wifi.static_dns));
    memset(s_wifi.static_dns2, 0, sizeof(s_wifi.static_dns2));
    memset(s_wifi.static_gateway, 0, sizeof(s_wifi.static_gateway));
    bool found = false;
    TEST_CHECK(wifi_prov_nvs_load_from(WIFI_NVS_PARTITION, &found) == ESP_OK, "load succeeds");
    TEST_CHECK(strcmp(s_wifi.static_dns, "1.1.1.1") == 0, "dns round-trips");
    TEST_CHECK(strcmp(s_wifi.static_dns2, "8.8.8.8") == 0, "dns2 round-trips");
    TEST_CHECK(strcmp(s_wifi.static_gateway, "192.168.1.1") == 0, "existing gateway key still round-trips beside them");

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, WIFI_NVS_PARTITION) == HAL_OK, "open for key erase");
    hal_kv_erase_key(&h, NVS_KEY_STATIC_DNS);
    hal_kv_erase_key(&h, NVS_KEY_STATIC_DNS2);
    hal_kv_commit(&h);
    hal_kv_close(&h);
    strcpy(s_wifi.static_dns, "stale");
    strcpy(s_wifi.static_dns2, "stale");
    TEST_CHECK(wifi_prov_nvs_load_from(WIFI_NVS_PARTITION, &found) == ESP_OK, "load of a pre-feature record succeeds");
    TEST_CHECK(s_wifi.static_dns[0] == '\0' && s_wifi.static_dns2[0] == '\0', "absent keys load as empty, not stale");
    TEST_CHECK(strcmp(s_wifi.static_gateway, "192.168.1.1") == 0 && s_wifi.ip_mode == WIFI_PROV_IP_MODE_STATIC,
               "the rest of the static config is unaffected");
}

// Seeds a pre-split board: the default partition's "wifi_cfg" namespace holds a
// provisioned legacy single-network credential and a mode flag; wifi_nvs is
// empty (as it is right after a wifi factory reset erased the partition).
static void seed_legacy_default_wifi(void)
{
    reset_state();
    memset(&s_legacy_single, 0, sizeof(s_legacy_single));
    fake_kv_reset_all();
    TEST_CHECK(hal_kv_init_partition(WIFI_NVS_PARTITION) == HAL_OK, "setup: init wifi_nvs");
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default partition");
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, NULL) == HAL_OK, "setup: open legacy wifi_cfg");
    TEST_CHECK(hal_kv_set_str(&h, NVS_KEY_SSID, "oldnet") == HAL_OK, "setup: legacy ssid");
    TEST_CHECK(hal_kv_set_str(&h, NVS_KEY_PASS, "oldpass") == HAL_OK, "setup: legacy pass");
    TEST_CHECK(hal_kv_set_u8(&h, NVS_KEY_HAS_CREDS, 1) == HAL_OK, "setup: legacy has_creds");
    TEST_CHECK(hal_kv_set_u8(&h, NVS_KEY_MODE, 0) == HAL_OK, "setup: legacy mode");
    TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "setup: commit");
    hal_kv_close(&h);
}

static void boot_wifi_migration(void)
{
    bool found = false;
    memset(&s_legacy_single, 0, sizeof(s_legacy_single));
    TEST_CHECK(wifi_prov_nvs_load_from(WIFI_NVS_PARTITION, &found) == ESP_OK, "boot: wifi_nvs load");
    wifi_prov_migrate_from_default_partition(found);
    nvs_load_saved_nets();
}

static void test_legacy_default_wifi_erase_keeps_board_unprovisioned(void)
{
    TEST_SECTION("wifi factory reset -- legacy default-partition wifi_cfg erase: the migration finds nothing "
                 "and the board stays unprovisioned (DEV_FIRMWARE_REVIEW_2 finding 1)");
    seed_legacy_default_wifi();
    boot_wifi_migration();
    TEST_CHECK(s_wifi.saved_nets.count == 1, "control: without the erase the old credentials are adopted");

    seed_legacy_default_wifi();
    TEST_CHECK(legacy_default_nvs_erase_wifi() == ESP_OK, "legacy wifi erase succeeds");
    boot_wifi_migration();
    TEST_CHECK(s_wifi.saved_nets.count == 0, "after the erase no saved network comes back");
    TEST_CHECK(!s_legacy_single.has, "after the erase no legacy single credential is found");
    s_wifi.state = WIFI_PROV_STATE_UNPROVISIONED;
    TEST_CHECK(wifi_prov_is_unprovisioned(), "board stays unprovisioned");

    /* Every key the wifi code knows must be erased: seed all of them. */
    seed_legacy_default_wifi();
    static const char *const all_keys[] = WIFI_PROV_NVS_ALL_KEYS_INIT;
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, NULL) == HAL_OK, "open");
    for (size_t i = 0; i < sizeof(all_keys) / sizeof(all_keys[0]); i++) {
        TEST_CHECK(hal_kv_set_blob(&h, all_keys[i], "x", 1) == HAL_OK, "seed every wifi key");
    }
    hal_kv_commit(&h);
    hal_kv_close(&h);
    TEST_CHECK(legacy_default_nvs_erase_wifi() == ESP_OK, "erase succeeds");
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, NULL) == HAL_OK, "reopen");
    for (size_t i = 0; i < sizeof(all_keys) / sizeof(all_keys[0]); i++) {
        char b[4];
        size_t l = sizeof(b);
        TEST_CHECK(hal_kv_get_blob(&h, all_keys[i], b, &l) == HAL_NOT_FOUND, "every wifi key is gone");
    }
    hal_kv_close(&h);
}

// F2 (REVIEW_RECOVERY_FIX2): recovery's nvs_erase_all leaves an EMPTY wifi_cfg
// namespace in the default partition. The migration must not count it as found
// (it would adopt an all-default legacy config over the wifi_nvs AP identity).
static void test_empty_legacy_namespace_is_not_found(void)
{
    TEST_SECTION("empty default-partition wifi_cfg namespace is not 'found' (REVIEW_RECOVERY_FIX2 F2)");
    seed_legacy_default_wifi();
    bool found = false;
    TEST_CHECK(wifi_prov_nvs_load_from(NULL, &found) == ESP_OK && found, "control: a namespace holding keys is found");

    TEST_CHECK(legacy_default_nvs_erase_wifi() == ESP_OK, "erase every key; the namespace itself remains");
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, NULL) == HAL_OK,
               "setup: the emptied namespace still opens (as after nvs_erase_all)");
    hal_kv_close(&h);
    found = true;
    TEST_CHECK(wifi_prov_nvs_load_from(NULL, &found) == ESP_OK, "load of the emptied namespace succeeds");
    TEST_CHECK(!found, "an emptied namespace is not found");

    /* End to end: a custom AP identity already in wifi_nvs survives the boot. */
    seed_legacy_default_wifi();
    TEST_CHECK(legacy_default_nvs_erase_wifi() == ESP_OK, "erase legacy keys");
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, WIFI_NVS_PARTITION) == HAL_OK, "open wifi_nvs");
    TEST_CHECK(hal_kv_set_str(&h, NVS_KEY_AP_SSID, "myap") == HAL_OK, "seed AP ssid override");
    TEST_CHECK(hal_kv_set_u8(&h, NVS_KEY_HAS_AP_SSID, 1) == HAL_OK, "seed has_ap_ssid");
    TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "commit");
    hal_kv_close(&h);
    boot_wifi_migration();
    TEST_CHECK(s_wifi.has_ap_ssid_override && strcmp(s_wifi.ap_ssid, "myap") == 0,
               "AP identity override survives an emptied legacy namespace");
}

static bool legacy_wifi_key_present(const char *key)
{
    hal_kv_handle_t h;
    if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, NULL) != HAL_OK) {
        return false;
    }
    bool present = hal_kv_key_exists(&h, key) == HAL_OK;
    hal_kv_close(&h);
    return present;
}

// A reboot: RAM state is lost, both NVS partitions persist.
static void simulate_reboot_state(void)
{
    reset_state();
    memset(&s_legacy_single, 0, sizeof(s_legacy_single));
}

// N1 (REVIEW_RECOVERY_FIX3): on target a typed getter returns NOT_FOUND for a key of
// another type, so a namespace holding only u8/blob keys must still count as saved
// data on the MAIN wifi_nvs load. fake_kv now models that NOT_FOUND.
static void test_main_load_keeps_ap_mode_saved_as_u8_only(void)
{
    TEST_SECTION("wifi_nvs holding only mode=AP (u8, no string keys) survives a reload "
                 "(REVIEW_RECOVERY_FIX3 N1)");
    seed_legacy_default_wifi();
    TEST_CHECK(legacy_default_nvs_erase_wifi() == ESP_OK, "legacy namespace emptied");
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, WIFI_NVS_PARTITION) == HAL_OK, "open wifi_nvs");
    TEST_CHECK(hal_kv_set_u8(&h, NVS_KEY_MODE, 1) == HAL_OK, "seed mode=AP as a u8 only");
    TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "commit");
    char b[4];
    size_t l = sizeof(b);
    TEST_CHECK(hal_kv_get_str(&h, NVS_KEY_MODE, b, &l) == HAL_NOT_FOUND,
               "model check: a string read of a u8 key is NOT_FOUND, as on target");
    hal_kv_close(&h);
    simulate_reboot_state();
    bool found = false;
    TEST_CHECK(wifi_prov_nvs_load_from(WIFI_NVS_PARTITION, &found) == ESP_OK, "reload succeeds");
    TEST_CHECK(found, "a u8-only namespace counts as found");
    TEST_CHECK(s_wifi.mode == WIFI_PROV_MODE_AP, "AP mode survives the reload");
    simulate_reboot_state();
    boot_wifi_migration();
    TEST_CHECK(s_wifi.mode == WIFI_PROV_MODE_AP, "AP mode survives the full boot sequence");
}

static void test_legacy_wifi_migration_is_one_shot(void)
{
    TEST_SECTION("legacy wifi_cfg migration is one-shot: legacy keys are erased after a verified copy and "
                 "a second boot does not re-adopt (DEV_FIRMWARE_REVIEW_5 M2)");
    seed_legacy_default_wifi();
    boot_wifi_migration();
    TEST_CHECK(s_wifi.saved_nets.count == 1 && strcmp(s_wifi.saved_nets.nets[0].ssid, "oldnet") == 0,
               "first boot adopts the legacy network");
    TEST_CHECK(!legacy_wifi_key_present(NVS_KEY_SSID) && !legacy_wifi_key_present(NVS_KEY_HAS_CREDS) &&
                   !legacy_wifi_key_present(NVS_KEY_MODE),
               "legacy keys are gone after the verified migration");
    simulate_reboot_state();
    boot_wifi_migration();
    TEST_CHECK(s_wifi.saved_nets.count == 1, "second boot keeps the migrated list");
    TEST_CHECK(!s_legacy_single.has, "second boot finds no legacy credential to adopt");
}

static void test_interrupted_first_migration_retries_next_boot(void)
{
    TEST_SECTION("a first migration that dies after the mode write (wifi_nvs namespace exists, no saved_nets) "
                 "keeps the legacy credential and migrates it next boot (DEV_FIRMWARE_REVIEW_6 M1)");
    seed_legacy_default_wifi();
    /* Boot 1, interrupted: the migrate step ran (mode written, namespace created)
     * but nvs_load_saved_nets() never persisted the list (power loss / save failure). */
    bool found = false;
    memset(&s_legacy_single, 0, sizeof(s_legacy_single));
    TEST_CHECK(wifi_prov_nvs_load_from(WIFI_NVS_PARTITION, &found) == ESP_OK, "boot1: load");
    wifi_prov_migrate_from_default_partition(found);
    s_legacy_erase_pending = false;
    TEST_CHECK(legacy_wifi_key_present(NVS_KEY_SSID), "boot1: legacy ssid still present");
    simulate_reboot_state();
    /* Boot 2: wifi_nvs now has the namespace + mode key but no saved_nets record. */
    boot_wifi_migration();
    TEST_CHECK(s_wifi.saved_nets.count == 1 && strcmp(s_wifi.saved_nets.nets[0].ssid, "oldnet") == 0,
               "boot2: the legacy credential is still migrated");
    TEST_CHECK(!legacy_wifi_key_present(NVS_KEY_SSID), "boot2: legacy erased only after the verified copy");
    simulate_reboot_state();
    boot_wifi_migration();
    TEST_CHECK(s_wifi.saved_nets.count == 1, "boot3: migrated list persists");
}

extern void fake_kv_script_write_status_after(unsigned skip, hal_status_t status);
extern void fake_kv_script_silent_set_noops(unsigned count);

static bool saved_net_oldnet_persisted(void)
{
    saved_nets_blob_t rb;
    return nvs_load_saved_nets_from(WIFI_NVS_PARTITION, &rb) == ESP_OK && rb.count == 1 &&
           strcmp(rb.nets[0].ssid, "oldnet") == 0 && strcmp(rb.nets[0].password, "oldpass") == 0;
}

static void test_legacy_migration_failures_never_lose_credential(void)
{
    TEST_SECTION("legacy wifi migration with an injected write failure at every write-class call (save fail, "
                 "legacy erase fail): the credential is never lost and the next clean boot completes it "
                 "(HOST_TEST_GAP_AUDIT gap 4)");
    int kept_legacy = 0, erase_failed_but_persisted = 0;
    for (unsigned skip = 0; skip < 40; skip++) {
        seed_legacy_default_wifi();
        fake_kv_script_write_status_after(skip, HAL_NO_MEM);
        bool found = false;
        memset(&s_legacy_single, 0, sizeof(s_legacy_single));
        TEST_CHECK(wifi_prov_nvs_load_from(WIFI_NVS_PARTITION, &found) == ESP_OK, "faulted boot: load");
        wifi_prov_migrate_from_default_partition(found);
        nvs_load_saved_nets();
        bool legacy_left = legacy_wifi_key_present(NVS_KEY_SSID);
        bool persisted = saved_net_oldnet_persisted();
        TEST_CHECK(legacy_left || persisted, "credential exists in the legacy copy or the new list (never neither)");
        TEST_CHECK(s_wifi.saved_nets.count == 1 || legacy_left,
                   "RAM list holds the network unless the legacy copy still does");
        if (legacy_left) kept_legacy++;
        if (legacy_left && persisted) erase_failed_but_persisted++;
        simulate_reboot_state(); /* clears the one-shot armed fault only if it never fired; re-seed none */
        fake_kv_script_write_status_after(0, HAL_OK);
        boot_wifi_migration();
        TEST_CHECK(s_wifi.saved_nets.count == 1 && strcmp(s_wifi.saved_nets.nets[0].ssid, "oldnet") == 0,
                   "clean reboot: network present");
        TEST_CHECK(saved_net_oldnet_persisted(), "clean reboot: network persisted in the new list");
        simulate_reboot_state();
        boot_wifi_migration();
        TEST_CHECK(!legacy_wifi_key_present(NVS_KEY_SSID), "after clean boots the legacy copy is erased");
    }
    TEST_CHECK(kept_legacy > 0, "some injected failure left the legacy copy in place (failure branch reached)");
    TEST_CHECK(erase_failed_but_persisted > 0, "some injected failure hit the legacy erase after a verified copy");

    /* Lossy write: set calls silently do nothing, so the read-back must differ and keep the legacy copy. */
    for (unsigned n = 1; n < 8; n++) {
        seed_legacy_default_wifi();
        fake_kv_script_silent_set_noops(n);
        boot_wifi_migration();
        bool legacy_left = legacy_wifi_key_present(NVS_KEY_SSID);
        TEST_CHECK(legacy_left || saved_net_oldnet_persisted(), "lossy write: credential never lost");
        simulate_reboot_state();
        fake_kv_script_silent_set_noops(0);
        boot_wifi_migration();
        TEST_CHECK(saved_net_oldnet_persisted(), "lossy write: next clean boot completes the migration");
    }
}

static void test_embedded_nul_refused(void)
{
    TEST_SECTION("embedded NUL in ssid/password is refused, not truncated (audit L15)");
    TEST_CHECK(wifi_prov_add_network("ho\0x", 5, "pw", 2) == ESP_ERR_INVALID_ARG, "add: NUL in ssid refused");
    TEST_CHECK(wifi_prov_add_network("home", 4, "p\0w", 3) == ESP_ERR_INVALID_ARG, "add: NUL in password refused");
    TEST_CHECK(wifi_prov_forget_network("ho\0x", 5) == ESP_ERR_INVALID_ARG, "forget: NUL in ssid refused");
}

static void test_forget_last_network_survives_reboot(void)
{
    TEST_SECTION("forget the last saved network, then reboot: zero saved nets, legacy copy does not return");
    seed_legacy_default_wifi();
    boot_wifi_migration();
    TEST_CHECK(s_wifi.saved_nets.count == 1, "setup: migrated network present");
    TEST_CHECK(do_forget_network("oldnet") == ESP_OK, "forget succeeds");
    TEST_CHECK(s_wifi.saved_nets.count == 0, "list is empty");
    simulate_reboot_state();
    boot_wifi_migration();
    TEST_CHECK(s_wifi.saved_nets.count == 0, "after reboot there are still zero saved nets");
}

static void test_mode_and_ap_name_change_survives_reboot(void)
{
    TEST_SECTION("a mode / AP-name change made after migration survives a reboot (adopt must not revert it)");
    seed_legacy_default_wifi();
    boot_wifi_migration();
    s_wifi.mode = WIFI_PROV_MODE_AP;
    strcpy(s_wifi.ap_ssid, "MyKiln");
    s_wifi.has_ap_ssid_override = true;
    TEST_CHECK(nvs_save_mode() == ESP_OK && nvs_save_ap_ssid() == ESP_OK, "setup: persist the change");
    simulate_reboot_state();
    boot_wifi_migration();
    TEST_CHECK(s_wifi.mode == WIFI_PROV_MODE_AP, "mode change survives the reboot");
    TEST_CHECK(s_wifi.has_ap_ssid_override && strcmp(s_wifi.ap_ssid, "MyKiln") == 0,
               "AP-name change survives the reboot");
}

static void test_apply_sta_config_dns(void)
{
    TEST_SECTION("apply_sta_config -- static mode pushes DNS: dns else gateway as MAIN, dns2 else the same as MAIN for BACKUP (never 0.0.0.0)");

    reset_state();
    s_wifi.sta_netif = esp_netif_create_default_wifi_sta();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_STATIC;
    strcpy(s_wifi.static_ip, "192.168.1.50");
    strcpy(s_wifi.static_netmask, "255.255.255.0");
    strcpy(s_wifi.static_gateway, "192.168.1.1");
    const uint32_t gw = 192u | (168u << 8) | (1u << 16) | (1u << 24);

    g_stub_dns_main = g_stub_dns_backup = 0xDEADBEEFu;
    apply_sta_config();
    TEST_CHECK(g_stub_dns_main == gw, "no dns configured: MAIN follows the gateway");
    TEST_CHECK(g_stub_dns_backup == gw, "no dns2 configured: BACKUP = MAIN (esp_netif_set_dns_info refuses 0.0.0.0, so a stale backup is overwritten, not zeroed)");

    // A previously configured dns2 must not linger once dns2 is removed.
    strcpy(s_wifi.static_dns, "1.1.1.1");
    strcpy(s_wifi.static_dns2, "8.8.8.8");
    apply_sta_config();
    s_wifi.static_dns2[0] = '\0';
    apply_sta_config();
    TEST_CHECK(g_stub_dns_backup == (1u | (1u << 8) | (1u << 16) | (1u << 24)), "dns2 removed: BACKUP follows MAIN, the old 8.8.8.8 is gone");
    s_wifi.static_dns[0] = '\0';

    strcpy(s_wifi.static_dns, "1.1.1.1");
    strcpy(s_wifi.static_dns2, "8.8.8.8");
    apply_sta_config();
    TEST_CHECK(g_stub_dns_main == (1u | (1u << 8) | (1u << 16) | (1u << 24)), "dns configured: MAIN = dns");
    TEST_CHECK(g_stub_dns_backup == (8u | (8u << 8) | (8u << 16) | (8u << 24)), "dns2 configured: BACKUP = dns2");

    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    g_stub_dns_set_calls = 0;
    apply_sta_config();
    TEST_CHECK(g_stub_dns_set_calls == 0, "DHCP mode never touches DNS");

    // static -> DHCP: do_set_dhcp() must empty the lwIP BACKUP slot (a DHCP
    // lease only overwrites servers its offer includes).
    reset_state();
    s_wifi.sta_netif = esp_netif_create_default_wifi_sta();
    g_stub_dns_backup = 0xDEADBEEFu;
    do_set_dhcp();
    TEST_CHECK(g_stub_lwip_backup_clears == 1 && g_stub_dns_backup == 0, "do_set_dhcp clears the lwIP backup resolver");
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

// ---- W1 reply-slot-pool tests (docs/HTTP_POST_OWNER_MIGRATION.md) ----
// claim_reply_slot()/free_reply_slot()/abandon_or_free_reply_slot()/owner_reply() are
// static in wifi_prov.c and reached here the same way do_*() is: this file
// #includes wifi_prov.c directly. These exercise the pure claim/free/abandon/
// reply protocol against the pool's own data structures -- the freertos/
// semphr.h stub does not model real per-semaphore give/take state (see its
// header comment), so a real blocking wait can't be simulated here; what CAN
// be verified on the host, and is the actual point of this fix, is that the
// pool never writes a reply into a slot the producer has already abandoned,
// and never lets a stale generation match after a slot is recycled.
static void reset_reply_pool(void)
{
    // Reset only the protocol state -- NOT the whole struct. A blanket
    // memset would also zero .sem/.sem_storage, but ensure_reply_pool_init()
    // only creates the semaphore handles once (it's a no-op once
    // s_reply_pool_mutex is non-NULL), so a later test would inherit a NULL
    // .sem and crash the moment claim_reply_slot()'s defensive drain (or
    // owner_reply()'s xSemaphoreGive()) touched it.
    for (int i = 0; i < WIFI_REPLY_SLOT_COUNT; i++) {
        s_reply_slots[i].in_use = false;
        s_reply_slots[i].abandoned = false;
        s_reply_slots[i].replied = false;
        s_reply_slots[i].generation = 0;
        memset(&s_reply_results[i], 0, sizeof(s_reply_results[i]));
    }
}

static void test_reply_slot_normal_roundtrip(void)
{
    TEST_SECTION("reply slot pool -- normal claim -> owner_reply -> free roundtrip");
    reset_reply_pool();

    uint32_t gen = 0xFFFFFFFF;
    int slot = claim_reply_slot(&gen);
    TEST_CHECK(slot >= 0, "a free pool has a slot to claim");
    TEST_CHECK(gen == 0, "a never-used slot starts at generation 0");
    TEST_CHECK(s_reply_slots[slot].in_use, "claimed slot is marked in_use");

    wifi_result_t result;
    memset(&result, 0, sizeof(result));
    result.err = ESP_OK;
    result.scan_count = 7;
    owner_reply(slot, gen, &result);

    TEST_CHECK(s_reply_slots[slot].in_use, "owner_reply() on a live slot does not free it -- the producer does");
    TEST_CHECK(!s_reply_slots[slot].abandoned, "owner_reply() on a live slot never marks it abandoned");
    TEST_CHECK(s_reply_slots[slot].replied, "owner_reply() sets replied on a live slot");
    TEST_CHECK(s_reply_results[slot].err == ESP_OK, "the result was written into s_reply_results");
    TEST_CHECK(s_reply_results[slot].scan_count == 7, "the full result struct was written, not just err");

    // Producer's side of a successful wait: copy out, then free.
    free_reply_slot(slot);
    TEST_CHECK(!s_reply_slots[slot].in_use, "free_reply_slot() releases the slot");
    TEST_CHECK(s_reply_slots[slot].generation == gen + 1, "freeing bumps the generation");
}

static void test_reply_slot_abandon_then_owner_recycles(void)
{
    TEST_SECTION("reply slot pool -- a timed-out producer's abandon means owner_reply() recycles, never writes");
    reset_reply_pool();

    uint32_t gen;
    int slot = claim_reply_slot(&gen);
    TEST_CHECK(slot >= 0, "slot claimed");

    // Producer gives up (timeout) BEFORE the owner ever replied --
    // abandon_or_free_reply_slot() must mark abandoned, not free, since the
    // owner may still be about to act on this exact command.
    wifi_result_t late;
    memset(&late, 0, sizeof(late));
    bool got_late = abandon_or_free_reply_slot(slot, gen, &late);
    TEST_CHECK(!got_late, "no reply was pending yet -- this is a genuine timeout, not a race");
    TEST_CHECK(s_reply_slots[slot].abandoned, "abandon_or_free_reply_slot() marks the slot abandoned");
    TEST_CHECK(s_reply_slots[slot].in_use, "abandon does not free the slot itself -- the owner recycles it");
    TEST_CHECK(s_reply_slots[slot].generation == gen, "abandon does not bump the generation");

    // Owner finally gets to the command. It must recycle the slot itself and
    // must NOT write into s_reply_results or leave it claimable-but-answered.
    wifi_result_t result;
    memset(&result, 0, sizeof(result));
    result.err = ESP_ERR_TIMEOUT; // a value that must NOT end up in the slot
    owner_reply(slot, gen, &result);

    TEST_CHECK(!s_reply_slots[slot].in_use, "owner_reply() recycles an abandoned slot instead of answering it");
    TEST_CHECK(!s_reply_slots[slot].abandoned, "the recycle clears the abandoned flag too");
    TEST_CHECK(s_reply_slots[slot].generation == gen + 1,
               "recycling bumps the generation so a future claimant never inherits the abandoned state");
    TEST_CHECK(s_reply_results[slot].err != ESP_ERR_TIMEOUT,
               "owner_reply() on an abandoned slot must NOT write the result -- nobody is waiting on it");
}

// 2026-09-25 review fix: the previous version of this pool gave the
// semaphore OUTSIDE owner_reply()'s lock, which left a window where a
// producer's timeout could race in, find "not abandoned yet" (truthfully, at
// that instant) under a SEPARATE lock acquisition, and mark an
// already-replied slot abandoned forever -- a permanent leak, since nothing
// ever frees an abandoned+in_use slot again once its generation is stuck.
// This test proves the fix: owner_reply() now finishes (write + replied flag
// + give) in ONE locked operation, so a producer that reaches
// abandon_or_free_reply_slot() AFTER that finishes sees replied==true and
// takes the late-success path -- copy the result out, free the slot -- never
// the leak path.
static void test_reply_slot_timeout_races_a_completed_reply_never_leaks(void)
{
    TEST_SECTION("reply slot pool -- a timeout racing an already-completed reply frees the slot, never leaks it");
    reset_reply_pool();

    uint32_t gen;
    int slot = claim_reply_slot(&gen);
    TEST_CHECK(slot >= 0, "slot claimed");

    // Owner "wins the race": replies before the producer's timeout call runs.
    wifi_result_t result;
    memset(&result, 0, sizeof(result));
    result.err = ESP_OK;
    result.scan_count = 3;
    owner_reply(slot, gen, &result);
    TEST_CHECK(s_reply_slots[slot].replied, "owner_reply() completed and set replied before the timeout got here");

    // Producer's xSemaphoreTake() timed out anyway (its own deadline fired
    // independently of the owner's timing) and it calls the timeout path.
    wifi_result_t late;
    memset(&late, 0, sizeof(late));
    bool got_late = abandon_or_free_reply_slot(slot, gen, &late);

    TEST_CHECK(got_late, "a reply that already landed is treated as a late success, not a leak");
    TEST_CHECK(late.err == ESP_OK, "the late result was copied out correctly");
    TEST_CHECK(late.scan_count == 3, "the full late result struct was copied out, not just err");
    TEST_CHECK(!s_reply_slots[slot].in_use, "the slot is freed immediately -- no leak");
    TEST_CHECK(!s_reply_slots[slot].abandoned, "a late-success free never leaves the slot marked abandoned");
    TEST_CHECK(!s_reply_slots[slot].replied, "the freed slot's replied flag is cleared for the next claimant");
    TEST_CHECK(s_reply_slots[slot].generation == gen + 1, "freeing bumps the generation, same as any other free");

    // The freed slot must be immediately reusable -- this is the actual
    // symptom the bug produced: 6 leaked slots (this pool's whole capacity)
    // meant every subsequent Wi-Fi call refused with "reply slot pool
    // exhausted" until reboot.
    uint32_t new_gen;
    int slot2 = claim_reply_slot(&new_gen);
    TEST_CHECK(slot2 == slot, "the freed slot is claimable again, not stuck forever");
    TEST_CHECK(new_gen == gen + 1, "the reclaim sees the bumped generation");
}

static void test_reply_slot_stale_generation_never_matches_after_reuse(void)
{
    TEST_SECTION("reply slot pool -- a stale (pre-recycle) generation from owner_reply() is a fail-closed no-op");
    reset_reply_pool();

    // First claimant abandons; owner recycles it (as in the test above).
    uint32_t old_gen;
    int slot = claim_reply_slot(&old_gen);
    wifi_result_t late;
    memset(&late, 0, sizeof(late));
    abandon_or_free_reply_slot(slot, old_gen, &late);
    wifi_result_t discard;
    memset(&discard, 0, sizeof(discard));
    owner_reply(slot, old_gen, &discard); // recycles

    // A second producer claims the now-free slot -- same index, new generation.
    uint32_t new_gen;
    int slot2 = claim_reply_slot(&new_gen);
    TEST_CHECK(slot2 == slot, "the pool reuses the just-freed slot index");
    TEST_CHECK(new_gen == old_gen + 1, "the new claim gets the bumped generation, not the stale one");

    // Simulate a delayed/duplicate owner_reply() call still carrying the OLD
    // generation (should never happen given the protocol, but this is
    // exactly the fail-closed guard for if it ever did): must not touch the
    // slot the SECOND producer now owns.
    wifi_result_t stale_result;
    memset(&stale_result, 0, sizeof(stale_result));
    stale_result.err = ESP_ERR_INVALID_ARG; // a value that must NOT land on the new claimant
    owner_reply(slot, old_gen, &stale_result);

    TEST_CHECK(s_reply_slots[slot].in_use, "the second producer's live claim is untouched by a stale-generation reply");
    TEST_CHECK(s_reply_slots[slot].generation == new_gen,
               "generation is unchanged by the stale call -- it never matched");
    TEST_CHECK(s_reply_results[slot].err != ESP_ERR_INVALID_ARG,
               "the stale reply's result must never be written into the new claimant's slot");
}

// 2026-09-25 review fix: reply-before-join means a client polling
// GET /api/status right after "ok" could otherwise observe the PRE-join
// state (UNPROVISIONED/AP_MODE) and stop polling, per
// wifi_provision_page.html's poll() logic -- breaking first-time
// provisioning over the fallback AP. do_add_network() now sets
// s_wifi.state = WIFI_PROV_STATE_CONNECTING itself, on the owner task,
// before returning -- i.e. before the reply is even visible to the waiting
// producer -- and defers the actual radio join to start_sta_join(), which
// owner_task() runs only AFTER replying (out_join_after_reply).
static void test_do_add_network_sets_connecting_and_defers_the_join(void)
{
    TEST_SECTION("do_add_network() sets CONNECTING and defers the join to after the reply");
    memset(&s_wifi, 0, sizeof(s_wifi));
    g_stub_wifi_connect_calls = 0;

    bool join_after_reply = false;
    esp_err_t err = do_add_network("TestSSID", "TestPassword1", &join_after_reply);

    TEST_CHECK(err == ESP_OK, "adding a network with room in the list succeeds");
    TEST_CHECK(join_after_reply, "do_add_network() asks the owner to join AFTER it replies");
    TEST_CHECK(s_wifi.state == WIFI_PROV_STATE_CONNECTING,
               "state flips to CONNECTING synchronously, before the reply is visible to the producer");
    TEST_CHECK(g_stub_wifi_connect_calls == 0,
               "do_add_network() itself never calls esp_wifi_connect() -- only start_sta_join(), "
               "run later by owner_task(), does that");
}

// 2026-09-25 review fix (mirrors do_add_network() above): do_set_mode()'s
// HOME branch has the identical reply-before-join hazard -- a client polling
// GET /api/status right after "ok" could observe the PRE-join state if
// CONNECTING weren't set until start_sta_join() got around to it. do_set_mode()
// sets s_wifi.state = WIFI_PROV_STATE_CONNECTING itself, on the owner task,
// before returning -- before the reply is visible to the waiting producer --
// and defers the actual radio join to start_sta_join(), which owner_task()
// runs only AFTER replying (out_join_after_reply).
static void test_do_set_mode_home_sets_connecting_and_defers_the_join(void)
{
    TEST_SECTION("do_set_mode() HOME branch sets CONNECTING and defers the join to after the reply");
    memset(&s_wifi, 0, sizeof(s_wifi));
    g_stub_wifi_connect_calls = 0;

    // Seed a saved network so the HOME branch takes the join-pending path
    // rather than the "no saved network" branch.
    bool seed_join_after_reply = false;
    esp_err_t seed_err = do_add_network("TestSSID", "TestPassword1", &seed_join_after_reply);
    TEST_CHECK(seed_err == ESP_OK, "seeding a saved network succeeds");
    g_stub_wifi_connect_calls = 0; // do_add_network() itself must not have joined either; reset for a clean assertion
    s_wifi.state = WIFI_PROV_STATE_AP_MODE; // do_add_network() already set CONNECTING; reset so the assert below only passes if do_set_mode() itself sets it

    bool join_after_reply = false;
    esp_err_t err = do_set_mode(WIFI_PROV_MODE_HOME, &join_after_reply);

    TEST_CHECK(err == ESP_OK, "setting HOME mode with a saved network succeeds");
    TEST_CHECK(join_after_reply, "do_set_mode() HOME branch asks the owner to join AFTER it replies");
    TEST_CHECK(s_wifi.state == WIFI_PROV_STATE_CONNECTING,
               "state flips to CONNECTING synchronously, before the reply is visible to the producer");
    TEST_CHECK(g_stub_wifi_connect_calls == 0,
               "do_set_mode() itself never calls esp_wifi_connect() -- only start_sta_join(), "
               "run later by owner_task(), does that");
}

// wifi_prov_is_unprovisioned() opens /wifi, /networks and /scan with no
// session (ROUTE_TIER_WIFI_SETUP). It must read true ONLY with no saved STA
// network: every other state, and UNPROVISIONED with a network saved (a
// future AP-fallback path reusing that state), must read false.
static void test_is_unprovisioned_requires_no_saved_network(void)
{
    TEST_SECTION("wifi_prov_is_unprovisioned() -- true only for UNPROVISIONED with zero saved networks");
    memset(&s_wifi, 0, sizeof(s_wifi));
    TEST_CHECK(!wifi_prov_is_unprovisioned(), "zeroed state (AP_MODE) is not unprovisioned");

    s_wifi.state = WIFI_PROV_STATE_UNPROVISIONED;
    TEST_CHECK(wifi_prov_is_unprovisioned(), "UNPROVISIONED with no saved network is unprovisioned");

    const wifi_prov_state_t others[] = { WIFI_PROV_STATE_AP_MODE, WIFI_PROV_STATE_CONNECTING,
                                         WIFI_PROV_STATE_CONNECTED, WIFI_PROV_STATE_RECONNECTING };
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        s_wifi.state = others[i];
        TEST_CHECK(!wifi_prov_is_unprovisioned(), "any non-UNPROVISIONED state is not unprovisioned");
    }

    s_wifi.state = WIFI_PROV_STATE_UNPROVISIONED;
    s_wifi.saved_nets.count = 1;
    TEST_CHECK(!wifi_prov_is_unprovisioned(),
               "UNPROVISIONED with a saved network (AP fallback with credentials) is NOT unprovisioned");
    memset(&s_wifi, 0, sizeof(s_wifi));
}

static void test_ap_fallback_tick_raises_ap_after_timeout(void)
{
    TEST_SECTION("do_ap_fallback_tick -- station never joined by the time the timer fires: bring the AP up "
                 "(2026-09-28 owner request: fail -> AP, WIFI_STA_CONNECT_TIMEOUT_MS now 60s)");

    reset_state();
    s_wifi.state = WIFI_PROV_STATE_CONNECTING; // still trying to join when the timer fired
    g_stub_wifi_mode = WIFI_MODE_STA;          // not associated -- reconcile_sta_state() must not find a live link
    g_stub_ap_info_result = ESP_FAIL; // not associated -- esp_wifi_sta_get_ap_info() fails
    g_stub_wifi_set_mode_calls = 0;

    do_ap_fallback_tick();

    TEST_CHECK(s_wifi.state == WIFI_PROV_STATE_RECONNECTING, "falls back to RECONNECTING once the AP is raised");
    TEST_CHECK(g_stub_wifi_set_mode_calls == 1, "esp_wifi_set_mode(APSTA) was called to raise the AP");
    TEST_CHECK(g_stub_wifi_mode == WIFI_MODE_APSTA, "radio mode is now APSTA -- home STA config is preserved");
}

static void test_got_ip_defers_ap_teardown_while_ap_session_active(void)
{
    TEST_SECTION("do_ev_got_ip -- home Wi-Fi back, web auth ON and an AP-origin session IS active: "
                 "AP teardown is deferred, never cut off a logged-in AP operator");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    s_wifi.state = WIFI_PROV_STATE_RECONNECTING; // was in fallback, home came back
    g_stub_web_auth_enabled = true;
    g_stub_any_ap_session_active = true;

    do_ev_got_ip();

    TEST_CHECK(s_wifi.state == WIFI_PROV_STATE_CONNECTED, "join still reaches CONNECTED");
    TEST_CHECK(g_stub_wifi_set_mode_calls == 0, "esp_wifi_set_mode() is NOT called -- AP teardown deferred");
    TEST_CHECK(s_wifi.ap_pending_teardown, "ap_pending_teardown is set so do_rescan_tick() retries later");
}

// *** 2026-09-29 owner decision, and the actual regression test for the bug
// report: a LAN-only admin session (e.g. the PC's MCP tools, which reach the
// board over the home LAN and never through the fallback AP) must NOT defer
// AP teardown by itself. At this layer, "no AP session" (g_stub_any_ap_session_active
// == false) is exactly what a LAN-only session looks like -- the fake models
// the production seam's AP-scoped answer, and the LAN-vs-AP distinction
// itself is proven at the lower layer in test_http_session_iface.c's
// test_any_ap_session_active() (a session touched only over the LAN never
// makes http_auth_any_ap_session_active() true). Before the 2026-09-29 fix,
// ap_teardown_should_defer() asked the broader http_auth_any_session_active()
// instead, which WOULD have deferred here even with zero AP clients, purely
// because SOME session (anywhere) was live -- this test would have failed
// against that old logic. ***
static void test_got_ip_drops_ap_for_lan_only_session_even_with_auth_on(void)
{
    TEST_SECTION("do_ev_got_ip -- home Wi-Fi back, web auth ON, a LAN-only admin session exists but NO AP "
                 "session and NO AP client: AP comes down immediately (2026-09-29 fix -- a LAN session must "
                 "never defer AP teardown by itself)");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    s_wifi.state = WIFI_PROV_STATE_RECONNECTING;
    g_stub_web_auth_enabled = true;
    g_stub_any_session_active = true;     // the LAN-only session itself IS active (review fix, round 2) --
                                           // makes this a genuine regression test: a revert to the old,
                                           // broader http_auth_any_session_active() call would defer here
                                           // (some session is active) and fail this test's own assertions,
                                           // not merely fail to link.
    g_stub_any_ap_session_active = false; // but no session was ever used over the AP
    g_stub_ap_sta_count = 0;              // and nothing is physically associated to the AP radio

    do_ev_got_ip();

    TEST_CHECK(s_wifi.state == WIFI_PROV_STATE_CONNECTED, "join reaches CONNECTED");
    TEST_CHECK(g_stub_wifi_set_mode_calls == 1,
               "AP is torn down immediately -- a LAN-only session must not hold it up");
    TEST_CHECK(g_stub_wifi_mode == WIFI_MODE_STA, "radio mode is STA-only");
    TEST_CHECK(!s_wifi.ap_pending_teardown, "nothing left pending");
}

// *** 2026-09-29 judgment call (explicitly requested by the task): with auth
// ON, a station physically connected to the AP radio but with NO session yet
// (e.g. mid-login, still typing a password on the captive page) also defers
// -- chosen deliberately in the direction of never stranding a connecting
// operator over the more convenient early teardown. This is the OR half of
// ap_teardown_should_defer()'s auth-on branch. ***
static void test_got_ip_defers_ap_teardown_auth_on_with_ap_client_but_no_session(void)
{
    TEST_SECTION("do_ev_got_ip -- web auth ON, no session at all, but a client IS associated to the fallback "
                 "AP (mid-login): teardown deferred (2026-09-29 judgment call)");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    s_wifi.state = WIFI_PROV_STATE_RECONNECTING;
    s_wifi.started = true; // AP fallback already brought the radio up (APSTA)
    g_stub_wifi_mode = WIFI_MODE_APSTA;
    g_stub_web_auth_enabled = true;
    g_stub_any_ap_session_active = false; // no session yet -- still on the login page
    g_stub_ap_sta_count = 1;

    do_ev_got_ip();

    TEST_CHECK(g_stub_wifi_set_mode_calls == 0,
               "AP teardown deferred -- a connected-but-unauthenticated station may be mid-login");
    TEST_CHECK(s_wifi.ap_pending_teardown, "ap_pending_teardown recorded");
}

static void test_got_ip_drops_ap_when_no_session_active(void)
{
    TEST_SECTION("do_ev_got_ip -- home Wi-Fi back, web auth ON but NO AP session and NO AP client: "
                 "AP comes down immediately");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    s_wifi.state = WIFI_PROV_STATE_RECONNECTING;
    g_stub_web_auth_enabled = true;
    g_stub_any_ap_session_active = false;

    do_ev_got_ip();

    TEST_CHECK(s_wifi.state == WIFI_PROV_STATE_CONNECTED, "join reaches CONNECTED");
    TEST_CHECK(g_stub_wifi_set_mode_calls == 1, "AP is torn down immediately -- nobody logged in");
    TEST_CHECK(g_stub_wifi_mode == WIFI_MODE_STA, "radio mode is STA-only");
    TEST_CHECK(!s_wifi.ap_pending_teardown, "nothing left pending");
}

static void test_got_ip_defers_ap_teardown_auth_off_with_ap_client(void)
{
    TEST_SECTION("do_ev_got_ip -- web auth OFF, a client IS associated to the fallback AP: teardown deferred "
                 "(the auth-off honest-signal branch of ap_teardown_should_defer())");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    s_wifi.state = WIFI_PROV_STATE_RECONNECTING;
    s_wifi.started = true; // AP fallback already brought the radio up (APSTA)
    g_stub_wifi_mode = WIFI_MODE_APSTA;
    g_stub_web_auth_enabled = false;
    g_stub_ap_sta_count = 1;

    do_ev_got_ip();

    TEST_CHECK(g_stub_wifi_set_mode_calls == 0, "AP teardown deferred while a client is still on the AP");
    TEST_CHECK(s_wifi.ap_pending_teardown, "ap_pending_teardown recorded");
}

static void test_got_ip_drops_ap_auth_off_no_ap_client(void)
{
    TEST_SECTION("do_ev_got_ip -- web auth OFF, no client on the AP: AP comes down immediately");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    s_wifi.state = WIFI_PROV_STATE_RECONNECTING;
    g_stub_web_auth_enabled = false;
    g_stub_ap_sta_count = 0;

    do_ev_got_ip();

    TEST_CHECK(g_stub_wifi_set_mode_calls == 1, "AP torn down -- no client to protect");
    TEST_CHECK(!s_wifi.ap_pending_teardown, "nothing left pending");
}

static void test_confirm_static_reachable_defers_ap_teardown_while_session_active(void)
{
    TEST_SECTION("do_confirm_static_reachable -- static join confirmed reachable but a session is active: "
                 "AP teardown deferred, same gate as the DHCP path");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_STATIC;
    strcpy(s_wifi.static_ip, "192.168.1.50");
    s_wifi.static_ip_confirmed = false;
    s_wifi.state = WIFI_PROV_STATE_CONNECTED;
    g_stub_web_auth_enabled = true;
    g_stub_any_ap_session_active = true;

    do_confirm_static_reachable();

    TEST_CHECK(s_wifi.static_ip_confirmed, "static_ip_confirmed still records regardless of the gate");
    TEST_CHECK(g_stub_wifi_set_mode_calls == 0, "AP teardown deferred -- a session is active");
    TEST_CHECK(s_wifi.ap_pending_teardown, "ap_pending_teardown recorded for do_rescan_tick() to retry");
}

static void test_rescan_tick_retries_deferred_teardown_until_session_ends(void)
{
    TEST_SECTION("do_rescan_tick -- retries a deferred AP teardown every tick, completing it once nobody's "
                 "logged in any more (reuses the existing 30s rescan cadence, no new timer)");

    reset_state();
    s_wifi.mode = WIFI_PROV_MODE_HOME;
    s_wifi.state = WIFI_PROV_STATE_CONNECTED;
    s_wifi.ap_pending_teardown = true;
    g_stub_web_auth_enabled = true;
    g_stub_any_ap_session_active = true;

    do_rescan_tick();
    TEST_CHECK(g_stub_wifi_set_mode_calls == 0, "still logged in on this tick -- AP left up, retried next tick");
    TEST_CHECK(s_wifi.ap_pending_teardown, "still pending");

    g_stub_any_ap_session_active = false; // the operator logged out (or session expired) between ticks
    do_rescan_tick();
    TEST_CHECK(g_stub_wifi_set_mode_calls == 1, "nobody logged in any more -- the deferred teardown completes");
    TEST_CHECK(g_stub_wifi_mode == WIFI_MODE_STA, "radio mode is now STA-only");
    TEST_CHECK(!s_wifi.ap_pending_teardown, "flag cleared once the teardown actually runs");
}

static void test_ap_fallback_tick_leaves_running_ap_alone(void)
{
    TEST_SECTION("do_ap_fallback_tick -- AP already up (APSTA): no set_mode/apply_ap_config on the live AP, "
                 "just record the fallback (2026-09-28 review fix)");

    reset_state();
    s_wifi.state = WIFI_PROV_STATE_CONNECTING;
    g_stub_wifi_mode = WIFI_MODE_APSTA; // boot join / start_sta_join() already run APSTA
    g_stub_ap_info_result = ESP_FAIL;

    do_ap_fallback_tick();

    TEST_CHECK(g_stub_wifi_set_mode_calls == 0, "the running AP is not re-applied (would disrupt its clients)");
    TEST_CHECK(s_wifi.state == WIFI_PROV_STATE_RECONNECTING, "state still records the fallback");
    TEST_CHECK(s_wifi.ap_fallback_active, "ap_fallback_active set -- retries move to the rescan cadence");
}

static void test_disconnect_while_fallback_ap_up_waits_for_rescan(void)
{
    TEST_SECTION("do_ev_sta_disconnected -- fallback AP up: no immediate esp_wifi_connect() retry loop "
                 "(2026-09-28 review fix); before the fallback, retries stay immediate");

    reset_state();
    s_wifi.mode = WIFI_PROV_MODE_HOME;
    s_wifi.saved_nets.count = 1;
    s_wifi.state = WIFI_PROV_STATE_CONNECTING;
    g_stub_ap_info_result = ESP_FAIL; // really down -- reconcile_sta_state() finds no live link
    g_stub_wifi_connect_calls = 0;

    s_wifi.ap_fallback_active = false;
    do_ev_sta_disconnected();
    TEST_CHECK(g_stub_wifi_connect_calls == 1, "inside the join timeout window: immediate retry, as before");

    s_wifi.ap_fallback_active = true;
    s_wifi.ap_pending_teardown = true; // stale from an earlier deferred teardown
    do_ev_sta_disconnected();
    TEST_CHECK(g_stub_wifi_connect_calls == 1, "fallback AP up: NO immediate retry -- do_rescan_tick() owns it");
    TEST_CHECK(s_wifi.state == WIFI_PROV_STATE_RECONNECTING, "state is RECONNECTING");
    TEST_CHECK(!s_wifi.ap_pending_teardown, "a real disconnect clears the now-meaningless pending teardown");

    do_rescan_tick();
    TEST_CHECK(g_stub_wifi_connect_calls == 2, "the 30 s rescan tick is what retries the join");
    TEST_CHECK(s_wifi.ap_fallback_active, "still in fallback until a join lands");
    memset(&s_wifi, 0, sizeof(s_wifi));
}

static void test_reconcile_sta_state_drops_ap_when_link_regained(void)
{
    TEST_SECTION("reconcile_sta_state -- IP lost then regained without a fresh GOT_IP event (e.g. a dropped "
                 "queued event, or LOST_IP/GOT_IP with nothing watching for it): the ground-truth repair now "
                 "also drops the fallback AP, not just the state field (2026-09-28 follow-up)");

    reset_state();
    s_wifi.mode = WIFI_PROV_MODE_HOME;
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    s_wifi.state = WIFI_PROV_STATE_RECONNECTING; // stale -- the station actually reconnected already
    s_wifi.ap_fallback_active = true;            // the fallback AP is up from the earlier drop
    s_wifi.sta_netif = esp_netif_create_default_wifi_sta(); // sta_link_is_live() bails out early without one
    g_stub_ap_info_result = ESP_OK;              // associated
    g_stub_netif_ip_addr = 0x0101A8C0;            // and holds a real lease -- link is genuinely live
    g_stub_web_auth_enabled = false;
    g_stub_ap_sta_count = 0;                     // nobody on the AP -- teardown must not be deferred

    bool live = reconcile_sta_state();

    TEST_CHECK(live, "reconcile_sta_state() reports the link live");
    TEST_CHECK(s_wifi.state == WIFI_PROV_STATE_CONNECTED, "state corrected to CONNECTED");
    TEST_CHECK(g_stub_wifi_set_mode_calls == 1, "the fallback AP is torn down, not left stranded up");
    TEST_CHECK(g_stub_wifi_mode == WIFI_MODE_STA, "radio mode is now STA-only");
    TEST_CHECK(!s_wifi.ap_fallback_active, "ap_fallback_active cleared");
    TEST_CHECK(!s_wifi.ap_pending_teardown, "nothing left pending");
}

static void test_reconcile_sta_state_defers_ap_drop_while_session_active(void)
{
    TEST_SECTION("reconcile_sta_state -- link regained the same way, but a web session is still active: "
                 "teardown defers exactly like a normal reconnect, not torn down out from under an operator");

    reset_state();
    s_wifi.mode = WIFI_PROV_MODE_HOME;
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    s_wifi.state = WIFI_PROV_STATE_RECONNECTING;
    s_wifi.ap_fallback_active = true;
    s_wifi.sta_netif = esp_netif_create_default_wifi_sta();
    g_stub_ap_info_result = ESP_OK;
    g_stub_netif_ip_addr = 0x0101A8C0;
    g_stub_web_auth_enabled = true;
    g_stub_any_ap_session_active = true;

    bool live = reconcile_sta_state();

    TEST_CHECK(live, "link still reported live");
    TEST_CHECK(s_wifi.state == WIFI_PROV_STATE_CONNECTED, "state still corrected to CONNECTED");
    TEST_CHECK(g_stub_wifi_set_mode_calls == 0, "AP teardown deferred -- a session is active");
    TEST_CHECK(s_wifi.ap_pending_teardown, "ap_pending_teardown set so do_rescan_tick() retries later");
    TEST_CHECK(s_wifi.ap_fallback_active, "ap_fallback_active untouched while deferred (AP still up)");
}

static void test_reconcile_sta_state_no_ap_action_when_state_already_agreed(void)
{
    TEST_SECTION("reconcile_sta_state -- state already says CONNECTED (nothing to repair): no AP action is "
                 "taken on every live-link tick, only when a disagreement is actually found and fixed");

    reset_state();
    s_wifi.mode = WIFI_PROV_MODE_HOME;
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    s_wifi.state = WIFI_PROV_STATE_CONNECTED; // already agrees with the live link
    s_wifi.ap_fallback_active = true;         // e.g. a deferred teardown mid-flight, owned by do_rescan_tick()
    s_wifi.sta_netif = esp_netif_create_default_wifi_sta();
    g_stub_ap_info_result = ESP_OK;
    g_stub_netif_ip_addr = 0x0101A8C0;

    bool live = reconcile_sta_state();

    TEST_CHECK(live, "link reported live");
    TEST_CHECK(g_stub_wifi_set_mode_calls == 0,
               "no radio call -- state already agreed, so this path leaves the fallback-AP decision to "
               "whichever path is already driving it (do_rescan_tick()'s existing pending-teardown recheck)");
}

static void test_reconcile_sta_state_never_touches_operator_chosen_ap(void)
{
    TEST_SECTION("reconcile_sta_state -- mode == WIFI_PROV_MODE_AP (operator's deliberate AP choice): a live "
                 "station link never tears the AP down");

    reset_state();
    s_wifi.mode = WIFI_PROV_MODE_AP;
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    s_wifi.state = WIFI_PROV_STATE_AP_MODE;
    s_wifi.sta_netif = esp_netif_create_default_wifi_sta();
    g_stub_wifi_mode = WIFI_MODE_AP;
    g_stub_ap_info_result = ESP_OK;
    g_stub_netif_ip_addr = 0x0101A8C0;

    bool live = reconcile_sta_state();

    TEST_CHECK(!live, "AP mode: reconcile reports not-live without looking at the station");
    TEST_CHECK(s_wifi.state == WIFI_PROV_STATE_AP_MODE, "state left at AP_MODE");
    TEST_CHECK(g_stub_wifi_set_mode_calls == 0, "no radio call -- the operator-chosen AP stays up");
}

static void test_got_ip_after_reconcile_dropped_ap_is_a_no_op(void)
{
    TEST_SECTION("try_drop_fallback_ap_after_join -- reconcile_sta_state() already dropped the AP, then a queued "
                 "GOT_IP arrives while a session is active: no second set_mode, no false pending teardown");

    reset_state();
    s_wifi.mode = WIFI_PROV_MODE_HOME;
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    s_wifi.state = WIFI_PROV_STATE_RECONNECTING;
    s_wifi.ap_fallback_active = true;
    s_wifi.sta_netif = esp_netif_create_default_wifi_sta();
    g_stub_ap_info_result = ESP_OK;
    g_stub_netif_ip_addr = 0x0101A8C0;

    (void)reconcile_sta_state();
    TEST_CHECK(g_stub_wifi_set_mode_calls == 1 && g_stub_wifi_mode == WIFI_MODE_STA, "setup: reconcile dropped the AP");

    g_stub_web_auth_enabled = true;
    g_stub_any_ap_session_active = true;
    do_ev_got_ip();

    TEST_CHECK(g_stub_wifi_set_mode_calls == 1, "no redundant esp_wifi_set_mode(STA)");
    TEST_CHECK(!s_wifi.ap_pending_teardown, "no pending teardown reported for an AP that is not running");
    TEST_CHECK(!s_wifi.ap_fallback_active, "ap_fallback_active stays cleared");
}

static void test_teardown_and_mode_changes_clear_fallback_active(void)
{
    TEST_SECTION("ap_fallback_active -- cleared by an actual AP teardown and by a switch to AP mode");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    s_wifi.state = WIFI_PROV_STATE_RECONNECTING;
    s_wifi.ap_fallback_active = true;
    do_ev_got_ip();
    TEST_CHECK(g_stub_wifi_mode == WIFI_MODE_STA, "setup: AP torn down (nobody logged in)");
    TEST_CHECK(!s_wifi.ap_fallback_active, "teardown clears ap_fallback_active");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    s_wifi.state = WIFI_PROV_STATE_RECONNECTING;
    s_wifi.ap_fallback_active = true;
    g_stub_web_auth_enabled = true;
    g_stub_any_ap_session_active = true;
    do_ev_got_ip();
    TEST_CHECK(s_wifi.ap_fallback_active, "a DEFERRED teardown keeps ap_fallback_active (AP still up)");

    bool join_after = true;
    do_set_mode(WIFI_PROV_MODE_AP, &join_after);
    TEST_CHECK(!s_wifi.ap_fallback_active, "switch to AP mode clears ap_fallback_active");
    TEST_CHECK(!s_wifi.ap_pending_teardown, "switch to AP mode clears ap_pending_teardown");
    TEST_CHECK(!wifi_prov_get_ap_pending_teardown(), "/status and the LCD no longer report [AP kept up]");
    memset(&s_wifi, 0, sizeof(s_wifi));
}

// Review fix (2026-09-29, round 2): wifi_prov_request_arrived_on_ap() itself
// had no direct test coverage -- the do_ev_got_ip() tests above only ever
// drive it indirectly via g_stub_any_ap_session_active, so its own AF_INET/
// AF_INET6/failure branches were never exercised. These four cover the
// detector directly: a plain-AF_INET socket (a non-IPv6 build config, kept
// for robustness), an IPv4-mapped AF_INET6 socket reporting the AP's own
// address (the real shape on this board's CONFIG_LWIP_IPV6=y build), the
// same mapped shape for an ordinary LAN address, and a getsockname()
// failure. See sockaddr_is_ap_default_ip()'s own comment in wifi_prov_link.c
// and stubs/lwip/sockets.h's g_stub_getsockname_family comment.
static void test_arrived_on_ap_plain_af_inet_not_matching(void)
{
    TEST_SECTION("wifi_prov_request_arrived_on_ap() -- plain AF_INET family, stub can't populate a real "
                 "address behind it, so this covers the AF_INET branch reporting no match");

    reset_state();
    g_stub_getsockname_result = 0;
    g_stub_getsockname_family = AF_INET;
    strcpy(g_stub_local_ip, "0.0.0.0");

    TEST_CHECK(!wifi_prov_request_arrived_on_ap(3), "AF_INET branch taken, zero address does not match the AP IP");
}

static void test_arrived_on_ap_v4_mapped_af_inet6_matches(void)
{
    TEST_SECTION("wifi_prov_request_arrived_on_ap() -- IPv4-mapped AF_INET6 reporting the AP's own address "
                 "(::ffff:192.168.4.1, the real shape observed on hardware): true");

    reset_state();
    g_stub_getsockname_result = 0;
    g_stub_getsockname_family = AF_INET6;
    strcpy(g_stub_local_ip, "::ffff:192.168.4.1");

    TEST_CHECK(wifi_prov_request_arrived_on_ap(3), "IPv4-mapped AF_INET6 AP address is recognized");
}

static void test_arrived_on_ap_v4_mapped_af_inet6_lan_address_no_match(void)
{
    TEST_SECTION("wifi_prov_request_arrived_on_ap() -- IPv4-mapped AF_INET6 reporting an ordinary LAN "
                 "address: false");

    reset_state();
    g_stub_getsockname_result = 0;
    g_stub_getsockname_family = AF_INET6;
    strcpy(g_stub_local_ip, "::ffff:192.168.1.50");

    TEST_CHECK(!wifi_prov_request_arrived_on_ap(3), "a LAN address, even IPv4-mapped, never reads as the AP");
}

static void test_arrived_on_ap_getsockname_failure_fails_closed(void)
{
    TEST_SECTION("wifi_prov_request_arrived_on_ap() -- getsockname() fails: reports 'not AP' (fail closed)");

    reset_state();
    g_stub_getsockname_result = -1;

    TEST_CHECK(!wifi_prov_request_arrived_on_ap(3), "an uninspectable socket is never reported as the AP");
}

// Review fix (2026-09-29, round 3): the four tests above never actually
// exercised the AF_INET *positive*-match branch (stubs/lwip/sockets.h's
// inet_ntop() used to ignore af entirely, so a plain-AF_INET call always got
// back whatever string a mapped-AF_INET6 test happened to leave behind) and
// never covered the uppercase "::FFFF:" prefix lwIP is documented to print.
// The stub's inet_ntop() now actually honours `af`: it returns "0.0.0.0" when
// `af` disagrees with the family marker getsockname() left at `src`, and
// g_stub_local_ip verbatim otherwise (see its own comment). The "::ffff:"/
// "::FFFF:" prefix stripping happens in production code, in
// get_local_ipv4_string() -- not in this stub.
static void test_arrived_on_ap_plain_af_inet_matches(void)
{
    TEST_SECTION("wifi_prov_request_arrived_on_ap() -- plain AF_INET family reporting the AP's own address: true");

    reset_state();
    g_stub_getsockname_result = 0;
    g_stub_getsockname_family = AF_INET;
    strcpy(g_stub_local_ip, "192.168.4.1");

    TEST_CHECK(wifi_prov_request_arrived_on_ap(3), "AF_INET branch taken, exact AP address matches");
}

static void test_arrived_on_ap_v4_mapped_af_inet6_uppercase_matches(void)
{
    TEST_SECTION("wifi_prov_request_arrived_on_ap() -- uppercase \"::FFFF:\" prefix (lwIP's own documented "
                 "case) is still recognized");

    reset_state();
    g_stub_getsockname_result = 0;
    g_stub_getsockname_family = AF_INET6;
    strcpy(g_stub_local_ip, "::FFFF:192.168.4.1");

    TEST_CHECK(wifi_prov_request_arrived_on_ap(3), "uppercase-prefixed IPv4-mapped AP address is recognized");
}

// Review fix (2026-09-29, round 3): wifi_prov_note_possible_static_reachability()'s
// own MEDIUM bug -- it read getsockname() into a plain sockaddr_in and always
// called inet_ntop(AF_INET, ...), so on this board's CONFIG_LWIP_IPV6=y
// build (the httpd socket is PF_INET6, family AF_INET6) it always saw
// "0.0.0.0" and never confirmed static-IP reachability. The existing
// test_static_wrong_address_keeps_ap_up()/test_static_correct_address_would_confirm()
// above pin g_stub_getsockname_family to AF_INET so their long-standing
// plain-string assertions keep meaning what they always meant; these three
// cover the fix itself, now that get_local_ipv4_string() (shared with
// wifi_prov_request_arrived_on_ap()) handles both families.
static void test_static_reachability_af_inet_matches(void)
{
    TEST_SECTION("wifi_prov_note_possible_static_reachability() -- plain AF_INET family, exact match posts");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_STATIC;
    strcpy(s_wifi.static_ip, "192.168.1.50");
    s_wifi.static_ip_confirmed = false;
    g_stub_getsockname_family = AF_INET;
    strcpy(g_stub_local_ip, "192.168.1.50");

    wifi_prov_note_possible_static_reachability(3);
    TEST_CHECK(g_stub_queue_send_calls == 1, "AF_INET exact match posts CMD_CONFIRM_STATIC_REACHABLE");
}

static void test_static_reachability_v4_mapped_af_inet6_matches(void)
{
    TEST_SECTION("wifi_prov_note_possible_static_reachability() -- IPv4-mapped AF_INET6 (the real shape on "
                 "hardware), exact match posts");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_STATIC;
    strcpy(s_wifi.static_ip, "192.168.1.50");
    s_wifi.static_ip_confirmed = false;
    g_stub_getsockname_family = AF_INET6;
    strcpy(g_stub_local_ip, "::ffff:192.168.1.50");

    wifi_prov_note_possible_static_reachability(3);
    TEST_CHECK(g_stub_queue_send_calls == 1,
               "mapped-AF_INET6 exact match posts CMD_CONFIRM_STATIC_REACHABLE -- this is the fix: before "
               "it, this case always read as \"0.0.0.0\" and never posted");
}

static void test_static_reachability_v4_mapped_af_inet6_no_match(void)
{
    TEST_SECTION("wifi_prov_note_possible_static_reachability() -- IPv4-mapped AF_INET6, non-matching "
                 "address does not post");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_STATIC;
    strcpy(s_wifi.static_ip, "192.168.1.50");
    s_wifi.static_ip_confirmed = false;
    g_stub_getsockname_family = AF_INET6;
    strcpy(g_stub_local_ip, "::ffff:192.168.4.1"); // the fallback AP's own address, not the static one

    wifi_prov_note_possible_static_reachability(3);
    TEST_CHECK(g_stub_queue_send_calls == 0, "a mismatched mapped address must not post");
    TEST_CHECK(!s_wifi.static_ip_confirmed, "still unconfirmed");
}

// Review fix (2026-09-29, round 3): a legacy 192.168.4.x static IP persisted
// in NVS from before wifi_prov_set_static_ip() refused to write one -- the
// setter guard can't protect a config that predates it, so
// wifi_prov_note_possible_static_reachability() must refuse to CONFIRM one
// too, even on an exact match. Without this guard an AP client's own request
// would "confirm" the AP's own address as reachable and tear the AP down out
// from under itself.
static void test_static_reachability_refuses_legacy_ap_subnet_static_ip(void)
{
    TEST_SECTION("wifi_prov_note_possible_static_reachability() -- refuses to confirm a legacy "
                 "192.168.4.x static IP already in NVS, even on an exact match");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_STATIC;
    strcpy(s_wifi.static_ip, "192.168.4.50"); // legacy value; the setter would refuse this today
    s_wifi.static_ip_confirmed = false;
    g_stub_getsockname_family = AF_INET;
    strcpy(g_stub_local_ip, "192.168.4.50"); // exact match to static_ip

    wifi_prov_note_possible_static_reachability(3);
    TEST_CHECK(g_stub_queue_send_calls == 0,
               "an exact match inside 192.168.4.0/24 must not post CMD_CONFIRM_STATIC_REACHABLE");
    TEST_CHECK(!s_wifi.static_ip_confirmed, "still unconfirmed -- never treated as proof");
}

// OWN, SEPARATE executable (build_host_tests.ps1's exe51), not part of the
// "main" combined executable this file used to live in: the fakes below for
// http_auth_policy_web_enabled()/http_auth_any_session_active() (added
// 2026-09-28 for wifi_prov_link.c's new ap_teardown_should_defer()) collide
// at link time with the REAL http_auth_policy_iface.c/http_session_iface.c
// that "main" also links for test_web_auth_login_http.c and friends -- same
// reasoning as test_time_sync.c's own move, see that file's header comment.
// So this file supplies its own g_test_count/g_test_failures and main(),
// same convention as test_time_sync.c.
int g_test_failures = 0;
int g_test_count = 0;

// ---- WIFI_REVIEW_2026-10-09 fixes ----

static void put_raw_saved_nets(const void *data, size_t len)
{
    fake_kv_reset_all();
    TEST_CHECK(hal_kv_init_partition(WIFI_NVS_PARTITION) == HAL_OK, "setup: init wifi_nvs partition");
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, WIFI_NVS_PARTITION) == HAL_OK, "open");
    TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_SAVED_NETS, data, len) == HAL_OK, "seed saved_nets");
    hal_kv_commit(&h);
    hal_kv_close(&h);
}

static void test_dns_query_admission(void)
{
    TEST_SECTION("MED-1: dns_hijack answers only plain queries from the AP subnet (no responses, opcodes, port 53, off-subnet)");
    uint8_t q[12] = { 0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0 };
    const uint8_t ap_client_b[4] = { 192, 168, 4, 2 };
    const uint8_t off_b[4] = { 10, 0, 0, 5 };
    const uint8_t gw_b[4] = { 192, 168, 4, 1 };
    uint32_t ap_client, off, gw;
    memcpy(&ap_client, ap_client_b, 4);
    memcpy(&off, off_b, 4);
    memcpy(&gw, gw_b, 4);
    TEST_CHECK(wifi_prov_dns_query_acceptable(q, 12, ap_client, 40000), "plain query from an AP client is answered");
    uint8_t r[12];
    memcpy(r, q, 12);
    r[2] |= 0x80;
    TEST_CHECK(!wifi_prov_dns_query_acceptable(r, 12, ap_client, 40000), "QR=1 (a response) is dropped");
    uint8_t o[12];
    memcpy(o, q, 12);
    o[2] = 0x08; /* opcode 1 */
    TEST_CHECK(!wifi_prov_dns_query_acceptable(o, 12, ap_client, 40000), "non-zero opcode is dropped");
    TEST_CHECK(!wifi_prov_dns_query_acceptable(q, 12, ap_client, 53), "source port 53 is dropped (reflection loop)");
    TEST_CHECK(!wifi_prov_dns_query_acceptable(q, 11, ap_client, 40000), "short packet is dropped");
    TEST_CHECK(!wifi_prov_dns_query_acceptable(q, 12, off, 40000), "source outside the AP subnet is dropped");
    TEST_CHECK(!wifi_prov_dns_query_acceptable(q, 12, gw, 40000), "the AP's own address as source is dropped");
}

static void test_saved_nets_loader_hardening(void)
{
    TEST_SECTION("LOW-1/LOW-2: count > 8 refused, fields NUL-terminated; unreadable/newer record is present-but-unreadable");
    reset_state();
    saved_nets_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = SAVED_NETS_VERSION;
    b.count = 1;
    memset(b.nets[0].ssid, 'A', sizeof(b.nets[0].ssid));
    memset(b.nets[0].password, 'B', sizeof(b.nets[0].password));
    put_raw_saved_nets(&b, sizeof(b));
    saved_nets_blob_t rb;
    TEST_CHECK(nvs_load_saved_nets_from(WIFI_NVS_PARTITION, &rb) == ESP_OK && rb.count == 1, "unterminated record loads");
    TEST_CHECK(rb.nets[0].ssid[sizeof(rb.nets[0].ssid) - 1] == '\0', "ssid forced NUL-terminated");
    TEST_CHECK(rb.nets[0].password[sizeof(rb.nets[0].password) - 1] == '\0', "password forced NUL-terminated");
    TEST_CHECK(!s_saved_nets_refused, "a good record is not refused");

    b.count = 200;
    put_raw_saved_nets(&b, sizeof(b));
    TEST_CHECK(nvs_load_saved_nets_from(WIFI_NVS_PARTITION, &rb) == ESP_OK && rb.count == 0, "count > 8 loads as empty");
    TEST_CHECK(s_saved_nets_refused, "count > 8 marks the record refused");

    b.count = 1;
    b.version = SAVED_NETS_VERSION + 1;
    put_raw_saved_nets(&b, sizeof(b));
    reset_state();
    nvs_load_saved_nets();
    TEST_CHECK(s_saved_nets_refused, "newer-version record is refused");
    s_wifi.state = WIFI_PROV_STATE_UNPROVISIONED;
    TEST_CHECK(s_wifi.saved_nets.count == 0 && !wifi_prov_is_unprovisioned(),
               "refused record: board is NOT unprovisioned");
    s_wifi.started = true;
    TEST_CHECK(wifi_prov_add_network("home", 4, "password1", 9) == ESP_ERR_NOT_SUPPORTED, "add refused");
    TEST_CHECK(wifi_prov_forget_network("home", 4) == ESP_ERR_NOT_SUPPORTED, "forget refused");
    TEST_CHECK(g_stub_queue_send_calls == 0, "refused add/forget post nothing");
    saved_nets_blob_t after;
    size_t len = sizeof(after);
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, WIFI_NVS_PARTITION) == HAL_OK, "reopen");
    TEST_CHECK(hal_kv_get_blob(&h, NVS_KEY_SAVED_NETS, &after, &len) == HAL_OK && len == sizeof(after) &&
                   after.version == SAVED_NETS_VERSION + 1 && after.count == 1,
               "the newer record is left untouched");
    hal_kv_close(&h);

    uint8_t junk[5] = { 1, 1, 0, 0, 0 };
    put_raw_saved_nets(junk, sizeof(junk));
    reset_state();
    nvs_load_saved_nets();
    TEST_CHECK(s_saved_nets_refused, "wrong-size record is refused");

    b.version = SAVED_NETS_VERSION + 1;
    put_raw_saved_nets(&b, sizeof(b));
    reset_state();
    memset(&s_legacy_single, 0, sizeof(s_legacy_single));
    s_legacy_single.has = true;
    strcpy(s_legacy_single.net.ssid, "oldnet");
    strcpy(s_legacy_single.net.password, "oldpass");
    nvs_load_saved_nets();
    len = sizeof(after);
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, WIFI_NVS_PARTITION) == HAL_OK, "reopen");
    TEST_CHECK(hal_kv_get_blob(&h, NVS_KEY_SAVED_NETS, &after, &len) == HAL_OK && after.version == SAVED_NETS_VERSION + 1,
               "legacy migration does not overwrite a refused record");
    hal_kv_close(&h);
    memset(&s_legacy_single, 0, sizeof(s_legacy_single));
    s_saved_nets_refused = false;
}

static void test_ap_setters_reject_nul(void)
{
    TEST_SECTION("LOW-3: AP ssid/password setters refuse embedded NUL; shared arg validators agree");
    reset_state();
    s_wifi.started = true;
    TEST_CHECK(wifi_prov_set_ap_ssid("ab\0cd", 5) == ESP_ERR_INVALID_ARG, "ssid with NUL refused");
    TEST_CHECK(wifi_prov_set_ap_password("pass\0word1", 9) == ESP_ERR_INVALID_ARG, "password with NUL refused");
    TEST_CHECK(g_stub_queue_send_calls == 0, "nothing posted");
    TEST_CHECK(wifi_prov_set_ap_ssid("abcd", 4) == ESP_ERR_TIMEOUT, "clean ssid reaches the post");
    TEST_CHECK(wifi_prov_set_ap_password("password1", 9) == ESP_ERR_TIMEOUT, "clean password reaches the post");
    TEST_CHECK(!wifi_prov_ap_ssid_arg_valid("ab\0cd", 5) && wifi_prov_ap_ssid_arg_valid("abcd", 4), "ssid validator");
    TEST_CHECK(!wifi_prov_ap_password_arg_valid("pass\0word1", 9) && !wifi_prov_ap_password_arg_valid("short", 5) &&
                   wifi_prov_ap_password_arg_valid("", 0) && wifi_prov_ap_password_arg_valid("password1", 9),
               "password validator");
}

static void test_static_ip_strict_validation(void)
{
    TEST_SECTION("LOW-4: static IP config validation (contiguous mask, gateway in subnet, no special addresses, no shorthand)");
    TEST_CHECK(wifi_prov_static_ip_config_valid("192.168.1.50", "255.255.255.0", "192.168.1.1"), "normal config");
    TEST_CHECK(wifi_prov_static_ip_config_valid("10.0.0.5", "255.0.0.0", "10.0.0.1"), "/8 config");
    TEST_CHECK(!wifi_prov_static_ip_config_valid("192.168.1.50", "255.0.255.0", "192.168.1.1"), "non-contiguous mask");
    TEST_CHECK(!wifi_prov_static_ip_config_valid("192.168.1.50", "0.0.0.0", "192.168.1.1"), "zero mask");
    TEST_CHECK(!wifi_prov_static_ip_config_valid("192.168.1.50", "255.255.255.255", "192.168.1.1"), "/32 mask");
    TEST_CHECK(!wifi_prov_static_ip_config_valid("192.168.1.50", "255.255.255.0", "192.168.2.1"), "gateway outside subnet");
    TEST_CHECK(!wifi_prov_static_ip_config_valid("0.0.0.0", "255.255.255.0", "192.168.1.1"), "0.0.0.0 ip");
    TEST_CHECK(!wifi_prov_static_ip_config_valid("192.168.1.50", "255.255.255.0", "0.0.0.0"), "0.0.0.0 gateway");
    TEST_CHECK(!wifi_prov_static_ip_config_valid("127.0.0.2", "255.0.0.0", "127.0.0.1"), "loopback");
    TEST_CHECK(!wifi_prov_static_ip_config_valid("239.1.1.5", "255.0.0.0", "239.1.1.1"), "multicast");
    TEST_CHECK(!wifi_prov_static_ip_config_valid("255.255.255.255", "255.255.255.0", "192.168.1.1"), "broadcast");
    TEST_CHECK(!wifi_prov_static_ip_config_valid("192.168.1.0", "255.255.255.0", "192.168.1.1"), "network address as host");
    TEST_CHECK(!wifi_prov_static_ip_config_valid("192.168.1.255", "255.255.255.0", "192.168.1.1"), "broadcast address as host");
    TEST_CHECK(!wifi_prov_static_ip_config_valid("192.168.1.50", "255.255.255.0", "192.168.1.255"), "gateway = broadcast");
    TEST_CHECK(!wifi_prov_static_ip_config_valid("192.168.1.50", "255.255.255.0", "192.168.1.50"), "gateway = ip");
    TEST_CHECK(!wifi_prov_static_ip_config_valid("10.1", "255.255.255.0", "10.0.0.1"), "shorthand ip");
    TEST_CHECK(!wifi_prov_static_ip_config_valid("192.168.001.50", "255.255.255.0", "192.168.1.1"), "leading zero");
    TEST_CHECK(!wifi_prov_static_ip_config_valid("192.168.1.50 ", "255.255.255.0", "192.168.1.1"), "trailing space");
    TEST_CHECK(!wifi_prov_static_ip_config_valid("192.168.1.50", "255.255.255.0", ""), "empty gateway");
    reset_state();
    s_wifi.started = true;
    TEST_CHECK(wifi_prov_set_static_ip("10.1", "255.255.255.0", "10.0.0.1", NULL, NULL) == ESP_ERR_INVALID_ARG,
               "setter refuses shorthand");
    TEST_CHECK(wifi_prov_set_static_ip("192.168.1.50", "255.0.255.0", "192.168.1.1", NULL, NULL) == ESP_ERR_INVALID_ARG,
               "setter refuses non-contiguous mask");
    TEST_CHECK(wifi_prov_set_static_ip("192.168.1.50", "255.255.255.0", "192.168.1.1", "1.1", NULL) == ESP_ERR_INVALID_ARG,
               "setter refuses shorthand dns");
}

static void test_sta_wpa2_min_pmf_capable(void)
{
    TEST_SECTION("LOW-5: STA config requires WPA2 minimum, PMF capable but not required");
    reset_state();
    s_wifi.sta_netif = esp_netif_create_default_wifi_sta();
    strcpy(s_wifi.active_ssid, "home");
    strcpy(s_wifi.active_password, "password1");
    apply_sta_config();
    wifi_config_t *c = stub_wifi_last_sta_cfg();
    TEST_CHECK(c->sta.threshold.authmode == WIFI_AUTH_WPA2_PSK, "authmode threshold is WPA2-PSK");
    TEST_CHECK(c->sta.pmf_cfg.capable && !c->sta.pmf_cfg.required, "PMF capable, not required");
}

static void test_status_ap_password_gate(void)
{
    TEST_SECTION("/api/wifi/status AP password view: only on_ap requests see it");
    TEST_CHECK(strcmp(wifi_prov_status_ap_password_view(true, "secretpw1"), "secretpw1") == 0, "on_ap sees it");
    TEST_CHECK(wifi_prov_status_ap_password_view(false, "secretpw1")[0] == '\0', "off-AP gets empty");
    TEST_CHECK(wifi_prov_status_ap_password_view(true, NULL)[0] == '\0', "NULL password is empty");
}

extern bool fake_kv_script_next_open_status(const char *namespace_name, hal_status_t status);

static void test_refused_record_never_readopts_legacy(void)
{
    TEST_SECTION("F1: refused (newer) saved_nets record + surviving legacy wifi_cfg: legacy is never adopted or erased");
    seed_legacy_default_wifi();
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, WIFI_NVS_PARTITION) == HAL_OK, "open wifi_nvs");
    TEST_CHECK(hal_kv_set_u8(&h, NVS_KEY_MODE, 1) == HAL_OK, "user-chosen mode (differs from legacy 0)");
    saved_nets_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = SAVED_NETS_VERSION + 1;
    b.count = 1;
    TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_SAVED_NETS, &b, sizeof(b)) == HAL_OK, "newer record");
    hal_kv_commit(&h);
    hal_kv_close(&h);
    for (int boot = 0; boot < 2; boot++) {
        boot_wifi_migration();
        TEST_CHECK(s_saved_nets_refused, "record stays refused");
        uint8_t mode = 99;
        TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, WIFI_NVS_PARTITION) == HAL_OK, "reopen wifi_nvs");
        TEST_CHECK(hal_kv_get_u8(&h, NVS_KEY_MODE, &mode) == HAL_OK && mode == 1, "wifi_nvs mode not overwritten by legacy");
        hal_kv_close(&h);
        char ssid[40];
        size_t len = sizeof(ssid);
        TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, NULL) == HAL_OK, "legacy namespace survives");
        TEST_CHECK(hal_kv_get_str(&h, NVS_KEY_SSID, ssid, &len) == HAL_OK && strcmp(ssid, "oldnet") == 0,
                   "legacy ssid not erased");
        hal_kv_close(&h);
    }
    memset(&s_legacy_single, 0, sizeof(s_legacy_single));
    s_saved_nets_refused = false;
}

static void test_transient_read_error_is_retried_and_hint(void)
{
    TEST_SECTION("F3: a transient open error is retried before latching refused; refused exposes a recovery hint");
    saved_nets_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = SAVED_NETS_VERSION;
    b.count = 1;
    strcpy(b.nets[0].ssid, "home");
    put_raw_saved_nets(&b, sizeof(b));
    reset_state();
    TEST_CHECK(fake_kv_script_next_open_status(NVS_NAMESPACE, HAL_IO), "script one open failure");
    saved_nets_blob_t rb;
    uint64_t t0_ms = hal_time_now_ms();
    TEST_CHECK(nvs_load_saved_nets_from(WIFI_NVS_PARTITION, &rb) == ESP_OK && rb.count == 1 &&
                   strcmp(rb.nets[0].ssid, "home") == 0,
               "load succeeded on retry");
    TEST_CHECK(hal_time_now_ms() - t0_ms >= 20, "INFO-3: a delay separates the retry from the failed read");
    TEST_CHECK(!s_saved_nets_refused, "transient error did not latch refused");
    TEST_CHECK(wifi_prov_saved_nets_recovery_hint() == NULL, "no hint when healthy");
    b.version = SAVED_NETS_VERSION + 1;
    put_raw_saved_nets(&b, sizeof(b));
    nvs_load_saved_nets_from(WIFI_NVS_PARTITION, &rb);
    TEST_CHECK(s_saved_nets_refused, "newer record refused (not retried into success)");
    const char *hint = wifi_prov_saved_nets_recovery_hint();
    TEST_CHECK(hint && strstr(hint, "factory reset") && strstr(hint, "wifi"), "hint names factory reset scope wifi");
    s_saved_nets_refused = false;
}

static void test_static_ip_problem_text(void)
{
    TEST_SECTION("F4: static IP problem names the specific rule");
    const char *nm = "255.255.255.0";
    TEST_CHECK(wifi_prov_static_ip_config_problem("192.168.1.50", nm, "192.168.1.1") == NULL, "valid -> NULL");
    const char *p = wifi_prov_static_ip_config_problem("192.168.1.50", nm, "192.168.2.1");
    TEST_CHECK(p && strstr(p, "gateway is outside"), "off-subnet gateway");
    p = wifi_prov_static_ip_config_problem("192.168.1.50", "255.255.255.255", "192.168.1.1");
    TEST_CHECK(p && strstr(p, "/31"), "/32 mask");
    p = wifi_prov_static_ip_config_problem("192.168.001.50", nm, "192.168.1.1");
    TEST_CHECK(p && strstr(p, "leading zeros"), "leading-zero octet");
    p = wifi_prov_static_ip_config_problem("192.168.1.50", "255.0.255.0", "192.168.1.1");
    TEST_CHECK(p && strstr(p, "contiguous"), "non-contiguous mask");
    p = wifi_prov_static_ip_config_problem("192.168.1.255", nm, "192.168.1.1");
    TEST_CHECK(p && strstr(p, "ip is not a usable"), "broadcast ip");
    p = wifi_prov_static_ip_config_problem("192.168.1.50", nm, "192.168.1.50");
    TEST_CHECK(p && strstr(p, "must differ"), "gateway equals ip");
    p = wifi_prov_static_ip_config_problem("192.168.1.50", nm, "192.168.1.255");
    TEST_CHECK(p && strstr(p, "gateway is not a usable"), "broadcast gateway");
}

// ======================= R2-D: wifi_prov_api.c command bodies =======================

static void r2d_seed_nets(int n)
{
    reset_state();
    for (int i = 0; i < n; i++) {
        snprintf(s_wifi.saved_nets.nets[i].ssid, sizeof(s_wifi.saved_nets.nets[i].ssid), "net%d", i);
        snprintf(s_wifi.saved_nets.nets[i].password, sizeof(s_wifi.saved_nets.nets[i].password), "pw%d-xxxxxxxx", i);
    }
    s_wifi.saved_nets.count = (uint8_t)n;
}

static void test_r2d_add_network_upsert_full_and_persist(void)
{
    TEST_SECTION("R2-D do_add_network: upsert, full-list refusal, AP->HOME, cache refresh, NVS persistence");
    fake_kv_reset_all();
    TEST_CHECK(hal_kv_init_partition(WIFI_NVS_PARTITION) == HAL_OK, "setup: init wifi_nvs");
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default partition");
    reset_state();
    bool join = false;
    TEST_CHECK(do_add_network("alpha", "passwordA1", &join) == ESP_OK && join, "first add ok, join requested");
    TEST_CHECK(do_add_network("beta", "passwordB1", &join) == ESP_OK, "second add ok");
    TEST_CHECK(s_wifi.saved_nets.count == 2, "two saved");
    TEST_CHECK(strcmp(s_wifi.saved_nets.nets[1].ssid, "beta") == 0 && strcmp(s_wifi.saved_nets.nets[1].password, "passwordB1") == 0,
               "beta stored with its password");
    TEST_CHECK(do_add_network("alpha", "newpassA2", &join) == ESP_OK, "upsert ok");
    TEST_CHECK(s_wifi.saved_nets.count == 2, "upsert does not grow the list");
    TEST_CHECK(strcmp(s_wifi.saved_nets.nets[0].password, "newpassA2") == 0, "alpha password replaced in place");
    TEST_CHECK(strcmp(s_wifi.saved_nets.nets[1].ssid, "beta") == 0, "beta untouched, order kept");
    TEST_CHECK(do_add_network("alpha", "short", &join) == ESP_OK, "shorter password ok");
    TEST_CHECK(strcmp(s_wifi.saved_nets.nets[0].password, "short") == 0, "alpha password is exactly the new, shorter one");
    wifi_prov_saved_network_t c[WIFI_PROV_MAX_SAVED_NETWORKS];
    size_t cn = 99;
    wifi_prov_get_saved_networks_cached(c, WIFI_PROV_MAX_SAVED_NETWORKS, &cn);
    TEST_CHECK(cn == 2 && strcmp(c[0].ssid, "alpha") == 0 && strcmp(c[1].ssid, "beta") == 0, "cache refreshed by the add");
    simulate_reboot_state();
    nvs_load_saved_nets();
    TEST_CHECK(s_wifi.saved_nets.count == 2, "after reboot two networks load");
    TEST_CHECK(strcmp(s_wifi.saved_nets.nets[0].ssid, "alpha") == 0 && strcmp(s_wifi.saved_nets.nets[0].password, "short") == 0,
               "persisted alpha carries the updated password");

    r2d_seed_nets(WIFI_PROV_MAX_SAVED_NETWORKS);
    s_wifi.state = WIFI_PROV_STATE_UNPROVISIONED;
    join = true;
    TEST_CHECK(do_add_network("overflow", "passwordZ9", &join) == ESP_ERR_NO_MEM, "full list refuses a new SSID with ESP_ERR_NO_MEM");
    TEST_CHECK(!join, "refusal asks for no join");
    TEST_CHECK(s_wifi.state == WIFI_PROV_STATE_UNPROVISIONED, "refusal leaves state alone");
    TEST_CHECK(s_wifi.saved_nets.count == WIFI_PROV_MAX_SAVED_NETWORKS, "count unchanged");
    TEST_CHECK(strcmp(s_wifi.saved_nets.nets[0].ssid, "net0") == 0, "no entry evicted");
    TEST_CHECK(do_add_network("net3", "updatedpw3", &join) == ESP_OK && strcmp(s_wifi.saved_nets.nets[3].password, "updatedpw3") == 0,
               "an existing SSID in a full list can still be updated");

    r2d_seed_nets(0);
    s_wifi.mode = WIFI_PROV_MODE_AP;
    TEST_CHECK(do_add_network("gamma", "passwordG1", &join) == ESP_OK, "add while in AP mode ok");
    TEST_CHECK(s_wifi.mode == WIFI_PROV_MODE_HOME, "AP mode switched to HOME");
}

static void test_r2d_forget_network_compacts_and_persists(void)
{
    TEST_SECTION("R2-D do_forget_network: compaction order, zeroed tail, unknown SSID no-op, cache, persistence");
    fake_kv_reset_all();
    TEST_CHECK(hal_kv_init_partition(WIFI_NVS_PARTITION) == HAL_OK, "setup: init wifi_nvs");
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default partition");
    r2d_seed_nets(4);
    wifi_prov_update_saved_nets_cache();
    TEST_CHECK(do_forget_network("nope") == ESP_OK && s_wifi.saved_nets.count == 4, "unknown SSID: ESP_OK, nothing removed");
    TEST_CHECK(do_forget_network("net1") == ESP_OK, "forget middle ok");
    TEST_CHECK(s_wifi.saved_nets.count == 3, "count drops by one");
    TEST_CHECK(strcmp(s_wifi.saved_nets.nets[0].ssid, "net0") == 0 && strcmp(s_wifi.saved_nets.nets[1].ssid, "net2") == 0 &&
               strcmp(s_wifi.saved_nets.nets[2].ssid, "net3") == 0, "later entries shifted down, order preserved");
    TEST_CHECK(strcmp(s_wifi.saved_nets.nets[1].password, "pw2-xxxxxxxx") == 0, "shifted entry keeps its own password");
    uint8_t zero[sizeof(s_wifi.saved_nets.nets[0])];
    memset(zero, 0, sizeof(zero));
    TEST_CHECK(memcmp(&s_wifi.saved_nets.nets[3], zero, sizeof(zero)) == 0, "vacated tail slot is zeroed (no stale password)");
    wifi_prov_saved_network_t c[WIFI_PROV_MAX_SAVED_NETWORKS];
    size_t cn = 0;
    wifi_prov_get_saved_networks_cached(c, WIFI_PROV_MAX_SAVED_NETWORKS, &cn);
    TEST_CHECK(cn == 3 && strcmp(c[1].ssid, "net2") == 0, "cache follows the removal");
    simulate_reboot_state();
    nvs_load_saved_nets();
    TEST_CHECK(s_wifi.saved_nets.count == 3 && strcmp(s_wifi.saved_nets.nets[1].ssid, "net2") == 0, "removal persisted");
    TEST_CHECK(do_forget_network("net0") == ESP_OK && do_forget_network("net3") == ESP_OK, "forget first and last");
    TEST_CHECK(s_wifi.saved_nets.count == 1 && strcmp(s_wifi.saved_nets.nets[0].ssid, "net2") == 0, "one entry left, net2");
}

static void test_r2d_saved_networks_getters(void)
{
    TEST_SECTION("R2-D do_get_saved_networks and the cached reader: truncation, bad args");
    r2d_seed_nets(5);
    wifi_result_t r;
    memset(&r, 0, sizeof(r));
    TEST_CHECK(do_get_saved_networks(3, &r) == ESP_OK && r.saved_count == 3, "max_results 3 truncates to 3");
    TEST_CHECK(strcmp(r.saved[0].ssid, "net0") == 0 && strcmp(r.saved[2].ssid, "net2") == 0, "first three SSIDs in order");
    memset(&r, 0, sizeof(r));
    TEST_CHECK(do_get_saved_networks(100, &r) == ESP_OK && r.saved_count == 5, "large max returns the whole list");
    memset(&r, 0, sizeof(r));
    TEST_CHECK(do_get_saved_networks(0, &r) == ESP_OK && r.saved_count == 0, "max_results 0 returns none");

    r2d_seed_nets(5);
    wifi_prov_update_saved_nets_cache();
    wifi_prov_saved_network_t out[8];
    size_t n = 77;
    wifi_prov_get_saved_networks_cached(out, 2, &n);
    TEST_CHECK(n == 2 && strcmp(out[1].ssid, "net1") == 0, "cached reader truncates to max_results");
    n = 77;
    wifi_prov_get_saved_networks_cached(NULL, 4, &n);
    TEST_CHECK(n == 0, "NULL out gives count 0");
    n = 77;
    wifi_prov_get_saved_networks_cached(out, 0, &n);
    TEST_CHECK(n == 0, "max_results 0 gives count 0");
    s_wifi.saved_nets.count = 1;
    n = 0;
    wifi_prov_get_saved_networks_cached(out, 8, &n);
    TEST_CHECK(n == 5, "cache stays at the last published snapshot");
    wifi_prov_update_saved_nets_cache();
    wifi_prov_get_saved_networks_cached(out, 8, &n);
    TEST_CHECK(n == 1, "update republishes");

    reset_state();
    size_t cnt = 5;
    TEST_CHECK(wifi_prov_get_saved_networks(NULL, 4, &cnt) == ESP_ERR_INVALID_ARG, "NULL out refused");
    TEST_CHECK(wifi_prov_get_saved_networks(out, 0, &cnt) == ESP_ERR_INVALID_ARG, "max 0 refused");
    TEST_CHECK(wifi_prov_get_saved_networks(out, 4, NULL) == ESP_ERR_INVALID_ARG, "NULL count refused");
    TEST_CHECK(wifi_prov_get_saved_networks(out, 4, &cnt) == ESP_ERR_INVALID_STATE && cnt == 0, "not started: INVALID_STATE, count zeroed");
    s_wifi.started = true;
    TEST_CHECK(wifi_prov_get_saved_networks(out, 4, &cnt) == ESP_ERR_TIMEOUT && cnt == 0, "started but owner never replies: TIMEOUT");
}

static void test_r2d_simple_getters(void)
{
    TEST_SECTION("R2-D direct-read getters: connected, pending teardown, ip mode, rssi, AP clients");
    reset_state();
    TEST_CHECK(!wifi_prov_is_sta_connected(), "not connected by default");
    s_wifi.state = WIFI_PROV_STATE_CONNECTED;
    TEST_CHECK(wifi_prov_is_sta_connected(), "CONNECTED reports true");
    s_wifi.state = WIFI_PROV_STATE_CONNECTING;
    TEST_CHECK(!wifi_prov_is_sta_connected(), "CONNECTING is not connected");
    TEST_CHECK(!wifi_prov_get_ap_pending_teardown(), "no pending teardown by default");
    s_wifi.ap_pending_teardown = true;
    TEST_CHECK(wifi_prov_get_ap_pending_teardown(), "pending teardown reported");
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_STATIC;
    strcpy(s_wifi.static_ip, "10.1.2.3");
    strcpy(s_wifi.static_netmask, "255.255.0.0");
    strcpy(s_wifi.static_gateway, "10.1.0.1");
    strcpy(s_wifi.static_dns, "9.9.9.9");
    strcpy(s_wifi.static_dns2, "8.8.4.4");
    TEST_CHECK(wifi_prov_get_ip_mode() == WIFI_PROV_IP_MODE_STATIC, "ip mode static");
    TEST_CHECK(strcmp(wifi_prov_get_static_ip(), "10.1.2.3") == 0 && strcmp(wifi_prov_get_static_netmask(), "255.255.0.0") == 0 &&
               strcmp(wifi_prov_get_static_gateway(), "10.1.0.1") == 0 && strcmp(wifi_prov_get_static_dns(), "9.9.9.9") == 0 &&
               strcmp(wifi_prov_get_static_dns2(), "8.8.4.4") == 0, "static strings returned verbatim");
}

static void test_r2d_sta_ip_cache(void)
{
    TEST_SECTION("R2-D STA ip cache");
    char ip[16], nm[16];
    wifi_prov_update_sta_ip_cache("192.168.1.20", "255.255.255.0");
    TEST_CHECK(wifi_prov_get_cached_sta_ip_netmask(ip, sizeof(ip), nm, sizeof(nm)) == ESP_OK &&
               strcmp(ip, "192.168.1.20") == 0 && strcmp(nm, "255.255.255.0") == 0, "cache round trip");
    TEST_CHECK(wifi_prov_get_cached_sta_ip_netmask(NULL, 16, nm, 16) == ESP_ERR_INVALID_ARG, "NULL ip refused");
    TEST_CHECK(wifi_prov_get_cached_sta_ip_netmask(ip, 0, nm, 16) == ESP_ERR_INVALID_ARG, "ip cap 0 refused");
    TEST_CHECK(wifi_prov_get_cached_sta_ip_netmask(ip, 16, NULL, 16) == ESP_ERR_INVALID_ARG, "NULL netmask refused");
    TEST_CHECK(wifi_prov_get_cached_sta_ip_netmask(ip, 16, nm, 0) == ESP_ERR_INVALID_ARG, "netmask cap 0 refused");
    wifi_prov_update_sta_ip_cache(NULL, NULL);
    TEST_CHECK(wifi_prov_get_cached_sta_ip_netmask(ip, sizeof(ip), nm, sizeof(nm)) == ESP_OK && ip[0] == '\0' && nm[0] == '\0',
               "NULL update clears the cache");
}

static void r2d_set_rec(int i, const char *ssid, int8_t rssi, wifi_auth_mode_t auth)
{
    memset(&g_r2d_scan_recs[i], 0, sizeof(g_r2d_scan_recs[i]));
    strncpy((char *)g_r2d_scan_recs[i].ssid, ssid, sizeof(g_r2d_scan_recs[i].ssid) - 1);
    g_r2d_scan_recs[i].rssi = rssi;
    g_r2d_scan_recs[i].authmode = auth;
}

static void test_r2d_do_scan(void)
{
    TEST_SECTION("R2-D do_scan: AP-mode refusal, STA bring-up, result mapping and caps");
    wifi_prov_scan_result_t res[30];
    size_t n = 99;
    reset_state();
    g_r2d_scan_start_calls = 0;
    g_r2d_scan_n = 0;
    g_r2d_scan_start_rc = ESP_OK;
    g_r2d_scan_recs_rc = ESP_OK;
    s_wifi.mode = WIFI_PROV_MODE_AP;
    TEST_CHECK(do_scan(res, 30, &n) == ESP_ERR_NOT_SUPPORTED && n == 0 && g_r2d_scan_start_calls == 0,
               "AP mode: refused, no scan started");
    s_wifi.mode = WIFI_PROV_MODE_HOME;
    g_stub_wifi_mode = WIFI_MODE_AP;
    TEST_CHECK(do_scan(res, 30, &n) == ESP_OK && n == 0, "AP-only radio, zero results");
    TEST_CHECK(g_stub_wifi_mode == WIFI_MODE_APSTA, "AP-only radio brought up to APSTA first");
    TEST_CHECK(g_r2d_scan_start_calls == 1, "scan started once");

    g_stub_wifi_mode = WIFI_MODE_STA;
    r2d_set_rec(0, "OpenNet", -40, WIFI_AUTH_OPEN);
    r2d_set_rec(1, "SecureNet", -72, (wifi_auth_mode_t)3);
    g_r2d_scan_n = 2;
    TEST_CHECK(do_scan(res, 30, &n) == ESP_OK && n == 2, "two results");
    TEST_CHECK(g_stub_wifi_mode == WIFI_MODE_STA, "STA radio mode untouched");
    TEST_CHECK(strcmp(res[0].ssid, "OpenNet") == 0 && res[0].rssi == -40 && !res[0].secure, "open network: secure=false");
    TEST_CHECK(strcmp(res[1].ssid, "SecureNet") == 0 && res[1].rssi == -72 && res[1].secure, "protected network: secure=true");

    TEST_CHECK(do_scan(res, 1, &n) == ESP_OK && n == 1 && strcmp(res[0].ssid, "OpenNet") == 0, "max_results 1 truncates");
    TEST_CHECK(g_r2d_last_fetch_request == 1, "driver asked for only max_results records");

    for (int i = 0; i < 25; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "N%02d", i);
        r2d_set_rec(i, nm, (int8_t)(-30 - i), WIFI_AUTH_OPEN);
    }
    g_r2d_scan_n = 25;
    TEST_CHECK(do_scan(res, 30, &n) == ESP_OK && n == 20, "internal record buffer caps at 20");
    TEST_CHECK(g_r2d_last_fetch_request == 20 && strcmp(res[19].ssid, "N19") == 0, "20th record is the last returned");

    g_r2d_scan_start_rc = ESP_ERR_INVALID_STATE;
    n = 99;
    TEST_CHECK(do_scan(res, 30, &n) == ESP_ERR_INVALID_STATE && n == 0, "scan_start failure propagates");
    g_r2d_scan_start_rc = ESP_OK;
    g_r2d_scan_recs_rc = ESP_FAIL;
    TEST_CHECK(do_scan(res, 30, &n) == ESP_FAIL && n == 0, "get_ap_records failure propagates, nothing reported");
    g_r2d_scan_recs_rc = ESP_OK;
    g_r2d_scan_n = 0;
}

void run_test_wifi_prov(void)
{
    test_refused_record_never_readopts_legacy();
    test_transient_read_error_is_retried_and_hint();
    test_static_ip_problem_text();
    test_static_ip_confirmed_false_at_boot();
    test_wifi_prov_start_sets_ram_storage();
    test_static_wrong_address_keeps_ap_up();
    test_static_correct_address_would_confirm();
    test_confirm_static_reachable_drops_ap();
    test_confirm_static_reachable_state_changed_since();
    test_dhcp_got_ip_drops_ap_immediately();
    test_static_already_confirmed_got_ip_drops_ap();
    test_set_static_ip_resets_confirmation();
    test_set_static_ip_rejects_ap_subnet();
    test_set_static_ip_dns_validation();
    test_set_static_ip_stores_and_clears_dns();
    test_dns_nvs_round_trip();
    test_legacy_default_wifi_erase_keeps_board_unprovisioned();
    test_empty_legacy_namespace_is_not_found();
    test_main_load_keeps_ap_mode_saved_as_u8_only();
    test_legacy_wifi_migration_is_one_shot();
    test_interrupted_first_migration_retries_next_boot();
    test_legacy_migration_failures_never_lose_credential();
    test_forget_last_network_survives_reboot();
    test_embedded_nul_refused();
    test_mode_and_ap_name_change_survives_reboot();
    test_apply_sta_config_dns();
    test_dns_query_admission();
    test_saved_nets_loader_hardening();
    test_ap_setters_reject_nul();
    test_static_ip_strict_validation();
    test_sta_wpa2_min_pmf_capable();
    test_status_ap_password_gate();
    test_set_dhcp_resets_confirmation();
    test_reply_slot_normal_roundtrip();
    test_reply_slot_abandon_then_owner_recycles();
    test_reply_slot_timeout_races_a_completed_reply_never_leaks();
    test_reply_slot_stale_generation_never_matches_after_reuse();
    test_do_add_network_sets_connecting_and_defers_the_join();
    test_do_set_mode_home_sets_connecting_and_defers_the_join();
    test_is_unprovisioned_requires_no_saved_network();
    test_ap_fallback_tick_raises_ap_after_timeout();
    test_got_ip_defers_ap_teardown_while_ap_session_active();
    test_got_ip_drops_ap_for_lan_only_session_even_with_auth_on();
    test_got_ip_defers_ap_teardown_auth_on_with_ap_client_but_no_session();
    test_got_ip_drops_ap_when_no_session_active();
    test_got_ip_defers_ap_teardown_auth_off_with_ap_client();
    test_got_ip_drops_ap_auth_off_no_ap_client();
    test_confirm_static_reachable_defers_ap_teardown_while_session_active();
    test_rescan_tick_retries_deferred_teardown_until_session_ends();
    test_ap_fallback_tick_leaves_running_ap_alone();
    test_disconnect_while_fallback_ap_up_waits_for_rescan();
    test_reconcile_sta_state_drops_ap_when_link_regained();
    test_reconcile_sta_state_defers_ap_drop_while_session_active();
    test_reconcile_sta_state_no_ap_action_when_state_already_agreed();
    test_reconcile_sta_state_never_touches_operator_chosen_ap();
    test_got_ip_after_reconcile_dropped_ap_is_a_no_op();
    test_teardown_and_mode_changes_clear_fallback_active();
    test_arrived_on_ap_plain_af_inet_not_matching();
    test_arrived_on_ap_plain_af_inet_matches();
    test_arrived_on_ap_v4_mapped_af_inet6_matches();
    test_arrived_on_ap_v4_mapped_af_inet6_uppercase_matches();
    test_arrived_on_ap_v4_mapped_af_inet6_lan_address_no_match();
    test_arrived_on_ap_getsockname_failure_fails_closed();
    test_static_reachability_af_inet_matches();
    test_static_reachability_v4_mapped_af_inet6_matches();
    test_static_reachability_v4_mapped_af_inet6_no_match();
    test_static_reachability_refuses_legacy_ap_subnet_static_ip();
    test_r2d_add_network_upsert_full_and_persist();
    test_r2d_forget_network_compacts_and_persists();
    test_r2d_saved_networks_getters();
    test_r2d_simple_getters();
    test_r2d_sta_ip_cache();
    test_r2d_do_scan();
}

int main(void)
{
    run_test_wifi_prov();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
