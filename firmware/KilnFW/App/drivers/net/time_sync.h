#pragma once

/* SNTP wall-clock time for KilnFW -- run_state.c's "no wall clock on this
 * board" note (line ~320, TODO.md) blocked the scheduled-start/candling
 * feature (docs/PROFILES.md, "Scheduled start + candling"). This module
 * closes that gap: it gets a POSIX epoch and a configured local timezone
 * onto the board, nothing else.
 *
 * HARD BOUNDARY, read before touching anything that measures a duration:
 * wall time from this module is DISPLAY-AND-SCHEDULING-INTENT ONLY. An SNTP
 * step can move time(NULL)/gettimeofday() backward (or forward) by an
 * arbitrary amount the instant a sync lands -- that is what SNTP steps DO,
 * this module does not (and, per the task that added it, must not) run in
 * "slew" mode. Every duration measurement in this firmware already uses
 * esp_timer_get_time() (monotonic, µs since boot, immune to wall-clock
 * steps) -- profile_executor.c, thermal_guard.c, the autotune engine, every
 * watchdog. That must stay true. This module adds a NEW use of wall time
 * (schedule-a-start-for-6am, stamp a display timestamp); it does not
 * convert anything that currently measures elapsed time.
 *
 * time_sync_notify_got_ip() never WAITS FOR A SYNC: it does not call
 * esp_netif_sntp_sync_wait() or any other wait for an actual NTP reply, and
 * the sync landing (or not) is reported later, asynchronously, via the sync
 * callback. It is NOT non-blocking, though: esp_netif_sntp_start() is
 * esp_netif_tcpip_exec(...) under the hood -- a bounded, synchronous IPC
 * call that posts to lwIP's tcpip task and blocks on a semaphore until that
 * task services it (typically sub-millisecond, but it is a real wait, not a
 * fire-and-forget post). It runs on wifi_prov.c's owner_task() same as every
 * other do_ev_got_ip() step, and that bounded wait is judged acceptable
 * there -- but "never blocks" is the wrong way to describe it. (Corrected
 * 2026-08-30 per code review; wifi_prov.c:1459 carries the same wrong claim
 * and needs the same fix from whoever owns that file.) */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#include "time_sync_tz.h"

/* cfg_fs relative path of this item's dual-write mirror (NVS side lives in kiln_nvs). */
#define TIME_SYNC_TZ_FILE_PATH "tz.dat"

typedef struct {
    /* True once ANY SNTP sync has landed since this boot. Never persisted
     * -- "synced" is a per-boot fact, not a stored one; a fresh boot always
     * starts false even if the RTC/epoch below happens to still hold a
     * plausible value from before. */
    bool ever_synced;
    /* Epoch seconds of the last successful sync, 0 if ever_synced is
     * false. Not "now" -- a caller wanting the current time reads
     * time(NULL) directly; this field answers "how stale is the clock",
     * which time(NULL) alone cannot. */
    time_t last_sync_epoch;
    /* Current epoch seconds (time(NULL) at the moment of the call), 0 if
     * ever_synced is false -- an un-synced board's time(NULL) reads
     * whatever the RTC free-ran to since power-on (near the 1970 epoch,
     * always misleading), so 0 is reported instead of that garbage. */
    time_t now_epoch;
    /* Configured POSIX TZ string, always a valid, NUL-terminated,
     * printable-ASCII string -- see time_sync_tz_effective(). Never empty,
     * never garbage: defaults to TIME_SYNC_TZ_DEFAULT ("UTC0"). */
    char tz[TIME_SYNC_TZ_MAX_LEN + 1];
} time_sync_status_t;

/* Call once at boot, before wifi_prov_start() need not be a hard ordering
 * requirement but IS the convention every other *_start() in main.c
 * follows -- loads the persisted TZ (or defaults to UTC, see
 * time_sync_tz_effective()), applies it via setenv("TZ",...)/tzset() so
 * every localtime() call in the firmware is correct from the first LCD
 * frame, and prepares (but does not start) the SNTP client. Non-fatal on
 * any failure: same "board still boots, feature just doesn't work this
 * boot" convention as unit_pref_start()/touch_cal_store_load(). */
esp_err_t time_sync_start(void);

/* Called from wifi_prov.c's do_ev_got_ip(), once STA has an IP (whether or
 * not a static-IP join is yet CONFIRMED reachable -- SNTP only needs UDP/53
 * DNS + UDP/123 out, not an inbound HTTP request the way static_ip_
 * confirmed is checked for). Arms/re-arms the SNTP client via
 * esp_netif_sntp_start() -- see this header's opening comment for exactly
 * what that call does and does not block on. It only kicks the lwIP SNTP
 * module's own state machine -- actual sync landing (or not) is reported
 * later, asynchronously, via the sync callback into this module, never by
 * this call waiting for it. Safe to call repeatedly (every reconnect) --
 * esp_netif_sntp_start() on an already-running client is a documented
 * no-op-with-error-code, logged and ignored here. A no-op if time_sync_
 * start() never got the SNTP client successfully initialized: starting a
 * client with no configured server would just be sntp_stop();sntp_init()
 * on nothing, so this returns immediately instead. */
void time_sync_notify_got_ip(void);

/* Fills *out with the current status snapshot. Cheap, RAM-only, safe to
 * call from any task (dashboard_http.c's status_get_handler() calls this on
 * the httpd worker task, a different task than the one time_sync_notify_
 * got_ip()/the SNTP sync callback run on -- guarded internally). */
void time_sync_get_status(time_sync_status_t *out);

/* Validates (time_sync_tz_is_valid()), persists to NVS, and immediately
 * applies (setenv+tzset) a new POSIX TZ string. Returns ESP_ERR_INVALID_ARG
 * and changes NOTHING -- neither the live TZ nor the stored one -- if tz
 * fails validation ("refuse, never clamp": settings_http.c's caller is
 * expected to have already checked at the HTTP-body level, but this is the
 * real gate; never trust a caller's check alone). A persist failure (NVS
 * write error) still applies the TZ live for this boot and is logged, not
 * fatal -- same "in-RAM truth first" convention unit_pref_set() documents. */
esp_err_t time_sync_set_tz(const char *tz);

// Read-only dual-write status for GET /api/cfgfs -- see unit_pref.h's
// unit_pref_get_dualwrite_status() for the full contract.
void time_sync_get_tz_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                        bool *diverged);
