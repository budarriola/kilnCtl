// link_task_announce_eval.h -- the pure evaluation of a decoded
// ANNOUNCE_VERSION frame against this build's own protocol/min_compatible,
// factored out of link_task_handle_announce_version() (link_task.c) for the
// same reason link_task_commit_reject.c was: link_task.c pulls in FreeRTOS
// and the pico-sdk and is not built for the host test executable, so this
// decision -- whether the ESP peer just announced is one this Pico build
// will accept -- could not be exercised at all. This file touches no lock,
// no hardware and no global state.
//
// UPDATE_PROTOCOL.md / LINK_PROTOCOL.md: `min_compatible` is the oldest
// peer protocol version this build still speaks to. A peer is compatible
// only if BOTH directions of the inequality hold:
//   peer.protocol >= self.min_compatible  AND  self.protocol >= peer.min_compatible
// link_frame_versions_compatible() already implements and host-tests that
// formula's full combination matrix; this file's job is narrower and was
// the actual untested gap: taking a *decoded* announce message and
// producing the real verdict link_task.c latches into
// s_degraded_no_context, using this build's real KILNLINK_PROTOCOL_VERSION/
// KILNLINK_MIN_COMPATIBLE by way of caller-supplied values so the test can
// pin both a refused-too-old-peer case and an accepted-compatible-peer case
// against the exact function link_task.c calls.
#ifndef SAFTYFW_TASKS_LINK_TASK_ANNOUNCE_EVAL_H
#define SAFTYFW_TASKS_LINK_TASK_ANNOUNCE_EVAL_H

#include <stdbool.h>
#include <stdint.h>

#include "kilnlink/kilnlink_announce.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // msg->protocol_version, cached unconditionally regardless of the
    // compatibility verdict below -- see link_task_handle_announce_version()'s
    // own comment on why an incompatible peer's protocol_version is still
    // real, meaningful data that link_task_send_status() needs.
    uint16_t peer_protocol_version;
    // The value link_task.c latches into s_degraded_no_context: true when
    // the peer is NOT compatible with this build (LINK_PROTOCOL.md section
    // 4 -- DEGRADED_NO_CONTEXT, no trip latched).
    bool degraded_no_context;
} link_task_announce_eval_t;

// Evaluates an already-decoded ANNOUNCE_VERSION payload against this
// build's own protocol_version/min_compatible. Pure function: no I/O, no
// global state, safe to call from a host test with no FreeRTOS/pico-sdk
// present.
link_task_announce_eval_t link_task_evaluate_announce_version(const kilnlink_announce_t *msg,
                                                                uint16_t self_protocol_version,
                                                                uint16_t self_min_compatible);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_LINK_TASK_ANNOUNCE_EVAL_H
