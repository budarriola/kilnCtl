// See link_task_commit_reject.h for why this lives outside link_task.c.
#include "link_task_commit_reject.h"

kilnlink_commit_config_reject_reason_t link_task_commit_config_reject_reason_for(
    config_store_write_decision_t decision, uint8_t persisted_tc_type, uint8_t candidate_tc_type)
{
    switch (decision) {
    case CONFIG_STORE_WRITE_REFUSED_ARMED:
        // 2026-09-15 (Opus adversarial re-review, F2): a MIXED change
        // (tc_type differs AND at least one other field also differs)
        // lands here too -- config_store_write_ex()'s own out_reason
        // already builds the specific sentence for it
        // (config_store_flash.c), but until F2 it never left the Pico's
        // console log because the wire frame collapsed it into the same
        // plain ARMED reason as every other ARMED refusal.
        //
        // 2026-09-15 (defect 1 of the re-review of that fix): the
        // comparison is against the PERSISTED tc_type, which is the exact
        // byte config_store_write_ex() compares (s_persisted_record.tc_type
        // != rec->tc_type) when it decides whether to build that sentence.
        // It is NOT config_store_get_tc_type(), which reads the cached
        // record a volatile install can have moved off flash truth -- see
        // this file's header comment and config_store_flash.c's own
        // s_persisted_record doc comment.
        return (persisted_tc_type != candidate_tc_type) ? KILNLINK_COMMIT_CONFIG_REJECT_ARMED_MIXED
                                                        : KILNLINK_COMMIT_CONFIG_REJECT_ARMED;
    case CONFIG_STORE_WRITE_REFUSED_ARMED_HEAT_ON:
        return KILNLINK_COMMIT_CONFIG_REJECT_ARMED_HEAT_ON;
    case CONFIG_STORE_WRITE_REFUSED_ARMED_HEAT_UNKNOWN:
        return KILNLINK_COMMIT_CONFIG_REJECT_ARMED_HEAT_UNKNOWN;
    case CONFIG_STORE_WRITE_FLASH_FAILURE:
        return KILNLINK_COMMIT_CONFIG_REJECT_STORAGE;
    case CONFIG_STORE_WRITE_OK: // not a refusal -- the caller never gets here
    default:
        return KILNLINK_COMMIT_CONFIG_REJECT_UNKNOWN;
    }
}
