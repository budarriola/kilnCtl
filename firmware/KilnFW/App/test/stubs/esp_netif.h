// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// for wifi_prov.c's host tests. Real network state is unreachable on the
// host build; these are no-ops just sufficient for wifi_prov.c to compile
// and link. The tests never exercise a code path that depends on
// esp_netif_get_ip_info()/esp_netif_set_ip_info() actually tracking state.
#ifndef TEST_STUB_ESP_NETIF_H
#define TEST_STUB_ESP_NETIF_H

#include <stdint.h>

#include "esp_err.h"

typedef struct esp_netif_obj *esp_netif_t;

typedef struct {
    uint32_t addr;
} esp_ip4_addr_t;

typedef struct {
    esp_ip4_addr_t ip;
    esp_ip4_addr_t netmask;
    esp_ip4_addr_t gw;
} esp_netif_ip_info_t;

/* 2026-10-03: DNS surface for wifi_prov_link.c's static-IP DNS apply. The stub
 * records the last MAIN/BACKUP address and call count so a test can check what
 * apply_sta_config() pushed. Same layout as ESP-IDF's esp_netif_dns_info_t
 * (a tagged union; only the v4 member is used here). */
typedef enum { ESP_NETIF_DNS_MAIN = 0, ESP_NETIF_DNS_BACKUP, ESP_NETIF_DNS_FALLBACK, ESP_NETIF_DNS_MAX } esp_netif_dns_type_t;
#define ESP_IPADDR_TYPE_V4 0
typedef struct {
    union {
        esp_ip4_addr_t ip4;
        uint32_t ip6_pad[4];
    } u_addr;
    uint8_t type;
} esp_ip_addr_t;
typedef struct {
    esp_ip_addr_t ip;
} esp_netif_dns_info_t;

extern uint32_t g_stub_dns_main;
extern uint32_t g_stub_dns_backup;
extern int g_stub_dns_set_calls;

/* IP_EVENT (the event base) is declared in esp_event.h, which wifi_prov.c
 * always includes first -- only the event ID constant belongs here. */
#define IP_EVENT_STA_GOT_IP 0

static inline esp_err_t esp_netif_init(void) { return ESP_OK; }

static inline esp_netif_t *esp_netif_create_default_wifi_ap(void)
{
    static int dummy;
    return (esp_netif_t *)&dummy;
}

static inline esp_netif_t *esp_netif_create_default_wifi_sta(void)
{
    static int dummy;
    return (esp_netif_t *)&dummy;
}

static inline esp_err_t esp_netif_dhcpc_start(esp_netif_t *netif)
{
    (void)netif;
    return ESP_OK;
}

static inline esp_err_t esp_netif_dhcpc_stop(esp_netif_t *netif)
{
    (void)netif;
    return ESP_OK;
}

static inline esp_err_t esp_netif_set_hostname(esp_netif_t *netif, const char *hostname)
{
    (void)netif;
    (void)hostname;
    return ESP_OK;
}

static inline esp_err_t esp_netif_set_ip_info(esp_netif_t *netif, const esp_netif_ip_info_t *info)
{
    (void)netif;
    (void)info;
    return ESP_OK;
}

static inline esp_err_t esp_netif_set_dns_info(esp_netif_t *netif, esp_netif_dns_type_t type,
                                               esp_netif_dns_info_t *dns)
{
    (void)netif;
    /* Real ESP-IDF (esp_netif_lwip.c): ESP_IP_IS_ANY(addr) -> ESP_ERR_ESP_NETIF_INVALID_PARAMS,
     * so 0.0.0.0 can never be used to clear a slot. Records nothing, like IDF. */
    if (dns == NULL || dns->ip.u_addr.ip4.addr == 0) {
        return ESP_ERR_ESP_NETIF_INVALID_PARAMS;
    }
    g_stub_dns_set_calls++;
    if (type == ESP_NETIF_DNS_MAIN) {
        g_stub_dns_main = dns->ip.u_addr.ip4.addr;
    } else if (type == ESP_NETIF_DNS_BACKUP) {
        g_stub_dns_backup = dns->ip.u_addr.ip4.addr;
    }
    return ESP_OK;
}

/* Runs `fn` synchronously (the real one hops to the tcpip thread). */
typedef esp_err_t (*esp_netif_callback_fn)(void *ctx);
static inline esp_err_t esp_netif_tcpip_exec(esp_netif_callback_fn fn, void *ctx)
{
    return fn(ctx);
}

/* 2026-09-28: controllable by a test (default 0, i.e. every pre-existing
 * caller's "no lease" behavior is unchanged) so wifi_prov_link.c's
 * sta_link_is_live()/reconcile_sta_state() can be exercised on a station
 * that genuinely holds a live IP -- see that comment's "associated but no
 * lease" check, which no test could previously satisfy since this stub
 * always reported addr 0. Declared here (not just defined in a .c) so any
 * host-test file including this stub may set it. */
extern uint32_t g_stub_netif_ip_addr;

static inline esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info)
{
    (void)netif;
    if (info) {
        info->ip.addr = g_stub_netif_ip_addr;
        info->netmask.addr = 0;
        info->gw.addr = 0;
    }
    return ESP_OK;
}

static inline char *esp_ip4addr_ntoa(const esp_ip4_addr_t *addr, char *buf, uint32_t buflen)
{
    (void)addr;
    if (buf && buflen > 0) {
        buf[0] = '\0';
    }
    return buf;
}

#endif // TEST_STUB_ESP_NETIF_H
