#include "benchproto/benchproto_link.h"

#include <string.h>

static benchproto_task_slot_t *find_slot(benchproto_link_t *link, uint8_t task_id)
{
    for (unsigned i = 0; i < BENCHPROTO_MAX_TASKS; ++i) {
        if (link->tasks[i].in_use && link->tasks[i].task_id == task_id) {
            return &link->tasks[i];
        }
    }
    return NULL;
}

static const benchproto_task_slot_t *find_slot_const(const benchproto_link_t *link, uint8_t task_id)
{
    for (unsigned i = 0; i < BENCHPROTO_MAX_TASKS; ++i) {
        if (link->tasks[i].in_use && link->tasks[i].task_id == task_id) {
            return &link->tasks[i];
        }
    }
    return NULL;
}

static bool dedup_check(const benchproto_task_slot_t *slot, uint8_t src_device, uint8_t src_task,
                         uint16_t msg_index)
{
    for (unsigned i = 0; i < BENCHPROTO_DEDUP_DEPTH; ++i) {
        if (slot->dedup[i].valid && slot->dedup[i].src_device == src_device &&
            slot->dedup[i].src_task == src_task && slot->dedup[i].msg_index == msg_index) {
            return true; /* already processed (delivered) this exact message */
        }
    }
    return false;
}

static void dedup_record(benchproto_task_slot_t *slot, uint8_t src_device, uint8_t src_task,
                          uint16_t msg_index)
{
    slot->dedup[slot->dedup_next].valid = true;
    slot->dedup[slot->dedup_next].src_device = src_device;
    slot->dedup[slot->dedup_next].src_task = src_task;
    slot->dedup[slot->dedup_next].msg_index = msg_index;
    slot->dedup_next = (uint8_t)((slot->dedup_next + 1u) % BENCHPROTO_DEDUP_DEPTH);
}

void benchproto_link_init(benchproto_link_t *link, uint8_t own_device)
{
    if (!link) {
        return;
    }
    memset(link, 0, sizeof(*link));
    link->own_device = own_device;
}

benchproto_link_status_t benchproto_link_register_task(benchproto_link_t *link, uint8_t task_id)
{
    if (!link) {
        return BENCHPROTO_LINK_ERR_INVALID_ARG;
    }
    if (find_slot(link, task_id)) {
        return BENCHPROTO_LINK_ERR_ALREADY_REGISTERED;
    }
    for (unsigned i = 0; i < BENCHPROTO_MAX_TASKS; ++i) {
        if (!link->tasks[i].in_use) {
            memset(&link->tasks[i], 0, sizeof(link->tasks[i]));
            link->tasks[i].in_use = true;
            link->tasks[i].task_id = task_id;
            return BENCHPROTO_LINK_OK;
        }
    }
    return BENCHPROTO_LINK_ERR_NO_SLOTS;
}

benchproto_link_status_t benchproto_link_unregister_task(benchproto_link_t *link, uint8_t task_id)
{
    if (!link) {
        return BENCHPROTO_LINK_ERR_INVALID_ARG;
    }
    benchproto_task_slot_t *slot = find_slot(link, task_id);
    if (!slot) {
        return BENCHPROTO_LINK_ERR_NOT_FOUND;
    }
    memset(slot, 0, sizeof(*slot));
    return BENCHPROTO_LINK_OK;
}

bool benchproto_link_is_registered(const benchproto_link_t *link, uint8_t task_id)
{
    if (!link) {
        return false;
    }
    return find_slot_const(link, task_id) != NULL;
}

uint16_t benchproto_link_next_msg_index(benchproto_link_t *link)
{
    return link->next_tx_index++;
}

void benchproto_pending_begin(benchproto_pending_request_t *pending, uint8_t dst_device, uint8_t dst_task,
                               uint16_t msg_index)
{
    if (!pending) {
        return;
    }
    pending->active = true;
    pending->dst_device = dst_device;
    pending->dst_task = dst_task;
    pending->msg_index = msg_index;
    pending->attempt = 1;
}

bool benchproto_pending_note_retry(benchproto_pending_request_t *pending)
{
    if (!pending || !pending->active) {
        return false;
    }
    if (pending->attempt >= BENCHPROTO_MAX_RETRIES) {
        benchproto_pending_clear(pending);
        return false;
    }
    pending->attempt++;
    return true;
}

void benchproto_pending_clear(benchproto_pending_request_t *pending)
{
    if (!pending) {
        return;
    }
    memset(pending, 0, sizeof(*pending));
}

benchproto_link_action_t benchproto_link_on_frame(benchproto_link_t *link,
                                                    benchproto_pending_request_t *pending,
                                                    const benchproto_frame_t *frame)
{
    if (!link || !frame) {
        return BENCHPROTO_LINK_ACTION_IGNORE;
    }

    if (frame->msg_type == BENCHPROTO_MSG_ACK || frame->msg_type == BENCHPROTO_MSG_NACK) {
        /* A reply to something *we* sent: the replier is our original dst. */
        if (!pending || !pending->active) {
            return BENCHPROTO_LINK_ACTION_IGNORE;
        }
        if (frame->src_device != pending->dst_device || frame->src_task != pending->dst_task ||
            frame->msg_index != pending->msg_index) {
            return BENCHPROTO_LINK_ACTION_IGNORE;
        }
        return (frame->msg_type == BENCHPROTO_MSG_ACK) ? BENCHPROTO_LINK_ACTION_ACK_MATCHED
                                                          : BENCHPROTO_LINK_ACTION_NACK_MATCHED;
    }

    if (frame->msg_type != BENCHPROTO_MSG_DATA && frame->msg_type != BENCHPROTO_MSG_BROADCAST) {
        return BENCHPROTO_LINK_ACTION_IGNORE;
    }

    if (frame->dst_device != link->own_device) {
        return BENCHPROTO_LINK_ACTION_IGNORE; /* not for us */
    }

    benchproto_task_slot_t *slot = find_slot(link, frame->dst_task);
    if (!slot) {
        if (frame->msg_type == BENCHPROTO_MSG_BROADCAST) {
            /* Fire-and-forget: an unregistered task_id is just dropped,
             * never NACKed -- the receiver is never obliged to reply to a
             * broadcast either way. */
            return BENCHPROTO_LINK_ACTION_IGNORE;
        }
        return BENCHPROTO_LINK_ACTION_NACK_UNROUTABLE;
    }

    if (frame->msg_type == BENCHPROTO_MSG_DATA &&
        dedup_check(slot, frame->src_device, frame->src_task, frame->msg_index)) {
        return BENCHPROTO_LINK_ACTION_DUPLICATE_REACK;
    }

    return BENCHPROTO_LINK_ACTION_DELIVER;
}

benchproto_link_status_t benchproto_link_mark_delivered(benchproto_link_t *link, uint8_t task_id,
                                                          uint8_t src_device, uint8_t src_task,
                                                          uint16_t msg_index)
{
    if (!link) {
        return BENCHPROTO_LINK_ERR_INVALID_ARG;
    }
    benchproto_task_slot_t *slot = find_slot(link, task_id);
    if (!slot) {
        return BENCHPROTO_LINK_ERR_NOT_FOUND;
    }
    dedup_record(slot, src_device, src_task, msg_index);
    return BENCHPROTO_LINK_OK;
}

void benchproto_link_reset_device(benchproto_link_t *link, uint8_t src_device)
{
    if (!link) {
        return;
    }
    for (unsigned i = 0; i < BENCHPROTO_MAX_TASKS; ++i) {
        if (!link->tasks[i].in_use) {
            continue;
        }
        benchproto_task_slot_t *slot = &link->tasks[i];
        for (unsigned d = 0; d < BENCHPROTO_DEDUP_DEPTH; ++d) {
            if (slot->dedup[d].valid && slot->dedup[d].src_device == src_device) {
                slot->dedup[d].valid = false;
            }
        }
    }
}
