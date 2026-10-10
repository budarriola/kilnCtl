# Review: D7 unsaved-changes guards (392da5b7f, 9c72a480f)

Date: 2026-10-10. Reviewer: Opus. Base: origin/dev 9c72a480f. Review only; no code changed.

Scope: the beforeunload guards on profiles, setup_wizard, safety_config, kiln_configs,
settings_display and zones, and the live_profile changes for REVIEW_WEBFIX L3/L5 (no
clobber of dirty edits by the poll, `editSeq` tracking for saves in flight).

## Verdict

No HIGH findings. One MEDIUM: on live_profile, a stale form can still silently overwrite a
newer working copy, and the L3 "FIXED" note claims more than the code does. The rest are LOW.
Programmatic renders do not arm the guards. Setting `.value`/`.checked` fires no events, and
the one programmatic `click()` (`resetEditor()` -> `addSegBtn.click()`) is followed by
`kcDirty = false`. Every save path clears the guard only on success.

## Checks run on dev tip (9c72a480f)

- `tools/check_page_js_tests.ps1`: 53/53 page .js tests passed.
- `tools/check_lint_pages.ps1`: 67 blocks, 0 problems.
- `tools/negtest.ps1 -RequireAssertion`, command = both new test files, baseline passed,
  real tree unchanged, copies removed:

| Mutation | Result |
|---|---|
| M1 live_profile: `if (editSeq === savedSeq) dirty = false;` -> `dirty = false;` | CAUGHT ("edits made while a save is in flight stay dirty") |
| M2 live_profile: input listener no longer bumps `editSeq` | CAUGHT (same assertion) |
| M3 live_profile: `if (dirty) {` -> `if (false) {` in the changed-copy branch | CAUGHT (3 assertions) |
| M4 setup_wizard: `if (r.ok) kcDirty = false;` -> unconditional | MISSED |
| M5 safety_config: clear moved above the `!r.ok` throw | MISSED |
| M6 profiles: `kcDirty = false;` removed from `resetEditor()` | MISSED |
| M7 settings_display: clear moved above the `!r.ok` throw | MISSED |

The in-flight `editSeq` case the author left without a negative test is covered (M1, M2).
M4 to M7 are test gaps (L5, L6), not defects in the shipped code.

## Findings

### M1 (MEDIUM): live_profile can still overwrite a newer working copy silently

`live_profile_page.html` detects a "changed working copy" only by `live.working_id !==
lastWorkingId`, but `working_id` is the constant `LIVE_EDIT_WORKING_SLOT_ID`
(`live_profile.h:64`, `= PROFILES_MAX_COUNT`). `POST /api/profile/live`
(`profiles_live_http.c` `api_profile_live_post_handler`) carries no generation or revision,
so the last writer wins. The new `if (dirty)` branch therefore runs only when the page has
already lost its own baseline (`lastWorkingId === null` after save, fork, reload or decide).
A real change made by someone else is never detected:

- A second tab or client edits the same working copy. The poll sees the same id and does
  nothing. This operator's Save then overwrites those edits with the stale form, silently.
- Another client discards the copy and forks again within one 2 s poll. The page never sees
  the `!hasWorking` gap, and the old edits are saved onto the fresh fork.

This is older than the fix and the server still validates every edit (bounds, window), so it
is not a safety issue. But 9c72a480f's L3 note ("a changed working copy no longer overwrites
dirty edits") overstates it. A real fix needs a generation in `GET /api/profile/live`, echoed
by the page and checked on POST (409 on mismatch). Severity is MEDIUM because the copy being
edited drives a firing that is running.

### L1 (LOW): misleading banner after a save with edits typed while it was in flight

When `editSeq !== savedSeq`, the save handler keeps `dirty`, sets `lastWorkingId = null` and
calls `refreshLive()`. That reaches the new dirty branch, which replaces "Saved." with "The
working copy changed on the board ... Press Reload to discard your edits". Every later 2 s
poll writes it again, because `lastWorkingId` stays null. Nobody else changed anything. The
banner tells the operator to press Reload, which throws away the very edits the `editSeq`
logic kept. No data is lost silently, but the guidance is wrong. In this state the editor also
stops tracking `editable_from_segment`, though the server still refuses edits outside the
window (409). Suggested fix: after a successful own save, set `lastWorkingId` to the id (no
reload) when `dirty` survives, or word the banner for that case.

### L2 (LOW): `loadWorkingCopy()` clears `dirty` unconditionally

After a clean save, `refreshLive()` -> `loadWorkingCopy()` re-renders and sets
`dirty = false` with no `editSeq` check. Edits typed between the POST resolving and the
`?content=1` fetch resolving (well under a second) are overwritten without warning. The 2 s
poll can also start a second, duplicate `loadWorkingCopy()` in the same window. The window is
small, so LOW.

### L3 (LOW): edits typed during an in-flight save on the six pages

As the author said, this case is not handled. Severity by page:

| Page | On save success | Effect on in-flight edits |
|---|---|---|
| profiles | clears `kcDirty`, editor DOM kept | edits stay in the form, unguarded, and the page says "Saved." |
| settings_display | clears `kcDirty`, DOM kept | same |
| safety_config | clears, then `loadCurrent()` re-renders | overwritten by the re-render (pre-existing) |
| zones | clears, then `loadCurrent()` re-renders | overwritten by the re-render (pre-existing) |
| setup_wizard | `postStepState` clears; callers usually `loadAll()` | overwritten by the re-render |
| kiln_configs | box emptied on success | typed text wiped (pre-existing) |

All of these are short LAN round trips. LOW. The live_profile `editSeq` pattern would close
the two "DOM kept" pages cheaply.

### L4 (LOW): zones relay name/type edits do not arm the guard

`zones_page.html:2750` attaches `markZonesFormDirty` to `zones`, `thermoCount`,
`relayCount`, `maxSimultaneous` and `continueOnZoneTrip`. `#relayNames` (line 371) sits
outside `#zones`, but Save posts its `.relaynameinput`/`.relaytypeinput` values
(around lines 2990-2998). Editing a relay name or device type arms no guard, so there is no
beforeunload prompt. The W5 reload-unless-dirty paths (sweep completion, autotune Accept) can
also overwrite those edits without warning. Fix: add `relayNames` to the list.

### L5 (LOW, test gap): no test that a failed save keeps the guard armed

`test_unsaved_guard_pages.js` covers no edit, edit, and successful save for each page. It
never covers a failed save, so moving the clear above the `!r.ok` check goes unnoticed
(M4, M5, M7 MISSED). Its last setup_wizard lines (`fire('change')` and a second
`postStepState`) assert nothing.

### L6 (LOW, test gap / harness): `_page_vm.js` fakes can hide real failures

- `querySelector()` now returns a fresh fake element for any selector. A production selector
  with a typo therefore reads `''` instead of throwing, in every test that uses the shared
  harness. It previously threw. Safer: return `null` by default and let tests opt in through
  a selector map, as `document.querySelectorAll` already does with `opts.groups`.
- `click()` is a no-op. In a browser, `resetEditor()`'s `addSegBtn.click()` fires
  `kcMarkDirty`, so removing the trailing `kcDirty = false` (M6) would prompt on every load of
  profiles. The fake cannot show that. The profiles test also never runs `resetEditor()`:
  `loadZones()` fetches `/api/zones`, which the test does not route, so the promise never
  settles.

### L7 (LOW): false prompts

- profiles: export uses `window.location.href = profileExportUrl(id)` (around line 1786).
  With the editor dirty, the browser can raise the leave-page prompt for a download.
- setup_wizard: a step whose config write succeeds while `postStepState` fails stays dirty.
  Returning to the overview keeps the flag set even though the step body is discarded on the
  next render.
- profiles, in-page and outside beforeunload's scope: Edit on another profile, Copy, or New
  replaces a dirty editor without asking. This is older than the change and noted only.

### N1 (NIT): duplicated guard code

Seven identical copies of the beforeunload handler now exist (live_profile, profiles,
setup_wizard, safety_config, kiln_configs, settings_display, zones). Every page loads
`app.js`, so one helper (for example `window.kcInstallUnsavedGuard(isDirtyFn)`) would remove
the risk that a later fix reaches only some copies. Four pages also declare a page-level
`var kcDirty`, which becomes `window.kcDirty`, inside the `kc*` prefix `app.js` uses for its
shared globals. Nothing collides today.

### N2 (NIT): redundant clear on zones

The `zonesFormDirty = false` added before `loadCurrent()` (around line 3039) is redundant on
the success path, because `loadCurrent()` clears it on render. Its only effect is when the
re-fetch fails, and there the form already holds the saved values, so it is harmless.

## Answers to the review questions

- Armed only by real user edits: yes on all seven pages. Gaps: L4 (missed real edits) and
  L7 (false prompts).
- Cleared on save or re-render, with no false prompt after a successful save: yes. Clears
  happen only on success, with no failure-path clear. Not covered by tests (L5).
- Edits lost while a save is in flight on the six pages: LOW (L3).
- live_profile, stale copy overwriting a newer one: still possible, and not detectable with a
  constant `working_id` and no server generation (M1). The `editSeq` part is correct and
  negative-tested. Its follow-on banner is misleading (L1).
- Inline snippets consistent across pages: identical today, duplicated 7 times (N1).
- `_page_vm.js` fakes masking failures: a risk (L6), with no false pass observed. All 53
  tests pass, and M1 to M3 are caught.
