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

/* Ceiling on how long a caller waits to start its own request/reply exchange.
 * One exchange is bounded by the ACK timeout times uart_protocol's retries plus
 * the reply timeout (~750 ms with a dead peer), so anything past a few of those
 * means the holder is wedged rather than merely unlucky. Waiting forever here
 * would let a stuck safety link take out whatever task asked it a question --
 * including the bridge task that answers the PC. Used directly by both
 * safety_link_inbox.c's safety_exchange() and safety_link_commands.c's
 * safety_link_send_rollback_ex() (which takes link->xact_lock itself, against
 * the same ceiling, for its longer boot-id-watch operation) -- shared here
 * rather than duplicated in each. */
#define SAFETY_XACT_LOCK_TIMEOUT_MS 5000u

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

/* SAFETY_FAULT_MIN_HOLD_MS's own comment (safety_link.h): finishes a
 * deassert that safety_link_set_fault_source() deferred because the source's
 * minimum hold time had not yet elapsed. Called from safety_poll_task()
 * (safety_link_poll.c) on every pass, including the sub-period idle-drain
 * chunks, so a pending release is applied within roughly one
 * SAFETY_LINK_IDLE_TICK_MS of the hold expiring -- never earlier, since it is
 * the poll task's own tick that drives it, not a timer or a sleep in the
 * original caller. No-op, cheap (one lock take, one mask check), whenever
 * nothing is pending. */
void     safety_link_service_pending_fault_deassert(SafetyLinkClass *link);

/* Shared bounded-wait/unknown-outcome helper (M15 B2) -- declared here
 * (public type/doc lives in safety_link.h) so every safety_link_*.c file can
 * reach it without an extra include. See safety_link.h for the full
 * contract. */
safety_link_await_result_t safety_link_await_or_unknown(uint32_t timeout_ms, uint32_t poll_interval_ms,
                                                          safety_link_await_poll_fn poll_fn, void *ctx);

/* --- safety_link_frames.c (frame decode/apply, outbound builders) ------- */

void safety_link_send_announce_version_burst(SafetyLinkClass *link);
void safety_build_and_send_context(SafetyLinkClass *link);
bool safety_apply_status(SafetyLinkClass *link, const uart_proto_message_t *msg);
bool safety_apply_power(SafetyLinkClass *link, const uart_proto_message_t *msg);
bool safety_apply_diag(SafetyLinkClass *link, const uart_proto_message_t *msg);
bool safety_apply_trip_event(SafetyLinkClass *link, const uart_proto_message_t *msg);
bool safety_apply_update_status(SafetyLinkClass *link, const uart_proto_message_t *msg);
void safety_apply_fw_version(SafetyLinkClass *link, const uart_proto_message_t *msg);
void safety_link_service_boot_clear_if_pending(SafetyLinkClass *link);

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
bool safety_take_stashed_reboot_result(SafetyLinkClass *link, uart_proto_message_t *out);
bool safety_take_stashed_stack_margin(SafetyLinkClass *link, uart_proto_message_t *out);
bool safety_take_stashed_param(SafetyLinkClass *link, uart_proto_message_t *out);
void safety_clear_stashed_rollback_result(SafetyLinkClass *link);
bool safety_take_stashed_ct_auto_zero_status(SafetyLinkClass *link, uart_proto_message_t *out);

/* --- safety_link_poll.c (poll task) -------------------------------------- */

void safety_poll_task(void *arg);

#ifdef __cplusplus
}
#endif

#endif // SAFETY_LINK_INTERNAL_H
