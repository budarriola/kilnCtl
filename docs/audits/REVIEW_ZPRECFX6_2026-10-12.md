# Review ZPRECFX6: fixes for REVIEW_ZPRECFX5, 2026-10-12

Reviewer: opus (rvzprecfx6). Subject: the two zprecfx6 commits on origin/dev,
`419e68634` and `10cd6197c` (the rebased forms of `c23d57040` and
`9a7e7ccb5`), which answer `docs/audits/REVIEW_ZPRECFX5_2026-10-12.md`.
Reviewed in worktree `C:\wt\rvzprecfx6_4sr6vv` at origin/dev `bb90c2758`,
which contains both commits. No SaftyFW file is touched.

**Verdict: 0 HIGH, 0 MED, 3 LOW, 3 INFO.**

- LOW-2 and LOW-3 are properly fixed, and every mutant below is CAUGHT.
- INFO-1 and INFO-2 are properly fixed.
- The LOW-1 "RESOLVED AS UNREACHABLE" verdict is wrong. The truncation clamp
  is reachable through the sync seam when one side holds +0.0 and the other
  holds -0.0. That case also exposes a pre-existing bug in its own right
  (LOW-A).
- The INFO-4 LCD change fixes the buffer, but the operator still never sees
  the ARMED hint on the LCD (LOW-B).

## 1. LOW-1: is the head_cap clamp reachable through the sync seam?

### 1.1 The fixer's arithmetic

The ARMED path appends its suffix only when `non_ceiling_diverged`. That
requires the ceiling field (field 0) to be **known on both sides and equal**:

- if `target_known` is false, the function returns early (`safety_ceiling_sync.c:402`);
- an unknown or different field 0 sets `ceiling_diverged`.

Field 0 therefore never names the culprit. The reason comes from one of
`config_divergence_check()`'s four producers, run over field 0 plus the extra
fields:

| Producer | Text | Reachable on the ARMED path? |
|---|---|---|
| `format_first_field_difference` | `config divergence: <name> ESP=<v> Pico=<v>` | yes |
| format_version mismatch (`config_divergence.c:182`) | ~150 chars | **no, anywhere**: both identities are computed with the same `CONFIG_IDENTITY_FORMAT_VERSION` (`:140-141`). REVIEW_ZPRECFX5 LOW-1 was wrong to call this reachable. |
| unknown-names (`:187`) | ~122 chars | no: this branch is reached only when every field matches or is unknown on both sides. ESP extras are always known, so a Pico-unknown extra is reported by the first-diff producer as `Pico=unconfirmed`. Field 0 unknown on both sides implies `!target_known`, which returns early. |
| hash mismatch with equal fields (`:229`) | **133 chars** | **yes**, see 1.2 |

The worst case for the first-diff text is 19 (`config divergence: `), plus at
most 23 for the name (`extra_names[...][24]`), plus 5 (` ESP=`), plus 15, plus
6 (` Pico=`), plus 15. That is **83**, not 82. The fixer counted the ESP side
at 15 and the Pico side at 14, but either side can be negative.

The widest `%.9g` output for a float32 is 15 characters, for example
`-3.40282347e+38`, `-1.17549421e-38` or `-1.40129846e-45`. The exponent never
reaches three digits. Other values are shorter:

- `nan`/`-inf`: 3-4 characters (MSVC's `-nan(ind)` is 9);
- `unconfirmed`: 11;
- a widened u16: at most 5.

So 83 is less than `head_cap` 91, and the first-diff producer cannot reach the
clamp. The comment's "82" is off by one but its conclusion holds for that
producer.

### 1.2 The producer the fixer missed: +0.0 against -0.0

`config_identity_normalize_f32()` keeps the sign of zero: `%.9g` gives `-0`,
and `strtof` turns that back into -0.0. The two producers then disagree:

- `config_identity_compute()` hashes the normalized float's **bytes**, so +0.0
  and -0.0 hash differently and `config_identity_matches()` returns false;
- `format_first_field_difference()` compares with `ne != np`, and
  `0.0f == -0.0f`, so it finds no differing field.

The call therefore falls through to the hash-collision text, which is 133
chars, more than 91. The clamp fires.

I confirmed this on the host. A temporary probe in
`test_safety_ceiling_sync_divergence.c` used the existing seam:

- expected extra field `0.0f`, Pico live extra `-0.0f`;
- ceiling agrees at 80;
- `s_stub_recent_armed_refusal = true`.

It printed:

```
PROBE warned=1 len=159 reason=[config divergence: identity hash mismatch with matching version/known-ness (hash 0xEA825558 (re-push refused: relay is ARMED -- disarm and retry commissioning)]
```

The head is cut after 91 chars, in the middle of the hash pair, and the suffix
follows it in full.

The length is 159 = 91 + 68, so the clamp was taken. I then removed the probe
by hand and confirmed `git status` was clean.

So the clamp is **reachable, not defensive-only**, and since `10cd6197c` no
test pins it:

- the `drop_head_clamp` mutant is MISSED (table below);
- `head_cap_minus_40` is MISSED too: the boundary arithmetic itself is unpinned.

The test is still named `..._truncates_long_reason`, its section header still
says the reason "must be truncated", and its last assertion message says
"even though the reason had to be truncated". None of that is true of the
input it now uses.

**Severity: LOW.** The clamp itself is correct (it was reviewed and
negative-tested in item H, and the probe shows a well-formed result). What is
wrong is the claim that it is unreachable, and with it the loss of its only
pin.

**Fix (pick one):**

- **(a)** Pin it through the seam with the ±0 input above. Assert
  `strlen(reason) == 159` and that the suffix is intact.
- **(b)** Preferred, together with LOW-A: canonicalize ±0, after which the
  clamp really is unreachable. Then extract the "truncate head, append suffix"
  step into a small static helper (for example
  `append_suffix_truncating_head(dst, cap, head, suffix)`) and unit-test it
  directly with a 159-char head. A direct test is the right coverage for code
  that is defensive by construction. A formatter-level test of
  `config_divergence_check` alone would not cover the clamp, which lives in
  `safety_ceiling_sync.c`.

## 2. LOW-A (new, pre-existing): a ±0 field produces a false "possible hash collision" standing divergence that never clears

This is the same mechanism as 1.2, judged on its own consequences. It needs
an F32 extra param whose kiln-config package holds -0.0 while the Pico holds
+0.0, or the reverse. For example, an operator types `-0` and later `0` for
an offset param on the commissioning page: `strtof` keeps the sign, and the
Pico stores the raw bits.

The values are numerically equal, yet:

1. **A false warning.** `safety_ceiling_sync` raises the standing (non-ceiling)
   warning with the text "identity hash mismatch ... possible hash collision".
   The operator cannot act on that text.
2. **It never clears.** `safety_link_poll.c:398` never runs the Pico-half
   recapture while standing-diverged, and `kiln_cfg_store.c:2802` defers the
   autosave. The recapture is the one thing that would copy the Pico's +0.0
   into the package, so this is the same never-clearing wedge that the
   tc_type exclusion (`kiln_cfg_store.c` MEDIUM 4 comment) was added to avoid.
3. **No heat effect.** Field 0 is always greater than 0 when it is compared,
   so ±0 never reaches the heat-halt path.

**Fix:** in `config_identity_normalize_f32()`, return `+0.0f` when the parsed
value `== 0.0f`. Then the hash and the first-diff compare agree on every
input. Add a `test_config_divergence.c` case asserting that `0.0f` against
`-0.0f` does **not** diverge.

## 3. LOW-2: whole-token `%.9g` match -- FIXED, verified

`test_close_values_print_distinguishably()` now builds
`"ESP=%.9g Pico=%.9g"` with the test's own `snprintf` and asserts `strstr` in
the forward direction. In the swapped direction it asserts `strcmp` against
the reason's tail, from `ESP=` to the NUL.

The Pico value is the last token, so a `%.2f` Pico print (`1260.30`) or a
`%.8g` print (`1260.3051`) fails both checks. All four format mutants are
CAUGHT with a real assertion (table).

The expected string comes from the host's own `%.9g`, so it pins the format
rather than a literal. That is right here: both sides of the comparison use
the same C runtime.

## 4. LOW-3: wizard boundary inputs -- FIXED, verified; semantics match the policy

`kgR6(1260.305)` is 1260.31. The double is 1260.30500000000006, so
`toPrecision(6)` rounds up deterministically. `kgR6(1260.294)` is 1260.29.
Checked with node:

- **step 6:** the old `1260.305 > 1260.3 + 0.01` is false, so the old code accepted. The new `1260.31 > 1260.3` refuses.
- **step 7:** the old `1260.294 + 0.01 < 1260.3` is false, so the old code accepted. The new `1260.29 < 1260.3` refuses.

Both pre-fix expressions, restored exactly, are now CAUGHT. So are the
`>=`/`<=` boundary mutants, through the existing equal-at-print-precision
cases.

**Policy match.** `safety_ceiling_policy` keeps the Pico ceiling never tighter
than the zone maxima:

- `guard_raise()` before a commit, any exact difference;
- `apply_lower()` after a commit, only down to the exact maximum.

The wizard enforces the same direction:

- step 6 refuses a zone above the Pico ceiling;
- step 7 refuses an `abs_max` below the zone maximum.

Both compare at `kgR6`, the Pico GET print precision. Because of that
rounding, step 7 can still accept an `abs_max` up to about 0.005 below the
zone float, and step 7 accepts an `abs_max` above the zone maximum. That is
same-or-looser by the owner rule, but under the exact divergence compare it
halts heat until the next zone save lowers it. Both are REVIEW_ZPRECFX5
INFO-3, unchanged and still open for the owner. They are not regressions.

## 5. INFO-1 / INFO-2: comment fixes -- FIXED, verified

- `safety_ceiling_policy.h:115-125` now says that reconcile heals only by
  raising, and that a Pico ceiling above the zone maximum is lowered by the
  next zone save, backup import or swap. That matches
  `safety_ceiling_sync.c:906`, which calls only `guard_raise`.
- The stale `SAFETY_CEILING_EPSILON_C` paragraph is gone from
  `safety_ceiling_policy.c`.
- `grep -rn SAFETY_CEILING_EPSILON` over .c/.h/.ps1/.py/.js finds nothing.

## 6. LOW-B: INFO-4 LCD "fix" does not make the ARMED hint visible

The buffer side of `ui_page_home_refresh.c:415` is fine:

- **snprintf:** `notice_buf[CONFIG_DIVERGENCE_REASON_MAX + 32]` is 192 bytes.
  The 24-char prefix plus a reason of at most 159 chars is 183 bytes, so
  snprintf cannot truncate, and `-Wformat-truncation` has no case.
- **Stack:** +32 B on the LVGL task. Its stack is 10240 B
  (`lvgl_port.c:1354`), and `diverge_reason[160]`/`notice_buf` sit in one
  block scope as before. Negligible.

But the label is `UI_PAGE_HOME_STRIP_HEIGHT_PX = 16 + 6`, documented as "One
line ... Fixed height makes LONG_DOT truncate instead of wrapping"
(`ui_page_home.c:747-749,785-788`), on a content width of about 464 px. One
line holds roughly 55-65 characters. `LV_LABEL_LONG_DOT` ellipsizes the
**tail**, and the ARMED hint starts at about character 115 of the notice.

So the operator sees `Config mismatch (Pico): config divergence: <name> ES...`,
and the hint is cut exactly as before. The new comment's claim that the ARMED
hint "is never cut here" holds for the buffer, not the screen.

The no-scroll rule (320x480, `feedback_lcd_no_scrolling`) is respected, because
the strip stays fixed-height. The fix just does not achieve what INFO-4 asked
for.

**Severity: LOW.** This is the remedy text for a warning-only state; the web
and readiness pages show it in full.

**Fix:** when the reason carries the ARMED suffix, show a short LCD-specific
message that leads with the action, for example `Pico config mismatch --
disarm & retry commissioning`. One option is a flag or a short-hint accessor
from `safety_ceiling_sync` beside `is_standing_diverged`. Otherwise, correct
the comment.

## 7. INFO

- **INFO-A:** the 82-char figure in the `test_safety_ceiling_sync_divergence.c`
  comment should be 83 (section 1.1). Its statement that version-mismatch and
  unknown-field reasons are unreachable is correct. Its conclusion that the
  clamp is unreachable is not (section 1.2).
- **INFO-B:** REVIEW_ZPRECFX5 LOW-1's line "still reachable in production
  through the format-version-mismatch and unknown-field reasons" was itself
  wrong for those two producers, as section 1.1 shows. It was right that the
  clamp is reachable, but for a different reason (±0).
- **INFO-C:** REVIEW_ZPRECFX5 INFO-3 is unchanged: step 7 accepts an
  `abs_max` above the zone maximum, and the `kgR6` band. It is still an owner
  question.

## Negative tests (run by this review)

All runs used `tools/negtest.ps1` against base `bb90c2758`, with a throwaway copy and a fresh `{OUT}` for every mutant. In both runs the unmutated baseline passed, `real_tree_unchanged` was true, and no "REAL TREE CHANGED" line appeared.

- **Host runs:** `-Command build_host_tests.ps1 -Only "ceiling|divergence"` with ExpectPattern `(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES`. The baseline passed in 164 s.
- **Node runs:** `-Preset check -PresetArg tools/check_page_js_tests.ps1`. The baseline passed in 38 s.

Each CAUGHT below is a compiled or loaded mutant with a failing assertion line. None of them is a build failure.

| Mutation | Runner | Verdict | Evidence |
|---|---|---|---|
| `pico_fmt_2f`: Pico side `%.9g` changed to `%.2f` | kilnfw-host | CAUGHT | `test_config_divergence.c:90: Pico value printed with %.9g (whole token, not %.2f)` |
| `pico_fmt_8g`: Pico side `%.9g` changed to `%.8g` | kilnfw-host | CAUGHT | `test_config_divergence.c:85` and `:90` whole-token asserts |
| `esp_fmt_2f`: ESP side `%.9g` changed to `%.2f` | kilnfw-host | CAUGHT | `test_config_divergence.c:85: ESP value printed with %.9g (whole token)` |
| `esp_fmt_8g`: ESP side `%.9g` changed to `%.8g` | kilnfw-host | CAUGHT | `test_config_divergence.c:85` and `:90` |
| `drop_head_clamp`: `head_len = head_cap;` changed to `(void)head_cap;` | kilnfw-host | **MISSED** | Exit 0 with both test files built and run. The ARMED clamp is unpinned (LOW-1). |
| `head_cap_minus_40`: head_cap made 40 bytes smaller | kilnfw-host | **MISSED** | Exit 0. Same cause (LOW-1). |
| `step6_slack001`: step 6 given 0.01 slack | node | CAUGHT | `FAIL: step6: 1260.305 vs 1260.3 is refused (kgR6 1260.31 > 1260.3) -- no 0.01 slack` |
| `step6_ge`: step 6 `>` changed to `>=` | node | CAUGHT | `FAIL: step6: a zone max_temp_c exactly at the Pico ceiling is allowed` |
| `step7_slack001`: step 7 given 0.01 slack | node | CAUGHT | `FAIL: step7: 1260.294 vs zone max 1260.3 is refused (kgR6 1260.29 < 1260.3) -- no 0.01 slack` |
| `step7_le`: step 7 `<` changed to `<=` | node | CAUGHT | `FAIL: step7: abs_max_temp_c exactly equal to the highest zone max_temp_c is accepted` |

The two MISSED results are the expected evidence for LOW-1. The clamp branch is reachable (see LOW-1 and LOW-A), but no test reaches it, so removing it or shrinking it goes unnoticed.

## Resolution (zprecfx7)

- **LOW-A FIXED:** `config_identity_normalize_f32()` maps -0.0f to +0.0f. SaftyFW does not compute this identity hash (no normalize/FNV there), so no Pico change. Test: `test_signed_zero_is_canonical` in `test_config_divergence.c`.
- **LOW-1 FIXED:** the suffix and clamp are now `config_divergence_append_suffix_truncating_head()` (used by `safety_ceiling_sync.c`), unit-tested directly in `test_append_suffix_truncating_head` (159-char fill, suffix intact, tiny-cap case). The seam test is renamed `..._appends_suffix_to_longest_first_diff_reason` and its messages corrected.
- **LOW-B FIXED:** `config_divergence_lcd_notice()` returns "Pico config mismatch: disarm, retry commissioning" (48 chars) when the reason carries the ARMED marker, else the old prefix + reason. Web/readiness text unchanged. Test: `test_lcd_notice_leads_with_armed_hint`.
- **INFO-A FIXED:** 82 -> 83 in the test comment.
- **INFO-B:** the zprecfx5 LOW-1 line was wrong for the version-mismatch/unknown-field producers; the clamp's real route was +-0, now closed by LOW-A. Recorded here as the correction.
- **INFO-C:** deferred (owner question, not trivial).
