// login_ip_scope.c -- see login_ip_scope.h.
#include "login_ip_scope.h"

#include <stdio.h>

// Parses a dotted-quad IPv4 string into a host-order uint32_t. Returns false
// (leaving *out untouched) on anything that is not exactly four
// 0-255 octets separated by '.', including empty strings and trailing
// garbage -- sscanf's "%3[0-9]" fields plus an explicit length check on the
// consumed count catch both "not four numbers" and "extra characters after
// the fourth octet".
static bool parse_ipv4(const char *s, uint32_t *out)
{
    if (!s || !s[0]) {
        return false;
    }
    unsigned a, b, c, d;
    int consumed = 0;
    if (sscanf(s, "%3u.%3u.%3u.%3u%n", &a, &b, &c, &d, &consumed) != 4) {
        return false;
    }
    if (s[consumed] != '\0') {
        return false; // trailing garbage, e.g. "1.2.3.4.5" or "1.2.3.4x"
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }
    *out = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
    return true;
}

static bool same_subnet(uint32_t ip, uint32_t net, uint32_t mask)
{
    return (ip & mask) == (net & mask);
}

login_ip_scope_t login_ip_scope_classify(const char *ip, const char *sta_ip, const char *sta_netmask)
{
    uint32_t ip_val;
    if (!parse_ipv4(ip, &ip_val)) {
        return LOGIN_IP_SCOPE_UNKNOWN;
    }

    // AP fallback subnet always counts as local, independent of whether the
    // STA interface has a lease at all.
    uint32_t ap_net, ap_mask;
    if (parse_ipv4(LOGIN_IP_SCOPE_AP_SUBNET, &ap_net) && parse_ipv4(LOGIN_IP_SCOPE_AP_NETMASK, &ap_mask) &&
        same_subnet(ip_val, ap_net, ap_mask)) {
        return LOGIN_IP_SCOPE_LOCAL;
    }

    uint32_t sta_ip_val, sta_mask_val;
    if (parse_ipv4(sta_ip, &sta_ip_val) && parse_ipv4(sta_netmask, &sta_mask_val)) {
        if (same_subnet(ip_val, sta_ip_val, sta_mask_val)) {
            return LOGIN_IP_SCOPE_LOCAL;
        }
    }

    return LOGIN_IP_SCOPE_REMOTE;
}
