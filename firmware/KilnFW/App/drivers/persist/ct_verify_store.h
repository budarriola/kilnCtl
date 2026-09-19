// ct_verify_store -- the persisted CT ATTRIBUTION VERDICT and the
// configuration fingerprint that keeps it honest.
// docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md, storage section.
//
// WHAT THIS STORES, AND WHY IT IS NOT IN THE WIZARD PROGRESS BLOB. The
// setup wizard's progress blob is tri-state (pending/done/skipped) and has
// no room for "inconclusive", which on this hardware is the NORMAL outcome
// (see the PHYSICAL CEILING note below). So the verdict lives here, in its
// own small NVS blob, and projects into the wizard through the
// `ct_attribution` readiness item rather than being written into the
// progress blob as a `done`.
//
// THE FINGERPRINT IS THE WHOLE POINT. A verdict is a statement about a
// SPECIFIC configuration: this zone's CT channel, that channel's clamp
// ratio and zero, the derived scale the Pico is armed with, and the zone's
// recorded normal current. Change any of those and the verdict no longer
// describes the board -- but nothing about the stored bytes would say so.
// That is exactly CLAUDE.md's standing "reset one side of a pair" class:
// two pieces of state joined by a contract nothing expresses. So the
// contract IS expressed, once, as ct_verify_fingerprint(): every field that
// can scale, move or re-route a CT reading is hashed into it, the hash is
// stored alongside the verdict, and a verdict whose stored fingerprint does
// not equal today's is reported as STALE -- never as the verdict it was.
// A stored verdict that survives a configuration change is the defect this
// module exists to prevent.
//
// PHYSICAL CEILING ON THIS BENCH (project note, not a limitation of the
// code): the bench fixture draws roughly 23 mA per zone against a 45 mA
// sweep noise floor, so no zone on it can ever clear
// zone_ct_verify_threshold_a() and every zone must come out INCONCLUSIVE.
// An implementation that can report PASS here is wrong. INCONCLUSIVE is
// therefore the initial value and the expected steady state on this board,
// not an error condition.
//
// NVS ONLY, deliberately: no `cfg` LittleFS dual-write. This is a
// measurement RESULT about one physical board, not a user preference --
// copying it into the config filesystem would make it travel with a config
// backup/restore onto a board whose clamps are somewhere else entirely.
#ifndef CT_VERIFY_STORE_H
#define CT_VERIFY_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sized to match ZONE_CT_CHANNEL_COUNT (zones_config_accessors.h) and this
 * board's three zones. Kept as its own constant rather than including that
 * header so this module stays free of the zones config stack -- the
 * _Static_assert in ct_verify_store.c holds the two together. */
#define CT_VERIFY_MAX_ZONES 3u
#define CT_VERIFY_CHANNELS  3u

/* Bumped only if the stored layout changes. A blob whose version this build
 * does not recognize is treated exactly like a missing key (no verdict),
 * never partially trusted -- same rule display_power_cfg.c applies. */
#define CT_VERIFY_BLOB_VERSION 1u

/* 0 is reserved: it means "no fingerprint" (never computed / not stored),
 * which can never equal a live fingerprint, so a zero stored fingerprint
 * always reads STALE. ct_verify_fingerprint() never returns it. */
#define CT_VERIFY_FINGERPRINT_NONE 0u

/* One zone's stored verdict. `verdict` and `reason` hold
 * zone_ct_verdict_t / zone_ct_verify_reason_t values (zones_http_internal.h)
 * narrowed to a byte -- stored as plain integers so this header does not
 * have to pull in the whole zones HTTP internal header to describe a blob. */
typedef struct {
    uint8_t verdict;      /* zone_ct_verdict_t: 0 INCONCLUSIVE, 1 PASS, 2 FAIL */
    uint8_t reason;       /* zone_ct_verify_reason_t */
    uint8_t responded_ch; /* channel that actually responded, or CT_VERIFY_CHANNELS if none */
    uint8_t reserved;     /* keeps the float below naturally aligned; always 0 */
    float   measured_a;   /* the responding channel's measured current */
    float   threshold_a;  /* the floor it was compared against */
} ct_verify_zone_t;

typedef struct {
    uint8_t  version;     /* CT_VERIFY_BLOB_VERSION */
    uint8_t  zone_count;  /* how many entries in zone[] are meaningful */
    uint8_t  reserved[2]; /* always 0 */
    uint32_t fingerprint; /* ct_verify_fingerprint() at the time of the run */
    int64_t  taken_unix;  /* wall-clock seconds, or 0 if time was never set */
    ct_verify_zone_t zone[CT_VERIFY_MAX_ZONES];
} ct_verify_blob_t;

/* Every configuration input that can change what a CT reading MEANS. If a
 * field scales, offsets, re-routes or re-scopes a reading and is not in
 * here, a change to it leaves a stale verdict standing -- that is the hole
 * this struct exists to close, so add to it rather than working around it.
 *
 * `trim_offset_a` / `trim_gain` are the operator-entered scale trim stored in
 * safety_ct_cal_blob_t since its v2 bump. They were hashed BEFORE they
 * existed, with the producer passing the identity values, precisely so that
 * landing the trim would be a one-line producer change that CANNOT forget to
 * extend the fingerprint -- which is how it landed. The producer now reads
 * them via safety_cfg_store_get_ct_cal_trim(). */
typedef struct {
    uint8_t zone_count;
    uint8_t ct_installed;                          /* param 0x0109 as cached; 1 when unknown */
    uint8_t ct_topology;                           /* param 0x031F as cached; 0 (per_zone) when unknown */
    uint8_t zone_ct_channel[CT_VERIFY_MAX_ZONES];  /* params 0x0320-0x0322 */
    uint8_t zone_relay_mask[CT_VERIFY_MAX_ZONES];  /* the zone-to-relay mapping */
    uint8_t ch_fitted[CT_VERIFY_CHANNELS];         /* per-channel fitted answer, 0/1 -- see note below */
    uint8_t ct_source[CT_VERIFY_CHANNELS];         /* safety_ct_cal_source_t provenance marker */
    float   i_normal_a[CT_VERIFY_MAX_ZONES];       /* params 0x031A-0x031C */
    float   a_fs[CT_VERIFY_CHANNELS];              /* operator-entered clamp ratio */
    float   zero_mv[CT_VERIFY_CHANNELS];           /* operator-entered zero */
    float   gain[CT_VERIFY_CHANNELS];              /* params 0x030B-0x030D */
    float   k_ct_v_per_a[CT_VERIFY_CHANNELS];      /* params 0x0308-0x030A, derived */
    uint16_t zero_counts[CT_VERIFY_CHANNELS];      /* params 0x0302-0x0304, derived (U16 on the wire) */
    float   trim_offset_a[CT_VERIFY_CHANNELS];     /* operator-entered offset trim, amps */
    float   trim_gain[CT_VERIFY_CHANNELS];         /* operator-entered scale trim, dimensionless */
} ct_verify_fingerprint_in_t;

/* PURE. FNV-1a over the fields above, in a fixed order, with every float
 * canonicalized (all NaNs hash alike, and -0.0f hashes as +0.0f) so that an
 * identical configuration always produces an identical hash regardless of
 * which particular NaN or signed zero a read produced. Never returns
 * CT_VERIFY_FINGERPRINT_NONE. NULL input returns CT_VERIFY_FINGERPRINT_NONE. */
uint32_t ct_verify_fingerprint(const ct_verify_fingerprint_in_t *in);

/* PURE. Wrong size, unknown version, zone_count out of range, or any
 * out-of-range enum/channel field -> false, i.e. treat as no verdict at all.
 * Shaped as a validate(bytes, len) so it reads the same way every other
 * blob store in this tree validates. */
bool ct_verify_blob_validate(const void *bytes, size_t len);

/* Loads the stored verdict from NVS into RAM. Non-fatal: a missing key, a
 * failed partition init or a blob that fails validation all leave the
 * in-RAM state at "no verdict", which every reader treats as NEVER RUN --
 * never as a pass. Safe to call more than once. */
esp_err_t ct_verify_store_start(void);

/* True (and fills *out when non-NULL) if a validated verdict is in RAM. */
bool ct_verify_store_get(ct_verify_blob_t *out);

/* Replaces the stored verdict wholesale and persists it. `blob` must pass
 * ct_verify_blob_validate() or this returns ESP_ERR_INVALID_ARG and changes
 * nothing. In-RAM truth updates first, so a failed NVS write means the
 * verdict will not survive a reboot, not that it failed to take effect now.
 *
 * MUST NOT be called from a PSRAM-stacked task: the underlying NVS write
 * refuses (and panics on real hardware) -- see safety_cfg_store.c's
 * caller_stack_is_external() note. The sweep task, its only caller, has an
 * internal-RAM stack. */
esp_err_t ct_verify_store_save(const ct_verify_blob_t *blob);

/* ---- the projection everything else reads -------------------------------
 *
 * THE resolved CT attribution fact: the stored verdict compared against
 * TODAY's configuration fingerprint, collapsed to the six-valued enum the
 * readiness item renders and the firing interlock blocks on
 * (readiness_http.h). Nothing outside this pair of functions may read the
 * stored verdict enum directly -- that is what makes it impossible to act on
 * a verdict without the staleness check happening in the same expression.
 *
 * DEFINED IN zones_current_sweep_task.c, not in ct_verify_store.c, because
 * gathering today's fingerprint needs the safety config cache, the zones
 * config and the CT calibration inputs -- exactly the dependencies this store
 * is kept free of. Declared here so the readiness page, the gate and the
 * producer all reach the ONE implementation.
 *
 * readiness_ct_attribution_fact_t is an int here rather than the enum itself
 * so this header does not pull the HTTP layer's header into the persist
 * layer; the caller assigns it straight into that enum. Values are exactly
 * READINESS_CT_ATTR_*. */
int ct_verify_current_fact(void);

/* Fills *out with today's live configuration, ready for
 * ct_verify_fingerprint(). Zeroes *out first, so a field no accessor can
 * answer hashes as a stable zero rather than as stack garbage -- garbage
 * would make every verdict read STALE at random. Defined alongside
 * ct_verify_current_fact(), for the same reason. */
void ct_verify_collect_fingerprint_in(ct_verify_fingerprint_in_t *out);

#ifdef __cplusplus
}
#endif

#endif /* CT_VERIFY_STORE_H */
