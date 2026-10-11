# Review: webfx6 (web UI JS audit fixes, PERSFX2 LOW-3, WEBFX4 LOWs) -- 2026-10-10

Reviewer: Opus, worktree at origin/dev `9c9600384`. No board access.

Commits in scope:

- `e36279885` -- the fixes (WEB_UI_JS_AUDIT L-1..L-17, I-1, I-5; PERSFX2 LOW-3; WEBFX4 LOW-1, LOW-2, LOW-4, LOW-6).
- `727aa12d9` -- titled "empty", but it is not: it deletes webfx6's duplicate `test_apply_refused_after_web_edit` (-30 lines) and fixes a `live_profile_fork` call arity. MED-1 coverage remains in the upstream `test_apply_stale_generation_refused` (`test_ui_edit_firing_apply.c`).
- `d607e4f56` -- audit-doc status marks.
- `8cda68715` -- removes duplicated lines a bad merge left in `live_profile_fork_gen()`.

## Verdict

No HIGH or MED findings. The C changes (random generation seed, `live_profile_clear()` under the save lock, the `fake_sysinfo.c` links) are correct and do not deadlock. The web fixes do what the audit asked. Most of them are untested: 7 of 9 sampled behavior mutations went MISSED (see Negative tests), because the new test file only pins the source text. Three LOWs and two doc corrections follow.

## Findings

### LOW-1: the I-1 hardening still has gaps (zones `s.rule`, profiles segment inputs)

**FIXED (webfx7).**

The question was whether the zones_page items webfx6 skipped (`s.state`, `s.rule`) reach innerHTML.

- **`s.state`: no.** `pollAutotune()` builds `line` starting with `AT_STATE_NAMES[s.state] || s.state` (zones_page.html ~3382). `line` reaches `el.innerHTML` only in three branches: relay-done (~3488), step-done (~3511) and aborted-with-model (~3545). Those branches require `s.state === 'done'` or `s.state === 'aborted'`, and both always map to constant strings in `AT_STATE_NAMES`. Every other state renders through `el.textContent` (~3547). Skipping it is correct.
- **`s.abort_reason`: no.** The plain append (~3403) only runs when the state is aborted and `!modelFieldsVisible`, which is the textContent branch. The innerHTML branch escapes it (`reasonHtml`, ~3538).
- **`s.rule`: yes, unescaped.** The relay-done branch writes `'Proposed (' + s.rule + ') Kp='` into `line` (~3484), then sets `el.innerHTML = line` (~3488). Today the value is `autotune_rule_name()`, a constant table name from firmware (`dashboard_json.c` ~373), so nothing can exploit it. Still, it is exactly the pattern I-1 exists to remove. Fix: wrap it in `window.kcEscapeHtml(String(s.rule))`.
- **profiles_page `rampFieldsHtml()` / `ioFieldsHtml()` were not hardened.** They are listed under I-1, but they still put `seg.target_c`, `seg.ramp_c_per_hr` and `seg.dwell_min` raw into `value="..."`. The values come from the board's numeric JSON; a client-side import posts to the board first and re-renders from the board's reply. That makes this defence-in-depth, not an exploitable path. Fix: `Number(...)`, the same as live_profile_page got. The webfx6 status note in WEB_UI_JS_AUDIT names only the zones leftovers and should list this one too.

### LOW-2: `live_profile_fork_gen()` reuse path contradicts its header and is not atomic

**FIXED (webfx7).**

webfx6 kept this write on the idempotent "already pending" path, and `8cda68715` left it in place:

```c
if (out_gen) {
    cfg_save_lock_take(&s_live_save_lock);
    *out_gen = atomic_load(&s_live_profile_generation);
    cfg_save_lock_give(&s_live_save_lock);
}
```

Problems:

- `live_profile.h` (~230) says that on this path "*out_gen is untouched: the caller keeps the generation it already holds".
- The working copy is read outside the lock and the generation inside it, so the two are not a consistent pair. A concurrent web save between them gives a generation for content the caller never saw.
- Taking the save lock around a single atomic load adds nothing.

It is latent today. The only caller, `ui_edit_firing_apply.c` (~214-272), uses `out_gen` only when `did_fork`. Fix: drop the write and keep the header contract. Or, if a future caller needs it, hold the lock across `load_working()` plus the generation read, and update the header.

Interaction with `5c89d3a3b` (MED-1): the fork path returns `own_gen` from `live_profile_save_working_if_gen()`. That value is correct and is what Apply compares against, so the reuse-path write does not weaken MED-1.

### LOW-3: Clear Trip's sticky refusal outlives a later success and ignores `reason`

**FIXED (webfx7).**

main_page L-2 stores `clearTripMsg = {text, until: now + 30 s}`, and `renderSafetyTrip()` re-renders it on every poll. Two problems:

- A later click that succeeds, or a new trip, does not clear it, so a stale "refused" can sit beside a cleared or new trip for up to 30 s.
- The text uses `j.error` only. Every other page in this same commit was moved to `reason || error` (PERSFX2 LOW-3).

Fix: reset `clearTripMsg` on a 2xx, and use `j.reason || j.error`.

### INFO

- **WEBFX4 LOW-2 (clear under the save lock): no deadlock.** `clear_locked()` takes the live save lock, then hal_kv (no flash-worker dispatch) and pref_cfg_fs. That is the same order as `save_working_locked()`, and it is allowed by `cfg_save_lock.h`'s nesting rule. Its callers (`profiles_live_http.c` `decide_apply_locked()` and the others) hold only `s_decide_lock`, never the profiles save lock or `s_exec.lock`. There is one pre-existing window, not a regression: `decide_apply_locked()` runs `load_working`, then `profiles_http_save_ex`, then `live_profile_clear`, and these are separate critical sections. An LCD Apply that lands between the save and the clear is wiped by the clear.
- **WEBFX4 LOW-1 (random seed): correct.** `live_profile_start()` seeds `hal_sysinfo_random_u32() & 0x3fffffff`. The chance of landing on the `applied_generation != 0` sentinel in `edit_firing_poll` is 1 in 2^30, which is negligible. Boot order is safe: nothing calls `profile_executor_run()` at boot (runs start only from user actions), so the executor's `gen` / `gen-1` seed is always taken after the random seed. The "warm-start resume" wording in the `profile_executor_run.c` ~802 comment is stale.
- **`fake_sysinfo.c` links: complete.** It was added to the four executables that `#include`/link `live_profile.c` standalone (profiles_http, live_profile, profiles_live_http, ui_edit_firing_apply). backup_import's executable links `live_profile.c` through the shared `$sources` list, which already carries `fake_sysinfo.c`. All 6 executables matching `-Only "live_profile|profiles_live_http|ui_edit_firing_apply|profiles_http|backup_import"` build and pass.
- **I-1 escaping that was done is correct.** main_page PID popup uses `kcEscapeHtml(String(z.pid_*))`; live_profile_page uses `Number()` coercion. safety_page L-10 removed the pre-escape inside `row(...)` calls, and `row()` itself escapes both label and value (safety_page.html ~429), so there is no double-escape and no gap.
- **Profile export filename (L-15)** matches `profiles_export_http.c` (~137-151) byte for byte: `[A-Za-z0-9_-]`, everything else becomes `_`, and an empty name becomes `profile`.
- **Setup wizard L-5:** `renderSweepStatus`, `step8StopPoll` and `step8Poll` are defined in the wizard (~1072, ~1167).

## Doc corrections

- WEB_UI_JS_AUDIT, REVIEW_PERSFX2 and REVIEW_WEBFX4 mark items "FIXED (webfx6, b51ab9455)". `b51ab9455` is not an ancestor of origin/dev (a pre-rebase SHA). The landed commit is `e36279885`.
- REVIEW_WEBFX4 attributes MED-1 and LOW-3 to webfx6. Those code fixes landed in `5c89d3a3b` / `1557df89a`.
- `727aa12d9`'s subject says "empty", but it carries a test deletion and an arity fix (see above).

## Negative tests (tools\negtest.ps1)

Web: `-Preset check -PresetArg tools/check_page_js_tests.ps1 -ExpectPattern FAIL`, 9 mutations. Each mutation leaves the pinned source text intact and disables the behavior.

| Mutation | Result |
|---|---|
| Control: kcExecPost `if (!r.ok)` alert becomes `if (false)` | CAUGHT |
| L-17: `var loadedOk = true` (gate never closes) | MISSED |
| L-16: `if (!freshZones) {} if (false) {` | MISSED |
| L-8: decide gate body made dead | MISSED |
| L-2: `until: 0` (sticky message expires at once) | MISSED |
| L-11: `password && !username && false` | MISSED |
| WEBFX4 LOW-4: `(true \|\| kcEditSeq === seqAtStart)` | CAUGHT |
| L-7: `if (false) if (!result \|\| typeof result.panic_disabled ...` | MISSED |
| L-3: `if (false && sweepLastRunning && !sweepPollTimer)` | MISSED |

C: `-Preset kilnfw-host`, LOW-1 seed forced to `0u` -- MISSED (no test pins the random seed; expected, accepted on review).

So `test_web_ui_js_audit_fixes.js` behavior-tests only kcExecPost. Everything else is a source-presence regex, which proves the text exists, not that it runs. This is the same false-green class as `project_test_gated_out_before_reaching_code`. Recommended follow-up: behavior tests through the existing `_page_vm.js` harness, for at least L-17, L-16, L-8 and L-11, since those guard writes. The seed and the clear-lock change are not unit-testable without a concurrency harness, and were accepted on review.

## Tests run

- `tools/check_page_js_tests.ps1`: 55/55 PASS.
- `tools/check_lint_pages.ps1`: PASS.
- `node firmware/KilnFW/App/test/test_web_ui_js_audit_fixes.js`: 23 passed, 0 failed.
- `build_host_tests.ps1 -Only "live_profile|profiles_live_http|ui_edit_firing_apply|profiles_http|backup_import"`: 6/6 built and passed.
