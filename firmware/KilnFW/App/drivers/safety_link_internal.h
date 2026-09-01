// Internal (non-public) declarations shared ONLY among the safety_link_*.c
// translation units -- safety_link.c, safety_link_frames.c, safety_link_
// inbox.c, safety_link_poll.c, safety_link_commands.c and safety_link_
// payload.c. Nothing outside this driver may include this header; the
// public surface stays exactly safety_link.h, unchanged by this split.
//
// This file exists purely because of the ROADMAP-suggested file split
// (safety_link.c was 4183 lines): every symbol declared below used to be
// `static` inside the one translation unit safety_link.c was. Splitting
// that unit into six turned each of them into a genuine cross-TU call, so
// each had to either become `static inline` here (safety_lock/safety_unlock
// -- trivial one-line mutex wrappers, moved verbatim, still no external
// symbol) or drop `static` and get a declaration here (everything else
// below -- real logic, kept as one defined body rather than duplicated).
// No behavior changed: same bodies, same call sites, only where each is
// DEFINED and how it is reached from another file moved.
#ifndef SAFETY_LINK_INTERNAL_H
#define SAFETY_LINK_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "safety_link.h"
#include "uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- safety_link.c (helpers / cache / staleness) ------------------------ */

static inline bool safety_lock(SafetyLinkClass *link)
{
    return xSemaphoreTake(link->state_lock, portMAX_DELAY) == pdTRUE;
}

static inline void safety_unlock(SafetyLinkClass *link)
{
    xSemaphoreGive(link->state_lock);
}

uint32_t safety_elapsed_ms(TickType_t since);
uint16_t safety_age_ms_locked(const SafetyLinkClass *link);
bool     safety_link_up_locked(const SafetyLinkClass *link);
void     safety_reset_stale_peer_info_if_link_down(SafetyLinkClass *link);

/* --- safety_link_frames.c (frame decode/apply, outbound builders) ------- */

void safety_link_send_announce_version_burst(SafetyLinkClass *link);
void safety_build_and_send_context(SafetyLinkClass *link);
bool safety_apply_status(SafetyLinkClass *link, const uart_proto_message_t *msg);
bool safety_apply_power(SafetyLinkClass *link, const uart_proto_message_t *msg);
bool safety_apply_diag(SafetyLinkClass *link, const uart_proto_message_t *msg);
bool safety_apply_trip_event(SafetyLinkClass *link, const uart_proto_message_t *msg);
bool safety_apply_update_status(SafetyLinkClass *link, const uart_proto_message_t *msg);
void safety_apply_fw_version(SafetyLinkClass *link, const uart_proto_message_t *msg);

/* --- safety_link_inbox.c (drain / stash / request-reply exchange) ------- */

esp_err_t safety_exchange(SafetyLinkClass *link, const uint8_t *request, size_t length,
                           bool expect_status);
bool safety_drain_inbox(SafetyLinkClass *link, uint32_t wait_ms);
bool safety_drain_inbox_for_status(SafetyLinkClass *link, uint32_t wait_ms);
bool safety_drain_inbox_ex(SafetyLinkClass *link, uint32_t wait_ms, bool want_status,
                            uart_proto_message_t *out_ct_cal, bool *out_got_ct_cal,
                            uart_proto_message_t *out_config_page, bool *out_got_config_page,
                            uart_proto_message_t *out_commit_rejected, bool *out_got_commit_rejected,
                            uart_proto_message_t *out_rollback_result, bool *out_got_rollback_result);
bool safety_take_stashed_config_page(SafetyLinkClass *link, uint8_t want_page_index,
                                      uart_proto_message_t *out);
void safety_clear_stashed_commit_rejected(SafetyLinkClass *link);
bool safety_take_stashed_rollback_result(SafetyLinkClass *link, uart_proto_message_t *out);
void safety_clear_stashed_rollback_result(SafetyLinkClass *link);

/* --- safety_link_poll.c (poll task) -------------------------------------- */

void safety_poll_task(void *arg);

#ifdef __cplusplus
}
#endif

#endif // SAFETY_LINK_INTERNAL_H
