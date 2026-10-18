# Review: HH-01/02/03 fixes (00d7eec68), 2026-10-18

Scope: `heat_enable.c` and `profile_executor.c` changes in 00d7eec68, plus `test_hh_fixes` in `test_heat_enable.c`.
Background: `REVIEW_HEATENABLE_HOLISTIC_2026_10_18.md`. Review only, nothing built or run.

Counts: HIGH 0, MED 1, LOW 1, INFO 3.

## Checks

- **HH-02:** no request storm. Reconcile retries only while `pending && !granted` with no hold, classify_pending or
  unseen, and one good send sets `granted`. No lost release: `he_release_common` owes a release on
  `granted || pending`, and the fatal branch does the same, so moving the grant into `pending` keeps the release.
  No stuck pending after release: the last release clears `pending`. After a benign verdict the next reconcile sends
  enable once. The exception is HF-01.
- **HH-01:** no permanent hold. The claimless branch only seeds `last_benign_reboot_ms`. `reboot_hold` is still set
  only in the claim branch and clears on the last release. A single benign reboot outside the window auto-resumes.
  The `since_ms > 1` guard keeps the first-sight sentinel out (AP-01).
- **HH-03:** `autotune_engine_abort_bounded` returns early when autotune is not running, so calling it every tick
  while unseen does not change state and logs one abort. See HF-03 and HF-02.

## Findings

### HF-01 MED: an enable that lands during the undecided window drops `pending`, so a benign verdict never re-requests heat
`heat_enable.c:366-373` (send_enable). An enable can be in flight when the reboot is noted. For example, the
new Pico boot acks a retry, or an F1 resend is in flight while HH-02 moves `granted` to `pending`. If it lands
while `reboot_classify_pending` is set, it sets `pending = false` and queues a release. The benign DIAG then
leaves `granted = false, pending = false` with the claim still held. Reconcile does not retry, and the executor
shows no pause reason: no hold, not unseen, and `k4_unconfirmed` needs a grant. The firing runs cold with no
message. That breaks the owner rule that a benign reboot auto-resumes. It is fail safe because no heat flows. The
bug predates this commit (SL3-R2 A3), but HH-02's "reconcile re-requests once" holds only if no send is in
flight. Fix: on a landing under classify_pending without `reboot_hold`, keep `pending = true`. The fatal branch
already clears it. Untested.

### HF-02 LOW: the HH-03 abort has no test
The source-scan test at `test_heat_enable.c:1574-1580` looks for `autotune_engine_abort_bounded` with
`pico_fatal_reboot` and `heat_grant_unconfirmed` only. Deleting the new `pico_boot_id_unseen` abort does not fail
any test.

### HF-03 INFO: the per-tick abort while unseen is benign
When autotune is not running, the call takes `s_at.lock` and returns. If the start path holds the lock, the
watchdog waits up to 100 ms and logs "lock busy" each tick, the same as the other two branches.

### HF-04 INFO: idle reboots now seed the window by design
Under HH-01, a Pico-only benign reboot while idle (no first sight) followed by a firing and a benign reboot within
15 min holds heat. That matches the owner's rule that a repeated reboot holds. An ESP boot resets the window.

### HF-05 INFO: the latch-plus-re-arm test is real but has no mutant
In the HH-07 block, each assertion maps to a distinct mutant: no latch consumption at acquire, no re-arm, and a
benign claim DIAG that clears `reboot_hold`. The HH-01 first-sight case also separates the `> 1u` guard, because
without it 120000 - 1 falls inside the window and would hold. No negtest has been run on any `test_hh_fixes` check,
so the "caught" result is not verified.
