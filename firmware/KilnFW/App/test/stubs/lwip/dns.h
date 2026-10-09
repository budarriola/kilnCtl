// Host-test stub for wifi_prov_link.c's lwIP backup-resolver clear.
// esp_netif_set_dns_info() refuses 0.0.0.0, so the only way to empty the BACKUP
// slot is lwIP's own dns_setserver(); slot 1 == ESP_NETIF_DNS_BACKUP.
#ifndef TEST_STUB_LWIP_DNS_H
#define TEST_STUB_LWIP_DNS_H

#include <stdint.h>

typedef struct {
    uint32_t addr;
} ip_addr_t;

#define ip_addr_set_zero(a) ((a)->addr = 0)

extern uint32_t g_stub_dns_backup;
extern int g_stub_lwip_backup_clears;

static inline void dns_setserver(uint8_t numdns, const ip_addr_t *dnsserver)
{
    if (numdns == 1 && dnsserver != NULL) {
        g_stub_dns_backup = dnsserver->addr;
        if (dnsserver->addr == 0) {
            g_stub_lwip_backup_clears++;
        }
    }
}

#endif // TEST_STUB_LWIP_DNS_H
