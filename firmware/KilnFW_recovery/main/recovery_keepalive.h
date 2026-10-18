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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Drives GPIO6 low (fault de-asserted, as the application latches it first),
// installs UART1 and starts the keepalive task. Never blocks boot; failures
// are logged (the Pico then trips S6b as before, which is the safe outcome).
void recovery_keepalive_start(void);

// RE-02: observable proof the keepalive is transmitting (the Pico never replies
// to it, so this is the only on-target evidence). Surfaced in /api/recovery/status.
typedef struct {
    uint32_t sent;         // frames fully accepted by uart_write_bytes()
    uint32_t send_errors;  // writes that returned an error or a short count
    int32_t last_err;      // 0 = last write ok; -1 = driver error; -2 = short write; -3 = frame build failed
    uint32_t last_send_ms; // uptime (ms) of the last attempted send; 0 = none yet
    bool task_running;     // the keepalive task exists (false after its window ended or on setup failure)
} recovery_keepalive_stats_t;
void recovery_keepalive_get_stats(recovery_keepalive_stats_t *out);

// Borrow UART1 for a Pico transfer. False if the UART cannot be set up.
bool recovery_keepalive_acquire(void);
// Give it back (waits for TX to drain; the driver and pin stay configured).
void recovery_keepalive_release(void);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_KEEPALIVE_H
