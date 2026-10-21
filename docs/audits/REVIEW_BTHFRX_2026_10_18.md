# REVIEW_BTHFRX_2026_10_18 (HIGH 0, MED 0, LOW 3, INFO 4)

Scope: fixes promoted in main 1342a0f1d: 9997a797f (HF-01, HF-02), 37d6b9674 (BT-01, BT-02), 1cd8eb405 (RX-01, RX-02),
298a4f4c6 (OP-01, OP-02). RAM, OTA and auth surface only. Review only: source read, no builds, no negtest, no bench.
Verdict: all eight fixes do what their rows say. One missed path in the RX-02 class, one label downgrade in BT-02,
one unmeasured stack cost from OP-01. No claim or lock leak found on any error path.

Checked and clean:
- HF-01: `pending = !reboot_hold` keeps the claim queued only for the undecided case; reconcile's retry still requires
  `!reboot_hold && !reboot_classify_pending && !boot_id_unseen`, and a fatal verdict withdraws pending (`granted || pending`
  branch in note_pico_boot). release_pending is still queued first, so the wire order is release then enable.
- BT-01: every early return before `job_try_begin()` holds no claim and no busy flag; the gate's update claim is released
  on the `fetch_busy` refusal and by start_job / fetch_task (`p.claimed`). No fetch-side code re-runs the interlock.
- OP-02: the handler holds the claim until the task runs and releases it itself only on xTaskCreate failure; the
  task releases once, after the flash_xip exit (agrees with RX-09).
- RX-01: text only. RX-02: the fence runs before body read and writer claim in all seven safety_cfg writers; import
  releases the guard on both outcomes of `safety_cfg_write_apply_pairs`.

### BX-01 LOW: backup import's Pico ceiling raise and lower still run outside the writer guard
`backup_import.c:2430` `safety_ceiling_sync_guard_raise()` and `:3161` `safety_ceiling_sync_apply_lower()` stage and
COMMIT_CONFIG `abs_max_temp_c` without `safety_cfg_writer_try_claim`. zones_http_post.c:279 wraps the same raise in the
guard; RX-02 wrapped only the i_normal_a push. A bench_preset async job or a current sweep that started before the
restore (the new fence only refuses new POSTs) still holds the guard and can have its half-staged set committed by the
import's ceiling commit, or vice versa. Same class as RX-02. Fix: claim SAFETY_CFG_WRITER_HTTP_SYNC around both calls
(refuse the import on a busy guard before the raise, so nothing is committed).

### BX-02 LOW: a later benign reboot relabels a standing fatal hold as pico_repeated_reboot
`heat_enable.c:914` assigns `hold_repeated_benign` on every pass through the hold path, even when `reboot_hold` is
already true. Sequence benign (seeds the window), fatal (hold, pico_fatal_reboot), benign inside the window: the hold
stays set (still fail-closed) but the getter now reports repeated-benign, so the executor, LCD and web show
`pico_repeated_reboot` and the fatal cause disappears from the operator view. Same for `hold_lost_trip_only = false`.
Fix: when `reboot_hold` was already set, only OR in stronger causes (`hold_repeated_benign &= ...`), never downgrade; add
a host test for the sequence.

### BX-03 LOW: OP-01 puts esp_image_verify on the 4096 B update_fetch_wr stack, unmeasured
WR_FINISH runs `update_stage_upload_finish` (now calling `io_image_verify`: `esp_image_metadata_t` on the stack plus
esp_image_verify's own frames and SHA context) in `update_fetch_wr`, `FETCH_WR_STACK_BYTES 4096`, whose own comment says
the high-water mark is still owed on the bench. The manual path runs it on the httpd stack. RX-06 covers time, not
stack. Fix: in the RX-06 bench run, read the stack_margin "update_fetch_wr" high-water after a real staged download and
bump the stack if margin is under 1 KB.

### BX-04 INFO: the BT-01 lost-race branch is not reachable and not tested
Check and download handlers both run on the single httpd task, and no other code sets `busy`, so the
`job_try_begin()` failure after the gate (with its `ota_http_update_end()`) cannot happen today. A mutant deleting that
release survives the host tests. Harmless; keep it as defence.

### BX-05 INFO: stale comment in the rollback handler
`ota_http_esp.c` ~line 834 still says the claim is "intentionally left held across the reboot ... nothing left to
release", which OP-02 now contradicts on the failure path.

### BX-06 INFO: HF-02 and BT-02 executor wiring are source scans
Both assert a string in profile_executor.c, not behaviour. A mutant that reorders the BT-02 `else if` after the fatal
branch is still caught (the string scan does not check order, but the getter is exclusive with lost-trip only). A
mutant that pauses with the right reason but skips `autotune_engine_abort_bounded("pico_repeated_reboot")` survives:
the HF-02 scan pins the unseen abort only.

### BX-07 INFO: OP-01 app hook stays fail-open on NULL
`update_stage.c:508` skips the verify when `io.image_verify == NULL`, while recovery makes `stage_verify` mandatory.
Not reachable today (RX-08); removing `.image_verify = io_image_verify` in update_http.c survives every host test.
