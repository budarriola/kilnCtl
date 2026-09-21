// login_ip_scope.h -- pure IPv4 subnet classification for the login backoff
// (docs/WEB_AUTH_PLAN.md section 2). Owner requirement, 2026-09-20 (verbal):
// "All remote ip addresses ie. not on the same subnet should be treated as
// the same ip as far as login timeouts go" -- reasoning being that a single
// remote attacker can rotate through addresses to defeat a per-IP ladder,
// while a genuine LAN user cannot spoof being on the LAN. web_auth_login_http.c
// uses this to decide whether a client's failures go into its own per-IP
// backoff slot (LOCAL) or the one shared "remote" slot every off-subnet
// client contends for (REMOTE).
//
// No ESP-IDF dependency -- host-testable, same discipline as ota_auth.h.
#ifndef KILNCTL_LOGIN_IP_SCOPE_H
#define KILNCTL_LOGIN_IP_SCOPE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOGIN_IP_SCOPE_LOCAL = 0,  // same subnet as the STA interface, or the AP fallback subnet
    LOGIN_IP_SCOPE_REMOTE,     // parses as a valid IPv4 address but is on neither subnet
    LOGIN_IP_SCOPE_UNKNOWN,    // `ip` did not parse as a dotted-quad IPv4 address at all
} login_ip_scope_t;

// The board's own AP-fallback subnet (CLAUDE.md's "AP-fallback 192.168.4.1"),
// fixed regardless of STA connectivity -- a client associated to the AP is
// always LOCAL even when the STA interface has no lease at all.
#define LOGIN_IP_SCOPE_AP_SUBNET "192.168.4.0"
#define LOGIN_IP_SCOPE_AP_NETMASK "255.255.255.0"

// Classifies `ip` (a dotted-quad string, e.g. from ota_http_get_client_ip())
// against the STA interface's CURRENT ip/netmask (also dotted-quad strings;
// pass "" for either when the STA interface has no lease -- the AP subnet
// check still applies) and the fixed AP subnet above.
//
// `ip` that fails to parse returns LOGIN_IP_SCOPE_UNKNOWN unconditionally --
// callers must not route the "unknown" client-address sentinel through this
// classifier at all (it has its own separate fail-closed handling; see
// web_auth_login_http.c), but this is also a safe fallback if they do, since
// UNKNOWN is never treated as LOCAL.
login_ip_scope_t login_ip_scope_classify(const char *ip, const char *sta_ip, const char *sta_netmask);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_LOGIN_IP_SCOPE_H
