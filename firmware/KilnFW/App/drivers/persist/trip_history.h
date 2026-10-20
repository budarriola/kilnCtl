// trip_history -- FD-05: persisted ring of the last TRIP_HISTORY_MAX Pico trips.
//
// Same shape as boot_history (FD-03): one NVS blob (kiln_cfg/trip_hist in KILN_NVS_PARTITION)
// with a CRC32, rewritten ONCE per NEW trip. The link RX task never touches flash: it calls
// trip_history_note() (lock-free single-producer/single-consumer queue, RAM only) and the
// telemetry_log task calls trip_history_flush() (FD-04 pattern), which hops the NVS write onto
// bx_flash_worker via flash_hop_run().
//
// FW-04 (flash wear): a trip with the same reason as the NEWEST entry, on the same boot and within
// TRIP_HISTORY_MERGE_WINDOW_S of it, is merged into that entry (count++, newest key/stamps), and the blob is
// rewritten only when the count reaches a power of 2. Unpersisted merges live in a RAM shadow of the entry.
//
// Dedupe: a Pico re-sends the same latched TRIP_EVENT on every status poll, and again after an
// ESP reboot. A trip is the triple (pico trip_seq, reason, pico uptime at trip); a trip already in
// the ring is dropped without a write. The wire frame carries no zone, so zone is 0xFF (unknown)
// unless a caller supplies one.
#ifndef KILNCTL_TRIP_HISTORY_H
#define KILNCTL_TRIP_HISTORY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TRIP_HISTORY_MAX 16
#define TRIP_HISTORY_VERSION 1u
#define TRIP_HISTORY_ZONE_UNKNOWN 0xFFu
#define TRIP_HISTORY_PENDING_MAX 4
#define TRIP_HISTORY_MERGE_WINDOW_S 60u /* FW-04: same-reason trips this close merge into the last entry */

typedef struct {
    uint32_t seq;            /* ESP-side sequence number, 1-based, strictly increasing */
    uint32_t pico_uptime_ms; /* Pico uptime at trip (dedupe key part) */
    uint32_t esp_uptime_s;   /* ESP uptime when the trip was noted */
    uint32_t utc;            /* UTC epoch seconds when noted, 0 = not synced */
    uint32_t boot_seq;       /* boot_history seq of the boot that noted it, 0 = unknown */
    float    safety_tc_c;    /* safety thermocouple at trip (0 when unknown) */
    float    threshold;      /* deciding threshold */
    uint8_t  reason;         /* SAFETY_TRIP_* (1..) */
    uint8_t  pico_trip_seq;  /* Pico's trip_seq (dedupe key part) */
    uint16_t mask;           /* 1 << (reason-1), 0 if reason out of 1..16 */
    uint8_t  zone;           /* TRIP_HISTORY_ZONE_UNKNOWN when not in the message */
    uint8_t  pad;
    uint16_t count;          /* FW-04: identical trips merged into this entry; 0 or 1 = a single trip */
} trip_history_entry_t;      /* 7*4 + 8 = 36 bytes */

typedef struct {
    uint8_t  version;
    uint8_t  count;
    uint8_t  head;
    uint8_t  reserved;
    uint32_t next_seq;
    uint32_t crc32; /* esp_crc32_le over the struct with crc32 zeroed */
    trip_history_entry_t entries[TRIP_HISTORY_MAX];
} trip_history_blob_t;

/* Pure ring logic (host-tested). */
bool trip_history_blob_valid(const trip_history_blob_t *b);
void trip_history_blob_init(trip_history_blob_t *b);
/* Returns false (ring untouched) when the trip is already present. Else assigns seq, wraps. */
bool trip_history_blob_push(trip_history_blob_t *b, const trip_history_entry_t *e);
bool trip_history_blob_has(const trip_history_blob_t *b, const trip_history_entry_t *e);
/* i = 0 is the newest. Returns false when i >= count. */
bool trip_history_blob_get(const trip_history_blob_t *b, unsigned i, trip_history_entry_t *out);
uint16_t trip_history_mask_for_reason(uint8_t reason);

/* Missing/corrupt/wrong-version blob yields an empty ring (returns false then). */
bool trip_history_load(trip_history_blob_t *out);

/* Link RX / any context: RAM only, no flash, no blocking. Stamps are caller supplied
 * (esp_uptime_s, utc = 0 if unsynced, boot_seq = 0 if unknown). Returns false if the
 * pending queue was full (the trip is then lost from the history; counted). */
bool trip_history_note(const trip_history_entry_t *e);
/* Same as trip_history_note() but builds the entry here (zone unknown), so a deep caller
 * (safety_apply_trip_event, on small task stacks) needs no entry struct in its own frame. */
bool trip_history_note_event(uint32_t pico_uptime_ms, uint32_t esp_uptime_s, uint32_t utc,
                             uint32_t boot_seq, float safety_tc_c, float threshold,
                             uint8_t reason, uint8_t pico_trip_seq);
unsigned trip_history_dropped(void);

/* Task context (telemetry_log): persists every pending, not-yet-recorded trip. At most one NVS
 * write per call (all pending entries ride the same blob write). Returns the number of NEW
 * entries recorded; 0 = nothing pending or all duplicates (no write). */
unsigned trip_history_flush(void);

#ifdef __cplusplus
}
#endif

#endif
