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
extern char g_stub_local_ip[16];

static inline int getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    (void)fd;
    (void)addr;
    (void)addrlen;
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

#endif // TEST_STUB_LWIP_SOCKETS_H
