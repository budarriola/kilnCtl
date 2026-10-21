# REVIEW_AFPZ_2026_10_18

Scope: 5dcca2a73 (AF-01..03, F2-02 self-heal gated on a configured admin) and 0ff3edffc (PZ-01/PZ-02, heat_enable).
Review only: source read, no builds, no negtest runs, no bench.
Counts: HIGH 0, MED 0, LOW 1, INFO 4. Verdict: both fixes sound.

## 5dcca2a73: FS-02 fail closed
- Latched reset (sentinel missing, mirror or auto-erase evidence) now heals only when
  `web_auth_store_password_configured(ADMIN)` is true (load OK and `configured`). Gesture blob (configured=false),
  stale CRC, and wrong size all stay latched. While latched, `load_policy` returns UNREADABLE (dashboards only) and
  readiness/LCD banner key on `web_auth_store_storage_reset_latched()`.
- No permanent latch: the gesture writes an unconfigured admin record, so `web_auth_store_admin_configured_or_unreadable()`
  is false and bootstrap opens; the admin password setters (web_auth_store.c ~680, ~783) call `storage_reset_resolve()`
  when latched. A corrupt blob keeps bootstrap closed (UNREADABLE counts as configured), and the gesture is the way out.
  Same as before.

## 0ff3edffc: PZ-01/PZ-02
- No grant before DIAG: the first sight with `held_mask != 0` sets `reboot_classify_pending`, and reconcile's grant
  condition (heat_enable.c ~1147) needs `!reboot_classify_pending`. In production, `note_boot_id_seen` runs before
  `note_pico_boot` on the same tick (profile_executor.c 2629-2630). If the DIAG is already in, that same call classifies it.
- No deadlock: a benign DIAG (`diag_since_reboot`, no fatal bits) clears pending and the next reconcile sends enable.
  The test asserts exactly one send.
- Hook: `git grep KILNCTL_HEAT_ENABLE_TEST_HOOK` matches only heat_enable.c/.h and build_host_tests.ps1 (`/D`). No CMake,
  sdkconfig or preset defines it. Callers are test_heat_enable.c and test_autotune_engine_prestart.c (host only).

## Findings
- **AP-01** LOW, `heat_enable.c` ~885 (classify branch) with first sight ~963. When first sight re-arms classify, `reboot_classify_since_ms`
  is 1 (not now). The benign DIAG then records `last_benign_reboot_ms = 1`. So a real benign Pico reboot within
  `HE_BENIGN_REBOOT_WINDOW_MS` (15 min) of ESP boot reads as "repeated benign" (SR-11) and sets `reboot_hold`. That is a
  false hold against the owner's benign auto-resume rule (2026-10-10). It fails closed. The acquire re-arm (~517) already
  had this; PZ-02 adds the held-claim path. Suggest: do not count the first-sight classification toward SR-11 (or stamp
  now_ms and skip the benign record when only first sight armed it).
- **AP-02** INFO. The AF tests are real by reading: the old `!= HAL_NOT_FOUND` heal would clear all three planted blobs
  (k=0 gesture, k=1 stale CRC, k=2 short), and the F2-02 positive case now sets `configured=true`. Not re-negtested here.
- **AP-03** INFO. The PZ-02 test asserts reconcile does not send before the DIAG; pre-fix code would grant there (pending false).
  Not re-negtested here.
- **AP-04** INFO. First sight now sets `reboot_had_claim`, so a later lost-trip latch from this episode is not expirable
  (PR-02). This is conservative and intended.
- **AP-05** INFO. Hook guard verified absent from target builds (grep above).
