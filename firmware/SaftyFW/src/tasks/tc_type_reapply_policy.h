// tc_type_reapply_policy.h -- pure decision extracted from link_task.c's
// three config-write handlers (SET_CONFIG, COMMIT_CONFIG, APPLY_CONFIG_
// VOLATILE), per the 2026-09-15 Opus review of the tc_type-settable feature
// (docs/audits/review_safety_tc_type_settable_2026-09-15.md, F5): the review
// found the only test the original commit added exercised the stack-budget
// script, not thermo_task.c's live-reapply behaviour, and that thermo_task.c
// is not linked into any host test at all -- so a negative test against it
// could never fail. This header has NO dependency on config_store.h,
// thermo_task.h, or any hardware/RTOS symbol, specifically so it CAN be
// linked into a plain host-test executable.
#ifndef SAFTYFW_TASKS_TC_TYPE_REAPPLY_POLICY_H
#define SAFTYFW_TASKS_TC_TYPE_REAPPLY_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// True iff a config write that is about to move the committed/installed
// tc_type from `prev_tc_type` to `new_tc_type` must trigger a live
// thermo_task_request_tc_type_reapply() call. Today this is exactly
// "the byte actually changed" -- a write that merely re-asserts the SAME
// type (e.g. a SET_CONFIG that only touched some other field, or a
// COMMIT_CONFIG replaying an unchanged staged value) must NOT force a
// spurious re-verification window (the reapply reports every reading
// invalid until the next confirmed CR1 readback, per thermo_task.h's own
// comment on thermo_task_request_tc_type_reapply() -- forcing that on every
// write regardless of content would needlessly re-open it).
//
// Every one of link_task.c's three config-write call sites (SET_CONFIG,
// COMMIT_CONFIG, APPLY_CONFIG_VOLATILE) must call this with the tc_type
// the record held immediately BEFORE the write and the tc_type it holds
// immediately after, and request the reapply iff this returns true --
// see F2/F4 in the review above: COMMIT_CONFIG (the path the owner's
// commissioning web page actually uses) and APPLY_CONFIG_VOLATILE
// (KILN_PROFILES_PLAN.md item 15's RAM-only install) previously never
// called the reapply at all, so a type change from either path left the
// MAX31856 on its old CR1 byte while max31856_tc_type_verified() stayed
// true -- exactly the "plausible and wrong" state that flag exists to rule
// out.
bool tc_type_reapply_policy_should_reapply(uint8_t prev_tc_type, uint8_t new_tc_type);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_TC_TYPE_REAPPLY_POLICY_H
