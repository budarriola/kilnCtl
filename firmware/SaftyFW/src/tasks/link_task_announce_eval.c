// See link_task_announce_eval.h for why this lives outside link_task.c.
#include "link_task_announce_eval.h"

#include "link_frame.h"

link_task_announce_eval_t link_task_evaluate_announce_version(const kilnlink_announce_t *msg,
                                                                uint16_t self_protocol_version,
                                                                uint16_t self_min_compatible)
{
    link_task_announce_eval_t out;

    uint16_t peer_protocol = msg->protocol_version;
    uint16_t peer_min_compatible = msg->min_compatible;

    out.peer_protocol_version = peer_protocol;

    bool compatible = link_frame_versions_compatible(self_protocol_version, self_min_compatible,
                                                       peer_protocol, peer_min_compatible);

    out.degraded_no_context = !compatible;

    return out;
}
