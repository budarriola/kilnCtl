// Host tests for App/drivers/common/httpd_socket_budget.h -- see that header's
// comment for the full 2026-09-01 incident this guards against: a permanent
// socket held by wifi_prov.c's dns_hijack_task() was never subtracted from
// wifi_provision_http.c's CONFIG_LWIP_MAX_SOCKETS budget, which silently
// disabled httpd's lru_purge_enable recovery path and produced a ~20-minute
// socket-exhaustion wedge under sustained dashboard polling during a live
// firing. httpd_socket_budget_has_headroom() is a pure, header-only inline
// function -- no ESP-IDF, no I/O -- so it is included directly here, same as
// every other pure-decision module this project host-tests.
#include "test_common.h"

#include "../drivers/common/httpd_socket_budget.h"

// The actual, current, in-tree configuration: CONFIG_LWIP_MAX_SOCKETS=18
// (sdkconfig.defaults), max_open_sockets=13 (wifi_provision_http.c),
// dns_hijack_task's 1 permanent socket (wifi_prov.c). Mirrors the
// _Static_assert wifi_provision_http.c compiles against -- this proves the
// SAME numbers pass the pure function the assert calls, on host, without
// needing an ESP-IDF toolchain.
static void test_current_config_has_headroom(void)
{
    bool ok = httpd_socket_budget_has_headroom(18, 13, 1);
    TEST_CHECK(ok, "18 sockets covers 13 sessions + 3 httpd-internal + 1 dns_hijack, with 1 spare");
}

// This is the exact regression that produced the live wedge: the
// pre-2026-09-01 budget (16 total, still assuming ONLY httpd's 3 internal +
// 13 sessions, i.e. dns_hijack's socket not counted at all) exactly filled
// the table with room for httpd's own bookkeeping to reach its cap. But
// once dns_hijack's real, always-open socket is included in what actually
// competes for the table, 16 no longer covers 13 + 3 + 1 = 17. This is the
// negative case: the same real dns_hijack_task consumption that shipped
// silently for weeks, checked against the OLD total, must read as NOT
// having headroom.
static void test_old_16_socket_budget_is_short_by_one(void)
{
    bool ok = httpd_socket_budget_has_headroom(16, 13, 1);
    TEST_CHECK(!ok, "16 sockets is one short of 13 + 3 + 1 -- must NOT report headroom");
}

// Exact equality (no spare beyond the last dns_hijack-equivalent socket) is
// still accepted: httpd's own session count reaches max_open_sockets at
// precisely the moment the OS table fills, which is enough for
// lru_purge_enable to engage (see the header's comment for why this is the
// boundary, not strictly-greater-than).
static void test_exact_fit_still_has_headroom(void)
{
    bool ok = httpd_socket_budget_has_headroom(17, 13, 1);
    TEST_CHECK(ok, "17 = 13 + 3 + 1 exactly: httpd's own cap is reached in step with the OS table, not after");
}

// A SECOND always-on responder added later, on top of dns_hijack, without
// anyone widening CONFIG_LWIP_MAX_SOCKETS to match -- proves this isn't
// hardcoded to "exactly one extra consumer" internally; the same class of
// bug reproduces at a different count and the function still catches it.
static void test_a_second_uncounted_permanent_socket_also_fails(void)
{
    bool ok = httpd_socket_budget_has_headroom(18, 13, 3);
    TEST_CHECK(!ok, "18 sockets does not cover 13 + 3 + 3 = 19 -- must NOT report headroom");
}

void run_test_httpd_socket_budget(void)
{
    TEST_SECTION("httpd_socket_budget_has_headroom");
    test_current_config_has_headroom();
    test_old_16_socket_budget_is_short_by_one();
    test_exact_fit_still_has_headroom();
    test_a_second_uncounted_permanent_socket_also_fails();
}
