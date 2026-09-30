// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// for wifi_prov.c's host tests.
//
// getsockname()/inet_ntop() are the two functions the centrepiece test
// actually depends on: wifi_prov_note_possible_static_reachability() calls
// them to learn which local address an incoming HTTP request landed on, then
// compares that string against s_wifi.static_ip. The stubs below are
// deliberately TEST-CONTROLLABLE rather than no-ops -- g_stub_getsockname_result
// and g_stub_local_ip let test_wifi_prov.c simulate "this request arrived on
// the static IP" vs. "this request arrived on some other address" (the AP's
// own IP, most realistically) without a real socket anywhere. Everything
// else here (socket/bind/recvfrom/sendto/close, htonl/htons/INADDR_ANY) only
// exists so dns_hijack_task() -- never invoked by the tests, since they never
// call wifi_prov_start() -- compiles and links.
#ifndef TEST_STUB_LWIP_SOCKETS_H
#define TEST_STUB_LWIP_SOCKETS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint32_t socklen_t;

struct in_addr {
    uint32_t s_addr;
};

struct sockaddr {
    uint16_t sa_family;
    char sa_data[14];
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};

#define AF_INET 2
#define SOCK_DGRAM 2
#define IPPROTO_UDP 17
#define INADDR_ANY 0u

static inline uint32_t htonl(uint32_t hostlong) { return hostlong; }
static inline uint16_t htons(uint16_t hostshort) { return hostshort; }

static inline int socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;
    return -1; /* dns_hijack_task() treats this as "captive portal disabled" and returns */
}

static inline int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    (void)fd;
    (void)addr;
    (void)len;
    return -1;
}

static inline int recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen)
{
    (void)fd;
    (void)buf;
    (void)len;
    (void)flags;
    (void)from;
    (void)fromlen;
    return -1;
}

static inline int sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen)
{
    (void)fd;
    (void)buf;
    (void)len;
    (void)flags;
    (void)to;
    (void)tolen;
    return -1;
}

static inline int close(int fd)
{
    (void)fd;
    return 0;
}

/* ---- Test-controllable half -------------------------------------------- */

/* 0 = succeed, non-zero = fail (getsockname() itself returning an error,
 * which wifi_prov_note_possible_static_reachability() treats as "nothing to
 * confirm" and returns early on). Defaults set in test_wifi_prov.c before
 * each scenario. */
extern int g_stub_getsockname_result;
/* The dotted-quad string inet_ntop() hands back -- this is the "local
 * address the incoming request arrived on" the real code reads via
 * getsockname()+inet_ntop() in sequence. The test sets this directly; the
 * stub getsockname() below never actually touches the address it's asked to
 * fill in, since only the string inet_ntop() returns is ever compared. */
// 2026-09-29: widened 16 -> 48 (>= INET6_ADDRSTRLEN) so an IPv4-mapped
// AF_INET6 string ("::ffff:192.168.4.1", 19 chars + NUL) fits -- see
// test_wifi_prov.c's definition-site comment.
extern char g_stub_local_ip[48];

/* 2026-09-29, added for wifi_prov_request_arrived_on_ap()'s
 * sockaddr_is_ap_default_ip(): that function branches on the address family
 * getsockname() reports (plain AF_INET vs. an IPv4-mapped AF_INET6, the real
 * shape on this board's CONFIG_LWIP_IPV6=y build -- see that function's own
 * comment) before calling inet_ntop(), so the family word has to be
 * test-controllable too, not just the resulting string. sin_family/
 * sin6_family are both a uint16_t at offset 0 of their respective structs,
 * so writing through a uint16_t* below is safe for either caller --
 * wifi_prov_note_possible_static_reachability() also calls getsockname()
 * with a plain sockaddr_in buffer and never reads the family word back, so
 * this write is inert for it. Defaults to AF_INET6, the shape actually
 * observed on hardware; a test wanting the plain-AF_INET branch sets this to
 * AF_INET first. */
extern int g_stub_getsockname_family;

static inline int getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    (void)fd;
    (void)addrlen;
    if (g_stub_getsockname_result == 0 && addr) {
        *(uint16_t *)(void *)addr = (uint16_t)g_stub_getsockname_family;
    }
    return g_stub_getsockname_result;
}

static inline const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    (void)af;
    (void)src;
    if (dst && size > 0) {
        strncpy(dst, g_stub_local_ip, size - 1);
        dst[size - 1] = '\0';
    }
    return dst;
}

/* ---- Added 2026-08-27 for ota_http.c's host tests (test_ota_http.c),
 * which links ota_http.c for real. get_client_ip()/ota_esp_do_transfer()
 * need sockaddr_in6/AF_INET6/getpeername()/setsockopt() to compile; a fixed
 * "no real socket here" failure (getpeername() returning -1, same as
 * socket() above) is enough -- get_client_ip() already treats that as
 * "unknown" and every test in this file supplies its own client_ip string
 * directly rather than depending on socket introspection. */
struct sockaddr_in6 {
    uint16_t sin6_family;
    uint16_t sin6_port;
    uint32_t sin6_flowinfo;
    uint8_t sin6_addr[16];
    uint32_t sin6_scope_id;
};

#define AF_INET6 10
#define SOL_SOCKET 1
#define SO_RCVTIMEO 20

struct timeval {
    long tv_sec;
    long tv_usec;
};

static inline int getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    (void)fd;
    (void)addr;
    (void)addrlen;
    return -1;
}

static inline int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen)
{
    (void)fd;
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    return 0;
}

#endif // TEST_STUB_LWIP_SOCKETS_H
