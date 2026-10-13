# Review: webfx8 (fixes for REVIEW_WEBFX7) -- 2026-10-11

Reviewer: Opus, worktree at origin/dev `fa6d0265a`. No board access. No code changed.

Commits reviewed (both ancestors of origin/dev):

- `2b51f307f` -- MED-1: decide keeps an edit made during the save and answers 409
  `LIVE_DECIDE_SAVED_BUT_EDITED` (`live_profile_load_working_gen()`, `live_profile_clear_if_gen()`,
  `decide_finish_clear()`). LOW-4: `live_profile_fork_gen()` check and save run under one hold of
  `s_live_save_lock`. LOW-1: Clear Trip message redraw looks up the live button. LOW-2: `Number()` on
  numeric fields in `profiles_page.html` and `zones_page.html`. LOW-3: test only.
- `3216d76d7` -- LOW-2 test: stale zone option reached with a number-comparable hostile value.

## Verdict

The five WEBFX7 items are fixed. No HIGH, no MED. Two LOW, both about how a "saved, but" result is
shown, and four INFO.

### Lock order (checked, no deadlock)

- `live_profile_load_working_gen()` and `live_profile_clear_if_gen()` (`live_profile.c` 824-846) each
  take and release `s_live_save_lock` on their own. `profiles_http_save_ex()` runs between them with no
  live lock held. The only lock held across the save is `s_decide_lock`, a leaf mutex in
  `profiles_live_http.c` that nothing else takes inside it.
- `live_profile_fork_gen()` (722-748) now holds `s_live_save_lock` across `load_record`,
  `live_profile_save_working_locked()` and `save_record`. None of those take the lock again, and
  `cfg_save_lock.h` allows pref/kv writes inside a cfg save lock. Nothing in the section takes
  `s_exec.lock`.
- Only `profiles_live_http.c` and `ui_edit_firing_apply.c` call the `s_live_save_lock` functions.
  `profiles_http.c`, `zones_config_store.c`, `backup_import.c` and `zone_aux_convert_http.c` never take
  it (`zone_aux_convert_http.c:74` only does an unlocked `live_profile_load_working()`). Nothing that
  holds the profiles save mutex or the zones lock takes the live lock, so there is no inversion.

### Generation compare (checked)

- Strict `!=` on a `_Atomic uint32_t` that only ever increments (seeded `random & 0x3fffffff` at
  boot, bumped by `save_working_locked` and `clear_locked`). A false match needs 2^32 bumps during
  one save. Not a practical risk.
- A fork during the save does not bump: a pending record for the same origin returns the existing
  copy, and a different origin is refused. So a fork cannot cause a spurious STALE.
- A web edit that rewrites the same content still bumps, giving a harmless spurious 409.
- A missed edit is not possible: every working-copy write path goes through
  `save_working_locked()` under the lock and bumps before release, and the compare is under the
  same lock.

### Failed clear (checked)

The 500 text says the slot was saved ("saved to slot N; clearing the working copy failed: ..."),
which is honest. Before this change the clear result was dropped. A retry of OVERWRITE rewrites the
same slot (idempotent). A retry of SAVE_AS with the same name is refused 400 (name in use); with a
new name it creates a second slot. See LOW-2.

## LOW-1: LCD shows a successful save as "Refused: saved to slot N; ..."

`ui_page_live_decide.c` `run_decision_apply()` (145-162) handles `LIVE_DECIDE_SAVED_BUT_EDITED`
itself, but `LIVE_DECIDE_SERVER_ERROR` falls into the generic branch: `"Refused: %s"`. After a save
whose clear failed, the LCD reads "Refused: saved to slot 12; clearing the working copy failed: ...",
which contradicts itself. The web page shows the same text without the "Refused:" prefix.

Also, `err` is 96 bytes. The prefix "saved to slot NN; clearing the working copy failed: " is about
52 characters and `clear_err` can be up to 63, so the LCD cuts the reason short. The 409 text
(about 66 characters) fits.

Fix: treat `SERVER_ERROR` with a non-zero `id` (or a message starting "saved to slot") like
`SAVED_BUT_EDITED` (show `err` as-is), and size `err` to 160 like the HTTP handler.

## LOW-2: a "saved, but" result looks like a plain failure to every client

Both new outcomes (409 SAVED_BUT_EDITED and the 500 after a save) return
`{"ok":false,"error":"saved to slot N; ..."}`. The slot id is only in the prose, not a field.

- MCP `profile_live_decide` (`mcp_server_profile_live.py` 223-226) returns `error: ... (HTTP 409)`.
  Its docstring says a 409 means "nothing decided", which is no longer true for this case.
  `profile_live_http_client.py`'s `decide_live_save_as()` docstring lists only 400 and 409 "nothing
  pending".
- A script that retries SAVE_AS on "error" with a new name makes a duplicate slot.
- The web page (`live_profile_page.html` `decide()` ~480) shows the text but does not call
  `refreshLive()` on `!res.ok`, so its `lastGen` stays old and the next "decide again" hits the
  gen-stale 409 until the user reloads.

Fix: add `"saved":true,"id":N` to both bodies, refresh the live view after any non-2xx decide on the
web page, and update the two docstrings. Nothing here loses data: the slot holds the
pre-edit copy and the edit stays pending, as the message says.

## INFO

- I-1: `zones_page.html` 3389-3390 leaves `relay_cycles_seen` / `relay_cycles_target` unwrapped. Not
  injectable: they are appended only for state `relay_approach`/`relay_cycling`, and that branch
  writes with `textContent`. `innerHTML` is used only in the done/aborted branches (3488, 3511,
  ~3545), which this change wrapped. `abort_reason` and raw `state` also reach only `textContent`.
  The firmware prints both fields with `%u` (`dashboard_json.c:377`).
- I-2: `profiles_page.html` `ioTargetOptionsHtml()` (656-674), listed in WEBFX7, emits only the loop
  counters as option values and compares `selected` with `===`, so it needed no change.
- I-3: the WEBFX7 recommended 409 text was shortened to "saved to slot N; working copy edited during
  save, decide again". Clear enough.
- I-4: `test_webfx7_escapes_behaviour.js`'s stale-option case uses a `Symbol.toPrimitive` object to
  pass `selected >= 0` with a hostile string. Contrived, but it is the only way a non-number reaches
  that branch, and J4 below shows the test bites.

## Tests run (worktree at `fa6d0265a`)

Both run as the unmutated baselines of the negative tests below, each in a fresh copy with a fresh
output directory.

- `build_host_tests.ps1 -Only "live_profile|profiles_live_http"`: PASS (70 s).
- `tools/check_page_js_tests.ps1`: PASS (23.7 s).

## Negative tests (`tools/negtest.ps1`; baselines passed; real tree unchanged, copies removed)

C (`build_host_tests.ps1 -Only "live_profile|profiles_live_http"`,
`-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`):

| Mutation | Result |
|---|---|
| M1 `decide_finish_clear()` uses a plain `live_profile_clear()` | CAUGHT (test_profiles_live_http.c:776-781, 796, 811) |
| M2 `live_profile_clear_if_gen()` never returns STALE | CAUGHT (same lines) |
| M3 clear failure after a save reported as OK | CAUGHT (test_profiles_live_http.c:825, 826) |
| M4 `fork_gen()` back to the unlocked check plus `save_working_if_gen()` (LOW-4 revert) | CAUGHT (test_live_profile.c:649, 650, 653) |
| M5 `live_profile_load_working_gen()` reports gen 0 | CAUGHT (11 asserts, e.g. :737, :825, :863) |

M4 answers the vacuity question for `test_fork_gen_check_and_save_are_atomic`: the one-shot
semaphore hook does land between the check and the save once the lock is removed.

Web (`-Preset check -PresetArg tools/check_page_js_tests.ps1 -ExpectPattern "(?m)^FAIL: (W1|W2|WEBFX7)"`):

| Mutation | Result |
|---|---|
| J1 LOW-1: message written to the detached button | CAUGHT (WEBFX7 LOW-1) |
| J2 `segmentTableHtml` dwell unwrapped | CAUGHT (W2) |
| J3 `relay_cycles_used` unwrapped | CAUGHT (W1) |
| J4 stale option `selN` unwrapped | CAUGHT (W2) |
| J5 `startS` unwrapped | CAUGHT (W2) |
| J6 autotune `zone` unwrapped | CAUGHT (W1) |
| J7 LOW-3: not-tripped reset removed | CAUGHT (WEBFX7 LOW-3) |

No vacuous test found. The LCD and client-side items in LOW-1 and LOW-2 have no tests; they are new
findings, not regressions.

## Fix status

- LOW-1: fixed. `ui_page_live_decide.c` shows a `SERVER_ERROR` starting "saved to slot" as-is (no "Refused:"); `err` is 160 bytes, `msg` 192.
- LOW-2: fixed. The 409 SAVED_BUT_EDITED and the saved-then-clear-failed 500 bodies carry `"saved":true,"id":N`; `live_profile_page.html` re-reads the live view after any failed decide; both Python docstrings updated. Host test asserts the fields.
