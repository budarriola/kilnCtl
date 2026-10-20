// telemetry_time_sync -- FD-04 follow-ups (FQ-01/02/03, BH-02): the per-boot SYS_BOOT marker, the
// TIME_SYNC event write and the boot_history wall-time backfill, split out of telemetry_log.c so a
// host test can drive them without FreeRTOS. Called once per second from the telemetry_log task.
#ifndef KILNCTL_TELEMETRY_TIME_SYNC_H
#define KILNCTL_TELEMETRY_TIME_SYNC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TIME_SYNC goes to the autotune store only on a boot's first sync or when the implied
 * wall-clock offset (utc - uptime) moved by more than this many seconds (FQ-03). */
#define TELEMETRY_TIME_SYNC_STEP_S 60

/* One pass. uptime_s = seconds since boot. Emits SYS_BOOT first (retried until it succeeds), then
 * logs a pending TIME_SYNC (firing store always; autotune store first-per-boot or on a step) and
 * acks it only after the firing-store write succeeded (FQ-02). Backfills boot_history wall_time on
 * the first successful sync (BH-02, at most once per boot). */
void telemetry_time_sync_service(uint32_t uptime_s);

#ifdef __cplusplus
}
#endif

#endif
