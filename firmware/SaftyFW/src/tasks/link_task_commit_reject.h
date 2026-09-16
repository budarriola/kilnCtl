// link_task_commit_reject.h -- the pure mapping from a config_store write
// decision (config_store_write_decision_t) to the COMMIT_CONFIG_REJECTED
// wire reason the ESP/commissioning page actually renders.
//
// Factored out of link_task_handle_commit_config() (link_task.c) for the
// same reason link_task_tc_type_gate.c was: link_task.c pulls in FreeRTOS
// and the pico-sdk and is not built for the host test executable, so a
// decision living inside it cannot be tested. This file touches no lock,
// no hardware and no global state -- it takes two byte values and a
// decision enum and returns a verdict.
//
// 2026-09-15 (Opus adversarial re-review of d43e96b2, defect 1): the MIXED
// classification MUST be derived from the tc_type that is actually
// PERSISTED ON FLASH, which is the record config_store_write_ex() itself
// compared against when it built its "...also modifies field(s) other than
// thermocouple type..." log sentence. Reading the CACHED tc_type
// (config_store_get_tc_type()) instead reintroduces exactly the read that
// config_store_flash.c:185-196's "item 15" comment records as deliberately
// removed: a prior config_store_write_volatile() install leaves the cached
// record carrying values that are not on flash, so the cached and persisted
// types can disagree, and the wire reason and the Pico's own log line for
// one single refusal then contradict each other. Callers must pass
// config_store_get_persisted_tc_type(), never config_store_get_tc_type().
#ifndef SAFTYFW_TASKS_LINK_TASK_COMMIT_REJECT_H
#define SAFTYFW_TASKS_LINK_TASK_COMMIT_REJECT_H

#include <stdint.h>

#include "config_store.h"
#include "kilnlink/kilnlink_commit_config_rejected.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maps the REAL decision config_store_write_ex() computed internally onto
// the wire reason. `persisted_tc_type` is the type currently on flash
// (config_store_get_persisted_tc_type()); `candidate_tc_type` is the
// tc_type of the record that was just refused.
//
// Only CONFIG_STORE_WRITE_REFUSED_ARMED consults the two type bytes: a
// refusal whose candidate also changes the thermocouple type is a MIXED
// change (tc_type differs AND at least one other field differs, since a
// tc_type-ONLY change would not have produced the plain ARMED decision in
// the first place), and gets the distinct ARMED_MIXED reason so the
// operator learns that a tc_type-only change would have been allowed.
// Every other decision ignores them.
//
// CONFIG_STORE_WRITE_OK is not a refusal and maps to UNKNOWN -- the caller
// never reaches this function on a successful write.
kilnlink_commit_config_reject_reason_t link_task_commit_config_reject_reason_for(
    config_store_write_decision_t decision, uint8_t persisted_tc_type, uint8_t candidate_tc_type);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_LINK_TASK_COMMIT_REJECT_H
