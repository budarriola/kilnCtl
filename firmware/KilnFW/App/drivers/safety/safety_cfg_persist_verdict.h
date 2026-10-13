#ifndef KILNCTL_SAFETY_CFG_PERSIST_VERDICT_H
#define KILNCTL_SAFETY_CFG_PERSIST_VERDICT_H

/* Safety link review 2026-10-09, F3. After a PERSISTENT COMMIT_CONFIG the ESP
 * reads the Pico's config back and compares values -- but the Pico serves its
 * RAM record, which includes an unpersisted volatile install. A commit the
 * Pico refused (it refuses persistent writes while ARMED) leaves that RAM
 * record alone, so a read-back that matches an earlier volatile install says
 * nothing about flash. The Pico already reports the missing fact: DIAG flags
 * bit 7 (SAFETY_LINK_DIAG_FLAG_CONFIG_VOLATILE_DIRTY) is set while RAM differs
 * from the persisted record. A persistent commit is only "landed" when that
 * bit reads clear. Header-only so a host test can pin the rule. */

#include <stdbool.h>
#include <stdint.h>

#include "safety_link.h"

typedef enum {
    SAFETY_CFG_PERSIST_VERDICT_PERSISTED = 0,  /* DIAG seen, volatile-dirty clear */
    SAFETY_CFG_PERSIST_VERDICT_STILL_DIRTY,    /* RAM still differs from flash */
    SAFETY_CFG_PERSIST_VERDICT_UNKNOWN         /* no DIAG ever received: cannot tell */
} safety_cfg_persist_verdict_t;

static inline safety_cfg_persist_verdict_t safety_cfg_persist_verdict(bool diag_ever_received,
                                                                      uint8_t diag_flags)
{
    if (!diag_ever_received) {
        return SAFETY_CFG_PERSIST_VERDICT_UNKNOWN;
    }
    return (diag_flags & SAFETY_LINK_DIAG_FLAG_CONFIG_VOLATILE_DIRTY) != 0u
               ? SAFETY_CFG_PERSIST_VERDICT_STILL_DIRTY
               : SAFETY_CFG_PERSIST_VERDICT_PERSISTED;
}

#endif
