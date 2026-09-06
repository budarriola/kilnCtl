/* Pure, host-testable accounting for the shared lwIP socket table
 * (CONFIG_LWIP_MAX_SOCKETS) that wifi_provision_http.c's single httpd
 * instance shares with every other permanent socket this firmware opens.
 *
 * 2026-09-01: this budget went silently one socket short of reality for
 * weeks -- wifi_prov.c's dns_hijack_task() has held a permanent UDP socket
 * since 2026-08-19, one day before wifi_provision_http.c's own "13 sessions
 * + 3 httpd-internal = 16" sizing comment was written, and that sizing pass
 * never counted it. The effect wasn't a refused connection; it was worse: it
 * silently disabled httpd's lru_purge_enable safety net. httpd only closes
 * its least-recently-used session when ITS OWN bookkeeping
 * (hd_sd_active_count in ESP-IDF's httpd_sess.c) reaches max_open_sockets.
 * With one socket permanently stolen elsewhere, that count could never
 * reach its configured cap before the OS-level lwIP socket table
 * (sockets[NUM_SOCKETS], a fixed-size array -- see lwip's sockets.c
 * alloc_socket()) filled up first, so accept() kept failing with ENFILE and
 * the purge path never engaged. Sustained dashboard polling during a live
 * firing reproduced a ~20-minute-long outage this way; see
 * sdkconfig.defaults' CONFIG_LWIP_MAX_SOCKETS comment and
 * wifi_provision_http.c's config.max_open_sockets comment for the full
 * writeup.
 *
 * This header exists so that invariant -- "the socket table is big enough
 * for httpd's own pool PLUS every other permanent consumer, with room to
 * spare so httpd's own accounting is always the tighter constraint" -- is a
 * pure function with a name, checked at compile time against the real
 * config (wifi_provision_http.c) and exercised with a negative case on host
 * (test_httpd_socket_budget.c), rather than only a comment a future change
 * can silently invalidate again. */
#ifndef HTTPD_SOCKET_BUDGET_H
#define HTTPD_SOCKET_BUDGET_H

#include <stdbool.h>

/* ESP-IDF's httpd_start() (components/esp_http_server/src/httpd_main.c)
 * permanently reserves exactly 3 sockets for its own use for the life of
 * the server: the listening socket, plus a send/receive pair for its
 * internal async-work control channel. Fixed by that library, not a knob
 * this project can retune. */
#define HTTPD_INTERNAL_RESERVED_SOCKETS 3

/* True when `lwip_max_sockets` (CONFIG_LWIP_MAX_SOCKETS) has at least one
 * socket of headroom above every known permanent/pooled consumer combined:
 * httpd's own 3 internal sockets, its `max_open_sockets`-sized session
 * pool, and `other_permanent_sockets` (every socket some OTHER part of this
 * firmware holds open for the app's entire lifetime, outside httpd's own
 * accounting -- currently just wifi_prov.c's dns_hijack_task(), so 1).
 *
 * The margin matters, not just non-negative slack: if the sum exactly EQUALS
 * lwip_max_sockets, httpd's own session count reaches max_open_sockets at
 * precisely the same moment the OS socket table fills, which is the
 * boundary case this function still accepts (httpd's accounting and the OS
 * table hit the wall together, so lru_purge_enable still engages -- it does
 * not need a session to already exist that it can close, it needs its OWN
 * count to reach its own cap, which it does). What must NOT happen is the
 * external consumers pushing the OS table's real ceiling BELOW
 * max_open_sockets, which is what silently happened here: strictly less
 * than the sum is the only failing case. */
/* The single source of truth for the rule, as a macro: usable directly as a
 * _Static_assert operand (a function call, even a `static inline` one over
 * compile-time-constant arguments, is NOT an integer constant expression in
 * C11/C17 -- _Static_assert requires one) as well as from ordinary runtime
 * code. httpd_socket_budget_has_headroom() below is a thin wrapper over
 * this SAME macro so the compile-time check and the host-testable function
 * cannot drift apart into two different rules. */
#define HTTPD_SOCKET_BUDGET_HAS_HEADROOM(lwip_max_sockets, max_open_sockets, other_permanent_sockets) \
    ((lwip_max_sockets) >= (max_open_sockets) + HTTPD_INTERNAL_RESERVED_SOCKETS + (other_permanent_sockets))

static inline bool httpd_socket_budget_has_headroom(int lwip_max_sockets, int max_open_sockets,
                                                      int other_permanent_sockets)
{
    return HTTPD_SOCKET_BUDGET_HAS_HEADROOM(lwip_max_sockets, max_open_sockets, other_permanent_sockets);
}

#endif /* HTTPD_SOCKET_BUDGET_H */
