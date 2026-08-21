// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// for wifi_prov.c's host tests. ip4addr_aton() is a REAL implementation, not
// a no-op: parse_ipv4() in wifi_prov.c (and, downstream, wifi_prov_set_static_ip()'s
// validation) is exactly what the "wrong-but-parseable static IP" regression
// depends on, so this has to actually reject non-dotted-quad input the same
// way the real lwIP one does.
#ifndef TEST_STUB_LWIP_IP4_ADDR_H
#define TEST_STUB_LWIP_IP4_ADDR_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint32_t addr;
} ip4_addr_t;

static inline int ip4addr_aton(const char *cp, ip4_addr_t *addr)
{
    if (!cp || !*cp) {
        return 0;
    }
    unsigned a, b, c, d;
    char extra;
    int n = sscanf(cp, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra);
    if (n != 4) {
        return 0;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) {
        return 0;
    }
    if (addr) {
        addr->addr = a | (b << 8) | (c << 16) | (d << 24);
    }
    return 1;
}

#endif // TEST_STUB_LWIP_IP4_ADDR_H
