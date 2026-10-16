// recovery_keepalive.h -- keeps the Pico's S6b link-dead timer fed while the
// recovery image runs, and drives the isolated fault line (GPIO6) to its idle
// level. Audit AUDIT_LINK_UPDATE_LIFECYCLE MED-1 / LOW-1. Pure decisions live
// in recovery_pico_proto.h (rpp_keepalive_*, RPP_FAULT_*), host-tested.
//
// Owns UART1 (TX GPIO5 / RX GPIO4) for the whole image lifetime; the Pico
// update relay (recovery_pico.c) borrows it through acquire/release, during
// which the relay's own frames keep the link alive and the keepalive stands
// down.
#ifndef RECOVERY_KEEPALIVE_H
#define RECOVERY_KEEPALIVE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Drives GPIO6 low (fault de-asserted, as the application latches it first),
// installs UART1 and starts the keepalive task. Never blocks boot; failures
// are logged (the Pico then trips S6b as before, which is the safe outcome).
void recovery_keepalive_start(void);

// Borrow UART1 for a Pico transfer. False if the UART cannot be set up.
bool recovery_keepalive_acquire(void);
// Give it back (waits for TX to drain; the driver and pin stay configured).
void recovery_keepalive_release(void);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_KEEPALIVE_H
