#ifndef CT_LEAK_ALARM_H
#define CT_LEAK_ALARM_H
/* H9 CT alarm (docs/SAFETY_CASE.md H9; owner decision 2026-10-04).
 *
 * Current flowing in a CT channel while EVERY relay is commanded off means
 * something downstream of the relays is still conducting: a welded contactor,
 * a stuck SSR, a bypass wire. The ESP cannot interrupt that current (the
 * stuck device is downstream of every authority it has), so this is an ALARM,
 * not a protection: it latches, is shown on the dashboard and the LCD, is
 * listed in GET /api/readiness, is logged at error level, and refuses to
 * start a new firing. SAFETY_CASE.md H9 states the residual risk.
 *
 * Pure and host-testable: ct_leak_alarm_tick() takes a plain snapshot and a
 * caller-owned state. The only global is the published snapshot below, which
 * the poll task writes and readiness/dashboard/LCD read.
 *
 * "Every relay commanded off" is the FULL relay shadow (all four relays,
 * i.e. zone, manual and aux outputs alike), expressed by the caller as
 * relays_off_ms (kiln_io_relays_off_ms(), UINT32_MAX while any relay is on).
 * Using zone heat demand alone would false-alarm on a vent or fan, which draws
 * current by design.
 */
#include <stdbool.h>
#include <stdint.h>

#define CT_LEAK_CHANNELS 3
/* The shadow must have read all-off this long before any sample counts, so
 * the decay of a just-released load is never read as a leak (the same 5 s
 * precondition the CT auto-zero uses). */
#define CT_LEAK_RELAYS_OFF_SETTLE_MS 5000u
/* Current must stay above the floor, on consecutive evaluated samples, this
 * long before the alarm raises. */
#define CT_LEAK_ASSERT_MS 10000u
/* Once raised the alarm holds until the channels have read below the floor,
 * with every relay still off, this long. */
#define CT_LEAK_CLEAR_MS 30000u

typedef struct {
    uint32_t now_ms;
    bool link_up;             /* current_a below is only meaningful when true */
    bool ct_installed;        /* safety_cfg ct_installed (unset reads true: default-safe) */
    bool summed;              /* ct_topology: only channel index 2 is fitted */
    bool activity;            /* profile / autotune / CT sweep running: relays chop, skip */
    uint32_t relays_off_ms;   /* kiln_io_relays_off_ms(); UINT32_MAX when any relay is on */
    float current_a[CT_LEAK_CHANNELS];
    float k_ct_v_per_a[CT_LEAK_CHANNELS]; /* live committed k_ct per channel, 0 = unknown */
} ct_leak_alarm_input_t;

typedef struct {
    bool alarm;                /* latched */
    uint8_t channel_mask;      /* channels above the floor at the last evaluated sample */
    float peak_a;              /* largest above-floor reading seen since the alarm raised */
    bool above_run;            /* a continuous above-floor run is in progress */
    uint32_t above_since_ms;
    bool below_run;            /* a continuous below-floor run is in progress (alarm held) */
    uint32_t below_since_ms;
} ct_leak_alarm_state_t;

void ct_leak_alarm_reset(ct_leak_alarm_state_t *s);

/* Returns true only on the tick that RAISES the alarm. */
bool ct_leak_alarm_tick(ct_leak_alarm_state_t *s, const ct_leak_alarm_input_t *in);

/* ---- published snapshot (poll task writes, others read) ---- */
void ct_leak_alarm_publish(const ct_leak_alarm_state_t *s);
bool ct_leak_alarm_is_active(void);
/* Short operator text, e.g. "CT current with all relays off: ch3 0.12 A".
 * Always NUL-terminated, never contains a quote or backslash. Empty when clear. */
void ct_leak_alarm_describe(char *out, unsigned cap);
#endif
