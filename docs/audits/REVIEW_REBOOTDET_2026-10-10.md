# Review: Pico reboot detection across link-down (2026-10-10)

Commits reviewed on origin/dev:

- `646cb6100` keeps the DIAG uptime baseline across link-down (fixes MED-1 in `REVIEW_FIRE2_2026-10-10.md`).
- `dd48f3ca7` marks that MED-1 fixed in the audit doc.
- `672a99fcf` adds a one-line `CHECK` to `firmware/SaftyFW/test/test_link_task_fuzz.c` (sanity check only).

Owner rule: a benign Pico reboot auto-resumes heat. A fatal one does not.

## Verdict

The change is a strict improvement for detecting a same-boot_id reboot. Keeping the baseline only
adds detections: every reboot the old code caught is still caught. Classification is correct on the
new path. `safety_apply_diag()` bumps `pico_reboot_seq`, then caches the new boot's `diag_boot_reason`
and sets `diag_since_reboot = true`, all under the same `state_lock`. The profile executor therefore
sees the reboot and the new boot's cause in one snapshot, and `heat_enable_note_pico_boot()` reaches a
verdict right away. `last_pico_tripped` is not overwritten before that verdict, because
`heat_enable_note_pico_state()` runs after `note_pico_boot()` in the same watchdog tick.

One MED regression: the change makes a double count of a single reboot common, and that double count
erases the T3 "lost trip" fatal latch. Four LOW/INFO items follow.

Tests run: `build_host_tests.ps1 -Only "^(safety_link)( |$)"` passes, 774/774 checks. The executable is
named `safety_link`, so the regex in the task, `^(safety_link_compile)( |$)`, matches nothing. Negtest
results are in the table at the end.

## HIGH

None.

## MED

### MED-1 (FIXED in f8a12608f): DIAG-before-FW_VERSION double-counts one reboot, and the second bump erases the T3 lost-trip latch

Files: `safety_link_frames.c` `safety_apply_fw_version()` (boot_id-changed branch, ~418) and
`safety_apply_diag()` (~1105); `heat_enable.c` `heat_enable_note_pico_boot()` (~753-781).

**Mechanism.** Before this change, a link-down cleared the uptime baseline, so the first DIAG after
relink only seeded it. A reboot that spanned a link-down was counted once, by FW_VERSION. Now the first
DIAG of the new boot is itself a reboot signal (seq +1, `diag_since_reboot = true`). Then the FW_VERSION
carrying the new boot_id arrives and counts the same reboot again (seq +2). It also sets
`diag_since_reboot = false` and clears `pico_uptime_baseline_known`.

**When FW_VERSION arrives after the DIAG.** The Pico pushes its FW_VERSION burst at boot, before its
first DIAG, so the order is normally safe. When the outage is physical (UART noise, a loose cable, a
connector), that boot burst is lost. On relink the ESP re-requests FW_VERSION every poll period
(`!peer_version_known`). The Pico sends a DIAG every 2 s. Which frame lands first is a race. The double
count already existed for reboots fast enough not to drop the link. This change extends it to the
common case: a reboot that spans a link-down.

**Effect on the verdict.** The second bump runs this code:
`reboot_was_tripped = (verdict_pending && was_tripped) || last_pico_tripped` and
`reboot_fatal_latched = false`. If an executor tick ran between the two frames, `last_pico_tripped`
has already been refreshed from the new boot's fresh GRACE/INIT DIAG. The 1 s watchdog period makes
that likely. So the trip snapshot is lost, and the next DIAG of the same boot recomputes
`fatal_latched` from `boot_reason` alone.

**Scenario.**
1. The Pico reports TRIPPED. The executor faults and releases its claim, so `last_pico_tripped = true`.
2. The cable drops, and the Pico resets without a watchdog or brownout cause (for example a power
   glitch with a POWERON boot reason). The RAM trip latch is gone.
3. The link returns. The new boot's DIAG arrives first and the verdict is reached: `fatal_latched = true`
   (T3).
4. The executor tick runs, so `last_pico_tripped = false` (GRACE).
5. The FW_VERSION reply arrives with a new boot_id. That is seq +2, and `fatal_latched` is reset to false.
6. The next DIAG gives POWERON, which is benign, so `fatal_latched` stays false.
7. The operator presses start or resume. The acquire is not withheld. MED-5/T3 wanted one forced
   withhold and pause, because the trip was lost, not cleared.

The same erasure applies with a claim held when the executor pauses on the first verdict before the
second bump. A fatal `boot_reason` (watchdog, brownout and so on) survives, because each re-classification
reads the same boot's reason. Only the T3 arm, a trip lost across a benign-looking reset, is lost. Heat
does not auto-resume in any of these paths, because an operator start or resume is still required. That
is why this is MED, not HIGH.

**Proof.** Two negtest probes, run as appended tests that assert the desired behaviour. Both are CAUGHT,
which means the assertion fails on current dev:

- P1 (`test_safety_link_compile.c`): after link-down, a DIAG with uptime 1500 followed by a FW_VERSION
  with a new boot_id. `pico_reboot_seq == s0 + 1` fails because it is `s0 + 2`, and
  `diag_since_reboot == true` fails.
- P2 (`test_heat_enable.c`): the scenario above, as `note_pico_boot`/`note_pico_state` calls.
  `heat_enable_reboot_hold()` after the acquire fails.

**Fix (pick one; both is better).**
- (a) In `safety_link_frames.c`, do not double count. Keep a per-link flag, `reboot_noted_by_uptime`,
  set when the uptime path calls `safety_note_pico_reboot_locked()` and cleared by the next
  FW_VERSION. When a boot_id change arrives while that flag is set and the current baseline is a
  new-boot uptime (still known, not regressed), record the new id but skip the second
  `safety_note_pico_reboot_locked()`. Keep `diag_since_reboot` and the baseline.
- (b) In `heat_enable.c`, make a newer reboot never downgrade an unconsumed fatal verdict:
  `reboot_was_tripped = ... || last_pico_tripped || reboot_fatal_latched`, and do not clear
  `reboot_fatal_latched` on a new seq. Clear it only when an acquire consumes it. (b) also covers the
  older non-link-down double count and a true fatal-then-benign double reboot.

Add P1 and P2 as permanent tests.

## LOW

### LOW-1 (FIXED in f8a12608f): the long-outage blind spot is not acknowledged, and the uptime test cannot close it

Files: `safety_link.c` ~314 (comment), `safety_link_frame.c` `safety_pico_uptime_regressed()`.

The new comment says that a Pico which kept running "only advances uptime". It does not mention the
converse: a Pico that rebooted and whose new uptime U1 has already passed the old baseline U0 by its
first DIAG after relink reads as "no reboot". A real reboot then goes undetected unless the boot_id
changes. That miss happens about 1 reboot in 256 (random 8-bit id, `link_task.c` ~3439). Even when the
boot burst is lost, the ESP's FW_VERSION re-request on relink still catches a changed id.

**Is it safe?** Not fully, but it is bounded and it is pre-existing. This commit does not widen it.
- **During a firing.** An outage of 30 s or more aborts the run (`SAFETY_LINK_FIRING_ABORT_SILENCE_MS`).
  Auto-resume can only follow an outage under 30 s, so U1 is under about 32 s, and a miss needs
  U0 <= U1. That means the Pico was already less than about 32 s into its previous boot: two reboots
  within about 30 s, plus a 1/256 id repeat. When it happens, nothing withholds heat. `granted` is still
  true, so the F1 K4 reconcile re-requests heat once the new boot reads ARMED with K4 open for 3 s. A
  fatal reboot would resume. In a watchdog boot loop, the earlier reboots in the loop are detected and
  already hold or pause, so the realistic exposure is smaller still.
- **Idle or long outage.** A miss loses the MED-5 `fatal_latched` withhold on the next operator start.
  It needs the same 1/256 repeat.

**Fix.** Compare against the expected uptime, not the last uptime. Store the ESP tick at the baseline
DIAG (`pico_uptime_baseline_esp_ms`) and treat
`(int32_t)(uptime_now - (baseline + esp_elapsed)) < -tol` as a reboot, with
`tol = 2 s + esp_elapsed * 200 ppm` to cover crystal drift and DIAG jitter. A Pico that kept running
must have advanced by about the ESP's elapsed time, so any reboot whose old boot was more than `tol` old
is caught, however long the outage. The modular difference also handles the 32-bit wrap for any outage
under about 24.8 days. Past that, fall back to "unknown, seed". At minimum, reword the comment so it
states this blind spot and its 1/256 x short-previous-boot exposure.

### LOW-2 (FIXED in f8a12608f): reinstated LOW-4 false positive when an outage spans the wrap; the comment overstates the wrap band and says false positives "pause"

Files: `safety_link_frame.h:49` (`SAFETY_PICO_UPTIME_WRAP_BAND_MS = 120000`), `safety_link.c` ~314.

The wrap band only exempts prev in the last 120 s before the wrap and now in the first 120 s after it.
With the baseline now kept across an outage, consider a Pico at about 49.7 days uptime and an outage
that starts more than 120 s before the wrap or ends more than 120 s after it. Example: the cable is out
from uptime 2^32 - 600 s to 2^32 + 300 s. That reads as a reboot. This is exactly what the old
kilnlink LOW-4 clear prevented.

The consequence is fail-safe but not as the comment claims. The verdict comes from the DIAG's
`boot_reason`, which is the reason for the Pico's real, 49-day-old boot.
- If that reason was benign, the false reboot is a no-op. Heat is re-requested from a Pico that never
  rebooted. Side effects: trip dedup reset (a duplicate TRIPPED log), relay-edge resync, a re-announce.
- If that reason was fatal (watchdog), or the last pre-outage DIAG was TRIPPED, the result is a spurious
  hold, pause or withhold.

So "any residual false positive pauses" is wrong in both directions. It may not pause at all, and when
it does, it is a nuisance pause. Heat never resumes unsafely from this path, because the Pico's own
state still gates K4. The expected-uptime fix in LOW-1 removes this false positive too.

### LOW-3 (FIXED in f8a12608f): the new test does not exercise the fatal-versus-benign consumer

File: `test_safety_link_compile.c` `test_low4_link_down_invalidates_uptime_baseline`.

The reason 1 and 2 loop only checks that `diag_boot_reason` is cached. Reason 2 is
`SAFETY_LINK_DIAG_BOOT_WATCHDOG`; reason 1 is POWERON. Nothing checks that the uptime-path snapshot,
seq bumped with `diag_since_reboot == true` and the reason set together, drives
`heat_enable_note_pico_boot()` to hold for 2 and resume for 1. Nothing checks the double-count order
from MED-1 either. I checked the atomicity by reading the code: all three fields are written under one
`state_lock` hold, and `safety_link_get_status()` copies them under the same lock. It holds today, but
nothing pins it.

**Fix.** Add a test asserting that after the uptime-path DIAG, `cached.diag_since_reboot == true` and
`pico_reboot_seq` was bumped in the same `safety_apply_diag()` call. Add the P2-style heat_enable
sequence for both reasons.

## INFO

### INFO-1: no reset-one-side issue introduced

`pico_boot_id_known` is still cleared on link-down while the uptime baseline is now kept. This is
consistent. Since MED-4, the boot_id comparison uses `pico_boot_id`/`pico_boot_id_ever_seen`, which are
never cleared on link-down, so both reboot signals now effectively persist across an outage. Both are
reset together on an ESP reboot, because both live in RAM alongside `heat_enable`'s `seen_reboot_seq`.
The boot_id-change branch still clears the baseline, so a new boot never compares against the old
boot's uptime. The only paired-state defect found is the double count in MED-1, which is a
two-signals-one-event issue, not a one-sided reset.

### INFO-2: the stale comment in `REVIEW_FIRE2_2026-10-10.md` is fine

The FIXED line in that doc is accurate as far as it goes. It should gain a pointer to MED-1 and LOW-1
here.

### INFO-3: `672a99fcf` is sane, with one placement note

The `CHECK(!g_cw_ret, ...)` runs right after `scenario_set_ct_cal()`. It pins the
`g_cw_ret = false` reset at the end of that scenario (line ~1218). No later scenario writes `g_cw_ret`
(grep), so placing it before `scenario_fuzz()` would be equivalent today. It would be slightly more
robust if a future scenario between the two set it.

### INFO-4: the `-Only` regex in the review request does not match

The KilnFW host test is registered as `safety_link` (`build_host_tests.ps1:1648`). Use
`-Only "^(safety_link)( |$)"`.

## Negtest results

Command: `build_host_tests.ps1 -Only "^(safety_link|main)( |$)"` through a wrapper script.
ExpectPattern: `(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES`. P1 and P2 use per-mutation
expectations, `PROBE_DOUBLE` and `PROBE_T3`.

| Mutation | What it does | Verdict | Meaning |
|---|---|---|---|
| M1 | Restores the link-down `pico_uptime_baseline_known = false` clear in `safety_link.c` | CAUGHT (`test_safety_link_compile.c:3168`, `:3193`) | The fixer's rewritten test really pins the fix |
| P1 | Appends a probe: DIAG (uptime 1500) then FW_VERSION (new id) after link-down; asserts seq +1 and `diag_since_reboot` | CAUGHT (`:3213`, `:3214`) | Double count confirmed (MED-1) |
| P2 | Appends a probe in `test_heat_enable.c`: tripped, then seq+1 with verdict, a GRACE state tick, seq+2, benign reason; asserts the acquire is held | CAUGHT (`test_heat_enable.c:1210`) | T3 latch erasure confirmed (MED-1) |

Baseline (unmutated) passed; the real tree was unchanged and the copies were removed (`base_sha` `672a99fcf`).
