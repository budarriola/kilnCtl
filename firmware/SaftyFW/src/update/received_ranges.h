// received_ranges.h -- tracks which chunks of an in-progress image transfer
// have arrived, for CommonFW/docs/UPDATE_PROTOCOL.md's unacknowledged
// UPDATE_DATA scheme (TODO.md Phase 10 item 10.8c): "Send UPDATE_DATA as
// unacknowledged broadcast frames, and have the Pico report the ranges it is
// missing." No ACK per frame -- the Pico's own gap report, re-sent
// periodically, is what drives retransmission.
//
// Fixed-capacity bitmap, no dynamic allocation (docs/BOOTLOADER.md section
// 3's "never enable interrupts it does not need" spirit extends to this
// receiver too, and this codebase's own "no dynamic allocation after init"
// rule, TODO.md "Deliberately not doing" section) -- sized for the largest
// possible image, one slot's worth (bootloader/flash_layout.h's
// BOOTLOADER_SLOT_FLASH_SIZE), at UPDATE_DATA's documented 248-byte-per-frame
// payload (UPDATE_PROTOCOL.md section 4: "UART_PROTO_MAX_PAYLOAD is 253, so
// 248 bytes of image per frame after the 4-byte offset").
//
// Pure, no RTOS/SDK dependency -- host-testable.
#ifndef SAFTYFW_UPDATE_RECEIVED_RANGES_H
#define SAFTYFW_UPDATE_RECEIVED_RANGES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "flash_layout.h" // BOOTLOADER_SLOT_FLASH_SIZE -- bootloader/, see CMakeLists include path

#ifdef __cplusplus
extern "C" {
#endif

#define UPDATE_CHUNK_LEN 248u // UPDATE_PROTOCOL.md section 4, UPDATE_DATA's per-frame image payload

// ceil(BOOTLOADER_SLOT_FLASH_SIZE / UPDATE_CHUNK_LEN). 851968 / 248 =
// 3435.35..., so 3436 -- computed as an integer-arithmetic ceiling division
// rather than hand-typed, so this recomputes correctly if either constant
// ever changes.
#define UPDATE_MAX_CHUNKS \
    ((BOOTLOADER_SLOT_FLASH_SIZE + UPDATE_CHUNK_LEN - 1u) / UPDATE_CHUNK_LEN)

#define UPDATE_RANGES_BITMAP_BYTES ((UPDATE_MAX_CHUNKS + 7u) / 8u)

typedef struct {
    uint8_t  bits[UPDATE_RANGES_BITMAP_BYTES];
    uint32_t total_chunks; // set by update_received_ranges_reset(), <= UPDATE_MAX_CHUNKS
} update_received_ranges_t;

// Clears every bit and computes total_chunks = ceil(image_length /
// UPDATE_CHUNK_LEN), clamped to UPDATE_MAX_CHUNKS (an image_length larger
// than one slot can hold is a caller error the header-validation layer
// should already have rejected -- this function clamps defensively rather
// than reading/writing past the bitmap, but does not itself report the
// clamp as an error).
void update_received_ranges_reset(update_received_ranges_t *r, uint32_t image_length);

// Marks the chunk starting at wire byte offset `offset` as received.
// `offset` is untrusted (it comes straight off an UPDATE_DATA frame) --
// returns false, leaving the bitmap unchanged, if `offset` is not an exact
// multiple of UPDATE_CHUNK_LEN or if the resulting chunk index is
// >= r->total_chunks. Idempotent: marking an already-set chunk again (a
// retransmission arriving after its gap was already filled by a duplicate)
// returns true and leaves the bitmap in the same state -- this is the
// expected steady state for an unacknowledged, retransmission-tolerant
// protocol, not an error.
bool update_received_ranges_mark(update_received_ranges_t *r, uint32_t offset);

// True iff every chunk in [0, r->total_chunks) is marked received.
bool update_received_ranges_is_complete(const update_received_ranges_t *r);

// How many of r->total_chunks are currently marked received. Used for
// UPDATE_STATUS's progress reporting.
uint32_t update_received_ranges_count(const update_received_ranges_t *r);

// Fills `out_chunk_indices` with up to `max_out` MISSING chunk indices,
// starting the scan at `start_index` (inclusive) and wrapping around to 0
// if the scan reaches r->total_chunks before filling max_out entries or
// exhausting the gaps -- this is what lets a caller with N > max_out gaps
// cycle through all of them across successive UPDATE_STATUS frames (a
// "gap report cursor", advanced by the caller between calls) rather than
// only ever reporting the first max_out gaps forever. Returns the number of
// indices actually written (0 if r->is_complete()). Pure: does not modify
// `r` or track the cursor itself -- the caller owns `start_index` and
// decides how to advance it (e.g. to the index one past the last one
// returned, or back to 0 once a full pass completes).
size_t update_received_ranges_find_gaps(const update_received_ranges_t *r, uint32_t start_index,
                                         uint32_t *out_chunk_indices, size_t max_out);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_UPDATE_RECEIVED_RANGES_H
