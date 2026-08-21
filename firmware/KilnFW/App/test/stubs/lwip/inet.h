// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// for wifi_prov.c's host tests. Empty: everything wifi_prov.c actually calls
// from the inet/sockets family (ip4addr_aton, getsockname, inet_ntop) lives
// in lwip/ip4_addr.h and lwip/sockets.h respectively.
#ifndef TEST_STUB_LWIP_INET_H
#define TEST_STUB_LWIP_INET_H

#endif // TEST_STUB_LWIP_INET_H
