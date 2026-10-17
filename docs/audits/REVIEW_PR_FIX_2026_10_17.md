# REVIEW_PR_FIX_2026_10_17

This review covers the PR fix 6c326e5f3 and its docs commit 3b1e4d675 on origin/dev. Those commits fix PR-01, PR-02, PR-04 and PR-06 from REVIEW_PF_FIX_2026_10_17.

Counts: HIGH 1 (fixed, e7c7df742), MED 0, LOW 1, INFO 1.

## PQ-01 HIGH (fixed): the PR-01 first-sight rule rarely ran
- PR-01 ran only on the `boot_id_unseen` edge from true to false, in `heat_enable_note_boot_id_seen`.
- `heat_enable_init` sets `boot_id_unseen = false`.
- The guard task (`watchdog_task_entry`) calls `vTaskDelay(2000 ms)` before its first tick.
- `safety_poll_task` sets `pico_boot_id_ever_seen` from the first V3 DIAG or FW_VERSION frame. That frame normally arrives well inside those 2 s.
- So the first tick usually reported "seen" already, the edge never happened, and a Pico WATCHDOG/ASSERT boot reason after an ESP reboot went unlatched again. This is the same failure PR-01 was meant to fix.
- The 6c326e5f3 tests passed because they always call `note_boot_id_seen(false)` first.
- Fix: a per-ESP-boot flag, `boot_id_first_sight_done`. The rule now runs on the first `seen == true` call, whatever came before it.
- New test: the first tick already sees the id, with seq 0 and a WATCHDOG boot reason. It checks that the fatal latch is set and heat is withheld.
- Checks: the full KilnFW host suite passed (104/104) and build_kilnfw passed. In negtest, a mutant that restores the edge-only trigger was CAUGHT: it compiled and the PQ-01 assertions failed.

## PQ-02 LOW: a claim taken before the first tick is not re-armed
- The first-sight block sets `reboot_verdict_pending` but not `reboot_classify_pending`.
- A claim taken before the first tick, while `boot_id_unseen` is still false, is re-armed only on its next acquire.
- No claim path inside the first 2 s of boot was found.
- Defaulting `boot_id_unseen` to true (PR-03) would close this.

## PQ-03 INFO: PR-02 and PR-04 verified
- PR-02: the latch expires only if the bits cleared on the new boot cover `latch_lost_bits`. That value must be nonzero, and no claim may have been involved. Unknown or missing trip bits mean the latch never expires, so the check fails closed.
- Trip info is fed only from a DIAG of the current boot (`diag_since_reboot`), so an older DIAG cannot clear the latch.
- PR-04: heat was already withheld while the boot id is unseen. The fix only adds the `pico_boot_id_unseen` pause reason, so it fails closed.

Verdict: promote OK once e7c7df742 is on dev.
