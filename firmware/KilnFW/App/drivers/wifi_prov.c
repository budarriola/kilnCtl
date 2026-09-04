/* Command-queue infra, wifi_prov_start() bring-up, and owner_task() itself.
 *
 * 2026-09-04 split (ROADMAP.md M15 A3, "files over 1500 lines should be
 * broken up where it makes sense" -- this file had grown to 2820 lines, the
 * KNOWN HAZARD notwithstanding: ARCHITECTURE.md calls the Wi-Fi driver event
 * callbacks the riskiest, least hardware-verified code in the repo). See
 * wifi_prov_internal.h's top-of-file comment for the full split rationale
 * and file map -- wifi_prov_nvs.c (NVS load/save/migration),
 * wifi_prov_link.c (driver config, event handlers, timers, DNS hijack) and
 * wifi_prov_api.c (network/mode/AP/IP-mode command bodies + producers) are
 * its three siblings. MOVE-ONLY: no logic, ordering, naming or visibility
 * change beyond what moving required.
 *
 * ---- Owning task + command queue (2026-08-19, TODO.md 10.14 Phase 4) ----
 *
 * Everything s_wifi (wifi_prov_internal.h) holds had, until this phase, ZERO
 * locking and FOUR independent writers: the Wi-Fi driver's own
 * default-event-loop task (on_wifi_event()/on_ip_event()), the esp_timer
 * service task (ap_fallback_timer_cb()/rescan_timer_cb()), lvgl_port_task via
 * ui_page_network.c, and esp_http_server's worker task via
 * wifi_provision_http.c -- plus uart_bridge_ext.c's wifi bridge task. Phase 4
 * gives s_wifi exactly one writer: owner_task() below.
 *
 * This is deliberately NOT the shape of Phases 1 and 2 (kiln_io_owner.c /
 * thermo_owner.c, each a new file wrapping a driver whose own API was already
 * public). s_wifi is module-private and there is nothing to wrap: the owner
 * task lives inside this file, and the public wifi_prov_*() API keeps its
 * EXACT existing signatures so none of the seven caller modules
 * (ui_page_network.c, wifi_provision_http.c, uart_bridge_ext.c,
 * wifi_status_ui.c, readiness_http.c, ota_http.c, main.c) needed a single
 * edit. Each public entry point became a thin producer: build a command, post
 * it, wait (bounded) for the answer. The real body of each moved into a
 * do_*() static that only ever runs on owner_task().
 *
 * What makes this phase materially riskier than 1 and 2, and why the plan put
 * it last: the Wi-Fi DRIVER'S OWN callbacks had to be rerouted too. An event
 * handler registered with esp_event_handler_instance_register() runs on the
 * event loop's task, and an esp_timer callback runs on the timer service
 * task; if those kept mutating s_wifi directly, "one writer" would be a
 * fiction. So on_wifi_event()/on_ip_event()/ap_fallback_timer_cb()/
 * rescan_timer_cb() are now nothing but post_event() calls -- fire-and-forget,
 * xQueueSend with a 0-tick timeout, never blocking the event loop or the
 * timer service on this module's queue.
 *
 * DROPPED EVENTS ARE SURVIVABLE BY DESIGN, and that is load-bearing for the
 * 0-tick post above. If the queue is full the event is logged and discarded;
 * nothing retries it. Every event this module consumes self-heals:
 *   - a dropped STA_START: the join it would have kicked off is retried by
 *     rescan_timer_cb() within WIFI_AP_FALLBACK_RESCAN_INTERVAL_MS.
 *   - a dropped STA_DISCONNECTED: the driver keeps emitting disconnect
 *     events while the link is down, and the rescan timer independently
 *     re-picks a candidate and reconnects.
 *   - a dropped GOT_IP: state stays CONNECTING/RECONNECTING one beat too
 *     long (the AP is left up -- the SAFE direction, never "reports
 *     connected when it isn't"); the next got-ip, disconnect, or rescan tick
 *     corrects it.
 *   - a dropped AP-fallback or rescan tick: both timers fire again.
 * The queue only fills if something upstream is posting far faster than the
 * radio can act, which is itself the bug worth seeing in the log.
 *
 * DEADLOCK RULE, non-negotiable: code running ON owner_task() must never call
 * a public wifi_prov_*() producer -- it would post to its own queue and then
 * wait forever for itself to drain it. This bit exactly one call site here
 * before the conversion: select_and_apply_join_candidate() (wifi_prov_link.c)
 * called wifi_prov_scan(). It now calls do_scan() (wifi_prov_api.c) directly,
 * and every other internal helper likewise works on raw state. Only the
 * public boundary posts.
 *
 * Consequence of one owner: wifi_prov_scan() is a genuinely blocking radio
 * scan (esp_wifi_scan_start with block=true, ~50-150ms per channel), and it
 * now runs ON owner_task(), which serializes it against every other Wi-Fi
 * command. That is intended -- a scan and a mode switch racing on the radio
 * is precisely what this phase exists to stop -- but it means a command
 * posted behind a scan waits for that scan first. Hence two timeouts, not
 * one, both sized from the caller side rather than the radio side (see
 * wifi_prov_api.c). */

#include "wifi_prov_internal.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "settings.h"
#include "wifi_provision_http.h"

const char *WIFI_PROV_TAG = "wifi_prov";

/* TODO.md 8.4: how often to re-scan for a saved network while the board is
 * sitting in AP fallback (home mode, but not currently joined) instead of
 * only retrying the same target on every disconnect event. 30s balances
 * "notices a network coming back into range reasonably promptly" against
 * not spending unnecessary scan time/radio contention when nothing has
 * changed -- there is no requirement driving a tighter number, this is
 * deliberately not the same cadence as WIFI_STA_CONNECT_TIMEOUT_MS (which
 * governs how long a single join attempt gets, not how often to look for a
 * *different* target). Runs via rescan_timer_cb() on the esp_timer service
 * task, never on the Wi-Fi driver's own event-loop task -- see that
 * function's comment (wifi_prov_link.c) for why that distinction matters. */
#define WIFI_AP_FALLBACK_RESCAN_INTERVAL_MS 30000

#define WIFI_OWNER_QUEUE_LEN 6

struct wifi_prov_state s_wifi;

QueueHandle_t s_cmd_queue;

/* Producer half of the request/response pair -- identical in shape to
 * kiln_io_owner.c's and thermo_owner.c's helper of the same name, including
 * the fail-closed default. Returns false if the owner task isn't up, the post
 * was refused (full queue), or the wait timed out; *result is meaningful only
 * when it returns true. */
bool post_and_wait(wifi_cmd_t *cmd, wifi_result_t *result, uint32_t wait_ms)
{
    memset(result, 0, sizeof(*result));
    result->err = ESP_ERR_INVALID_STATE; /* fail closed if the owner never answers */

    if (!s_cmd_queue) {
        return false;
    }

    /* Static, stack-resident semaphore -- same fix as uart_owner_transfer()
     * and i2c_owner_transfer() (2026-08-20). Not a hot loop itself, but kept
     * consistent with every other owner's post_and_wait() now that the
     * pattern is known to matter under load. */
    StaticSemaphore_t done_storage;
    SemaphoreHandle_t done = xSemaphoreCreateBinaryStatic(&done_storage);
    if (!done) {
        return false;
    }

    cmd->result = result;
    cmd->done = done;

    bool ok = false;
    if (xQueueSend(s_cmd_queue, cmd, 0) == pdTRUE) {
        ok = xSemaphoreTake(done, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
        if (!ok) {
            ESP_LOGW(WIFI_PROV_TAG, "owner task did not answer command %d within %ums -- failing closed",
                     (int)cmd->type, (unsigned)wait_ms);
        }
    } else {
        ESP_LOGW(WIFI_PROV_TAG, "command queue full -- refusing command %d", (int)cmd->type);
    }

    vSemaphoreDelete(done);
    return ok;
}

/* Fire-and-forget half -- the ONLY thing the Wi-Fi event handlers and
 * esp_timer callbacks (wifi_prov_link.c) do now. 0-tick post: an event loop
 * that blocks on this module's queue is an event loop not delivering anyone
 * else's events. See the "dropped events are survivable" paragraph above for
 * why discarding is the correct answer to a full queue here. */
void post_event(wifi_cmd_type_t type)
{
    if (!s_cmd_queue) {
        /* Between esp_event_handler_instance_register() and the owner task
         * existing there is no queue yet -- see wifi_prov_start() below,
         * which creates the task BEFORE esp_wifi_start() precisely to keep
         * this window from covering any event the radio can actually
         * emit. */
        return;
    }
    wifi_cmd_t cmd = { .type = type, .result = NULL, .done = NULL };
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(WIFI_PROV_TAG, "command queue full -- dropping Wi-Fi event %d (self-heals: see this file's "
                      "owner-task comment)", (int)type);
    }
}

/* Defined at the very bottom of this file; forward-declared here because
 * wifi_prov_start() (which sits above it) is what creates it. */
static void owner_task(void *arg);

/* ---- Public API --------------------------------------------------------- */

esp_err_t wifi_prov_start(void)
{
    if (s_wifi.started) {
        return ESP_OK;
    }

    s_wifi.sta_rssi = -127; /* not connected */

    /* 2026-08-13: zones_http/rules_http/profiles_http/run_state/relay_cycles
     * now each own their real (kiln_nvs / profiles_nvs) partition's init and
     * persistence -- see those files. The default `nvs` partition is no
     * longer written by anyone; it is only ever READ, once, by each module's
     * one-time migration-off-the-old-location step, and every one of those
     * migration reads happens after wifi_prov_start() returns (main.c calls
     * this first). This init call stays here for exactly that: it is the
     * one thing that must run before any module's migration read of the old
     * default-partition data can succeed. If this fails, migration reads
     * elsewhere fail closed (nvs_open_from_partition on an uninitialized
     * partition errors, same as a blank one) and every module just starts
     * from its own (already-migrated, or first-boot-empty) real partition --
     * never fatal, never a reason to block Wi-Fi bring-up. */
    esp_err_t default_err = wifi_prov_nvs_partition_init(NVS_DEFAULT_PART_NAME);
    if (default_err != ESP_OK) {
        ESP_LOGW(WIFI_PROV_TAG, "default NVS init failed: %s -- one-time migration reads for "
                 "zones/rules/profiles/run_state/relay_cycles will find nothing to migrate "
                 "(harmless if already migrated; otherwise those sections start unconfigured)",
                 esp_err_to_name(default_err));
    }

    esp_err_t err = wifi_prov_nvs_partition_init(WIFI_NVS_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "NVS init for '%s' failed: %s -- Wi-Fi credentials cannot persist",
                 WIFI_NVS_PARTITION, esp_err_to_name(err));
        /* Deliberately not a return: an unusable credential partition means
         * nothing persists, but the AP still has to come up so the board can
         * be reached and reprovisioned at all. Falling through leaves s_wifi
         * zeroed, i.e. unprovisioned/home -- the first-boot state. */
    }

    bool found_in_wifi_nvs = false;
    if (err == ESP_OK) {
        err = nvs_load_from(WIFI_NVS_PARTITION, &found_in_wifi_nvs);
        if (err != ESP_OK) {
            ESP_LOGW(WIFI_PROV_TAG, "wifi_cfg load from '%s' failed: %s -- starting unprovisioned",
                     WIFI_NVS_PARTITION, esp_err_to_name(err));
            s_wifi.mode = WIFI_PROV_MODE_HOME;
        } else {
            if (default_err == ESP_OK) {
                /* Only worth attempting when the default partition actually
                 * mounted -- there is nothing to migrate from otherwise. */
                wifi_prov_migrate_from_default_partition(found_in_wifi_nvs);
            } else {
                /* No cross-partition migration possible, but WIFI_NVS_PARTITION
                 * itself may still hold legacy single-key credentials from a
                 * pre-8.4 build of THIS partition's own format -- make sure
                 * nvs_load_saved_nets() below still has a chance to pick them
                 * up. wifi_prov_migrate_from_default_partition() would normally set
                 * this; do it directly here since that function didn't run. */
                nvs_load_legacy_single(WIFI_NVS_PARTITION, &s_legacy_single.net, &s_legacy_single.has);
            }
            /* Load the saved-networks list (and, the first time, migrate the
             * legacy single-key credential s_legacy_single now holds into
             * it) -- must happen after the block above so it reflects
             * whichever copy the cross-partition decision settled on. */
            nvs_load_saved_nets();
        }
    }

    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }
    /* ESP_ERR_INVALID_STATE means a default loop already exists -- fine,
     * some other subsystem may have created it first. Anything else is a
     * real failure. */
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(WIFI_PROV_TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    s_wifi.ap_netif = esp_netif_create_default_wifi_ap();
    s_wifi.sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_wifi.ap_netif || !s_wifi.sta_netif) {
        ESP_LOGE(WIFI_PROV_TAG, "esp_netif_create_default_wifi_* failed");
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, NULL);

    const esp_timer_create_args_t timer_args = {
        .callback = &ap_fallback_timer_cb,
        .name = "wifi_ap_fallback",
    };
    err = esp_timer_create(&timer_args, &s_wifi.ap_fallback_timer);
    if (err != ESP_OK) {
        ESP_LOGW(WIFI_PROV_TAG, "esp_timer_create failed: %s -- no AP-fallback-on-reconnect-timeout", esp_err_to_name(err));
        s_wifi.ap_fallback_timer = NULL;
    }

    /* Periodic, started unconditionally and left running for the module's
     * whole lifetime -- rescan_timer_cb() itself no-ops unless mode is HOME
     * and state isn't CONNECTED, so an always-on timer is simpler than
     * starting/stopping it around every state transition, at the cost of one
     * cheap wakeup every WIFI_AP_FALLBACK_RESCAN_INTERVAL_MS regardless of
     * state -- negligible next to the AP/STA radio work it occasionally
     * triggers. */
    const esp_timer_create_args_t rescan_timer_args = {
        .callback = &rescan_timer_cb,
        .name = "wifi_ap_fallback_rescan",
    };
    err = esp_timer_create(&rescan_timer_args, &s_wifi.rescan_timer);
    if (err != ESP_OK) {
        ESP_LOGW(WIFI_PROV_TAG, "esp_timer_create failed: %s -- no periodic AP-fallback rescan", esp_err_to_name(err));
        s_wifi.rescan_timer = NULL;
    } else {
        err = esp_timer_start_periodic(s_wifi.rescan_timer, (uint64_t)WIFI_AP_FALLBACK_RESCAN_INTERVAL_MS * 1000);
        if (err != ESP_OK) {
            ESP_LOGW(WIFI_PROV_TAG, "esp_timer_start_periodic failed: %s -- no periodic AP-fallback rescan",
                     esp_err_to_name(err));
        }
    }

    /* esp_wifi_set_config() fails with ESP_ERR_WIFI_MODE if the driver's
     * current mode doesn't already include the target interface -- right
     * after esp_wifi_init() the mode is WIFI_MODE_NULL, so calling
     * apply_ap_config()/apply_sta_config() before esp_wifi_set_mode() below
     * (as this used to do, unconditionally, ahead of all three branches)
     * failed on every single boot. Fixed 2026-08-13 by setting the mode
     * first in each branch and applying config only once it succeeds. */
    if (s_wifi.mode == WIFI_PROV_MODE_AP) {
        s_wifi.state = WIFI_PROV_STATE_AP_MODE;
        err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (err == ESP_OK) {
            apply_ap_config();
        }
        ESP_LOGI(WIFI_PROV_TAG, "AP mode: AP '%s' only, station never attempted", wifi_prov_get_ap_ssid());
    } else if (s_wifi.saved_nets.count > 0) {
        /* Initial boot config, before the Wi-Fi driver/task exists at all --
         * no scan-based tie-break here (wifi_prov_scan() requires the driver
         * already started, which it isn't yet at this point in bring-up).
         * Just take the first saved entry; the scan-based tie-break in
         * select_and_apply_join_candidate() (wifi_prov_link.c) takes over
         * from the very next join attempt onward (a disconnect, or an
         * explicit add/mode change). */
        strncpy(s_wifi.active_ssid, s_wifi.saved_nets.nets[0].ssid, sizeof(s_wifi.active_ssid) - 1);
        s_wifi.active_ssid[sizeof(s_wifi.active_ssid) - 1] = '\0';
        strncpy(s_wifi.active_password, s_wifi.saved_nets.nets[0].password, sizeof(s_wifi.active_password) - 1);
        s_wifi.active_password[sizeof(s_wifi.active_password) - 1] = '\0';
        s_wifi.state = WIFI_PROV_STATE_CONNECTING;
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err == ESP_OK) {
            apply_ap_config();
            apply_sta_config();
        }
        start_ap_fallback_timer();
        ESP_LOGI(WIFI_PROV_TAG, "attempting station join to '%s', AP '%s' available meanwhile", s_wifi.active_ssid,
                 wifi_prov_get_ap_ssid());
    } else {
        s_wifi.state = WIFI_PROV_STATE_UNPROVISIONED;
        err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (err == ESP_OK) {
            apply_ap_config();
        }
        ESP_LOGI(WIFI_PROV_TAG, "no saved credentials: AP '%s' for first-boot provisioning", wifi_prov_get_ap_ssid());
    }
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    /* The owner task comes up HERE -- after every synchronous bring-up step
     * above (NVS load/migration, netif, esp_wifi_init, handler registration,
     * timer creation, the initial mode/config decision) and immediately
     * BEFORE esp_wifi_start(). Both halves of that placement are deliberate.
     *
     * Not earlier: everything above this line mutates s_wifi from THIS task
     * (app_main's), and wifi_prov_start() deliberately stays a plain
     * synchronous init rather than becoming its own command -- there is no
     * one to serialize against yet, and turning bring-up into a queued
     * command would mean the task had to exist before the state it owns did.
     *
     * Not later (e.g. after esp_wifi_start()): esp_wifi_start() is what makes
     * the driver start emitting WIFI_EVENT_STA_START and friends. Creating
     * the task after it would open a window where on_wifi_event() fires,
     * finds s_cmd_queue still NULL, and silently drops a STA_START -- costing
     * the first join attempt of every boot. Creating it here means the queue
     * exists before the radio can produce a single event.
     *
     * The overlap this leaves is small and harmless: between this line and
     * `s_wifi.started = true` below, owner_task() may already be draining
     * real events (legitimate state-machine transitions, exactly what it is
     * for) while this function does esp_wifi_start() and
     * wifi_provision_http_start(). Neither of those writes s_wifi; the only
     * remaining write is the single `started` bool, and every producer
     * refuses until it is set. If the task fails to create, this returns the
     * error and `started` is never set, so every producer fails closed rather
     * than running unserialized. */
    s_cmd_queue = xQueueCreate(WIFI_OWNER_QUEUE_LEN, sizeof(wifi_cmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(WIFI_PROV_TAG, "xQueueCreate(wifi_owner) failed");
        return ESP_ERR_NO_MEM;
    }
    BaseType_t task_created =
        xTaskCreatePinnedToCore(owner_task, "wifi_prov_owner", 4096, NULL, 5, NULL, tskNO_AFFINITY);
    if (task_created != pdPASS) {
        ESP_LOGE(WIFI_PROV_TAG, "xTaskCreatePinnedToCore(wifi_prov_owner) failed");
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* See dns_hijack_task()'s header comment (wifi_prov_link.c) for what this
     * fixes (phones joining the fallback AP then giving up). Started here,
     * after esp_wifi_start() rather than right after netif creation: this
     * file's own uart_protocol_register_task() comment documents a real
     * internal-SRAM contention window during "wifi_prov_start()'s STA+AP
     * bring-up" that starved xQueueCreate() for every UART bridge task
     * racing it at boot (bench-observed 2026-08-18, root-caused and retried
     * around there). socket()/bind() also allocate from that same
     * internal-SRAM pool (lwIP's socket/pcb structures are never satisfied
     * from PSRAM either) -- starting this task any earlier than here sits it
     * inside that exact contention window instead of after it, which is
     * suspected (2026-08-19 bench observation, not yet certain) to have
     * caused a UART bridge task registration failure the same boot. */
    start_dns_hijack_task();

    err = wifi_provision_http_start();
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "wifi_provision_http_start failed: %s -- Wi-Fi is up but not provisionable over HTTP",
                 esp_err_to_name(err));
        /* Not fatal to wifi_prov itself -- Wi-Fi bring-up already
         * succeeded and this module's own hard requirement (never touch
         * relay/safety state) doesn't depend on the HTTP server. */
    }

    s_wifi.started = true;
    return ESP_OK;
}

wifi_prov_state_t wifi_prov_get_state(void)
{
    return s_wifi.state;
}

wifi_prov_mode_t wifi_prov_get_mode(void)
{
    return s_wifi.mode;
}

const char *wifi_prov_get_saved_ssid(void)
{
    if (s_wifi.active_ssid[0] != '\0') {
        return s_wifi.active_ssid;
    }
    if (s_wifi.saved_nets.count > 0) {
        return s_wifi.saved_nets.nets[0].ssid;
    }
    return "";
}

const char *wifi_prov_get_ap_ssid(void)
{
    return s_wifi.has_ap_ssid_override ? s_wifi.ap_ssid : WIFI_AP_SSID;
}

const char *wifi_prov_get_ap_password(void)
{
    return s_wifi.has_ap_password_override ? s_wifi.ap_password : WIFI_AP_DEFAULT_PASSWORD;
}

/* ---- The owner task itself ---------------------------------------------
 * The ONE task that ever writes s_wifi or calls esp_wifi_*() outside
 * wifi_prov_start()'s synchronous bring-up. Blocks portMAX_DELAY on the queue:
 * unlike relay_owner.c on the RP2040 side (whose bounded 200ms receive exists
 * so its watchdog check-in and state tick still run on an idle queue), this
 * task has no periodic duty of its own -- the two esp_timer callbacks provide
 * the cadence by posting, which is exactly the same mechanism as any other
 * command rather than a second path into the state. */
static void owner_task(void *arg)
{
    (void)arg;

    for (;;) {
        wifi_cmd_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        wifi_result_t local;
        wifi_result_t *r = cmd.result ? cmd.result : &local;
        memset(r, 0, sizeof(*r));
        r->err = ESP_FAIL;

        switch (cmd.type) {
        case CMD_ADD_NETWORK:
            r->err = do_add_network(cmd.args.add_network.ssid, cmd.args.add_network.password);
            break;
        case CMD_FORGET_NETWORK:
            r->err = do_forget_network(cmd.args.forget_network.ssid);
            break;
        case CMD_GET_SAVED_NETWORKS:
            r->err = do_get_saved_networks(cmd.args.get_saved_networks.max_results, r);
            break;
        case CMD_SET_MODE:
            r->err = do_set_mode(cmd.args.set_mode.mode);
            break;
        case CMD_SET_AP_SSID:
            r->err = do_set_ap_ssid(cmd.args.set_ap_ssid.ssid);
            break;
        case CMD_SET_AP_PASSWORD:
            r->err = do_set_ap_password(cmd.args.set_ap_password.password);
            break;
        case CMD_GET_STA_IP:
            r->err = do_get_sta_ip(cmd.args.get_sta_ip.out_cap, r);
            break;
        case CMD_SCAN: {
            size_t max = cmd.args.scan.max_results > WIFI_OWNER_SCAN_STAGE_MAX
                             ? WIFI_OWNER_SCAN_STAGE_MAX
                             : cmd.args.scan.max_results;
            r->err = do_scan(s_scan_stage, max, &r->scan_count);
            break;
        }
        case CMD_SET_DHCP:
            r->err = do_set_dhcp();
            break;
        case CMD_SET_STATIC_IP:
            r->err = do_set_static_ip(cmd.args.set_static_ip.ip, cmd.args.set_static_ip.netmask,
                                      cmd.args.set_static_ip.gateway);
            break;

        /* Fire-and-forget events -- cmd.result/cmd.done are NULL for these,
         * so the r->err written above goes nowhere, on purpose. */
        case CMD_EV_STA_START:
            do_ev_sta_start();
            break;
        case CMD_EV_STA_DISCONNECTED:
            do_ev_sta_disconnected();
            break;
        case CMD_EV_GOT_IP:
            do_ev_got_ip();
            break;
        case CMD_TMR_AP_FALLBACK:
            do_ap_fallback_tick();
            break;
        case CMD_TMR_RESCAN:
            do_rescan_tick();
            break;
        case CMD_CONFIRM_STATIC_REACHABLE:
            do_confirm_static_reachable();
            break;
        }

        if (cmd.done) {
            /* Given LAST, after the result slot is fully written -- the
             * producer is blocked on exactly this and reads *result the
             * instant it is released. */
            xSemaphoreGive(cmd.done);
        }
    }
}
