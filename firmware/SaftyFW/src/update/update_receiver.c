// update_receiver.c -- see update_receiver.h.
#include "update_receiver.h"

uint8_t update_preconditions_check(const update_preconditions_t *p)
{
    uint8_t flags = 0;
    if (!p->relay_open) {
        flags |= (uint8_t)UPDATE_PRECOND_RELAY_CLOSED;
    }
    if (!p->no_trip_pending) {
        flags |= (uint8_t)UPDATE_PRECOND_TRIP_PENDING;
    }
    if (!p->temp_known_and_low) {
        flags |= (uint8_t)UPDATE_PRECOND_TOO_HOT;
    }
    return flags;
}

update_begin_decision_t update_receiver_handle_begin(const update_image_header_t *hdr,
                                                       const update_preconditions_t *precond,
                                                       uint8_t active_slot,
                                                       uint32_t max_image_length)
{
    update_begin_decision_t d;
    d.outcome = UPDATE_BEGIN_ACCEPTED;
    d.precondition_flags = 0;
    d.header_check = UPDATE_IMAGE_HEADER_OK;
    d.target_slot = 0;
    d.requested_slot_mismatch = false;

    // Preconditions first (see header comment: refuse before even looking
    // at whether the image itself is well-formed).
    uint8_t precond_flags = update_preconditions_check(precond);
    if (precond_flags != 0u) {
        d.outcome = UPDATE_BEGIN_REFUSED_PRECONDITION;
        d.precondition_flags = precond_flags;
        return d;
    }

    update_image_header_check_t header_check = update_image_header_validate(hdr, max_image_length);
    if (header_check != UPDATE_IMAGE_HEADER_OK) {
        d.outcome = UPDATE_BEGIN_REFUSED_HEADER_INVALID;
        d.header_check = header_check;
        return d;
    }

    // Note: protocol_version/min_compatible COMPATIBILITY is deliberately
    // not checked here -- src/tasks/link_frame.c's already-built,
    // already-host-tested link_frame_versions_compatible() is the single
    // source of truth for that formula, and this module does not duplicate
    // it. A caller that wants UPDATE_BEGIN_REFUSED_VERSION_INCOMPATIBLE
    // reported should call that function itself and short-circuit before
    // even reaching this one, or this function's return value simply never
    // becomes that variant in the current call graph -- the enum value
    // exists for that caller-side check to report through the same result
    // type, not because this function produces it.

    uint8_t target = (active_slot == 0u) ? 1u : 0u; // BOOTLOADER_SLOT_A/_B are 0/1 -- see header comment
    d.target_slot = target;
    d.requested_slot_mismatch = (hdr->requested_slot != target);

    return d;
}

bool update_retransmit_should_continue(uint32_t round_count)
{
    return round_count < UPDATE_MAX_RETRANSMIT_ROUNDS;
}
