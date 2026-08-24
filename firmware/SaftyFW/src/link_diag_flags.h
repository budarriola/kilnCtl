// link_diag_flags.h -- pure assembly of Frame B (SAFETY_CMD_DIAG)'s `flags`
// byte from the individual booleans link_task.c already tracks, split out
// of link_task.c's link_task_send_diag() the same way the other *_policy.h
// files in this directory are split out of their FreeRTOS/pico-sdk-owning
// callers: this file has no pico-sdk/FreeRTOS/hardware includes (it only
// needs CommonFW's kilnlink_diag.h, itself a pure wire-format header), so it
// is directly host-testable (test/test_link_diag_flags.c links this .c file
// alone) even though link_task.c itself cannot be (it needs FreeRTOS.h,
// pico/time.h, etc. -- see that file's own includes).
//
// TODO.md Phase 8, "DIAG's calibration_missing bit is still not wired to the
// real config_store record." Verified 2026-08-24: KILNLINK_DIAG_FLAG_
// CALIBRATION_MISSING already exists in the wire format
// (CommonFW/include/kilnlink/kilnlink_diag.h) and link_task_send_diag() was
// already setting it -- unconditionally, to 1, every boot, regardless of
// config_store's actual calibration_missing record. That is a WIRING fix,
// not a wire-format change: no new bit, no KILNLINK_PROTOCOL_VERSION bump,
// nothing for the PC-link's UART_PROTOCOL_VERSION hard-equality gate to
// react to. This function is the fix, factored out into its own pure,
// testable unit rather than left as an inline expression in link_task.c,
// so the "the bit follows the record" claim has a real test behind it
// instead of only a build-verified assertion.
#ifndef SAFTYFW_LINK_DIAG_FLAGS_H
#define SAFTYFW_LINK_DIAG_FLAGS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Assembles Frame B's `flags` byte (kilnlink_diag_t.flags, KILNLINK_DIAG_
// FLAG_* bits from CommonFW/include/kilnlink/kilnlink_diag.h):
//   bit0 SIM_CONTEXT_SEEN     <- sim_context_seen        (link_task.c's
//                                                          s_context_sim_seen,
//                                                          latched, never
//                                                          cleared -- see
//                                                          that variable's
//                                                          own declaration)
//   bit1 CALIBRATION_MISSING <- calibration_missing      (config_store_is_
//                                                          calibration_missing(),
//                                                          the real record --
//                                                          this is the bit
//                                                          this pass wires)
//   bit2 ESTOP_UNWIRED_SUSPECT is NOT set here -- no detection heuristic is
//   specified in SAFETY_MODEL.md/HARDWARE.md or built anywhere in this
//   codebase yet (TODO.md does not list it as open either); always 0, same
//   as link_task_send_diag() already documented before this change.
uint8_t link_diag_flags_compute(bool calibration_missing, bool sim_context_seen);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_LINK_DIAG_FLAGS_H
