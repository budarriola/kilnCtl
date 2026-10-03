#ifndef WIFI_PROV_INTERNAL_H
#define WIFI_PROV_INTERNAL_H

/* Internal seams for the wifi_prov.c split (2026-09-04, ROADMAP.md M15 A3:
 * "files over 1500 lines should be broken up where it makes sense" --
 * wifi_prov.c had grown to 2820 lines). This header is NOT public API --
 * wifi_prov.h stays that -- it exists purely so pieces that used to be one
 * translation unit (and could reach each other's `static` state and helpers
 * for free) can still do so now that they are four. Same shape as
 * profile_executor.c's 2026-09-01 split (see profile_executor_internal.h):
 * every symbol declared below was `static` in the original single file and
 * is widened to file-scope-internal linkage ONLY because a sibling .c file
 * in this split now calls or reads it directly.
 *
 *   wifi_prov.c        -- command-queue infra (wifi_prov_post_and_wait/post_event),
 *                          wifi_prov_start() bring-up, owner_task() itself,
 *                          and the handful of trivial state getters used
 *                          from inside wifi_prov_start()'s own log lines
 *   wifi_prov_nvs.c     -- WIFI_NVS_PARTITION / default-partition load,
 *                          save, one-time migration and saved-networks-list
 *                          persistence
 *   wifi_prov_link.c    -- Wi-Fi driver config application, the Wi-Fi/IP
 *                          event handlers, the AP-fallback and rescan
 *                          timers, ground-truth reconciliation, and the
 *                          captive-portal DNS hijack task
 *   wifi_prov_api.c     -- the do_*() bodies and public wifi_prov_*()
 *                          producers for network/mode/AP-identity/IP-mode
 *                          management plus the blocking scan
 *
 * THIS IS A MOVE-ONLY REFACTOR: no logic, ordering, naming or visibility
 * change beyond what moving requires. s_wifi (the module's single piece of
 * shared state, guarded by the "owner_task() is its only writer" rule
 * documented at length in wifi_prov.c) moved here unchanged so all four
 * files see the identical layout; the original anonymous
 * `static struct wifi_prov_state s_wifi;` is now defined (non-static) in
 * wifi_prov.c and `extern`-declared here. Same treatment for s_wifi_cmd_queue,
 * s_scan_stage and TAG. */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "hal_status.h"

#include "wifi_prov.h"

/* ---- shared log tag ------------------------------------------------------
 * Defined (non-static) in wifi_prov.c; every split file logs under the same
 * "wifi_prov" tag the single file used to, unchanged. */
extern const char *WIFI_PROV_TAG;

/* Dedicated NVS partition for Wi-Fi credentials -- see wifi_prov_nvs.c's
 * doc comment (moved there from the top of the original single file) for
 * the full rationale. Needed here too: wifi_prov.c's wifi_prov_start()
 * calls wifi_prov_nvs_partition_init()/wifi_prov_nvs_load_from() on it directly. */
#include "nvs_key_check.h" /* NVS_KEY_LEN_CHECK -- see that header */

#define WIFI_NVS_PARTITION "wifi_nvs"
NVS_KEY_LEN_CHECK(WIFI_NVS_PARTITION);

/* TODO.md 8.4: a bounded list of saved networks. WIFI_PROV_MAX_SAVED_NETWORKS
 * and SAVED_NETS_VERSION are needed here (not just in wifi_prov_nvs.c) because
 * saved_nets_blob_t -- a member of struct wifi_prov_state below -- is sized by
 * the former, and wifi_prov_api.c's do_add_network()/do_get_saved_networks()
 * both read the latter. */
#define WIFI_PROV_MAX_SAVED_NETWORKS 8
#define SAVED_NETS_VERSION 1

typedef struct {
    char ssid[WIFI_PROV_SSID_MAX_LEN + 1];
    char password[WIFI_PROV_PASSWORD_MAX_LEN + 1];
} saved_net_t;

typedef struct {
    uint8_t version; /* SAVED_NETS_VERSION at save time */
    uint8_t count;
    saved_net_t nets[WIFI_PROV_MAX_SAVED_NETWORKS];
} saved_nets_blob_t;

/* ---- module state ---------------------------------------------------------
 * See wifi_prov.c's own top-of-file comment (moved there unchanged) for the
 * full field-by-field rationale -- this is a straight relocation of the
 * original `static struct wifi_prov_state s_wifi` definition, now `extern`
 * here and defined (non-static) in wifi_prov.c. */
struct wifi_prov_state {
    bool started;
    esp_netif_t *ap_netif;
    esp_netif_t *sta_netif;
    esp_timer_handle_t ap_fallback_timer;
    esp_timer_handle_t rescan_timer;

    saved_nets_blob_t saved_nets;
    char active_ssid[WIFI_PROV_SSID_MAX_LEN + 1];
    wifi_prov_mode_t mode;

    char ap_ssid[WIFI_PROV_SSID_MAX_LEN + 1];
    bool has_ap_ssid_override;
    char ap_password[WIFI_PROV_PASSWORD_MAX_LEN + 1];
    bool has_ap_password_override;

    wifi_prov_ip_mode_t ip_mode;
    char static_ip[WIFI_PROV_IPV4_STR_MAX];
    char static_netmask[WIFI_PROV_IPV4_STR_MAX];
    char static_gateway[WIFI_PROV_IPV4_STR_MAX];

    bool static_ip_confirmed;

    char active_password[WIFI_PROV_PASSWORD_MAX_LEN + 1];

    wifi_prov_state_t state;
    int8_t sta_rssi;

    /* 2026-09-28 owner request: home Wi-Fi is back (GOT_IP, or the static-IP
     * reachability confirmation) but the fallback AP teardown was DEFERRED
     * because a user was logged in to the website at that moment -- see
     * ap_teardown_should_defer() in wifi_prov_link.c. do_rescan_tick() (the
     * same 30s timer that already retries a stuck join) re-checks this flag
     * on every tick while it is set and completes the teardown once nobody
     * is logged in, without needing a second timer or task. Same "defer,
     * flag it, recheck on the next natural tick" shape as static_ip_confirmed
     * just above -- deliberately NOT reusing that field, since the two
     * conditions (static-IP reachability vs. no-logged-in-user) are
     * independent and a static join can need BOTH satisfied before the AP
     * actually comes down. */
    bool ap_pending_teardown;

    /* 2026-09-28 review fix: true once do_ap_fallback_tick() has raised (or
     * found already up) the fallback AP after a failed/lost home join, until
     * that AP is actually torn down again (do_ev_got_ip()/
     * do_confirm_static_reachable()/do_rescan_tick()'s deferred teardown),
     * an operator-initiated start_sta_join(), or a switch to AP mode. While
     * set, do_ev_sta_disconnected() neither retries esp_wifi_connect()
     * immediately nor re-arms the fallback timer: with the home network gone
     * every failed connect produces another DISCONNECTED, so the immediate
     * retry kept the STA radio scanning off the AP's channel nearly
     * continuously, and each re-armed timer re-ran do_ap_fallback_tick()'s
     * esp_wifi_set_mode(APSTA)+apply_ap_config() on the already-running AP
     * about once a minute -- both disrupt the very clients the fallback AP
     * exists for. do_rescan_tick()'s 30 s cadence is the retry instead. */
    bool ap_fallback_active;
};

extern struct wifi_prov_state s_wifi;

/* ---- owning task + command queue (2026-08-19, TODO.md 10.14 Phase 4) -----
 * See wifi_prov.c's own top-of-file comment for the full rationale --
 * unchanged by this split, only relocated/declared extern here. */

typedef enum {
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

    CMD_EV_STA_START,
    CMD_EV_STA_DISCONNECTED,
    CMD_EV_GOT_IP,
    CMD_TMR_AP_FALLBACK,
    CMD_TMR_RESCAN,
    CMD_CONFIRM_STATIC_REACHABLE,
} wifi_cmd_type_t;

typedef struct {
    esp_err_t err;
    wifi_prov_saved_network_t saved[WIFI_PROV_MAX_SAVED_NETWORKS];
    size_t saved_count;
    char sta_ip[16];
    char sta_netmask[16]; // filled alongside sta_ip by do_get_sta_ip(), for
                           // wifi_prov_get_sta_ip_netmask() (login backoff
                           // subnet check)
    size_t scan_count;
} wifi_result_t;

/* W1 (docs/HTTP_POST_OWNER_MIGRATION.md): a command used to carry a
 * pointer straight into the PRODUCER's own stack frame (result) plus a
 * stack-resident semaphore handle (done) -- correct only as long as the
 * producer is still waiting when the owner gets around to answering. A
 * timed-out producer returns, and its stack frame is free to be reused by
 * its caller, yet the owner still wrote into *result and gave `done` later:
 * a write into a dead stack frame. Fixed by replacing both with a slot index
 * into a small pool of module-owned (never stack-owned) reply slots
 * (s_reply_slots, wifi_prov.c) plus a generation number captured at claim
 * time. See wifi_prov.c's "reply slot pool" comment for the full protocol:
 * in short, a timed-out producer makes ONE locked decision
 * (abandon_or_free_reply_slot()): if the owner's reply already landed
 * (`replied` true), the producer copies the result out and frees the slot
 * itself (late success); otherwise it marks the slot abandoned and leaves
 * freeing it to the owner, which recycles an abandoned slot instead of
 * answering it. Either way the decision of "who frees this slot" is made
 * under the pool mutex by whichever side reaches it, so there is exactly one
 * writer of "is this slot still alive" at every instant and never a write
 * or a signal aimed at a producer that already gave up. */
typedef struct {
    wifi_cmd_type_t type;
    bool has_reply;
    int slot_idx;
    uint32_t generation;
    union {
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
        struct {
            char ip[WIFI_PROV_IPV4_STR_MAX];
            char netmask[WIFI_PROV_IPV4_STR_MAX];
            char gateway[WIFI_PROV_IPV4_STR_MAX];
        } set_static_ip;
    } args;
} wifi_cmd_t;

extern QueueHandle_t s_wifi_cmd_queue;

/* Scan results staging -- defined (non-static) in wifi_prov_api.c (do_scan()
 * writes it, wifi_prov_scan() copies out of it), but wifi_prov.c's
 * owner_task() also passes it straight to do_scan() for the CMD_SCAN case,
 * so both need to see it. */
#define WIFI_OWNER_SCAN_STAGE_MAX 20
extern wifi_prov_scan_result_t s_scan_stage[WIFI_OWNER_SCAN_STAGE_MAX];

/* ---- queue producer/consumer helpers (wifi_prov.c) ------------------------
 * wifi_prov_post_and_wait() is used by every producer in wifi_prov_api.c;
 * post_event() is used by every event handler/timer callback in
 * wifi_prov_link.c. */
bool wifi_prov_post_and_wait(wifi_cmd_t *cmd, wifi_result_t *result, uint32_t wait_ms);
void post_event(wifi_cmd_type_t type);

/* ---- NVS load/save/migration (wifi_prov_nvs.c) ---------------------------- */
/* HAL Phase 3 item 3 (hal_kv migration): thin wrapper over
 * hal_kv_init_partition(), returns its hal_status_t verbatim rather than
 * collapsing to esp_err_t here -- callers that need an esp_err_t convert at
 * their own boundary via hal_status_to_esp_err(), same convention as
 * relay_cycles.c/display_power_cfg.c/time_sync.c's identical wrappers. */
hal_status_t wifi_prov_nvs_partition_init(const char *partition);
esp_err_t wifi_prov_nvs_load_from(const char *partition, bool *out_found);
void nvs_load_legacy_single(const char *partition, saved_net_t *out_net, bool *out_has);
void wifi_prov_migrate_from_default_partition(bool found_in_wifi_nvs);
void nvs_load_saved_nets(void);
esp_err_t nvs_save_saved_nets(void);

/* ---- non-blocking saved-networks cache (wifi_prov_api.c) ------------------
 * A short-spinlock-guarded mirror of s_wifi.saved_nets, refreshed by
 * owner_task() (do_add_network()/do_forget_network()) and by
 * wifi_prov_start()'s initial nvs_load_saved_nets() -- never by a producer.
 * See wifi_prov.h's wifi_prov_get_saved_networks_cached() doc comment for
 * why this exists: lvgl_port_task must never take the up-to-12s queued path
 * a scan-in-flight can force wifi_prov_get_saved_networks() onto. */
void wifi_prov_update_saved_nets_cache(void);
esp_err_t nvs_save_mode(void);
esp_err_t nvs_save_ap_ssid(void);
esp_err_t nvs_save_ap_password(void);
esp_err_t nvs_save_ip_config(void);

/* Winning legacy single-network credential, set by
 * wifi_prov_migrate_from_default_partition() and consumed by nvs_load_saved_nets() --
 * both in wifi_prov_nvs.c, but wifi_prov.c's wifi_prov_start() also sets it
 * directly on the no-cross-partition-migration path. */
struct wifi_prov_legacy_single {
    bool has;
    saved_net_t net;
};
extern struct wifi_prov_legacy_single s_legacy_single;

/* ---- Wi-Fi driver config / event handlers / timers (wifi_prov_link.c) ---- */
void apply_ap_config(void);
bool parse_ipv4(const char *s, esp_ip4_addr_t *out);
void apply_sta_config(void);
void cancel_ap_fallback_timer(void);
bool reconcile_sta_state(void);
void do_ap_fallback_tick(void);
void ap_fallback_timer_cb(void *arg);
void do_rescan_tick(void);
void rescan_timer_cb(void *arg);
void start_ap_fallback_timer(void);
void start_sta_join(void);
void do_ev_sta_start(void);
void do_ev_sta_disconnected(void);
void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data);
void do_ev_got_ip(void);
void do_confirm_static_reachable(void);
void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data);
void start_dns_hijack_task(void);

/* ---- network/mode/AP/IP-mode command bodies + blocking scan
 * (wifi_prov_api.c) --------------------------------------------------------- */
esp_err_t do_add_network(const char *new_ssid, const char *password, bool *out_join_after_reply);
esp_err_t do_forget_network(const char *target);
esp_err_t do_get_saved_networks(size_t max_results, wifi_result_t *r);
esp_err_t do_set_mode(wifi_prov_mode_t mode, bool *out_join_after_reply);
esp_err_t do_set_ap_ssid(const char *ssid);
esp_err_t do_set_ap_password(const char *password);
esp_err_t do_set_dhcp(void);
esp_err_t do_set_static_ip(const char *ip, const char *netmask, const char *gateway);
esp_err_t do_get_sta_ip(size_t out_cap, wifi_result_t *r);
esp_err_t do_scan(wifi_prov_scan_result_t *results, size_t max_results, size_t *out_count);

/* Defined in wifi_prov_api.c; called from wifi_prov_link.c's do_ev_got_ip()
 * (owner_task()) to refresh the cheap-read cache the same event handler's
 * own comment describes. See wifi_prov.h's wifi_prov_get_cached_sta_ip_
 * netmask() for the public read side. */
void wifi_prov_update_sta_ip_cache(const char *ip, const char *netmask);

#endif /* WIFI_PROV_INTERNAL_H */
