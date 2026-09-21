// login_ip_scope.c -- see login_ip_scope.h.
#include "login_ip_scope.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

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

// Review fix (2026-09-21, finding 2): a PF_INET6 httpd listener (
// CONFIG_LWIP_IPV6=y) hands getpeername() strings back as IPv4-mapped IPv6,
// not plain dotted-quad -- confirmed on-board form is
// "::FFFF:192.168.1.87" (case-insensitive "::ffff:" prefix, dotted-quad
// tail). If `s` starts with that prefix, writes the remainder (the tail
// after the prefix) into `out` (size `out_cap`) and returns true; otherwise
// returns false and leaves `out` untouched. This only strips the prefix --
// the tail is still whatever form it was (dotted-quad or hex-group), parsed
// by the two paths in try_parse_mapped_or_plain() below.
static bool strip_ipv4_mapped_prefix(const char *s, char *out, size_t out_cap)
{
    static const char prefix[] = "::ffff:";
    size_t prefix_len = sizeof(prefix) - 1;
    if (!s) {
        return false;
    }
    size_t i;
    for (i = 0; i < prefix_len; i++) {
        if (!s[i] || tolower((unsigned char)s[i]) != prefix[i]) {
            return false;
        }
    }
    size_t tail_len = strlen(s + prefix_len);
    if (tail_len + 1 > out_cap) {
        return false;
    }
    memcpy(out, s + prefix_len, tail_len + 1);
    return true;
}

// Best-effort: parses lwIP's hex-group spelling of the same mapped address,
// e.g. "C0A8:0157" (two colon-separated 16-bit hex groups encoding the same
// 4 bytes as "192.168.1.87"). Not the form actually observed on this board
// (see login_ip_scope.h) -- handled defensively only.
static bool parse_ipv4_hex_groups(const char *s, uint32_t *out)
{
    unsigned hi, lo;
    int consumed = 0;
    if (sscanf(s, "%4x:%4x%n", &hi, &lo, &consumed) != 2) {
        return false;
    }
    if (s[consumed] != '\0' || hi > 0xFFFFu || lo > 0xFFFFu) {
        return false;
    }
    *out = ((uint32_t)hi << 16) | (uint32_t)lo;
    return true;
}

// Tries, in order: plain dotted-quad; an IPv4-mapped-IPv6 dotted-quad tail;
// an IPv4-mapped-IPv6 hex-group tail (best-effort). Returns false if none
// match.
static bool try_parse_mapped_or_plain(const char *s, uint32_t *out)
{
    if (parse_ipv4(s, out)) {
        return true;
    }
    char tail[64];
    if (strip_ipv4_mapped_prefix(s, tail, sizeof(tail))) {
        if (parse_ipv4(tail, out)) {
            return true;
        }
        if (parse_ipv4_hex_groups(tail, out)) {
            return true;
        }
    }
    return false;
}

login_ip_scope_t login_ip_scope_classify(const char *ip, const char *sta_ip, const char *sta_netmask)
{
    uint32_t ip_val;
    if (!try_parse_mapped_or_plain(ip, &ip_val)) {
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
