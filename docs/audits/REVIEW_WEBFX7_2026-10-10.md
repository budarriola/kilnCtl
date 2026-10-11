# Review: webfx7 (fixes for REVIEW_WEBFX6) -- 2026-10-10

Reviewer: Opus, worktree at origin/dev `0f2c057f7`. No board access. No code changed.

Commits reviewed (all ancestors of origin/dev):

- `f7f467c45`, `d81702d55` -- `live_profile_fork_gen()` reuse path no longer writes `*out_gen`; new
  `test_fork_reuse_path_leaves_out_gen_alone`; `test_apply_refused_after_web_edit` restored.
- `f8403b1af`, `7077b5db4` -- web fixes: zones autotune `s.rule` escaped, `Number()` on profile ramp/IO
  fields, Clear Trip refusal text (reason first, cleared on success, dropped on trip-reason change or
  when no longer tripped), behaviour tests, audit-doc SHA corrections.

## Verdict

The webfx6 items are fixed as described. No caller depended on the old `*out_gen` write:
`ui_edit_firing_apply.c:235` is the only `live_profile_fork_gen()` caller and reads `fork_gen` only
under `if (did_fork)`. `profiles_live_http.c:401` uses `live_profile_fork()`, which has no `out_gen`.
The stale `b51ab9455` SHA now appears only inside REVIEW_WEBFX6's own finding text.

No HIGH. One MED (the decide-path gap the task asked about). Four LOW. The new code introduces one LOW,
the Clear Trip detached-button case.

## MED-1: decide (save_as / overwrite) loses an LCD edit applied during the save

`profiles_live_http.c` `decide_apply_locked()`: SAVE_AS (661-685) and OVERWRITE (708-726) run
`live_profile_load_working()`, then `profiles_http_save_ex()`, then `live_profile_clear()`, and
ignore clear's return value. `s_decide_lock` serialises decides only. It does not stop an LCD Apply
(`ui_edit_firing_apply.c`, `live_profile_save_working_if_gen()`), which takes only `s_live_save_lock`.

Scenario: web user clicks "Save as" while the profile runs. The handler passes `live_gen_stale()`
and loads working copy W1. `profiles_http_save_ex()` is a flash write of hundreds of ms. During it
the operator taps Apply on the LCD. The generation still matches, so W2 is saved and the generation
bumps. Then `live_profile_clear()` erases W2 and the pending record. The new slot holds W1. The LCD
reported success, but W2 is gone and the executor never picks it up. Nothing reports this.
The window is load_working through clear, not only save_ex through clear.

This is MED because an operator edit acknowledged as applied is silently discarded during a firing.
It is not a safety hazard: the executor keeps running the origin or W1 values, both of which passed
validation.

Fix (lock-rule compliant; no lock held across `profiles_http_save_ex()`):

1. Add `live_profile_load_working_gen(profile_t *out, uint32_t *gen)`. It reads the working copy and
   the generation under `s_live_save_lock`. Only `pref_cfg_fs` / kv reads happen inside, which
   `cfg_save_lock.h` allows.
2. Call `profiles_http_save_ex()` outside every live lock, as now.
3. Add `live_profile_clear_if_gen(uint32_t expected, char *err, size_t cap)`. Under
   `s_live_save_lock` it compares the generation. If it still matches it runs
   `live_profile_clear_locked()`. Otherwise it returns STALE and leaves the working copy and the
   pending record untouched.
4. Add a decide result such as `LIVE_DECIDE_SAVED_BUT_EDITED`. HTTP returns 409 with "saved to slot N;
   the working copy was edited during the save -- the newer edit is still pending, decide again".
   `ui_page_live_decide.c:148` shows the same text. Stop ignoring clear's return value; a plain clear
   failure must be reported as well.

`s_decide_lock` stays a leaf and the nesting rule is unchanged.
Test: a hook in the fake `profiles_http_save_ex()` performs `live_profile_save_working_if_gen()`
mid-decide. Assert the decide returns the new result, the pending record survives, and the working
copy equals the hook's edit. Negative-test it by swapping `clear_if_gen` back to plain clear.

## LOW-1: Clear Trip reply after a poll re-render is reported as "network error" (new in f8403b1af)

`main_page.html` `showClearTripMsg()` (2049): `btn` is the button from the render that installed the
click handler. When `#clearTripMsg` is missing it calls `btn.insertAdjacentHTML('afterend', ...)` and
then `getElementById('clearTripMsg').textContent = text`. If the status poll re-rendered the banner
between the click and the reply, `btn` is detached and the insert goes into the detached old banner.
`getElementById` returns null and the assignment throws a TypeError. The `.catch` at 2074 then stores
"Clear Trip was not sent (network error)." in `clearTripMsg`. That text shows for 30 s instead of the
real refusal, and showClearTripMsg throws again inside the catch (unhandled rejection).

Scenario: on a dashboard without a login, `/api/safety/clear_trip` is ROUTE_TIER_ADMIN. The app.js
401 wrapper opens the login modal, and the operator takes several seconds to sign in. The poll runs
every few seconds and re-renders the banner. A real refusal (or an HTTP 4xx) is then shown as a
network error, so the operator retries a request that did reach the board.

Fix: look up the live `#clearTripBtn` at show time (or render the message row from `clearTripMsg` on
the next render only, and never insert from the handler). Make the row update null-safe. Use
`.then(onOk, onErr)` so an exception in the response handler is not reported as a network error.
Test: in `test_web_ui_write_gates_behaviour.js`, run a re-render between the click and the reply,
then assert the refusal text survives. The current test stubs `insertAdjacentHTML` to a no-op and
never re-renders mid-request, so it cannot see this.

## LOW-2: remaining unescaped numeric fields; no test pins the escaping

These are defense-in-depth only. Every value comes from firmware JSON or a parsed number, and names
are escaped everywhere they reach innerHTML.

- `profiles_page.html` `ooRuleFieldsHtml()` (1098): `tempC`/`startS`/`stopS` go into `value="..."`
  raw; `ooZoneOptionsHtml` stale-option value and label; `ioTargetOptionsHtml` `selected` value.
- `profiles_page.html` `segmentTableHtml()` (2012-2023): `seg.dwell_min` raw into innerHTML (saved
  list previews and `onOffRulesSummaryHtml` via `ooRuleRow`).
- `zones_page.html` autotune lines (3382-3383, 3484): `s.zone`, `s.elapsed_s`, `s.sample_count`,
  `s.relay_cycles_used`.

Fix: wrap each in `Number(...) || 0` (or `kcEscapeHtml(String(...))` for strings). Negative tests W1
and W2 below show no test fails when the webfx7 escapes themselves are reverted. Add one behaviour
test per page that feeds a `"<img>"` string into a field and asserts no element is created.

## LOW-3: no test for "no longer tripped drops the refusal"

`renderSafetyTrip()` resets `clearTripMsg` when `!tripped`. Mutation W6 removes that reset and every
test still passes. Fix: add a case that renders tripped, receives a refusal, renders not-tripped, then
renders tripped again with the same reason, and asserts no stale refusal text.

## LOW-4: pre-existing fork TOCTOU in `live_profile_fork_gen()`

`live_profile.c` 738-760: `live_profile_load_record()` sees no pending record. Then
`live_profile_save_working_if_gen(origin, false, 0, ...)` saves the origin without a generation check.
Both happen outside `s_live_save_lock`. If a web fork and a web edit both land between those two
calls, the LCD's unconditional save overwrites the web edit with the origin, `did_fork` is true, and
the LCD Apply then succeeds. The web edit is lost. The window is a few ms and needs two web requests
inside it, so it is LOW.

Fix: do the pending check, the origin save and the record write under one `s_live_save_lock` hold
(a `fork_locked` helper). Or sample the generation together with the record read and make the save
conditional on it, returning "already pending" on STALE.

## INFO

- I-1: `test_fork_reuse_path_leaves_out_gen_alone` (b) passes even if the caller ignores `did_fork`
  (mutation C2), because `fork_gen` stays 0 and Apply is still refused as stale. Part (a) pins the
  callee contract directly, and C3 (C1+C2) is caught, so coverage is adequate.
- I-2: the restored `test_apply_refused_after_web_edit` overlaps `test_apply_stale_generation_refused`.
  The duplicate is harmless.
- I-3 (pre-existing): `dashboard_exec_http.c:899` clear_trip only sends the frame and returns
  `{"ok":true}`. It never returns `reason`. The Pico's "cause still present" refusal is asynchronous and
  is never shown as a Clear Trip message. The new reason-first branch is therefore reached only through
  `error` or a non-2xx today. That is fine, but the UI cannot tell "sent" from "cleared".
- I-4: `zones_page.html` `s.state` / `abort_reason` handling is fine (as webfx6 said).

## Tests run (worktree at `0f2c057f7`)

- `tools/check_page_js_tests.ps1`: 56/56 PASS, exit 0.
- `tools/check_lint_pages.ps1`: PASS, exit 0.
- `build_host_tests.ps1 -Only '^(ui_edit_firing_apply|live_profile)( |$)'`: 2/2 built and passed, exit 0.

## Negative tests (`tools/negtest.ps1`; baselines passed; real tree unchanged, copies removed)

Web (`-Preset check -PresetArg tools\check_page_js_tests.ps1 -ExpectPattern FAIL`):

| Mutation | Result |
|---|---|
| W1 zones `s.rule` escape reverted | MISSED (LOW-2) |
| W2 profiles `Number()` reverted (target, ramp) | MISSED (LOW-2) |
| W3 trip-reason change no longer drops refusal | CAUGHT |
| W4 success no longer clears refusal | CAUGHT |
| W5 reason precedence reverted to `error` only | CAUGHT |
| W6 not-tripped render keeps refusal | MISSED (LOW-3) |
| W7 L-17 gate dead | CAUGHT |
| W8 L-16 gate dead | CAUGHT |
| W9 L-8 decide gate dead | CAUGHT |
| W10 L-12 password-only gate dead | CAUGHT |
| W11 L-12 username-only gate dead | CAUGHT |

C (`build_host_tests.ps1 -Only '^(ui_edit_firing_apply|live_profile)( |$)'`, KilnFW FAIL pattern):

| Mutation | Result |
|---|---|
| C1 reuse path writes `*out_gen` again | CAUGHT (test_ui_edit_firing_apply.c:454) |
| C2 caller ignores `did_fork` | MISSED (I-1, expected) |
| C3 C1 + C2 | CAUGHT (lines 454, 467, 468, 472) |
