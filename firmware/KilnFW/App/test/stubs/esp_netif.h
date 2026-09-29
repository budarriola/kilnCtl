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

static inline esp_err_t esp_netif_set_ip_info(esp_netif_t *netif, const esp_netif_ip_info_t *info)
{
    (void)netif;
    (void)info;
    return ESP_OK;
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
