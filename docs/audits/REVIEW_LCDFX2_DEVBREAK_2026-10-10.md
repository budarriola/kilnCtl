# Review: lcdfx2 and devbreak on origin/dev, 2026-10-10

Adversarial review of two fixers' commits on origin/dev (reviewed at `9c9600384`):

- **lcdfx2**: `5c89d3a3b` (LOW-1..4 of `REVIEW_LCDFX_2026-10-10.md`: relock
  closes the network page modals, five `check_lcd_admin_gates.ps1` rules for the
  Config hub USER gates, `live_profile_fork_gen()`, stale-generation Apply
  tests), `1557df89a` (test asserts the early check refuses before the
  validator), `17e6ce311` (audit doc), `16108208f` (LF restore of
  `ui_page_network_manage.c`).
- **devbreak**: `dea054328` (cfg_fs_degraded stub in the
  `dashboard_settings_http` and `setup_progress_http` host exes;
  `kiln_cfg_swap.c` rollback-refused reason built by literal concatenation),
  `696529d9a` (raw NUL byte replaced by `'\0'`).

No firmware code changed in this review. No board access.

## Verdict

No HIGH or MED findings. The four lcdfx LOWs are fixed as described, and the
devbreak fixes are correct with the message text unchanged. Three LOW and four
INFO findings follow. One LOW is a recommended check, because the same
escape-to-raw-byte damage as `5c89d3a3b` already sits in four other tracked
files.

## Encoding and whitespace damage

`5c89d3a3b` wrote `ui_page_network_manage.c` as CRLF (a 1846-line diff) and
turned the `'\0'` escape into a raw NUL. `16108208f` and `696529d9a` repair
both. Checked at the dev tip:

- `git diff 5c89d3a3b^ origin/dev -- ui_page_network_manage.c` is exactly the
  intended 10-line `ui_page_network_manage_relock_close()` addition, with no
  other byte changes.
- Every file touched by the six commits has zero CR, zero NUL, zero other
  control bytes, and LF in the index (`git ls-files --eol`). There is no
  `git diff --check` output for `5c89d3a3b^..origin/dev` over those files.
- `git diff -w 5c89d3a3b^ 5c89d3a3b` is 123+/7-, which matches the commit's
  real content.

There is no other damage in these commits.

### LOW-1: no check refuses raw control bytes or line-ending flips; four files already carry the same damage

No `check_*.ps1` looks for NUL or stray CR bytes in sources. `.gitattributes`
covers only `*.ps1 eol=crlf`. The working tree uses `core.autocrlf=true`, so a
CRLF write is normalized by git on commit only when git still sees the file as
text. A lone CR or a NUL makes git treat the file as `-text`, and the damage
is committed byte for byte. A whole-tree scan of tracked `firmware/**`,
`tools/*.ps1` and PcTools `.c/.h/.ps1/.py/...` files found the same "escape
written as a raw byte" damage in four files:

| File | Damage | Effect |
|---|---|---|
| `firmware/KilnFW_recovery/main/test_recovery_health.c:89` | `'\r'` written as a raw CR inside a char literal (from `9a17648ab`); the line-333 ending is also CRLF in an LF file | Git flags the file `-text`. MSVC accepts it (`check_recovery_health.ps1` PASS, 110 assertions, run in this review). GCC treats a lone CR as a newline and rejects it. |
| `tools/check_negtest.ps1:299,300,304` | `\r?\n` in three regexes written with a raw CR | Equivalent regex (a raw CR matches CR), but fragile. |
| `firmware/SaftyFW/tools/check_bootloader_builds.ps1:50` | `\f`, `\b` in a path comment written as FF and BS | Comment only, unreadable. |
| `firmware/SaftyFW/tools/check_isolation.ps1:36` | `\b` in a regex comment written as BS | Comment only. |

**Fixed (lcdfx3):** `tools/check_source_bytes.ps1` added (scans the bytes git would commit via a private index copy refreshed with `git add -u`; refuses NUL, control bytes, lone CR, CRLF outside .ps1, `i/-text` text files); the four files were repaired (the whole-tree scan found no others; `test_recovery_health.c` is also LF throughout now). Negtested: NUL, CRLF, lone CR, control byte all CAUGHT.

Recommendation: add `tools/check_source_bytes.ps1`, marked `# checkcache: ok`.
For every tracked text source (`*.c *.h *.ps1 *.py *.js *.html *.css *.cmake
CMakeLists.txt *.json *.csv`, excluding submodules and `.kicad_*`) it reads the
index blob (`git ls-files -s` + `git cat-file --batch`) and refuses:

- any NUL byte;
- any control byte other than TAB and LF, or CR outside a CRLF pair;
- CRLF in the index for anything except `*.ps1`;
- `i/mixed` or `i/-text` from `git ls-files --eol` on these extensions.

Fix the four files above in the same change rather than allowlisting them.
Negative-test it by committing a NUL, a lone CR and a CRLF `.c` file into a
scratch copy. This is a cheap guard: the NUL in `5c89d3a3b` broke the
target build, and the CRLF flip would have buried a one-line change in an
1846-line diff.

## lcdfx2 (`5c89d3a3b`, `1557df89a`, `17e6ce311`, `16108208f`)

### LOW-1 of REVIEW_LCDFX (modals on relock): fixed

`handle_lcd_relock_to_home()` (`kiln_ui.c:139-143`) now calls
`ui_page_network_relock_close()` and `ui_page_network_manage_relock_close()`
first, before the "already on home" early return, so the modals close even when
home is showing. Both functions are NULL-guarded on the modal pointer, which is
created in the same build function as the textareas and labels they clear, so
they are safe before the page is built. They run on the LVGL timer
(`ui_lcd_lock.c:289`, the `s_relock_cb`). The AP modal clears the SSID and
password textareas and the status label. The connect modal clears its password
textarea (`connect_modal_close()`) and `s_connect_ssid`.

### INFO-1: pending-lock-gate action runs against the cleared modal state

This extends INFO-1 of REVIEW_LCDFX. A keypad raised as the PIN gate for a
pending force-lock survives the relock edge (`ui_lcd_lock.c:273`). If that
keypad was raised by connect submit, `connect_submit_apply()` runs after the
PIN with `s_connect_ssid` and the textarea already empty, and
`wifi_prov_add_network()` refuses `ssid_len == 0` (`wifi_prov_api.c:131`).
Nothing is written. The operator sees a failure on a hidden modal instead of a
clean cancel.

The AP path does not clear `s_ap_pending_ssid`/`s_ap_pending_password`. These
module buffers are never displayed, so the gated apply still uses the values
the admin confirmed. Behavior is consistent and safe. If the result should be
a clean cancel instead, the relock hooks could also clear the gate context.

### LOW-2: the relock-close hooks have no test or check

**Fixed (lcdfx3):** `check_lcd_admin_gates.ps1` now requires both `*_relock_close()` calls inside `handle_lcd_relock_to_home`; deleting either is CAUGHT.

No host test compiles `kiln_ui.c`, `ui_page_network.c` or
`ui_page_network_manage.c`. Outside `kiln_ui.c` itself, no check or test
references `*_relock_close`. Deleting either call from
`handle_lcd_relock_to_home()` passes every test and check. A cheap guard is
one more `check_lcd_admin_gates.ps1` rule that requires both calls inside
`handle_lcd_relock_to_home` in `kiln_ui.c`, using the same string-level style
as its existing L9 rule.

### LOW-2 of REVIEW_LCDFX (Config hub USER gates): fixed, but only the first half

The five rules match the five `run_gated(..., LCD_PIN_ROLE_USER,
*_open_apply` calls in `ui_page_config.c:80/92/123/135/152`. The check passes
at the tip (19 gates). REVIEW_LCDFX also suggested refusing any direct
`kiln_ui_show(...)` in that file outside an `*_open_apply` function. That half
was not done, and the rules are plain substring matches over raw text,
comments included. Negtest (preset `check`, `tools\check_lcd_admin_gates.ps1`):

| Mutation | Verdict |
|---|---|
| C1: network cell role USER -> ADMIN | CAUGHT (`FAIL: L3 hub network USER gate`) |
| C2: safety cell calls `safety_open_apply(NULL)` directly, gate call left in a `/* */` comment | **MISSED** |
| C3: profiles cell `kiln_ui_show("profile_picker"); return;` before the gate | **MISSED** |

**Fixed (lcdfx3):** the text rules now blank `//` and `/* */` comments before matching (C2 CAUGHT), a new rule refuses a `kiln_ui_show`/`*_open(` ahead of the `run_gated` call in a USER-gated hub nav callback (C3 CAUGHT), and the failure wording names the role (USER or admin).

Classed INFO-level hardening, not a new LOW. The regex style is the
established pattern for the ADMIN rules too, and C2/C3 are deliberate bypasses
rather than likely regressions. Strip `//` and `/* */` comments before
matching. Optionally add the `kiln_ui_show` refusal. Cosmetic: a USER rule's
failure message says "lacks an admin gate" (the shared format string at
`check_lcd_admin_gates.ps1:41`).

### LOW-3 and LOW-4 of REVIEW_LCDFX (LCD generation after fork; stale-gen test): fixed

`live_profile_fork_gen()` captures `own_gen` from
`live_profile_save_working_if_gen(origin, false, 0, &own_gen, ...)`, which
reads under `s_live_save_lock`. It reports `*out_forked = true` only on the
real fork path. `edit_firing_apply()` (`ui_edit_firing_apply.c:247-252`)
adopts `fork_gen` only when `did_fork`. Otherwise it keeps `ctx->generation`,
so both interleavings in REVIEW_LCDFX LOW-3 now end in "edited elsewhere". A
web save that lands between the fork's own save and `save_record()` also
refuses stale, which is the intended direction. `live_profile_fork()` keeps
its old signature as a NULL-out wrapper, so the web route is unchanged.

The new test `test_apply_stale_generation_refused()` covers (a) a web save
before Apply, caught by the early check (`1557df89a` pins that the validator
never ran), and (b) a web fork plus save injected from the validator fake,
after the early check, caught only by the compare-and-save. Negtest over
`build_host_tests.ps1 -Only "edit_firing_apply|kiln_cfg_swap|dashboard_settings_http"`
(baseline PASS; `-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`):

| Mutation | Verdict | Caught by |
|---|---|---|
| M1: early generation check -> `if (0)` | CAUGHT | `test_ui_edit_firing_apply.c:400` |
| M2: compare-and-save `check_gen=false` | CAUGHT | `:411`, `:412`, `:415` |
| M3: `if (did_fork)` -> `if (1)` | **MISSED** | none (LOW-3 below) |
| M4: fork reports `own_gen + 1` | CAUGHT | `:291`, `:293`, `:301`, `:305`, `:431`, `:498` |

### LOW-3: the did_fork guard is untested, and the idempotent path writes *out_gen although the header says it does not

**FIXED (webfx7).** The reuse-path write is removed; `test_fork_reuse_path_leaves_out_gen_alone()` pins `*out_gen` untouched and the interleaved Apply refusal (hook between the pending check and the fork). Note: with the write gone, `if (did_fork)` -> `if (1)` is behaviourally equivalent (fork_gen stays 0, Apply refuses stale either way), so that mutation cannot be CAUGHT by any test; re-adding the write is caught.

The header says that on the idempotent "already pending" path `*out_gen` is
untouched (`live_profile.h:228-231`). The code writes the current generation
there under the lock (`live_profile.c:753-757`). Today the only caller ignores
`fork_gen` unless `did_fork`, so behavior is correct. However, M3 shows the
guard has no test: test (b) injects the web fork before
`live_profile_has_pending_for_origin()`, so the LCD never calls the fork. A
caller that drops the guard would adopt the web edit's generation and
overwrite it, which is exactly REVIEW_LCDFX LOW-3 interleaving 1. Because the
code fills `*out_gen`, that regression would not fail safe.

Fix either way, ideally both:

- Make the code match the header by not writing `*out_gen` on the idempotent
  path. A caller that ignores `out_forked` then compares against 0 and refuses
  instead of overwriting.
- Add the missing interleaving test: a hook that performs the web fork and
  save after `has_pending_for_origin()` has returned false. For example, the
  validator hook could fork via a path that leaves no pending record until
  after the check, or a fake seam in `live_profile_load_record()` on its
  second call. Then expect "edited elsewhere" with the web edit intact.
  Negtest it with M3.

### INFO-2: out-of-range changes observed while diffing

`git diff -w 5c89d3a3b^ origin/dev -- live_profile.c` also shows the
REVIEW_WEBFX4 changes: `live_profile_clear()` under the save lock, and a
random boot seed for the generation. These come from other commits and are
not reviewed here. The file comment at `live_profile.c:89-90` still says the
generation "restarts at 0 on reboot", which is now stale after the random
seed in `live_profile_start()`.

## devbreak (`dea054328`, `696529d9a`)

- **Message text unchanged.** The old code reached
  `snprintf(..., "...boot): %s", sub)` only inside
  `if (strcmp(sub, ZONES_IMPORT_REASON_RUN_CLAIMED) == 0)`
  (`kiln_cfg_swap.c:537`), so `sub` was exactly the macro text. The new code
  concatenates the same literal (`zones_import_reasons.h:8`,
  `"a profile or autotune run is active -- retry when it ends"`, no `%`), so
  the output bytes are identical, and GCC can now size it at compile time.
  `test_kiln_cfg_swap.c:785-788` (REFUSED, not FAILED, full reason text)
  passes. Negtest M5 (drop the macro from the literal) is CAUGHT at `:788`.
- **INFO-3 (fixed, lcdfx3):** the comment directly above (`kiln_cfg_swap.c:543-544`, "%s, not
  %.40s ... prefix + sub must fit") now describes the old form. The new
  one-line comment explains the change, but the older comment could be
  trimmed.
- **Host-test stubs.** `test_stub_cfg_fs_degraded.c` is appended to `$cmdDsh`
  and `$cmdSph` (`build_host_tests.ps1:1534`, `:1951`), matching how other
  exes pull that stub. Both exes build and pass. Negtest M6 (remove the
  `$cmdDsh` line) is CAUGHT as a BUILD FAILURE.
- **`696529d9a`**: the one-byte fix restores `'\0'`. Checked above; the file
  now matches the pre-`5c89d3a3b` bytes plus the intended addition.
- **INFO-4:** the "uifx test" the brief names is the `kiln_cfg_swap` host test
  (uifx moved `kiln_cfg_swap.c` reasons to `zones_import_reasons.h`,
  `REVIEW_WEB7_2026-10-10.md`). It passes, as above.

## Tests run

All runs were in worktree `C:\wt\rvlcd2_yn8m9k` at `origin/dev` `9c9600384`.

- `build_host_tests.ps1 -Only "edit_firing_apply|live_profile|kiln_cfg_swap|dashboard_settings_http|setup_progress_http|lcd_lock|profiles_live_http"`:
  10/10 executables built and passed.
- `tools\check_lcd_admin_gates.ps1`: PASS (19 gates plus L9).
- `firmware\KilnFW_recovery\main\check_recovery_health.ps1`: PASS. Run to
  confirm that the raw-CR file in LOW-1 still compiles under MSVC.
- `tools\negtest.ps1`, two runs. Each had its baseline PASS,
  `real_tree_unchanged: true` and `copies_removed: true`. Verdicts are in the
  tables above: host M1/M2/M4/M5/M6 CAUGHT and M3 MISSED; check C1 CAUGHT and
  C2/C3 MISSED.
