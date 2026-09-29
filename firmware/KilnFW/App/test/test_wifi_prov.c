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
char g_stub_local_ip[16] = "0.0.0.0";
int g_stub_queue_send_calls = 0;
unsigned char g_stub_last_queue_item[256];
// esp_wifi_ap_get_sta_list()'s reported station count (2026-09-28) -- drives
// wifi_prov_get_ap_client_count(), which ap_teardown_should_defer() (wifi_prov_link.c)
// consults when web auth is off. See stubs/esp_wifi.h.
int g_stub_ap_sta_count = 0;

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

// http_auth_policy_web_enabled()/http_auth_any_session_active() are declared
// by the real http_auth_policy_iface.h/http_session_iface.h (off-limits --
// their real bodies pull in PSA crypto/hal_time/web_auth_store, none of which
// this narrow host-test build links) and called from wifi_prov_link.c's new
// ap_teardown_should_defer() (2026-09-28 owner request: "reconnect to wifi
// ... when there are no users logged in to the website"). Fakes here are
// directly controllable (unlike time_sync_notify_got_ip() above, which only
// counts) so tests can drive both branches of ap_teardown_should_defer().
bool g_stub_web_auth_enabled = false;
bool http_auth_policy_web_enabled(void) { return g_stub_web_auth_enabled; }
bool g_stub_any_session_active = false;
bool http_auth_any_session_active(void) { return g_stub_any_session_active; }

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
    g_stub_any_session_active = false;
    g_stub_ap_sta_count = 0;
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

// ---- W1 reply-slot-pool tests (docs/HTTP_POST_OWNER_MIGRATION_PLAN.md) ----
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

static void test_got_ip_defers_ap_teardown_while_session_active(void)
{
    TEST_SECTION("do_ev_got_ip -- home Wi-Fi back, web auth ON and a session IS active: "
                 "AP teardown is deferred, never cut off a logged-in operator");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    s_wifi.state = WIFI_PROV_STATE_RECONNECTING; // was in fallback, home came back
    g_stub_web_auth_enabled = true;
    g_stub_any_session_active = true;

    do_ev_got_ip();

    TEST_CHECK(s_wifi.state == WIFI_PROV_STATE_CONNECTED, "join still reaches CONNECTED");
    TEST_CHECK(g_stub_wifi_set_mode_calls == 0, "esp_wifi_set_mode() is NOT called -- AP teardown deferred");
    TEST_CHECK(s_wifi.ap_pending_teardown, "ap_pending_teardown is set so do_rescan_tick() retries later");
}

static void test_got_ip_drops_ap_when_no_session_active(void)
{
    TEST_SECTION("do_ev_got_ip -- home Wi-Fi back, web auth ON but NO session active: AP comes down immediately");

    reset_state();
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    s_wifi.state = WIFI_PROV_STATE_RECONNECTING;
    g_stub_web_auth_enabled = true;
    g_stub_any_session_active = false;

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
    g_stub_any_session_active = true;

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
    g_stub_any_session_active = true;

    do_rescan_tick();
    TEST_CHECK(g_stub_wifi_set_mode_calls == 0, "still logged in on this tick -- AP left up, retried next tick");
    TEST_CHECK(s_wifi.ap_pending_teardown, "still pending");

    g_stub_any_session_active = false; // the operator logged out (or session expired) between ticks
    do_rescan_tick();
    TEST_CHECK(g_stub_wifi_set_mode_calls == 1, "nobody logged in any more -- the deferred teardown completes");
    TEST_CHECK(g_stub_wifi_mode == WIFI_MODE_STA, "radio mode is now STA-only");
    TEST_CHECK(!s_wifi.ap_pending_teardown, "flag cleared once the teardown actually runs");
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
    test_reply_slot_normal_roundtrip();
    test_reply_slot_abandon_then_owner_recycles();
    test_reply_slot_timeout_races_a_completed_reply_never_leaks();
    test_reply_slot_stale_generation_never_matches_after_reuse();
    test_do_add_network_sets_connecting_and_defers_the_join();
    test_do_set_mode_home_sets_connecting_and_defers_the_join();
    test_is_unprovisioned_requires_no_saved_network();
    test_ap_fallback_tick_raises_ap_after_timeout();
    test_got_ip_defers_ap_teardown_while_session_active();
    test_got_ip_drops_ap_when_no_session_active();
    test_got_ip_defers_ap_teardown_auth_off_with_ap_client();
    test_got_ip_drops_ap_auth_off_no_ap_client();
    test_confirm_static_reachable_defers_ap_teardown_while_session_active();
    test_rescan_tick_retries_deferred_teardown_until_session_ends();
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
