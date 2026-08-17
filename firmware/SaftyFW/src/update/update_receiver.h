// update_receiver.h -- the two pieces of Phase 10's update-receive flow
// that are genuinely decision logic rather than I/O, factored out so they
// can be host-tested against synthetic inputs before anything touches real
// flash or the link: (1) whether to accept an UPDATE_BEGIN at all, and
// which slot to actually target, and (2) the retransmission-round cap for
// UPDATE_PROTOCOL.md section 4's gap-report scheme (TODO.md item 10.8c:
// "Cap total retransmission rounds, and fail cleanly rather than looping if
// a range never lands").
//
// Deliberately NOT a general state-machine abstraction covering the whole
// BEGIN/DATA/END/ABORT flow -- most of that flow (erase the slot, stream
// bytes to flash, CRC read back) is real flash I/O with no meaningful pure
// core beyond what image_header.h and received_ranges.h already provide.
// Inventing a bigger FSM wrapper around calls this codebase's own pure
// modules already serve would be exactly the kind of speculative
// abstraction CLAUDE.md's project conventions warn against.
//
// Pure, no RTOS/SDK dependency -- host-testable.
#ifndef SAFTYFW_UPDATE_RECEIVER_H
#define SAFTYFW_UPDATE_RECEIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "image_header.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Preconditions (TODO.md item 10.9, CommonFW/docs/UPDATE_PROTOCOL.md
// section 1's table) -----------------------------------------------------
//
// "The Pico enforces the last three [preconditions] itself. It does not
// take the ESP's word for it, for the same reason the whole safety
// processor exists." This struct is the Pico's OWN evidence, gathered by
// the caller from safety_core/thermo_task (this module has no link/RTOS
// dependency and cannot gather it itself) -- never from anything the ESP
// claims in the UPDATE_BEGIN frame.
typedef struct {
    bool relay_open;         // !relay_energized (safety_core_get_output_status())
    bool no_trip_pending;    // safety_core_get_diag_status()'s trip_reason == SAFETY_TRIP_NONE
    bool temp_known_and_low; // a valid safety-TC reading exists AND it is below the
                              // configured update-ceiling. An UNKNOWN reading (no
                              // valid thermocouple data at all) must NOT be treated
                              // as "low enough" -- UPDATE_PROTOCOL.md section 1: "A
                              // cooling kiln is still a hot kiln", and an unreadable
                              // one is worse than a known-hot one, since there is no
                              // basis to refuse OR to permit. The caller computes
                              // this single bool from both facts combined, since this
                              // module has no notion of NaN/thermocouple validity of
                              // its own (image_header.h/received_ranges.h's whole
                              // point is staying decoupled from that).
} update_preconditions_t;

// One bit per unmet precondition -- UPDATE_PROTOCOL.md section 1: "The ESP
// must present a refusal with the specific unmet precondition, not a
// generic failure." A caller ORs these into whatever UPDATE_STATUS's
// last_error field reports, or into a log line, rather than reporting one
// opaque "refused".
typedef enum {
    UPDATE_PRECOND_RELAY_CLOSED = 1u << 0,
    UPDATE_PRECOND_TRIP_PENDING = 1u << 1,
    UPDATE_PRECOND_TOO_HOT      = 1u << 2,
} update_precondition_flag_t;

// Returns the OR of every unmet precondition's flag, or 0 if all three are
// satisfied.
uint8_t update_preconditions_check(const update_preconditions_t *p);

// --- UPDATE_BEGIN acceptance decision ------------------------------------

typedef enum {
    UPDATE_BEGIN_ACCEPTED = 0,
    UPDATE_BEGIN_REFUSED_PRECONDITION,   // see precondition_flags for which
    UPDATE_BEGIN_REFUSED_HEADER_INVALID, // see header_check for which
    UPDATE_BEGIN_REFUSED_VERSION_INCOMPATIBLE,
} update_begin_outcome_t;

typedef struct {
    update_begin_outcome_t outcome;

    // Meaningful only when outcome == UPDATE_BEGIN_REFUSED_PRECONDITION:
    // OR of update_precondition_flag_t.
    uint8_t precondition_flags;

    // Meaningful only when outcome == UPDATE_BEGIN_REFUSED_HEADER_INVALID.
    update_image_header_check_t header_check;

    // Meaningful only when outcome == UPDATE_BEGIN_ACCEPTED: which slot
    // (BOOTLOADER_SLOT_A/_B from bootloader/metadata.h -- this file does not
    // include that header to stay decoupled, so this is a plain 0/1, the
    // same numbering) the Pico will actually stage into. ALWAYS "whichever
    // slot is not currently active", regardless of what the image header's
    // requested_slot said.
    uint8_t target_slot;

    // True if hdr->requested_slot disagreed with target_slot above -- not a
    // refusal reason (the Pico's own choice always wins, per this file's
    // header comment), but worth logging: it usually means the ESP's view
    // of which slot is active is stale.
    bool requested_slot_mismatch;
} update_begin_decision_t;

// `active_slot` is the Pico's own current active slot (0 or 1, matching
// bootloader/metadata.h's BOOTLOADER_SLOT_A/_B numbering -- passed as a
// plain uint8_t rather than including that header, to keep this module
// decoupled from the bootloader's own headers the way image_header.h and
// received_ranges.h already are). `max_image_length` is the slot capacity
// (the caller passes BOOTLOADER_SLOT_FLASH_SIZE).
//
// Order of checks, matching UPDATE_PROTOCOL.md section 4's flow step 2
// ("Pico re-checks its own preconditions... and either accepts or refuses
// with a reason") -- preconditions are checked FIRST, before the header is
// even inspected, since a kiln that is not idle should refuse an update
// attempt regardless of whether the uploaded image itself is well-formed.
update_begin_decision_t update_receiver_handle_begin(const update_image_header_t *hdr,
                                                       const update_preconditions_t *precond,
                                                       uint8_t active_slot,
                                                       uint32_t max_image_length);

// --- Retransmission round cap (item 10.8c) -------------------------------
//
// UPDATE_PROTOCOL.md section 4: "Cap total retransmission rounds, and fail
// cleanly rather than looping if a range never lands. A link that cannot
// deliver the same 248 bytes after ten attempts is broken, and saying so
// beats retrying forever." One "round" = one full gap-report cycle (every
// missing chunk named once, per received_ranges.h's cursor scheme) that
// still finds at least one gap when it completes.
#define UPDATE_MAX_RETRANSMIT_ROUNDS 10u

// Returns true if `round_count` (the number of gap-report rounds completed
// so far, each of which still found at least one missing chunk) is still
// within the cap and the transfer should keep retrying. Returns false once
// the cap is reached -- the caller should then abort the transfer and
// report UPDATE_PROTOCOL.md's own "broken link" outcome rather than loop.
bool update_retransmit_should_continue(uint32_t round_count);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_UPDATE_RECEIVER_H
