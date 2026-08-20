#include "wifi_prov.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "settings.h"
#include "wifi_provision_http.h"

static const char *TAG = "wifi_prov";

#define NVS_NAMESPACE "wifi_cfg"

/* Dedicated NVS partition for Wi-Fi credentials -- see partitions.csv, which
 * appends it at 0x187000 in flash that the stock single-app-large layout left
 * unused above the app image.
 *
 * Why it isn't just another namespace in the default `nvs` partition (which is
 * where every version of this file before 2026-08-12 put it): NVS corruption
 * recovery is partition-wide, not namespace-wide. The only cure for
 * ESP_ERR_NVS_NO_FREE_PAGES / ESP_ERR_NVS_NEW_VERSION_FOUND is to erase the
 * entire partition. The code below used to answer that with a blanket
 * nvs_flash_erase(), which took zones, rules, profiles, the run-state
 * breadcrumb AND the Wi-Fi credentials with it -- so a corrupt profile blob
 * knocked the board off the network at exactly the moment the operator needed
 * the network to go fix it. Splitting the credentials into their own partition
 * means neither wipe can reach the other, in either direction.
 *
 * 2026-08-13: the same reasoning was extended to the REST of the default
 * `nvs` partition's former contents. zones/rules/relay_cycles/run_state now
 * live in their own `kiln_nvs` partition and fire profiles in their own
 * `profiles_nvs` (see partitions.csv) -- each with the same scoped
 * nvs_partition_init()-and-erase-ONLY-that-partition pattern this file
 * pioneered, and each doing its own one-time migration off the default
 * partition independently, the same way this module migrates Wi-Fi
 * credentials below. This module no longer initializes the default `nvs`
 * partition as a side effect for anyone else -- that was a historical
 * accident (main.c happened to call wifi_prov_start() first), and every
 * module that touches NVS now owns its own partition's init.
 *
 * The same split is what makes "reset the kiln's configuration" (erase
 * `kiln_nvs` and/or `profiles_nvs`) a thing the operator can do without
 * stranding the board off Wi-Fi, and without one corrupt section taking any
 * other section with it.
 *
 * It does NOT make the credentials immortal: `esptool erase_flash` clears the
 * whole chip and every partition goes with it. The survival matrix is:
 *   flash bootloader+partition-table+app  -> credentials survive (this is the
 *                                            common case, and the reason the
 *                                            first three partitions.csv rows
 *                                            are kept byte-identical to the
 *                                            stock table)
 *   erase the default `nvs` partition     -> credentials survive; kiln_nvs
 *                                            and profiles_nvs are UNAFFECTED
 *                                            (default `nvs` is only read now,
 *                                            during migration, never written)
 *   erase `kiln_nvs`                      -> zones/rules/relay_cycles/
 *                                            run_state lost; credentials and
 *                                            profiles survive
 *   erase `profiles_nvs`                  -> fire profiles lost; everything
 *                                            else survives
 *   esptool erase_flash                   -> nothing survives, credentials
 *                                            included */
#define WIFI_NVS_PARTITION "wifi_nvs"

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
 * function's comment for why that distinction matters. */
#define WIFI_AP_FALLBACK_RESCAN_INTERVAL_MS 30000

#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"
#define NVS_KEY_HAS_CREDS "has_creds"
/* Legacy single-network keys (NVS_KEY_SSID/NVS_KEY_PASS/NVS_KEY_HAS_CREDS)
 * are never written by this build any more -- see NVS_KEY_SAVED_NETS below --
 * but are still READ once, by the one-time list-format migration in
 * nvs_load_saved_nets(), for boards provisioned by firmware that predates
 * TODO.md 8.4's bounded-list rework. Left in place afterward, same rationale
 * as every other "old copy stays, never re-read" migration in this file. */
#define NVS_KEY_SAVED_NETS "saved_nets"
#define WIFI_PROV_MAX_SAVED_NETWORKS 8
#define SAVED_NETS_VERSION 1
#define NVS_KEY_MODE "mode"           /* u8: 0 = WIFI_PROV_MODE_HOME, 1 = WIFI_PROV_MODE_AP */
#define NVS_KEY_LOCAL_ONLY "local_only" /* legacy, read-only: pre-2026-08-11 firmware's
                                          * only mode flag. Migrated into NVS_KEY_MODE the
                                          * first time this runs against an old NVS blob;
                                          * never written by this build. See nvs_load(). */
#define NVS_KEY_AP_SSID "ap_ssid"
#define NVS_KEY_HAS_AP_SSID "has_ap_ssid"
#define NVS_KEY_AP_PASS "ap_pass"
#define NVS_KEY_HAS_AP_PASS "has_ap_pass"

/* 2026-08-20, web-GUI-only static-IP addition (see wifi_prov.h's "Static IP"
 * section). New keys, same partition/namespace as everything else in this
 * file -- never repurposing an existing key. Absent (first boot, or a board
 * that predates this feature) reads back as DHCP with empty strings, which
 * is exactly today's always-on default behavior. */
#define NVS_KEY_IP_MODE "ip_mode" /* u8: 0 = DHCP, 1 = STATIC */
#define NVS_KEY_STATIC_IP "static_ip"
#define NVS_KEY_STATIC_NETMASK "static_netmask"
#define NVS_KEY_STATIC_GW "static_gw"

/* TODO.md 8.4: a bounded list of saved networks, replacing the old single
 * ssid/password/has_creds slot. Versioned the same way zones_http.c/
 * rules_http.c version their NVS blobs (see nvs_load_saved_nets() below) --
 * this is a same-partition version bump, so the 3-outcome load logic there is
 * simpler than wifi_prov's own cross-partition credential migration: exact
 * match uses it, unreadable/absent/wrong-size treats it as an empty list,
 * and a version NEWER than this build's SAVED_NETS_VERSION refuses to load
 * (leaves flash untouched) rather than risk misinterpreting a layout this
 * build doesn't know about -- the same firmware-rollback-safety rationale as
 * zones_cfg_t's version check. */
typedef struct {
    char ssid[WIFI_PROV_SSID_MAX_LEN + 1];
    char password[WIFI_PROV_PASSWORD_MAX_LEN + 1];
} saved_net_t;

typedef struct {
    uint8_t version; /* SAVED_NETS_VERSION at save time */
    uint8_t count;
    saved_net_t nets[WIFI_PROV_MAX_SAVED_NETWORKS];
} saved_nets_blob_t;

/* Named (rather than anonymous) so the migration path in wifi_prov_start()
 * can take a whole-struct snapshot of what the new partition yielded before
 * speculatively re-loading over it from the old one. */
static struct wifi_prov_state {
    bool started;
    esp_netif_t *ap_netif;
    esp_netif_t *sta_netif;
    esp_timer_handle_t ap_fallback_timer; /* one-shot; brings the AP back if a
                                           * reconnect doesn't land in time */
    esp_timer_handle_t rescan_timer; /* periodic; see rescan_timer_cb() --
                                      * looks for a stronger/available saved
                                      * network while sitting in AP fallback
                                      * that the operator didn't choose */

    saved_nets_blob_t saved_nets; /* the bounded list of saved networks --
                                   * "has credentials" is now simply
                                   * saved_nets.count > 0, derived everywhere
                                   * it's needed rather than stored separately,
                                   * which is what closes out the 2026-08-13
                                   * has_creds/ssid disagreement class for
                                   * good: there is no second flag left to
                                   * drift out of sync with the list. */
    char active_ssid[WIFI_PROV_SSID_MAX_LEN + 1]; /* the SSID apply_sta_config()
                                   * most recently configured for a join --
                                   * "the network currently being tried or
                                   * connected to", independent of what's in
                                   * the saved list. Empty if nothing has been
                                   * configured yet this boot. */
    wifi_prov_mode_t mode;

    /* The fallback AP's OWN identity -- distinct from the station
     * credentials above, which are for joining a *different* network. Only
     * meaningful when the corresponding has_*_override is set; otherwise
     * apply_ap_config() falls back to the compile-time WIFI_AP_SSID /
     * WIFI_AP_DEFAULT_PASSWORD Kconfig values. Kept separate from has_creds
     * so an operator can override just the AP's own identity without
     * touching (or requiring) any saved station network. */
    char ap_ssid[WIFI_PROV_SSID_MAX_LEN + 1];
    bool has_ap_ssid_override;
    char ap_password[WIFI_PROV_PASSWORD_MAX_LEN + 1];
    bool has_ap_password_override;

    /* 2026-08-20, web-GUI-only: STA IP mode + static config. Only meaningful
     * while mode == WIFI_PROV_MODE_HOME; see wifi_prov.h's "Static IP"
     * section. Dotted-quad strings, empty unless ip_mode == STATIC. */
    wifi_prov_ip_mode_t ip_mode;
    char static_ip[WIFI_PROV_IPV4_STR_MAX];
    char static_netmask[WIFI_PROV_IPV4_STR_MAX];
    char static_gateway[WIFI_PROV_IPV4_STR_MAX];

    char active_password[WIFI_PROV_PASSWORD_MAX_LEN + 1]; /* password paired with
                                   * active_ssid above -- kept in RAM only for
                                   * apply_sta_config() to use, same as every
                                   * password this module already holds
                                   * in-memory in plaintext. */

    wifi_prov_state_t state;
    int8_t sta_rssi; /* signal strength (dBm) when connected, -127 if not */
} s_wifi;

/* Winning legacy single-network credential (pre-8.4 NVS_KEY_SSID/PASS/
 * HAS_CREDS format), set by migrate_from_default_partition() and consumed
 * exactly once by nvs_load_saved_nets() to wrap it into saved_nets.nets[0]
 * the first time this build runs against an old NVS blob. Module-static
 * (rather than a parameter threaded through) because migrate_from_default_
 * partition() already has to decide "which whole config wins" for the
 * mode/AP-identity fields it does carry in s_wifi -- this rides along with
 * that same decision instead of duplicating it. */
static struct {
    bool has;
    saved_net_t net;
} s_legacy_single;

/* ---- Owning task + command queue (2026-08-19, TODO.md 10.14 Phase 4) ----
 *
 * Everything above this comment is module state that, until this phase, had
 * ZERO locking and FOUR independent writers: the Wi-Fi driver's own
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
 * before the conversion: select_and_apply_join_candidate() called
 * wifi_prov_scan(). It now calls do_scan() directly, and every other internal
 * helper likewise works on raw state. Only the public boundary posts.
 *
 * Consequence of one owner: wifi_prov_scan() is a genuinely blocking radio
 * scan (esp_wifi_scan_start with block=true, ~50-150ms per channel), and it
 * now runs ON owner_task(), which serializes it against every other Wi-Fi
 * command. That is intended -- a scan and a mode switch racing on the radio
 * is precisely what this phase exists to stop -- but it means a command
 * posted behind a scan waits for that scan first. Hence two timeouts, not
 * one, both sized from the caller side rather than the radio side (see
 * below). It also means the two `static` scratch arrays in this file
 * (do_scan()'s records[], select_and_apply_join_candidate()'s scan_results[])
 * are now genuinely single-threaded, where before they were shared statics
 * reachable from three tasks. */

#define WIFI_OWNER_QUEUE_LEN 6

/* How long a scan's producer waits. A full active scan of every 2.4GHz
 * channel at 50-150ms dwell is seconds, not milliseconds, and the existing
 * callers already tolerate exactly that: ui_page_network.c runs it on a
 * dedicated short-lived worker task (never lvgl_port_task), and
 * wifi_provision_http.c/uart_bridge_ext.c block their own task on it today.
 * 15s is "a scan, plus one already-in-flight scan ahead of it in the queue,
 * plus slack" -- not a radio-derived number. */
#define WIFI_OWNER_SCAN_WAIT_MS 15000

/* How long every other producer waits. Sized by the WORST CASE AHEAD OF IT
 * in the queue, which is a scan, not by its own (millisecond-scale) work --
 * a 200ms timeout of the kind thermo_owner.c uses would spuriously fail any
 * status poll unlucky enough to land behind an operator tapping Scan. A
 * producer that does time out fails closed: it returns an error, exactly as
 * if the operation itself had failed. */
#define WIFI_OWNER_WAIT_MS 12000

typedef enum {
    /* External (request/response) -- one per public mutator or compound
     * reader. See the header for which readers deliberately stayed direct. */
    CMD_ADD_NETWORK,
    CMD_FORGET_NETWORK,
    CMD_GET_SAVED_NETWORKS,
    CMD_SET_MODE,
    CMD_SET_AP_SSID,
    CMD_SET_AP_PASSWORD,
    CMD_GET_STA_IP,
    CMD_SCAN,
    CMD_SET_DHCP,
    CMD_SET_STATIC_IP,

    /* Internal (fire-and-forget) -- the Wi-Fi driver's and esp_timer's own
     * callbacks, rerouted here so they mutate s_wifi on the same one task as
     * everything above rather than on their own. */
    CMD_EV_STA_START,
    CMD_EV_STA_DISCONNECTED,
    CMD_EV_GOT_IP,
    CMD_TMR_AP_FALLBACK,
    CMD_TMR_RESCAN,
} wifi_cmd_type_t;

typedef struct {
    esp_err_t err;
    wifi_prov_saved_network_t saved[WIFI_PROV_MAX_SAVED_NETWORKS];
    size_t saved_count;
    char sta_ip[16]; /* dotted-quad, matching wifi_prov_get_sta_ip()'s contract */
    size_t scan_count; /* entries the owner task left in s_scan_stage */
} wifi_result_t;

typedef struct {
    wifi_cmd_type_t type;
    wifi_result_t *result; /* caller-owned; NULL for fire-and-forget events */
    SemaphoreHandle_t done; /* caller-owned binary semaphore, given last; NULL
                             * for fire-and-forget events */
    union {
        /* CMD_ADD_NETWORK: SSID/password are COPIED into the command rather
         * than pointed at. The producer's caller is an HTTP handler parsing a
         * request body it will free the moment it returns -- a pointer into
         * that would dangle the instant a producer ever stopped waiting.
         * Fixed-size and small enough (98 bytes) to keep every queue entry
         * uniform, per the plan's "a tag plus fixed fields, never a
         * variable-length payload" rule. */
        struct {
            char ssid[WIFI_PROV_SSID_MAX_LEN + 1];
            char password[WIFI_PROV_PASSWORD_MAX_LEN + 1];
        } add_network;
        struct { char ssid[WIFI_PROV_SSID_MAX_LEN + 1]; } forget_network;
        struct { size_t max_results; } get_saved_networks;
        struct { wifi_prov_mode_t mode; } set_mode;
        struct { char ssid[WIFI_PROV_SSID_MAX_LEN + 1]; } set_ap_ssid;
        struct { char password[WIFI_PROV_PASSWORD_MAX_LEN + 1]; } set_ap_password;
        struct { size_t out_cap; } get_sta_ip;
        struct { size_t max_results; } scan;
        /* CMD_SET_STATIC_IP: strings copied in, same rationale as
         * add_network above -- the producer's caller (an HTTP handler) frees
         * its buffers the moment it returns. */
        struct {
            char ip[WIFI_PROV_IPV4_STR_MAX];
            char netmask[WIFI_PROV_IPV4_STR_MAX];
            char gateway[WIFI_PROV_IPV4_STR_MAX];
        } set_static_ip;
    } args;
} wifi_cmd_t;

static QueueHandle_t s_cmd_queue;

/* Scan results staging. Written ONLY by owner_task(), copied out by the
 * producer after its semaphore is given. Deliberately module-static rather
 * than a member of wifi_result_t (which lives on the producer's stack): at 20
 * entries this is ~720 bytes, and several callers run on 4096-byte worker
 * task stacks. Nothing is lost by staging here -- only one command is ever
 * being serviced at a time, and a producer that timed out simply never copies
 * out, leaving the next one to overwrite it. */
#define WIFI_OWNER_SCAN_STAGE_MAX 20
static wifi_prov_scan_result_t s_scan_stage[WIFI_OWNER_SCAN_STAGE_MAX];

/* Producer half of the request/response pair -- identical in shape to
 * kiln_io_owner.c's and thermo_owner.c's helper of the same name, including
 * the fail-closed default. Returns false if the owner task isn't up, the post
 * was refused (full queue), or the wait timed out; *result is meaningful only
 * when it returns true. */
static bool post_and_wait(wifi_cmd_t *cmd, wifi_result_t *result, uint32_t wait_ms)
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
            ESP_LOGW(TAG, "owner task did not answer command %d within %ums -- failing closed",
                     (int)cmd->type, (unsigned)wait_ms);
        }
    } else {
        ESP_LOGW(TAG, "command queue full -- refusing command %d", (int)cmd->type);
    }

    vSemaphoreDelete(done);
    return ok;
}

/* Fire-and-forget half -- the ONLY thing the Wi-Fi event handlers and
 * esp_timer callbacks do now. 0-tick post: an event loop that blocks on this
 * module's queue is an event loop not delivering anyone else's events. See
 * the "dropped events are survivable" paragraph above for why discarding is
 * the correct answer to a full queue here. */
static void post_event(wifi_cmd_type_t type)
{
    if (!s_cmd_queue) {
        /* Between esp_event_handler_instance_register() and the owner task
         * existing there is no queue yet -- see wifi_prov_start(), which
         * creates the task BEFORE esp_wifi_start() precisely to keep this
         * window from covering any event the radio can actually emit. */
        return;
    }
    wifi_cmd_t cmd = { .type = type, .result = NULL, .done = NULL };
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full -- dropping Wi-Fi event %d (self-heals: see wifi_prov.c's "
                      "owner-task comment)", (int)type);
    }
}

/* Defined down with the rest of the do_*() bodies (next to the public
 * wifi_prov_scan() producer that posts for it), forward-declared here because
 * select_and_apply_join_candidate() -- which also runs on the owner task --
 * has to call it DIRECTLY rather than via wifi_prov_scan(). That call used to
 * be the public function, which is now a producer: leaving it would have the
 * owner task post to its own queue and wait for itself. This declaration is
 * the deadlock rule made mechanical. */
static esp_err_t do_scan(wifi_prov_scan_result_t *results, size_t max_results, size_t *out_count);

/* Defined at the very bottom of this file, after every do_*() body it
 * dispatches to; forward-declared here because wifi_prov_start() (which sits
 * above them all) is what creates it. */
static void owner_task(void *arg);

/* ---- NVS -------------------------------------------------------------- */

/* Reads the whole persisted config out of `partition`'s NVS_NAMESPACE into
 * s_wifi. Parameterized on the partition (rather than hard-wired to
 * WIFI_NVS_PARTITION) purely so the one-time migration in wifi_prov_start()
 * can point the exact same reader -- legacy-key handling and all -- at the old
 * default-partition copy without duplicating any of it.
 *
 * *out_found reports whether the namespace existed at all, which is what the
 * migration keys off: an absent namespace means "nothing was ever saved here",
 * which is distinct from "saved, but with has_creds == 0". */
static esp_err_t nvs_load_from(const char *partition, bool *out_found)
{
    if (out_found) {
        *out_found = false;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(partition, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_PART_NOT_FOUND) {
        /* NOT_FOUND: nothing saved yet -- first boot. PART_NOT_FOUND: this
         * build's partition table isn't on the chip (e.g. a new app flashed
         * without the new table). Neither is an error worth refusing to bring
         * Wi-Fi up over -- the board just comes up unprovisioned. */
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (out_found) {
        *out_found = true;
    }

    /* The legacy single-network keys (NVS_KEY_SSID/NVS_KEY_PASS/
     * NVS_KEY_HAS_CREDS) are no longer read here -- they're only consulted
     * by the one-time list-format migration in nvs_load_saved_nets_from(),
     * which runs after this function (and after migrate_from_default_partition(),
     * on whichever single-key data wins that decision). This function no
     * longer touches s_wifi.saved_nets/active_ssid at all. */
    size_t len;
    uint8_t u8 = 0;

    /* Mode: prefer the new key. If it's missing, this is an NVS blob
     * written by pre-2026-08-11 firmware -- fall back to the old
     * "local_only" flag so a device that was in local-only mode boots back
     * into AP mode (its closest equivalent) rather than silently defaulting
     * to home mode and trying to join a network the user explicitly opted
     * out of. Never writes NVS_KEY_LOCAL_ONLY itself; the migration is
     * read-only and completes for real the next time the mode is saved
     * (nvs_save_mode() below only ever writes NVS_KEY_MODE). */
    u8 = 0;
    err = nvs_get_u8(h, NVS_KEY_MODE, &u8);
    if (err == ESP_OK) {
        s_wifi.mode = u8 ? WIFI_PROV_MODE_AP : WIFI_PROV_MODE_HOME;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        uint8_t legacy_local_only = 0;
        esp_err_t legacy_err = nvs_get_u8(h, NVS_KEY_LOCAL_ONLY, &legacy_local_only);
        if (legacy_err == ESP_OK && legacy_local_only) {
            ESP_LOGI(TAG, "migrating legacy local_only=1 NVS flag to mode=AP");
            s_wifi.mode = WIFI_PROV_MODE_AP;
        } else {
            s_wifi.mode = WIFI_PROV_MODE_HOME;
        }
    } else {
        nvs_close(h);
        return err;
    }

    len = sizeof(s_wifi.ap_ssid);
    err = nvs_get_str(h, NVS_KEY_AP_SSID, s_wifi.ap_ssid, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return err;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_wifi.ap_ssid[0] = '\0';
    }

    u8 = 0;
    err = nvs_get_u8(h, NVS_KEY_HAS_AP_SSID, &u8);
    s_wifi.has_ap_ssid_override = (err == ESP_OK) && u8;

    len = sizeof(s_wifi.ap_password);
    err = nvs_get_str(h, NVS_KEY_AP_PASS, s_wifi.ap_password, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return err;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_wifi.ap_password[0] = '\0';
    }

    u8 = 0;
    err = nvs_get_u8(h, NVS_KEY_HAS_AP_PASS, &u8);
    s_wifi.has_ap_password_override = (err == ESP_OK) && u8;

    /* 2026-08-20, web-GUI-only static-IP fields. Absent (pre-feature NVS
     * blob, or genuinely never configured) reads back as DHCP with empty
     * strings -- today's always-on default, unchanged. */
    u8 = 0;
    err = nvs_get_u8(h, NVS_KEY_IP_MODE, &u8);
    s_wifi.ip_mode = (err == ESP_OK && u8) ? WIFI_PROV_IP_MODE_STATIC : WIFI_PROV_IP_MODE_DHCP;

    len = sizeof(s_wifi.static_ip);
    err = nvs_get_str(h, NVS_KEY_STATIC_IP, s_wifi.static_ip, &len);
    if (err != ESP_OK) {
        s_wifi.static_ip[0] = '\0';
    }
    len = sizeof(s_wifi.static_netmask);
    err = nvs_get_str(h, NVS_KEY_STATIC_NETMASK, s_wifi.static_netmask, &len);
    if (err != ESP_OK) {
        s_wifi.static_netmask[0] = '\0';
    }
    len = sizeof(s_wifi.static_gateway);
    err = nvs_get_str(h, NVS_KEY_STATIC_GW, s_wifi.static_gateway, &len);
    if (err != ESP_OK) {
        s_wifi.static_gateway[0] = '\0';
    }

    nvs_close(h);
    return ESP_OK;
}

/* Every writer below targets WIFI_NVS_PARTITION unconditionally. The old
 * default-partition copy is deliberately never written again (nor deleted --
 * see the migration note in wifi_prov_start()), so a rollback to firmware that
 * predates the split still finds the credentials it knew about, just frozen at
 * whatever they were when this build first ran. */

/* Reads the legacy single-network keys (NVS_KEY_SSID/NVS_KEY_PASS/
 * NVS_KEY_HAS_CREDS) out of `partition`, with the same has_creds-missing
 * inference the pre-8.4 nvs_load_from() used to apply in place: if the flag
 * key is genuinely absent, trust the presence of a non-empty ssid rather than
 * silently reporting "nothing saved" (covers a partial write that landed
 * ssid/pass but not the flag). This is read-only and exists purely to feed
 * the one-time list-format migration in nvs_load_saved_nets() -- nothing
 * else in this build ever reads these keys again, and nothing writes them. */
static void nvs_load_legacy_single(const char *partition, saved_net_t *out_net, bool *out_has)
{
    memset(out_net, 0, sizeof(*out_net));
    *out_has = false;

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(partition, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return; /* namespace/partition absent -- nothing legacy to find */
    }

    size_t len = sizeof(out_net->ssid);
    err = nvs_get_str(h, NVS_KEY_SSID, out_net->ssid, &len);
    if (err != ESP_OK) {
        out_net->ssid[0] = '\0';
    }

    len = sizeof(out_net->password);
    err = nvs_get_str(h, NVS_KEY_PASS, out_net->password, &len);
    if (err != ESP_OK) {
        out_net->password[0] = '\0';
    }

    uint8_t u8 = 0;
    err = nvs_get_u8(h, NVS_KEY_HAS_CREDS, &u8);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out_has = out_net->ssid[0] != '\0';
    } else {
        *out_has = (err == ESP_OK) && u8;
    }

    nvs_close(h);
}

/* Reads the current saved_nets_blob_t out of `partition`, applying the same
 * 3-outcome version-check pattern as zones_http.c/rules_http.c's blob
 * loaders: exact SAVED_NETS_VERSION match uses it as-is; an absent/unreadable/
 * wrong-size blob is treated as an empty list (count 0, not an error -- first
 * boot looks the same as "nothing saved"); a version NEWER than this build's
 * SAVED_NETS_VERSION means the blob was written by newer firmware (the
 * firmware-rollback case) -- refuse to load it and leave flash untouched
 * rather than risk misinterpreting fields/layout this build doesn't know
 * about, same rationale as zones_cfg_t's version check. */
static esp_err_t nvs_load_saved_nets_from(const char *partition, saved_nets_blob_t *out_blob)
{
    memset(out_blob, 0, sizeof(*out_blob));

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(partition, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_PART_NOT_FOUND) {
        out_blob->version = SAVED_NETS_VERSION;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(*out_blob);
    err = nvs_get_blob(h, NVS_KEY_SAVED_NETS, out_blob, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(out_blob, 0, sizeof(*out_blob));
        out_blob->version = SAVED_NETS_VERSION;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saved_nets blob read from '%s' failed (%s) -- treating as empty",
                 partition, esp_err_to_name(err));
        memset(out_blob, 0, sizeof(*out_blob));
        out_blob->version = SAVED_NETS_VERSION;
        return ESP_OK;
    }
    if (len != sizeof(*out_blob)) {
        ESP_LOGW(TAG, "saved_nets blob from '%s' is the wrong size -- treating as empty", partition);
        memset(out_blob, 0, sizeof(*out_blob));
        out_blob->version = SAVED_NETS_VERSION;
        return ESP_OK;
    }

    if (out_blob->version == SAVED_NETS_VERSION) {
        return ESP_OK; /* current version -- happy path */
    }
    if (out_blob->version < SAVED_NETS_VERSION) {
        /* v1 is the first version that has ever existed -- hook point for a
         * future migration, nothing to convert yet. */
        out_blob->version = SAVED_NETS_VERSION;
        return ESP_OK;
    }
    /* out_blob->version > SAVED_NETS_VERSION: written by newer firmware
     * (firmware-rollback case, same as zones_cfg_t) -- refuse to load,
     * flash data left untouched. */
    ESP_LOGW(TAG, "saved_nets blob from '%s' is version %u, newer than this firmware's %u -- "
                  "refusing to load, flash data left untouched",
             partition, (unsigned)out_blob->version, (unsigned)SAVED_NETS_VERSION);
    memset(out_blob, 0, sizeof(*out_blob));
    out_blob->version = SAVED_NETS_VERSION;
    return ESP_OK;
}

static esp_err_t nvs_save_saved_nets(void)
{
    s_wifi.saved_nets.version = SAVED_NETS_VERSION;

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(WIFI_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_SAVED_NETS, &s_wifi.saved_nets, sizeof(s_wifi.saved_nets));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_save_mode(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(WIFI_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY_MODE, s_wifi.mode == WIFI_PROV_MODE_AP ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_save_ap_ssid(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(WIFI_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_AP_SSID, s_wifi.ap_ssid);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_HAS_AP_SSID, s_wifi.has_ap_ssid_override ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_save_ap_password(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(WIFI_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_AP_PASS, s_wifi.ap_password);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_HAS_AP_PASS, s_wifi.has_ap_password_override ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* 2026-08-20, web-GUI-only: persists ip_mode + the three static-IP strings
 * together (they only ever change as a group -- see do_set_dhcp()/
 * do_set_static_ip()). Same partition/namespace/commit pattern as every
 * other nvs_save_*() in this file. */
static esp_err_t nvs_save_ip_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(WIFI_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY_IP_MODE, s_wifi.ip_mode == WIFI_PROV_IP_MODE_STATIC ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_STATIC_IP, s_wifi.static_ip);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_STATIC_NETMASK, s_wifi.static_netmask);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_STATIC_GW, s_wifi.static_gateway);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* Brings up one NVS partition, erasing ONLY that partition if its contents are
 * unusable. The scoping is the entire point of the 2026-08-12 split: the old
 * code answered a recovery condition with a blanket nvs_flash_erase(), whose
 * blast radius was the default partition -- credentials, zones, rules,
 * profiles and run_state all at once. Now each partition's recovery can only
 * destroy its own contents, so a corrupt default NVS cannot take the Wi-Fi
 * credentials with it and a corrupt wifi_nvs cannot take the kiln config.
 *
 * NO_FREE_PAGES / NEW_VERSION_FOUND genuinely have no other cure -- NVS cannot
 * mount at all in either state -- so erasing is the only way forward; the
 * requirement is just that it stays inside the partition that is actually
 * broken. */
static esp_err_t nvs_partition_init(const char *partition)
{
    esp_err_t err = nvs_flash_init_partition(partition);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition '%s' needs erase (%s) -- erasing THAT PARTITION ONLY and retrying",
                 partition, esp_err_to_name(err));
        err = nvs_flash_erase_partition(partition);
        if (err == ESP_OK) {
            err = nvs_flash_init_partition(partition);
        }
    }
    return err;
}

/* One-time move of the persisted config out of the default partition's
 * wifi_cfg namespace and into WIFI_NVS_PARTITION's, for boards that were
 * provisioned by firmware predating the split. s_wifi must already hold
 * whatever WIFI_NVS_PARTITION yielded; this decides whether the old copy
 * should win and, if so, leaves s_wifi holding it and persists it to the new
 * home.
 *
 * The old copy is intentionally left in place rather than deleted: someone
 * rolling back to pre-split firmware to chase a regression should still find a
 * board that can join its network. The cost is a stale duplicate that this
 * build never reads again after the migration and never writes at all --
 * cheap, and strictly safer than the alternative. */
static void migrate_from_default_partition(bool found_in_wifi_nvs)
{
    /* Snapshot first: nvs_load_from() writes straight into s_wifi, so the
     * speculative read of the old copy below would otherwise clobber a
     * perfectly good new-partition config if the old namespace turns out to
     * hold nothing useful. */
    struct wifi_prov_state from_wifi_nvs = s_wifi;

    /* Legacy single-network credentials, read directly -- nvs_load_from()
     * itself no longer touches NVS_KEY_SSID/PASS/HAS_CREDS (see
     * nvs_load_saved_nets_from() for the current, list-based loader). These
     * are needed only to decide which side's legacy credentials should feed
     * the one-time list-format migration in nvs_load_saved_nets(): whichever
     * whole config "wins" the adopt decision below is also whichever legacy
     * credentials win, so s_legacy_single is set on every return path. */
    saved_net_t wifi_nvs_legacy_net;
    bool wifi_nvs_has_legacy = false;
    nvs_load_legacy_single(WIFI_NVS_PARTITION, &wifi_nvs_legacy_net, &wifi_nvs_has_legacy);

    bool found_in_default = false;
    esp_err_t err = nvs_load_from(NVS_DEFAULT_PART_NAME, &found_in_default);
    if (err != ESP_OK || !found_in_default) {
        s_wifi = from_wifi_nvs;
        s_legacy_single.has = wifi_nvs_has_legacy;
        s_legacy_single.net = wifi_nvs_legacy_net;
        return;
    }

    saved_net_t default_legacy_net;
    bool default_has_legacy = false;
    nvs_load_legacy_single(NVS_DEFAULT_PART_NAME, &default_legacy_net, &default_has_legacy);

    /* Adopt the old copy when the new home has nothing at all (the true
     * first-boot-after-the-split case), or when the new home exists but has no
     * legacy credentials while the old one does -- which is what a board looks
     * like if it reached the new firmware, saved only an AP-identity override,
     * and still has its real network sitting in the old partition. Anything
     * the new partition has already recorded otherwise wins outright; the new
     * location is the source of truth from the moment it holds credentials. */
    bool adopt = !found_in_wifi_nvs || (default_has_legacy && !wifi_nvs_has_legacy);
    if (!adopt) {
        s_wifi = from_wifi_nvs;
        s_legacy_single.has = wifi_nvs_has_legacy;
        s_legacy_single.net = wifi_nvs_legacy_net;
        return;
    }

    ESP_LOGI(TAG, "migrating Wi-Fi config from the default NVS partition to '%s' (ssid '%s', mode %s)",
             WIFI_NVS_PARTITION, default_legacy_net.ssid, s_wifi.mode == WIFI_PROV_MODE_AP ? "AP" : "home");

    s_legacy_single.has = default_has_legacy;
    s_legacy_single.net = default_legacy_net;

    /* Write the mode/AP-identity fields through, same as before -- they're
     * part of the same persisted config and would otherwise silently revert
     * to defaults on the next boot, once this function stops adopting the old
     * copy. The legacy credentials themselves are NOT written back out here
     * (no nvs_save_creds() any more): s_legacy_single is only ever consumed
     * by nvs_load_saved_nets(), which persists it in the new list format, not
     * the old single-key one. Failures are logged and survivable -- s_wifi is
     * already correct for this boot either way, and the migration simply gets
     * retried next time. */
    esp_err_t save_err = nvs_save_mode();
    if (save_err == ESP_OK && s_wifi.has_ap_ssid_override) {
        save_err = nvs_save_ap_ssid();
    }
    if (save_err == ESP_OK && s_wifi.has_ap_password_override) {
        save_err = nvs_save_ap_password();
    }
    if (save_err != ESP_OK) {
        ESP_LOGE(TAG, "migration write to '%s' failed: %s -- running from the old copy this boot, will retry",
                 WIFI_NVS_PARTITION, esp_err_to_name(save_err));
    }
}

/* Loads the current saved-networks list from WIFI_NVS_PARTITION and, the
 * first time this ever runs against a board that has no list saved yet but
 * does have a winning legacy single-network credential (s_legacy_single, set
 * by migrate_from_default_partition() just before this is called), wraps
 * that single credential into nets[0]/count=1 and persists it in the new
 * format. The old single-key entries are never deleted and never read again
 * after this -- same "leave the old copy in place" rationale as every other
 * migration in this file. Must be called after migrate_from_default_
 * partition() so s_legacy_single reflects the cross-partition "which copy
 * wins" decision, not just whatever WIFI_NVS_PARTITION alone happened to
 * hold. */
static void nvs_load_saved_nets(void)
{
    esp_err_t err = nvs_load_saved_nets_from(WIFI_NVS_PARTITION, &s_wifi.saved_nets);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saved_nets load from '%s' failed: %s -- starting with an empty list",
                 WIFI_NVS_PARTITION, esp_err_to_name(err));
        memset(&s_wifi.saved_nets, 0, sizeof(s_wifi.saved_nets));
        s_wifi.saved_nets.version = SAVED_NETS_VERSION;
    }

    if (s_wifi.saved_nets.count == 0 && s_legacy_single.has && s_legacy_single.net.ssid[0] != '\0') {
        ESP_LOGI(TAG, "migrating legacy single-network credential ('%s') to the saved-networks list",
                 s_legacy_single.net.ssid);
        s_wifi.saved_nets.nets[0] = s_legacy_single.net;
        s_wifi.saved_nets.count = 1;
        s_wifi.saved_nets.version = SAVED_NETS_VERSION;
        esp_err_t save_err = nvs_save_saved_nets();
        if (save_err != ESP_OK) {
            ESP_LOGE(TAG, "saving migrated saved_nets list to '%s' failed: %s -- will retry next boot",
                     WIFI_NVS_PARTITION, esp_err_to_name(save_err));
        }
    }
}

/* ---- Wi-Fi driver config helpers -------------------------------------- */

static void apply_ap_config(void)
{
    wifi_config_t ap_cfg = { 0 };
    const char *ssid = s_wifi.has_ap_ssid_override ? s_wifi.ap_ssid : WIFI_AP_SSID;
    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);
    ap_cfg.ap.channel = WIFI_AP_CHANNEL;
    ap_cfg.ap.max_connection = 4;
    const char *pw = s_wifi.has_ap_password_override ? s_wifi.ap_password : WIFI_AP_DEFAULT_PASSWORD;
    if (strlen(pw) >= 8) {
        strncpy((char *)ap_cfg.ap.password, pw, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        /* WPA2-PSK requires an 8-63 char password; a shorter configured
         * default would otherwise fail esp_wifi_set_config outright. Open is
         * safer than silently refusing to start the AP a phone needs to
         * reach in order to provision the board at all. */
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        /* wifi_prov_set_ap_password() itself refuses a 1-7 char override, so
         * this only fires for a misconfigured KILNCTL_WIFI_AP_DEFAULT_PASSWORD
         * (Kconfig isn't validated the same way) or a deliberate empty
         * override (open AP). */
        ESP_LOGW(TAG, "AP password is shorter than 8 chars -- AP is OPEN");
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %s", esp_err_to_name(err));
    }
}

/* 2026-08-20, web-GUI-only: parses a dotted-quad IPv4 string via lwIP's
 * ip4addr_aton() (lwip/ip4_addr.h) into an esp_ip4_addr_t. Both types are a
 * single uint32_t addr in network byte order under the hood, hence the
 * plain .addr copy rather than a cast -- they are deliberately different
 * *types* (lwIP's own vs. esp_netif's public one) even though the layout
 * matches. Returns false (out left untouched) on anything that doesn't
 * parse, including empty/NULL -- callers must not feed that to
 * esp_netif_set_ip_info(). */
static bool parse_ipv4(const char *s, esp_ip4_addr_t *out)
{
    if (!s || !*s) {
        return false;
    }
    ip4_addr_t addr;
    if (ip4addr_aton(s, &addr) == 0) {
        return false;
    }
    out->addr = addr.addr;
    return true;
}

/* Configures the STA driver for whatever is currently in s_wifi.active_ssid/
 * active_password -- callers are responsible for having set those first (see
 * select_and_apply_join_candidate() and the direct nets[0] assignment in
 * wifi_prov_start()).
 *
 * 2026-08-20, web-GUI-only static-IP addition: also applies s_wifi.ip_mode to
 * the STA netif, BEFORE returning to every caller's subsequent
 * esp_wifi_connect() -- start_sta_join(), do_rescan_tick(), and
 * wifi_prov_start()'s own initial-config branch all call this and only then
 * call/trigger esp_wifi_connect() (the latter via WIFI_EVENT_STA_START once
 * esp_wifi_start() runs). ESP-IDF's own static-IP examples set this up
 * BEFORE the STA associates, not after -- doing it here, inside the one
 * function every join path already funnels through, is what makes that
 * ordering automatic instead of something each call site has to remember.
 *
 * KNOWN LIMITATION, not fixed here: 802.11 association does not depend on L3
 * correctness. A wrong-but-well-formed static config (bad gateway, wrong
 * subnet) still lets the join reach WIFI_PROV_STATE_CONNECTED -- esp_netif
 * fires IP_EVENT_STA_GOT_IP locally once a static IP is set and the link
 * associates, it does not wait on a DHCP handshake that would otherwise
 * time out and let ap_fallback_timer notice a failure. So a bad static IP
 * does NOT trigger this module's usual AP-fallback safety net; the board
 * will report "connected" while actually unreachable at that address. It
 * does not brick Wi-Fi (the AP can still be reached by switching back to
 * DHCP or AP mode via this same HTTP API, or by power-cycling into
 * AP-fallback if nothing ever reaches CONNECTED), but this asymmetry is
 * worth a human double-checking before this ships to hardware anyone
 * depends on for remote access. */
static void apply_sta_config(void)
{
    wifi_config_t sta_cfg = { 0 };
    strncpy((char *)sta_cfg.sta.ssid, s_wifi.active_ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, s_wifi.active_password, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = strlen(s_wifi.active_password) > 0 ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(STA) failed: %s", esp_err_to_name(err));
    }

    if (!s_wifi.sta_netif) {
        return;
    }
    if (s_wifi.ip_mode == WIFI_PROV_IP_MODE_STATIC) {
        esp_ip4_addr_t ip, netmask, gw;
        if (parse_ipv4(s_wifi.static_ip, &ip) && parse_ipv4(s_wifi.static_netmask, &netmask) &&
            parse_ipv4(s_wifi.static_gateway, &gw)) {
            esp_err_t dhcp_err = esp_netif_dhcpc_stop(s_wifi.sta_netif);
            if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
                ESP_LOGW(TAG, "esp_netif_dhcpc_stop failed: %s", esp_err_to_name(dhcp_err));
            }
            esp_netif_ip_info_t ip_info = { .ip = ip, .netmask = netmask, .gw = gw };
            esp_err_t set_err = esp_netif_set_ip_info(s_wifi.sta_netif, &ip_info);
            if (set_err != ESP_OK) {
                ESP_LOGE(TAG, "esp_netif_set_ip_info failed: %s -- static IP not applied for this join",
                         esp_err_to_name(set_err));
            }
        } else {
            /* Stored config that no longer parses (shouldn't happen --
             * wifi_prov_set_static_ip() validates before ever persisting
             * this -- but NVS is not immune to bit rot). Fail toward
             * reachability rather than an unconfigured/zero IP. */
            ESP_LOGE(TAG, "stored static IP config failed to parse -- falling back to DHCP for this join");
            esp_netif_dhcpc_start(s_wifi.sta_netif);
        }
    } else {
        esp_err_t dhcp_err = esp_netif_dhcpc_start(s_wifi.sta_netif);
        if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGW(TAG, "esp_netif_dhcpc_start failed: %s", esp_err_to_name(dhcp_err));
        }
    }
}

/* Selects which saved network to attempt next and configures the STA driver
 * for it (active_ssid/active_password + apply_sta_config()). Only called
 * from home-mode join paths (start_sta_join() and the disconnect branch of
 * on_wifi_event()) where s_wifi.saved_nets.count > 0 is already guaranteed
 * by the caller.
 *
 * Tie-break policy (2026-08-13, TODO.md 8.4): scan first and prefer the
 * HIGHEST-RSSI saved network actually in range. Iterating the saved list in
 * order and only replacing the current best on a STRICT improvement means
 * equal RSSI favors the earlier entry in the saved list -- list order is a
 * secondary preference, applied automatically by this iteration order rather
 * than as an explicit second comparison. If the scan finds NONE of the saved
 * SSIDs in range (scan failed, everything out of range, or a saved network is
 * hidden and doesn't show up in an active scan), fall back to the saved list
 * in order starting at index 0 -- today's effective single-network behavior,
 * generalized. This never blocks or skips connecting just because the scan
 * came up empty of matches.
 *
 * 2026-08-19, TODO.md 10.14 Phase 4 -- the CAUTION that used to stand here is
 * RESOLVED, and the resolution is worth stating because it was the single
 * largest reason this function was left half-finished by TODO.md 8.4. The old
 * warning was: this does a genuinely blocking scan, and on_wifi_event() runs
 * on the Wi-Fi driver's own default-event-loop task, so a disconnect-triggered
 * reconnect stalled the driver's event processing for the scan's duration.
 * That can no longer happen: on_wifi_event() does nothing but post a command
 * now, and this function only ever runs on owner_task(), a task of this
 * module's own whose whole job is to be the thing that blocks. A scan here
 * delays other *Wi-Fi commands* (intended -- they contend for one radio), not
 * the driver's event delivery.
 *
 * It calls do_scan() rather than the public wifi_prov_scan() for the deadlock
 * reason given at do_scan()'s forward declaration. */
static void select_and_apply_join_candidate(void)
{
    if (s_wifi.saved_nets.count == 0) {
        return;
    }

    int best_idx = -1;
    int8_t best_rssi = INT8_MIN;

    static wifi_prov_scan_result_t scan_results[20];
    size_t scan_count = 0;
    esp_err_t scan_err =
        do_scan(scan_results, sizeof(scan_results) / sizeof(scan_results[0]), &scan_count);
    if (scan_err == ESP_OK) {
        for (uint8_t i = 0; i < s_wifi.saved_nets.count; i++) {
            for (size_t s = 0; s < scan_count; s++) {
                if (strcmp(scan_results[s].ssid, s_wifi.saved_nets.nets[i].ssid) != 0) {
                    continue;
                }
                if (best_idx < 0 || scan_results[s].rssi > best_rssi) {
                    best_idx = i;
                    best_rssi = scan_results[s].rssi;
                }
                break; /* saved net i found in the scan; move to the next saved net */
            }
        }
    } else if (scan_err != ESP_ERR_NOT_SUPPORTED) {
        /* ESP_ERR_NOT_SUPPORTED (AP-only mode) shouldn't happen here since
         * this only runs from home-mode join paths, but isn't worth logging
         * as a warning if it somehow does -- anything else genuinely is. */
        ESP_LOGW(TAG, "auto-join scan failed: %s -- falling back to saved-list order", esp_err_to_name(scan_err));
    }

    if (best_idx < 0) {
        best_idx = 0; /* nothing in range matched -- try list order, starting at 0 */
    }

    strncpy(s_wifi.active_ssid, s_wifi.saved_nets.nets[best_idx].ssid, sizeof(s_wifi.active_ssid) - 1);
    s_wifi.active_ssid[sizeof(s_wifi.active_ssid) - 1] = '\0';
    strncpy(s_wifi.active_password, s_wifi.saved_nets.nets[best_idx].password,
            sizeof(s_wifi.active_password) - 1);
    s_wifi.active_password[sizeof(s_wifi.active_password) - 1] = '\0';

    apply_sta_config();
}

static void cancel_ap_fallback_timer(void)
{
    if (s_wifi.ap_fallback_timer) {
        /* esp_timer_stop on an already-stopped one-shot timer is a no-op
         * error we don't care about. */
        esp_timer_stop(s_wifi.ap_fallback_timer);
    }
}

/* Runs on owner_task(), posted for by ap_fallback_timer_cb() below (which runs
 * on the esp_timer service task and does nothing but post). */
static void do_ap_fallback_tick(void)
{
    if (s_wifi.state == WIFI_PROV_STATE_CONNECTED) {
        return; /* reconnected before the timer fired */
    }
    ESP_LOGW(TAG, "station join did not land within the timeout -- bringing the fallback AP up");
    /* Mode first, then config -- the current mode here can be STA-only (see
     * on_ip_event()), which does not include the AP interface; the same
     * ESP_ERR_WIFI_MODE ordering bug fixed in wifi_prov_start(). */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA) failed: %s", esp_err_to_name(err));
    } else {
        apply_ap_config();
    }
    s_wifi.state = WIFI_PROV_STATE_RECONNECTING;
}

static void ap_fallback_timer_cb(void *arg)
{
    (void)arg;
    post_event(CMD_TMR_AP_FALLBACK);
}

/* TODO.md 8.4: periodically look for a saved network while the board is
 * sitting in AP fallback that the OPERATOR did not choose -- i.e. mode is
 * still WIFI_PROV_MODE_HOME (a deliberate switch to WIFI_PROV_MODE_AP is
 * left alone; that AP is intentional, permanent, and this must never
 * second-guess it) but state isn't WIFI_PROV_STATE_CONNECTED, meaning
 * either no join has landed yet or a previous one dropped and is being
 * retried.
 *
 * 2026-08-19, Phase 4: this now runs on owner_task() (posted for by
 * rescan_timer_cb() below), not on the esp_timer service task. The original
 * reason this path existed at all -- "the timer service task is a safe place
 * to block on a scan, the Wi-Fi event-loop task is not" -- is subsumed by
 * there now being ONE task where every blocking Wi-Fi operation happens. The
 * periodic cadence is still worth keeping for its own sake (it re-picks a
 * better candidate rather than retrying the same one), so nothing about the
 * policy changed, only which task executes it. */
static void do_rescan_tick(void)
{
    if (s_wifi.mode != WIFI_PROV_MODE_HOME || s_wifi.state == WIFI_PROV_STATE_CONNECTED ||
        s_wifi.saved_nets.count == 0) {
        return;
    }
    ESP_LOGI(TAG, "periodic rescan: looking for a saved network while in AP fallback");
    select_and_apply_join_candidate();
    esp_wifi_connect();
}

static void rescan_timer_cb(void *arg)
{
    (void)arg;
    post_event(CMD_TMR_RESCAN);
}

static void start_ap_fallback_timer(void)
{
    if (!s_wifi.ap_fallback_timer) {
        return;
    }
    cancel_ap_fallback_timer();
    esp_err_t err =
        esp_timer_start_once(s_wifi.ap_fallback_timer, (uint64_t)WIFI_STA_CONNECT_TIMEOUT_MS * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_once failed: %s", esp_err_to_name(err));
    }
}

/* Common to wifi_prov_add_network() and wifi_prov_set_mode(HOME): starts (or
 * restarts) a station join attempt, AP staying up alongside it until the
 * join is confirmed. Only meaningful when s_wifi.saved_nets.count > 0 already
 * and mode is already WIFI_PROV_MODE_HOME -- callers are responsible for
 * having gotten there first. This is the "don't strand the phone" guarantee:
 * the AP is never torn down before IP_EVENT_STA_GOT_IP confirms the join
 * actually worked. Picks which saved network to try via
 * select_and_apply_join_candidate()'s scan-based tie-break -- see that
 * function's comment for the policy and the blocking-scan caveat. */
static void start_sta_join(void)
{
    select_and_apply_join_candidate();
    cancel_ap_fallback_timer();
    s_wifi.state = WIFI_PROV_STATE_CONNECTING;
    esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (mode_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA) failed: %s", esp_err_to_name(mode_err));
    }
    start_ap_fallback_timer();
    esp_wifi_connect();
}

/* ---- Event handlers ----------------------------------------------------
 * The two esp_event handlers below run on the default event loop's own task,
 * and the two esp_timer callbacks above run on the timer service task. As of
 * 2026-08-19 (TODO.md 10.14 Phase 4) NONE of the four touches s_wifi: each is
 * a single post_event() call, and the real work happens in the do_*() bodies
 * here, on owner_task(). That is the whole point of Phase 4 -- with these left
 * mutating state directly, "s_wifi has one writer" would have been false no
 * matter what the public API did.
 *
 * Nothing in any of this touches kiln_io/safety_link (unchanged requirement,
 * see this module's header). */

/* Runs on owner_task(). */
static void do_ev_sta_start(void)
{
    if (s_wifi.saved_nets.count > 0 && s_wifi.mode == WIFI_PROV_MODE_HOME) {
        esp_wifi_connect();
    }
}

/* Runs on owner_task(). */
static void do_ev_sta_disconnected(void)
{
    {
        s_wifi.sta_rssi = -127; /* lost connection */
        if (s_wifi.mode == WIFI_PROV_MODE_AP || s_wifi.saved_nets.count == 0) {
            return; /* not attempting station at all */
        }
        bool was_connected = (s_wifi.state == WIFI_PROV_STATE_CONNECTED);
        s_wifi.state = was_connected ? WIFI_PROV_STATE_RECONNECTING : WIFI_PROV_STATE_CONNECTING;
        ESP_LOGI(TAG, "station disconnected, retrying join");
        /* Still deliberately NOT re-running select_and_apply_join_candidate()'s
         * scan-based tie-break here, but for a DIFFERENT reason than the
         * 2026-08-13 fix this replaces. That reason was "this runs on the
         * Wi-Fi driver's event-loop task and must not block it" -- no longer
         * true after Phase 4 (this is owner_task() now). The reason it stays
         * is behavioral: a flapping link produces disconnect after
         * disconnect, and running a multi-second scan on each one would keep
         * the owner task -- and therefore every operator-initiated Wi-Fi
         * command behind it -- busy scanning instead of reconnecting. Retry
         * the SAME active_ssid/active_password (fast); let do_rescan_tick()'s
         * fixed cadence be the thing that re-picks a better candidate. */
        esp_wifi_connect();
        if (!s_wifi.ap_fallback_timer) {
            return;
        }
        /* Only (re)arm the fallback timer if it isn't already counting down
         * from a previous disconnect -- esp_timer_start_once on an already
         * running timer returns ESP_ERR_INVALID_STATE, which is fine to
         * ignore, but re-arming would keep pushing the AP fallback out on a
         * flapping link instead of ever bringing it back. */
        esp_timer_start_once(s_wifi.ap_fallback_timer,
                              (uint64_t)WIFI_STA_CONNECT_TIMEOUT_MS * 1000);
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == WIFI_EVENT_STA_START) {
        post_event(CMD_EV_STA_START);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        post_event(CMD_EV_STA_DISCONNECTED);
    }
}

/* Runs on owner_task(). */
static void do_ev_got_ip(void)
{
    cancel_ap_fallback_timer();
    ESP_LOGI(TAG, "station joined, dropping fallback AP");
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(STA) failed: %s", esp_err_to_name(err));
    }
    s_wifi.state = WIFI_PROV_STATE_CONNECTED;

    /* Capture RSSI of the connected network */
    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        s_wifi.sta_rssi = ap_info.rssi;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    post_event(CMD_EV_GOT_IP);
}

/* ---- Captive-portal DNS hijack -------------------------------------------
 *
 * 2026-08-19, explicit user report: phones joining the fallback AP
 * associate at the radio layer (device log shows a clean "station ... join,
 * AID=1") but then disassociate themselves ~30-40s later (802.11 reason 8,
 * station-initiated) in a repeating join/leave loop -- and never reach the
 * board's setup page. This matches WIFI_PROVISIONING.md's own "What's still
 * open" list, which already named the root cause: "No captive portal. A
 * phone joining the fallback AP has to browse to the board's address;
 * nothing redirects it there." Without a captive-portal redirect, a modern
 * phone OS's own connectivity check (an HTTP GET to a well-known probe URL
 * over DNS names this board's DHCP-assigned "no upstream router" AP can
 * never resolve to anything real) times out, the OS concludes "no internet"
 * and gives up on the network entirely -- exactly the observed loop. A real
 * DNS responder (any name resolves to this board's own AP IP) plus an HTTP
 * redirect for anything not already a known route turns that probe into a
 * hit, which is what makes every phone OS auto-pop its "sign in to network"
 * browser instead of dropping the connection.
 *
 * Bound to 0.0.0.0:53 rather than the AP interface specifically, and
 * started unconditionally alongside the netifs below (same "always-on,
 * negligible cost, no per-mode start/stop wiring needed" reasoning
 * start_ap_fallback_timer()'s own rescan_timer_cb() already uses for this
 * file): in STA-only operation nothing ever sends this board a DNS query
 * (it isn't anyone's configured resolver), so the task just blocks forever
 * on recvfrom() and costs nothing. It only ever answers queries that
 * actually arrive, which in practice only happens while the AP is up. */
static void dns_hijack_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns_hijack: socket() failed (errno %d) -- captive-portal redirect disabled", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr = { 0 };
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(53);
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGE(TAG, "dns_hijack: bind(:53) failed (errno %d) -- captive-portal redirect disabled", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    /* Fixed at the softAP's well-known default (esp_netif_create_default_wifi_ap()
     * always assigns 192.168.4.1 and nothing in this file overrides it -- no
     * esp_netif_set_ip_info() call exists anywhere in wifi_prov.c) rather than
     * queried live from s_wifi.ap_netif, since this task starts before
     * esp_wifi_start() even runs and must not depend on AP-up ordering. */
    const uint32_t ap_ip = htonl(0xC0A80401u); /* 192.168.4.1 */

    uint8_t buf[512];
    while (true) {
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from_addr, &from_len);
        if (len < 12) {
            continue; /* shorter than a DNS header -- not a real query */
        }

        /* Only handles the single-question case every real stub resolver
         * sends (multi-question DNS queries are vanishingly rare and not
         * worth parsing for a hijack responder that only needs to satisfy
         * "does this name resolve to *something*"). QDCOUNT lives at bytes
         * 4-5; bail rather than build a wrong answer if it's not exactly 1. */
        uint16_t qdcount = ((uint16_t)buf[4] << 8) | buf[5];
        if (qdcount != 1) {
            continue;
        }

        /* Walk the QNAME (length-prefixed labels, terminated by a 0 byte) to
         * find where it ends, rather than assuming a fixed offset -- domain
         * names are variable length and this is the only way to find QTYPE/
         * QCLASS (4 bytes right after) or where to start appending the
         * answer record. */
        int qname_start = 12;
        int i = qname_start;
        while (i < len && buf[i] != 0) {
            i += buf[i] + 1;
            if (i >= len) break; /* malformed -- length ran off the packet */
        }
        if (i >= len || i + 5 > len) {
            continue; /* malformed query, nothing safe to reply to */
        }
        int qname_end = i + 1; /* one past the terminating 0 byte */
        int question_end = qname_end + 4; /* + QTYPE(2) + QCLASS(2) */

        /* Reply buffer: header + question (echoed verbatim) + one A answer
         * record. Answer uses name-compression pointer 0xC00C (back to the
         * question's QNAME at offset 12) instead of repeating the name. */
        uint8_t reply[512];
        if (question_end > (int)sizeof(reply) - 16) {
            continue; /* question too long to fit a reply in this buffer -- drop it */
        }
        memcpy(reply, buf, (size_t)question_end);

        reply[2] = 0x81; /* QR=1 (response), OPCODE=0, AA=1, TC=0, RD=echoed below */
        reply[3] = 0x80; /* RA=1, Z=0, RCODE=0 (no error) */
        reply[2] |= (buf[2] & 0x01); /* echo RD */
        reply[6] = 0x00; reply[7] = 0x01; /* ANCOUNT = 1 */
        reply[8] = 0x00; reply[9] = 0x00; /* NSCOUNT = 0 */
        reply[10] = 0x00; reply[11] = 0x00; /* ARCOUNT = 0 */

        int p = question_end;
        reply[p++] = 0xC0; reply[p++] = 0x0C; /* name = pointer to offset 12 */
        reply[p++] = 0x00; reply[p++] = 0x01; /* TYPE = A */
        reply[p++] = 0x00; reply[p++] = 0x01; /* CLASS = IN */
        reply[p++] = 0x00; reply[p++] = 0x00; reply[p++] = 0x00; reply[p++] = 0x3C; /* TTL = 60s */
        reply[p++] = 0x00; reply[p++] = 0x04; /* RDLENGTH = 4 */
        memcpy(&reply[p], &ap_ip, 4);
        p += 4;

        sendto(sock, reply, (size_t)p, 0, (struct sockaddr *)&from_addr, from_len);
    }
}

static void start_dns_hijack_task(void)
{
    BaseType_t created = xTaskCreatePinnedToCore(dns_hijack_task, "dns_hijack", 3072, NULL, 4, NULL,
                                                 tskNO_AFFINITY);
    if (created != pdPASS) {
        ESP_LOGW(TAG, "xTaskCreatePinnedToCore(dns_hijack) failed -- no captive-portal DNS redirect");
    }
}

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
    esp_err_t default_err = nvs_partition_init(NVS_DEFAULT_PART_NAME);
    if (default_err != ESP_OK) {
        ESP_LOGW(TAG, "default NVS init failed: %s -- one-time migration reads for "
                 "zones/rules/profiles/run_state/relay_cycles will find nothing to migrate "
                 "(harmless if already migrated; otherwise those sections start unconfigured)",
                 esp_err_to_name(default_err));
    }

    esp_err_t err = nvs_partition_init(WIFI_NVS_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init for '%s' failed: %s -- Wi-Fi credentials cannot persist",
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
            ESP_LOGW(TAG, "wifi_cfg load from '%s' failed: %s -- starting unprovisioned",
                     WIFI_NVS_PARTITION, esp_err_to_name(err));
            s_wifi.mode = WIFI_PROV_MODE_HOME;
        } else {
            if (default_err == ESP_OK) {
                /* Only worth attempting when the default partition actually
                 * mounted -- there is nothing to migrate from otherwise. */
                migrate_from_default_partition(found_in_wifi_nvs);
            } else {
                /* No cross-partition migration possible, but WIFI_NVS_PARTITION
                 * itself may still hold legacy single-key credentials from a
                 * pre-8.4 build of THIS partition's own format -- make sure
                 * nvs_load_saved_nets() below still has a chance to pick them
                 * up. migrate_from_default_partition() would normally set
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
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }
    /* ESP_ERR_INVALID_STATE means a default loop already exists -- fine,
     * some other subsystem may have created it first. Anything else is a
     * real failure. */
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    s_wifi.ap_netif = esp_netif_create_default_wifi_ap();
    s_wifi.sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_wifi.ap_netif || !s_wifi.sta_netif) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_* failed");
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
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
        ESP_LOGW(TAG, "esp_timer_create failed: %s -- no AP-fallback-on-reconnect-timeout", esp_err_to_name(err));
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
        ESP_LOGW(TAG, "esp_timer_create failed: %s -- no periodic AP-fallback rescan", esp_err_to_name(err));
        s_wifi.rescan_timer = NULL;
    } else {
        err = esp_timer_start_periodic(s_wifi.rescan_timer, (uint64_t)WIFI_AP_FALLBACK_RESCAN_INTERVAL_MS * 1000);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_timer_start_periodic failed: %s -- no periodic AP-fallback rescan",
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
        ESP_LOGI(TAG, "AP mode: AP '%s' only, station never attempted", wifi_prov_get_ap_ssid());
    } else if (s_wifi.saved_nets.count > 0) {
        /* Initial boot config, before the Wi-Fi driver/task exists at all --
         * no scan-based tie-break here (wifi_prov_scan() requires the driver
         * already started, which it isn't yet at this point in bring-up).
         * Just take the first saved entry; the scan-based tie-break in
         * select_and_apply_join_candidate() takes over from the very next
         * join attempt onward (a disconnect, or an explicit add/mode
         * change). */
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
        ESP_LOGI(TAG, "attempting station join to '%s', AP '%s' available meanwhile", s_wifi.active_ssid,
                 wifi_prov_get_ap_ssid());
    } else {
        s_wifi.state = WIFI_PROV_STATE_UNPROVISIONED;
        err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (err == ESP_OK) {
            apply_ap_config();
        }
        ESP_LOGI(TAG, "no saved credentials: AP '%s' for first-boot provisioning", wifi_prov_get_ap_ssid());
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
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
        ESP_LOGE(TAG, "xQueueCreate(wifi_owner) failed");
        return ESP_ERR_NO_MEM;
    }
    BaseType_t task_created =
        xTaskCreatePinnedToCore(owner_task, "wifi_prov_owner", 4096, NULL, 5, NULL, tskNO_AFFINITY);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore(wifi_prov_owner) failed");
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* See dns_hijack_task()'s header comment for what this fixes (phones
     * joining the fallback AP then giving up). Started here, after
     * esp_wifi_start() rather than right after netif creation: this file's
     * own uart_protocol_register_task() comment documents a real internal-
     * SRAM contention window during "wifi_prov_start()'s STA+AP bring-up"
     * that starved xQueueCreate() for every UART bridge task racing it at
     * boot (bench-observed 2026-08-18, root-caused and retried around
     * there). socket()/bind() also allocate from that same internal-SRAM
     * pool (lwIP's socket/pcb structures are never satisfied from PSRAM
     * either) -- starting this task any earlier than here sits it inside
     * that exact contention window instead of after it, which is
     * suspected (2026-08-19 bench observation, not yet certain) to have
     * caused a UART bridge task registration failure the same boot. */
    start_dns_hijack_task();

    err = wifi_provision_http_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_provision_http_start failed: %s -- Wi-Fi is up but not provisionable over HTTP",
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

/* Runs on owner_task(). Both strings are NUL-terminated and already
 * length-validated by the producer -- validation stays on the CALLER's side
 * of the queue throughout this file, so a malformed request is refused
 * immediately and never costs a queue slot or a task hop. */
static esp_err_t do_add_network(const char *new_ssid, const char *password)
{
    size_t password_len = strlen(password);

    /* Upsert by exact SSID match -- update the password in place if this
     * SSID is already saved, otherwise append a new entry. */
    int idx = -1;
    for (uint8_t i = 0; i < s_wifi.saved_nets.count; i++) {
        if (strcmp(s_wifi.saved_nets.nets[i].ssid, new_ssid) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (s_wifi.saved_nets.count >= WIFI_PROV_MAX_SAVED_NETWORKS) {
            /* List is full and this is a genuinely new SSID -- refuse
             * without touching anything already saved, rather than silently
             * evicting an existing entry the operator didn't ask to remove. */
            return ESP_ERR_NO_MEM;
        }
        idx = s_wifi.saved_nets.count;
        s_wifi.saved_nets.count++;
        strncpy(s_wifi.saved_nets.nets[idx].ssid, new_ssid, sizeof(s_wifi.saved_nets.nets[idx].ssid) - 1);
        s_wifi.saved_nets.nets[idx].ssid[sizeof(s_wifi.saved_nets.nets[idx].ssid) - 1] = '\0';
    }
    memcpy(s_wifi.saved_nets.nets[idx].password, password, password_len);
    s_wifi.saved_nets.nets[idx].password[password_len] = '\0';

    esp_err_t err = nvs_save_saved_nets();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_saved_nets failed: %s -- this network will not survive a reboot",
                 esp_err_to_name(err));
        /* Still attempt the join below -- the operator asked for this
         * network right now, whether or not it persists. */
    }

    if (s_wifi.mode == WIFI_PROV_MODE_AP) {
        /* Submitting a network is an explicit choice to join something -- it
         * supersedes a previously-set AP-mode preference the same way
         * picking a network in the Network settings page (TODO.md section 4)
         * would. Without this, AP mode had no way back out through the HTTP
         * API: the provisioning page's mode toggle set it, but nothing ever
         * cleared it again from this path. */
        ESP_LOGI(TAG, "network submitted while in AP mode -- switching to home mode");
        s_wifi.mode = WIFI_PROV_MODE_HOME;
        esp_err_t mode_err = nvs_save_mode();
        if (mode_err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_save_mode failed: %s -- choice will not survive a reboot",
                     esp_err_to_name(mode_err));
        }
    }

    /* start_sta_join() re-runs the scan-based tie-break, so this may join a
     * different (stronger, already-in-range) saved network than the one just
     * added -- that's intended, not a bug: adding a network is "make this
     * available", not "connect to this one specifically". */
    start_sta_join();
    return ESP_OK;
}

esp_err_t wifi_prov_add_network(const char *ssid, size_t ssid_len, const char *password,
                                 size_t password_len)
{
    if (!ssid || ssid_len == 0 || ssid_len > WIFI_PROV_SSID_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!password || password_len > WIFI_PROV_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_cmd_t cmd = { .type = CMD_ADD_NETWORK };
    memcpy(cmd.args.add_network.ssid, ssid, ssid_len);
    cmd.args.add_network.ssid[ssid_len] = '\0';
    memcpy(cmd.args.add_network.password, password, password_len);
    cmd.args.add_network.password[password_len] = '\0';

    wifi_result_t r;
    /* The generous wait matters most here: do_add_network() calls
     * start_sta_join(), which re-runs the scan-based tie-break -- a real
     * blocking scan -- before it returns. This producer's caller is either an
     * HTTP handler or ui_page_network.c's connect_job_t worker task, both of
     * which already blocked for exactly this work before Phase 4. */
    if (!post_and_wait(&cmd, &r, WIFI_OWNER_SCAN_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

esp_err_t wifi_prov_set_credentials(const char *ssid, size_t ssid_len, const char *password,
                                    size_t password_len)
{
    return wifi_prov_add_network(ssid, ssid_len, password, password_len);
}

/* Runs on owner_task(). */
static esp_err_t do_forget_network(const char *target)
{
    int idx = -1;
    for (uint8_t i = 0; i < s_wifi.saved_nets.count; i++) {
        if (strcmp(s_wifi.saved_nets.nets[i].ssid, target) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return ESP_OK; /* nothing saved under this SSID -- not a failure */
    }

    /* Compact: shift everything after idx down by one. Deliberately no
     * "can't remove the last one" guard -- bringing count to 0 is a valid,
     * fully-supported state (falls back to AP-capable/unprovisioned
     * behavior via the count > 0 checks elsewhere in this file, the same way
     * has_creds == false used to). If this was the currently-active/
     * connected network, the existing disconnect/reconnect event handlers
     * discover on their own that there's nothing left to reconnect to --
     * this function doesn't force a disconnect itself. */
    for (uint8_t i = (uint8_t)idx; i + 1 < s_wifi.saved_nets.count; i++) {
        s_wifi.saved_nets.nets[i] = s_wifi.saved_nets.nets[i + 1];
    }
    s_wifi.saved_nets.count--;
    memset(&s_wifi.saved_nets.nets[s_wifi.saved_nets.count], 0, sizeof(s_wifi.saved_nets.nets[0]));

    esp_err_t err = nvs_save_saved_nets();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_saved_nets failed: %s -- removal will not survive a reboot",
                 esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t wifi_prov_forget_network(const char *ssid, size_t ssid_len)
{
    if (!ssid || ssid_len == 0 || ssid_len > WIFI_PROV_SSID_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_cmd_t cmd = { .type = CMD_FORGET_NETWORK };
    memcpy(cmd.args.forget_network.ssid, ssid, ssid_len);
    cmd.args.forget_network.ssid[ssid_len] = '\0';

    wifi_result_t r;
    if (!post_and_wait(&cmd, &r, WIFI_OWNER_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

/* Runs on owner_task(). Writes into the result slot rather than the caller's
 * array -- the whole saved list is at most 8 SSIDs (264 bytes), so copying it
 * twice is cheaper than the lifetime question a caller pointer would raise. */
static esp_err_t do_get_saved_networks(size_t max_results, wifi_result_t *r)
{
    size_t n = s_wifi.saved_nets.count;
    if (n > max_results) {
        n = max_results;
    }
    if (n > WIFI_PROV_MAX_SAVED_NETWORKS) {
        n = WIFI_PROV_MAX_SAVED_NETWORKS; /* defensive; count can never exceed this */
    }
    for (size_t i = 0; i < n; i++) {
        strncpy(r->saved[i].ssid, s_wifi.saved_nets.nets[i].ssid, WIFI_PROV_SSID_MAX_LEN);
        r->saved[i].ssid[WIFI_PROV_SSID_MAX_LEN] = '\0';
        /* Password deliberately not copied out -- wifi_prov_saved_network_t
         * has no field for it, by design; see the .h doc comment. */
    }
    r->saved_count = n;
    return ESP_OK;
}

esp_err_t wifi_prov_get_saved_networks(wifi_prov_saved_network_t *out, size_t max_results, size_t *out_count)
{
    if (!out || max_results == 0 || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_cmd_t cmd = { .type = CMD_GET_SAVED_NETWORKS,
                       .args.get_saved_networks = { .max_results = max_results } };
    wifi_result_t r;
    if (!post_and_wait(&cmd, &r, WIFI_OWNER_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    if (r.err == ESP_OK) {
        memcpy(out, r.saved, r.saved_count * sizeof(*out));
        *out_count = r.saved_count;
    }
    return r.err;
}

/* Runs on owner_task(). */
static esp_err_t do_set_mode(wifi_prov_mode_t mode)
{
    s_wifi.mode = mode;
    esp_err_t err = nvs_save_mode();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_mode failed: %s -- choice will not survive a reboot",
                 esp_err_to_name(err));
    }

    if (mode == WIFI_PROV_MODE_AP) {
        cancel_ap_fallback_timer();
        s_wifi.state = WIFI_PROV_STATE_AP_MODE;
        /* Mode first, then config -- switching here from home mode while
         * connected leaves the driver in WIFI_MODE_STA, which doesn't
         * include the AP interface yet; same ordering bug as
         * wifi_prov_start()/ap_fallback_timer_cb(). */
        esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (mode_err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode(AP) failed: %s", esp_err_to_name(mode_err));
        } else {
            apply_ap_config();
        }
        ESP_LOGI(TAG, "AP mode enabled: station never attempted");
    } else if (s_wifi.saved_nets.count > 0) {
        ESP_LOGI(TAG, "home mode enabled, resuming join to saved network");
        start_sta_join();
    } else {
        s_wifi.state = WIFI_PROV_STATE_UNPROVISIONED;
        ESP_LOGI(TAG, "home mode enabled, no saved network to join");
    }
    return ESP_OK;
}

esp_err_t wifi_prov_set_mode(wifi_prov_mode_t mode)
{
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_cmd_t cmd = { .type = CMD_SET_MODE, .args.set_mode = { .mode = mode } };
    wifi_result_t r;
    /* Scan-length wait: the HOME branch of do_set_mode() calls
     * start_sta_join(), which scans. */
    if (!post_and_wait(&cmd, &r, WIFI_OWNER_SCAN_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

/* Runs on owner_task(). NUL-terminated, already length-validated. */
static esp_err_t do_set_ap_ssid(const char *ssid)
{
    strncpy(s_wifi.ap_ssid, ssid, sizeof(s_wifi.ap_ssid) - 1);
    s_wifi.ap_ssid[sizeof(s_wifi.ap_ssid) - 1] = '\0';
    s_wifi.has_ap_ssid_override = true;

    esp_err_t err = nvs_save_ap_ssid();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_ap_ssid failed: %s -- choice will not survive a reboot",
                 esp_err_to_name(err));
        /* Still apply it live below -- the operator asked for this right
         * now, whether or not it persists past a reboot. */
    }

    apply_ap_config();
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK && (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)) {
        /* Mirrors wifi_prov_set_ap_password()'s pattern: esp_wifi_set_config
         * alone doesn't kick already-associated clients off a running AP, so
         * re-applying the same mode forces the radio to pick up the new
         * SSID immediately instead of only after the next boot/mode change.
         * Station side (if APSTA) is untouched. */
        esp_err_t mode_err = esp_wifi_set_mode(mode);
        if (mode_err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(mode_err));
        }
    }
    ESP_LOGI(TAG, "AP SSID changed to '%s'", s_wifi.ap_ssid);
    return ESP_OK;
}

esp_err_t wifi_prov_set_ap_ssid(const char *ssid, size_t ssid_len)
{
    if (!ssid || ssid_len == 0 || ssid_len > WIFI_PROV_SSID_MAX_LEN) {
        /* The AP always needs to be reachable by something -- an empty
         * SSID isn't a valid "leave it alone" no-op here the way it can be
         * for a station password (open network); refuse it outright. */
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_cmd_t cmd = { .type = CMD_SET_AP_SSID };
    memcpy(cmd.args.set_ap_ssid.ssid, ssid, ssid_len);
    cmd.args.set_ap_ssid.ssid[ssid_len] = '\0';

    wifi_result_t r;
    if (!post_and_wait(&cmd, &r, WIFI_OWNER_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

/* Runs on owner_task(). NUL-terminated, already length-validated (including
 * the 1-7 char WPA2-PSK refusal, which stays in the producer). */
static esp_err_t do_set_ap_password(const char *password)
{
    strncpy(s_wifi.ap_password, password, sizeof(s_wifi.ap_password) - 1);
    s_wifi.ap_password[sizeof(s_wifi.ap_password) - 1] = '\0';
    s_wifi.has_ap_password_override = true;
    size_t password_len = strlen(s_wifi.ap_password);

    esp_err_t err = nvs_save_ap_password();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_ap_password failed: %s -- choice will not survive a reboot",
                 esp_err_to_name(err));
        /* Still apply it live below -- the operator asked for this right
         * now, whether or not it persists past a reboot. */
    }

    apply_ap_config();
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK && (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)) {
        /* Mirrors wifi_prov_set_ap_ssid()'s pattern: esp_wifi_set_config
         * alone doesn't kick already-associated clients off a running AP, so
         * re-applying the same mode forces the radio to pick up the new PSK
         * immediately instead of only after the next boot/mode change.
         * Station side (if APSTA) is untouched. */
        esp_err_t mode_err = esp_wifi_set_mode(mode);
        if (mode_err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(mode_err));
        }
    }
    ESP_LOGI(TAG, "AP password changed (%s)", password_len > 0 ? "WPA2-PSK" : "open");
    return ESP_OK;
}

esp_err_t wifi_prov_set_ap_password(const char *password, size_t password_len)
{
    if (!password || password_len > WIFI_PROV_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (password_len > 0 && password_len < 8) {
        /* WPA2-PSK requires 8-63 chars; a non-empty password shorter than
         * that can never work on real hardware, so refuse it outright here
         * rather than silently falling back to an open AP the way a bad
         * compile-time Kconfig default does in apply_ap_config() (there is
         * no request to fail in that case, just a build to warn about). */
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_cmd_t cmd = { .type = CMD_SET_AP_PASSWORD };
    memcpy(cmd.args.set_ap_password.password, password, password_len);
    cmd.args.set_ap_password.password[password_len] = '\0';

    wifi_result_t r;
    if (!post_and_wait(&cmd, &r, WIFI_OWNER_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

wifi_prov_ip_mode_t wifi_prov_get_ip_mode(void)
{
    /* Direct read -- one aligned enum word, same reasoning as
     * wifi_prov_get_mode(). */
    return s_wifi.ip_mode;
}

const char *wifi_prov_get_static_ip(void)
{
    return s_wifi.static_ip;
}

const char *wifi_prov_get_static_netmask(void)
{
    return s_wifi.static_netmask;
}

const char *wifi_prov_get_static_gateway(void)
{
    return s_wifi.static_gateway;
}

/* Common tail for do_set_dhcp()/do_set_static_ip(): if a station join is
 * currently active or in flight, force it to pick up the new netif config
 * right away rather than leaving a stale IP applied until the next natural
 * disconnect/reconnect. apply_sta_config() (already called by the caller
 * before this) has updated the netif's DHCP-client/static state; a
 * disconnect+reconnect is what makes the STA driver actually re-run through
 * that netif state for an already-associated link. Mirrors
 * wifi_prov_set_ap_ssid()'s/wifi_prov_set_ap_password()'s "re-apply to force
 * pickup" pattern on the AP side. No-op (and safe) if nothing is
 * connected/connecting -- do_ev_sta_disconnected() then simply has nothing
 * to retry. */
static void reapply_sta_if_active(void)
{
    if (s_wifi.mode != WIFI_PROV_MODE_HOME) {
        return; /* AP mode: no station activity to reapply */
    }
    if (s_wifi.state == WIFI_PROV_STATE_CONNECTED || s_wifi.state == WIFI_PROV_STATE_CONNECTING ||
        s_wifi.state == WIFI_PROV_STATE_RECONNECTING) {
        ESP_LOGI(TAG, "IP config changed while a station join is active -- reconnecting to apply it");
        esp_wifi_disconnect();
        esp_wifi_connect();
    }
}

/* Runs on owner_task(). */
static esp_err_t do_set_dhcp(void)
{
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    s_wifi.static_ip[0] = '\0';
    s_wifi.static_netmask[0] = '\0';
    s_wifi.static_gateway[0] = '\0';

    esp_err_t err = nvs_save_ip_config();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_ip_config failed: %s -- choice will not survive a reboot",
                 esp_err_to_name(err));
    }

    apply_sta_config();
    reapply_sta_if_active();
    ESP_LOGI(TAG, "STA IP mode set to DHCP");
    return ESP_OK;
}

esp_err_t wifi_prov_set_dhcp(void)
{
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_cmd_t cmd = { .type = CMD_SET_DHCP };
    wifi_result_t r;
    if (!post_and_wait(&cmd, &r, WIFI_OWNER_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

/* Runs on owner_task(). Strings are already NUL-terminated, but NOT yet
 * validated as dotted-quad IPv4 -- that happens in the producer
 * (wifi_prov_set_static_ip() below) before this is ever posted, same
 * "validation stays on the caller's side of the queue" convention as
 * do_add_network(). */
static esp_err_t do_set_static_ip(const char *ip, const char *netmask, const char *gateway)
{
    strncpy(s_wifi.static_ip, ip, sizeof(s_wifi.static_ip) - 1);
    s_wifi.static_ip[sizeof(s_wifi.static_ip) - 1] = '\0';
    strncpy(s_wifi.static_netmask, netmask, sizeof(s_wifi.static_netmask) - 1);
    s_wifi.static_netmask[sizeof(s_wifi.static_netmask) - 1] = '\0';
    strncpy(s_wifi.static_gateway, gateway, sizeof(s_wifi.static_gateway) - 1);
    s_wifi.static_gateway[sizeof(s_wifi.static_gateway) - 1] = '\0';
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_STATIC;

    esp_err_t err = nvs_save_ip_config();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_ip_config failed: %s -- choice will not survive a reboot",
                 esp_err_to_name(err));
        /* Still apply it live below -- the operator asked for this right
         * now, whether or not it persists past a reboot. Same convention as
         * every other setter in this file. */
    }

    apply_sta_config();
    reapply_sta_if_active();
    ESP_LOGI(TAG, "STA IP mode set to STATIC (%s/%s via %s)", s_wifi.static_ip, s_wifi.static_netmask,
             s_wifi.static_gateway);
    return ESP_OK;
}

esp_err_t wifi_prov_set_static_ip(const char *ip, const char *netmask, const char *gateway)
{
    if (!ip || !netmask || !gateway) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Validated here, on the CALLER's side of the queue -- a malformed
     * request never costs a queue slot or a task hop, same rule as every
     * other producer in this file. esp_ip4_addr_t values themselves are
     * discarded; this call only needs to know whether each string parses. */
    esp_ip4_addr_t tmp;
    if (!parse_ipv4(ip, &tmp) || !parse_ipv4(netmask, &tmp) || !parse_ipv4(gateway, &tmp)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ip) >= WIFI_PROV_IPV4_STR_MAX || strlen(netmask) >= WIFI_PROV_IPV4_STR_MAX ||
        strlen(gateway) >= WIFI_PROV_IPV4_STR_MAX) {
        return ESP_ERR_INVALID_ARG; /* can't happen if parse_ipv4 succeeded, defensive only */
    }
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_cmd_t cmd = { .type = CMD_SET_STATIC_IP };
    strncpy(cmd.args.set_static_ip.ip, ip, sizeof(cmd.args.set_static_ip.ip) - 1);
    strncpy(cmd.args.set_static_ip.netmask, netmask, sizeof(cmd.args.set_static_ip.netmask) - 1);
    strncpy(cmd.args.set_static_ip.gateway, gateway, sizeof(cmd.args.set_static_ip.gateway) - 1);

    wifi_result_t r;
    if (!post_and_wait(&cmd, &r, WIFI_OWNER_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

bool wifi_prov_is_sta_connected(void)
{
    /* Direct read, deliberately -- see wifi_prov.h's "readers that stay
     * direct" note. One aligned enum word, single-instruction load. */
    return s_wifi.state == WIFI_PROV_STATE_CONNECTED;
}

/* Runs on owner_task(). Writes the dotted-quad into the result slot; the
 * producer copies it out. */
static esp_err_t do_get_sta_ip(size_t out_cap, wifi_result_t *r)
{
    if (s_wifi.state != WIFI_PROV_STATE_CONNECTED || !s_wifi.sta_netif) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(s_wifi.sta_netif, &ip_info);
    if (err != ESP_OK) {
        return err;
    }
    if (out_cap < 16) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_ip4addr_ntoa(&ip_info.ip, r->sta_ip, (uint32_t)sizeof(r->sta_ip));
    return ESP_OK;
}

esp_err_t wifi_prov_get_sta_ip(char *out, size_t out_cap)
{
    if (!out || out_cap < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Compound state (the state enum AND the netif handle AND a driver call
     * that reads the netif's current lease), so this one goes through the
     * queue even though it is nominally a getter -- the split rule stated in
     * wifi_prov.h. */
    wifi_cmd_t cmd = { .type = CMD_GET_STA_IP, .args.get_sta_ip = { .out_cap = out_cap } };
    wifi_result_t r;
    if (!post_and_wait(&cmd, &r, WIFI_OWNER_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    if (r.err == ESP_OK) {
        strncpy(out, r.sta_ip, out_cap - 1);
        out[out_cap - 1] = '\0';
    }
    return r.err;
}

/* Runs on owner_task() -- either drained from a CMD_SCAN posted by
 * wifi_prov_scan() below, or called straight from
 * select_and_apply_join_candidate()'s auto-join tie-break, which is already
 * on this task. This is the blocking part: esp_wifi_scan_start(block=true).
 * The `started` check lives in the producer; the AP-mode refusal stays here
 * so the internal caller gets it too. */
static esp_err_t do_scan(wifi_prov_scan_result_t *results, size_t max_results, size_t *out_count)
{
    *out_count = 0;
    if (s_wifi.mode == WIFI_PROV_MODE_AP) {
        /* AP mode means no station-radio activity at all, and a scan --
         * even though it never joins anything -- still means bringing the
         * STA interface up. Refuse rather than bend that guarantee for a
         * settings-page convenience. */
        return ESP_ERR_NOT_SUPPORTED;
    }

    wifi_mode_t mode;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) {
        return err;
    }
    if (mode == WIFI_MODE_AP) {
        /* Unprovisioned: only the AP interface is up. Bring STA up too so a
         * scan is possible -- this does not by itself connect to anything;
         * on_wifi_event() only calls esp_wifi_connect() when has_creds is
         * true, and unprovisioned means it isn't. */
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            return err;
        }
    }

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 50,
        .scan_time.active.max = 150,
    };
    err = esp_wifi_scan_start(&scan_cfg, true /* block */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        return ESP_OK;
    }
    if (found > max_results) {
        found = (uint16_t)max_results;
    }

    static wifi_ap_record_t records[20];
    uint16_t to_fetch = found > (sizeof(records) / sizeof(records[0]))
                            ? (uint16_t)(sizeof(records) / sizeof(records[0]))
                            : found;
    err = esp_wifi_scan_get_ap_records(&to_fetch, records);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t n = to_fetch < max_results ? to_fetch : max_results;
    for (size_t i = 0; i < n; i++) {
        strncpy(results[i].ssid, (const char *)records[i].ssid, WIFI_PROV_SSID_MAX_LEN);
        results[i].ssid[WIFI_PROV_SSID_MAX_LEN] = '\0';
        results[i].rssi = records[i].rssi;
        results[i].secure = records[i].authmode != WIFI_AUTH_OPEN;
    }
    *out_count = n;
    return ESP_OK;
}

esp_err_t wifi_prov_scan(wifi_prov_scan_result_t *results, size_t max_results, size_t *out_count)
{
    if (!results || max_results == 0 || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_cmd_t cmd = { .type = CMD_SCAN, .args.scan = { .max_results = max_results } };
    wifi_result_t r;
    if (!post_and_wait(&cmd, &r, WIFI_OWNER_SCAN_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    if (r.err == ESP_OK && r.scan_count > 0) {
        size_t n = r.scan_count > max_results ? max_results : r.scan_count;
        memcpy(results, s_scan_stage, n * sizeof(*results));
        *out_count = n;
    }
    return r.err;
}

int8_t wifi_prov_get_sta_rssi(void)
{
    /* Direct read, same reasoning as wifi_prov_is_sta_connected(): one bool,
     * one enum word, one int8 -- see wifi_prov.h. */
    if (!s_wifi.started || s_wifi.state != WIFI_PROV_STATE_CONNECTED) {
        return -127; /* not connected */
    }
    return s_wifi.sta_rssi;
}

uint8_t wifi_prov_get_ap_client_count(void)
{
    if (!s_wifi.started) {
        return 0;
    }

    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        return 0;
    }
    if ((mode != WIFI_MODE_AP) && (mode != WIFI_MODE_APSTA)) {
        return 0; /* AP is not active */
    }

    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) {
        return 0;
    }
    return (uint8_t)(sta_list.num > 255 ? 255 : sta_list.num); /* cap at uint8 */
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
        }

        if (cmd.done) {
            /* Given LAST, after the result slot is fully written -- the
             * producer is blocked on exactly this and reads *result the
             * instant it is released. */
            xSemaphoreGive(cmd.done);
        }
    }
}
