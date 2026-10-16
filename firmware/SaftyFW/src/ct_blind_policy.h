// ct_blind_policy.h -- PN-02 (docs/KNOWN_ISSUES.md): pure decision for a CT
// reading that has gone stale on a board whose CTs are commissioned.
//
// A stale current snapshot (ADC wedged, a channel timing out every window)
// used to collapse any_current_present to false, i.e. "no current", which
// silently blinded S3/S8/S6b/S14. When CT sensing is commissioned and the
// snapshot has been stale for longer than CT_BLIND_GRACE_MS, the caller must
// instead treat current as PRESENT (fail safe): S3 then trips when no relay is
// commanded, the same way S5 treats a blind TC. No fabricated amps are made:
// amps_valid stays false.
#ifndef SAFTYFW_CT_BLIND_POLICY_H
#define SAFTYFW_CT_BLIND_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define CT_BLIND_GRACE_MS 10000u

// ct_sensing_active: CT sensing commissioned, not disabled, >=1 channel fitted.
// clock_stalled: the age arithmetic is meaningless -> false (other guards own that).
// timestamp_ms == 0 (never published) is aged from boot: a wedged-from-boot ADC
// is blind after the grace too.
bool ct_blind_treat_as_present(bool ct_sensing_active, bool clock_stalled, bool current_fresh, uint32_t now_ms,
                               uint32_t timestamp_ms);

#endif
