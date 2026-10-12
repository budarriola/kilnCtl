# Review SAFTYFX5 (2026-10-12): S8 post-window TEST-clear fixes

Scope: `6a51f70c4` on origin/dev, which closes `REVIEW_SAFTYFX4_2026-10-12.md`. The code
is in the preceding commit, `1cbd50884`:

- `firmware/SaftyFW/src/safety_guards.c`: comment-only changes.
- `firmware/SaftyFW/test/test_safety_guards.c`: a new test, `test_test_clear_s8_post_window_checks` (LOW-1, LOW-2).
- `firmware/KilnFW/App/test/test_safety_link_compile.c`: the K2 `0x42` assert.
- `docs/audits/REVIEW_SAFTYFX3_2026-10-11.md`: the LOW-3 correction.

Reviewer worktree base: `893859644`.

## Verdict

There are no HIGH or MED findings. Every gap found lets a TEST clear go through while a
rise is still in progress, but each one costs at most about one S8 window of latency
before S8 trips again. The S1 abs_max limit stays in force throughout, and every TEST trip
opens K4 first. The findings are LOW: one undocumented gap, two imprecise comment claims,
and three test gaps.

## Checks requested

1. **Comment-only.** Confirmed. The file was stripped of comments and whitespace at
   `9ba887351` and at `1cbd50884`, and the two code streams are identical.
2. **The documented gaps.** Both are real, but the wording is imprecise (LOW-2, LOW-3), and a third gap is missing (LOW-1).
3. **Can a TEST clear succeed while S8's condition persists?** Yes, in three ways (LOW-1, LOW-2, LOW-4). The safety impact is LOW (see below).
4. **Does the test pin the rollover boundary and partial versus completed windows?**
   - Partial versus completed windows, for the sign: yes. F8 and F9 are CAUGHT.
   - The rollover reset: yes. F11 is CAUGHT.
   - Reading the wrong window: yes. F7 is CAUGHT.
   - The window clamp: yes. F12 to F15 are CAUGHT.
   - The rollover boundary itself: no, it is pinned only to within one tick (F1, F2 MISSED, negligible).
   - The 5 s partial floor and the partial divisor: no (LOW-5).
5. **Extra mutations.** 17 were run. 10 were CAUGHT and 7 MISSED, and one of the MISSED is an equivalent mutant. See the appendix.
6. **Full KilnFW host suite.** `EXIT=0`, "all 98 host test executables built and passed".

### Why the safety impact is LOW

- A granted TEST clear runs `safety_guards_clear()`, so S8 restarts from zero. S8 still
  needs two consecutive over-rate windows to trip. The worst added latency is therefore
  about one window compared with no clear at all.
- S1 abs_max is independent of S8 and is never bypassed.
- TEST_TRIP is admin-only, limited to one every 10 s
  (`LINK_FRAME_TEST_TRIP_MIN_INTERVAL_MS`), and opens K4 every time. A trip-and-clear
  cycle repeated by an operator could keep S8 from ever building up a streak, but every
  cycle drops heat.
- No gap exists in the first `trip_verify_s` (default 10 s) after the latch. The S9 verify
  hold refuses the clear there. A probe confirmed that every rise in the first 4 s after
  the latch is refused.

## Findings

### LOW-1: an undocumented gap, partial-window dilution

In the non-strict branch, the partial-window check is
`(tc - s8_post_start_c) / (elapsed/60) > max`. A rise that starts partway through a post
window is averaged over the whole elapsed time, including the flat part before the rise.
So a clear is granted while a rise is still in progress, as long as the rise so far is no
more than `max_rate × elapsed`.

The probes ran against the real `safety_guards.c`:

| W | max (C/min) | Rise | Clear at | Result |
|---|---|---|---|---|
| 60 | 15 | 30 C/min (2x), starting at 30 s | 55 s | GRANTED |
| 600 | 15 | 30 C/min (2x), starting at 300 s | 550 s | GRANTED |

The rise was still going when the clear was granted. The comment in `s8_clear_refuse()`
lists gaps (1) and (2) but not this one. The impact is the same as LOW-2 (see Why the
safety impact is LOW). Fix: document it. If the gap should be closed, also check the rise
over the last `S8_POST_MIN_S`-sized sub-window.

### LOW-2: gap 2 is accurate, but it covers a rise of any rate

The comment says a rise in the first `S8_POST_MIN_S` after a rollover is seen only by the
whole-hold average. That is true. The comment does not say that this applies at any rate
and while the rise is still in progress. In the probes, a rise starting exactly at the
rollover was granted 2-3 s later, at 30 C/min (2x) and at 150 C/min (10x).

Fix: reword the comment to "any rate, in progress, up to S8_POST_MIN_S after each
rollover". The streak-1 rule and the whole-hold average still bound the total rise.

### LOW-3: gap 1's "up to 2x max_rate" bound is imprecise

The non-strict branch checks only the last completed window and the partial window. So a
burst that straddles a boundary is forgotten once it has passed. The real bound is a rise
per window of at most `max_rate × W` at any burst rate, not "2x". The clear is granted
only after the burst ends, and the same clear is refused while the partial window still
sees the burst.

| Burst | Clear at | Result |
|---|---|---|
| 29 C/min, 30-90 s | 125 s | GRANTED |
| Same burst | 95 s | REFUSED |
| 4x, 30 s, straddling (59 and 61 C/min windows) | after the burst | GRANTED |

This matches S8's own streak-2 resolution and is harmless. It is a wording problem only.

### LOW-4: the post-window clamp is coarser than S8 when rate_window_s < 60

The post window is clamped to 60-600 s, but `rate_window_s` is a U16 with no bound
(SAFTYFX3's pooled item is still NOT DONE). With `rate_window_s=10`, the post window
becomes 60 s.

In a probe, a 2x rise in progress for 25 s was GRANTED. S8 itself, with 10 s windows,
would have tripped after about 20 s. That contradicts the comment's claim that the check
works at "the same resolution as S8".

This is config-dependent. Fix: qualify the claim in the comment, or bound
`rate_window_s >= 60` (the open SAFTYFX3 item).

### LOW-5: test gaps in the partial-window refusal

The new test refuses the partial window only with large rises, so these mutations survive:

- **F4, F5:** the partial floor `S8_POST_MIN_S` raised to 10 s and to 30 s. Both MISSED.
- **F10:** the partial check divides by the full window length (`s8_post_win_s`) instead
  of the elapsed time. MISSED. The SAFTYFX4 R5 divisor mutant (+60) was caught, but this
  one is not.

Fix: add these cases.

- A rise that is just over the limit and only partial-window-visible, cleared at
  `elapsed = 6 s` after a rollover. This catches F4 and F5.
- A rise that is over the limit on elapsed time but under it on the window length. This catches F10.

### INFO

- **F1, F2:** the rollover at `>` or 1 s early. A one-tick boundary, negligible.
- **F3:** the partial floor `>` instead of `>=`. Exact equality only, negligible.
- **F6:** the completed-window check `>=` at equality. Negligible.
- **F16:** `windows_done >= 0u` is an equivalent mutant, because `s8_post_last_rate_c_per_min` is 0 before the first window.
- **K2 assert:** `s_stub_broadcast_last_payload[2] == 0x42u` is correct. The scripted RNG seed is `0x40`, so the first id is `0x41` and the second is `0x42`.
- **SAFTYFX3 LOW-3 correction:** verified.
  - The row credits `57c4e0454`.
  - The find string `static bool s_test_trip_request_id_seeded;` exists at `safety_link_commands.c:1015`.
  - The `:3197` citation lands on the LOCAL_STALE assert in `test_safety_link_compile.c`.

## Fix status

| Finding | Status |
|---|---|
| LOW-1 | OWNER Q (comment only). Gap documented as (3) in the `s8_clear_refuse()` comment. Behavior unchanged; close it (check the rise over the last `S8_POST_MIN_S`-sized sub-window) only on an owner decision. |
| LOW-2 | OWNER Q (comment only). Gap (2) now says "in progress, any rate, first `S8_POST_MIN_S` after each rollover". Behavior unchanged. |
| LOW-3 | FIXED (comment only). Gap (1) now states the real bound: at most max_rate x W per window at any burst rate, clear granted only after the burst. |
| LOW-4 | OWNER Q (comment only). The false "same resolution as S8" claim is removed; gap (4) says the check is coarser than S8 below a 60 s `rate_window_s`. Bounding `rate_window_s >= 60` is the open SAFTYFX3 item. |
| LOW-5 | FIXED. `test_test_clear_s8_partial_floor_and_divisor` (test_safety_guards.c): 4.5 s after a rollover a 1.5 C rise is granted, 5.5 s refuses, 10 s with a 2 C rise refuses (elapsed divisor). F4, F5, F10 all CAUGHT by assertion (:5169; F4/F5 also :4950, :3534), `-Preset saftyfw-host`, base 5870895e4, `real_tree_unchanged: true`. |

safety_guards.c: comment-only (diff with comments and whitespace stripped is empty).

## Appendix A: mutation run

Command: `tools\negtest.ps1 -Preset saftyfw-host -Mutations mut.json -ExpectPattern "FAIL .*\.c:\d+|SAFTYFW HOST TESTS: FAILED"`.

The baseline passed. `real_tree_unchanged=true` and `copies_removed=true`. Every CAUGHT
mutant compiled and failed a test assertion. None failed to build.

| Id | Mutation in `safety_guards.c` | Verdict | First failing line in `test_safety_guards.c` |
|---|---|---|---|
| F1 | rollover `>= win` changed to `> win` | MISSED | |
| F2 | rollover `>= win - 1` | MISSED | |
| F3 | partial floor `>` instead of `>=` | MISSED | |
| F4 | partial floor 10 s | MISSED | |
| F5 | partial floor 30 s | MISSED | |
| F6 | completed check `>=` | MISSED | |
| F7 | completed check reads the previous window rate | CAUGHT | :5083 |
| F8 | partial rate sign flipped | CAUGHT | :5028, :5125 |
| F9 | completed rate sign flipped in the tick | CAUGHT | :5074 |
| F10 | partial divides by `s8_post_win_s` | MISSED | |
| F11 | `s8_post_start_c` not reset at rollover | CAUGHT | :5037, :5087 |
| F12 | `S8_POST_WIN_MAX_S` 300 | CAUGHT | :5074 |
| F13 | `S8_POST_WIN_MAX_S` 1200 | CAUGHT | :3576 |
| F14 | `S8_POST_WIN_MIN_S` 30 | CAUGHT | :5169 |
| F15 | `S8_POST_WIN_MIN_S` 120 | CAUGHT | :5169 |
| F16 | `windows_done >= 0u` | MISSED (equivalent mutant) | |
| F17 | completed rate divided by 6 (10x) | CAUGHT | :5074 |

## Appendix B: probe method

A standalone harness compiled the real `safety_guards.c` and drove
`safety_guards_tick()` at `dt_s=0.1` with `link_up=true` and
`sample_counter_advancing=true`. It latched a TEST trip after a flat pre-window, then
scripted the TC trace and called the TEST clear at the stated times. A flat control trace
was granted, which confirms the harness itself did not cause the refusals.
