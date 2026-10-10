# Review: tooling batch B and web/auth batch (2026-10-09)

Scope, all on origin/dev at `06c7d81b2`:

- **A. Tooling batch B:** `0c2846fb` (PcTools write-tool gates, read-backs, strict-bool gate names,
  build-job marker, TOTP verify).
- **B. Web/auth batch:**
  - `20e1263f`: http_form parser F1-F3.
  - `f333e9e7`: login page inline host-refusal mapper (MED-1).
  - `382c3936`: zones/wizard LOW-1..5.
  - `6b62a757`: doc SHA corrections.

Compared against `DEV_WEB_REVIEW_2026-10-09.md`, `HTTP_PARSER_TEST_FINDINGS_2026-10-09.md` and
`PCTOOLS_WRITE_TOOLS_REVIEW_2026-10-09.md`. This was a review only; nothing was fixed. Every
batch B finding (MED-1, LOW-1..5, F1-F3) is genuinely fixed. Findings below are residual gaps.

## Findings

### MED-1: `flash_firmware` cannot flash a board with the link down and a trip latched; no flag overrides it

`0c2846fb` adds a board-state precheck to `flash_firmware`
(`_recovery_board_state_refusals(host, allow_link_down=True)`, implemented in
`recovery_flash.board_state_refusals`, `recovery_flash.py:272`).

In link-down mode (and when ARMED), it calls `armed_conditions_fn()`. That function reports
"a safety trip is latched (trip_reason=N)" from the ESP's *cached* Pico status. This line is
classed as a **hazard**, not as unreadable, so neither `allow_unreadable_board_state` nor any
other flag waives it.

A dead link is normally accompanied by a latched trip: S6b `SAFETY_TRIP_LINK_DEAD`, or the S6a
that a dual reset produces. `safety_clear_trip()` needs the link up. So the board the
link-down waiver exists for (bring-up, or a Pico or ESP image that cannot complete the
handshake) is exactly the board the tool now refuses, with no way out from inside the tool.

Before this commit `flash_firmware` had no board-state check at all, so this is a new block on
a recovery path. A latched trip is the *safe* state (heat is de-energized), so refusing to
flash because of it adds no safety.

Suggested direction: in link-down mode, treat a latched trip as a note, or make it waivable
with an explicit flag. Keep running-profile, autotune and energized-relay hazards as hard
refusals.

### LOW-1: bench FL-10/FL-11 opt-in is still truthy; FL-10 now auto-passes `confirm=True`

`bench_test/cases_fl.py:351` and `:386` still use `if not ctx.get("allow_flash")`. FL-10
passes `confirm=True` to `flash_firmware` itself, so `allow_flash` is the only operator gate
for a JTAG flash from the runner.

The facade's strict-bool check (`registry._gate_flag_problem`, prefix `allow_`) protects
`bench_test_run(allow_flash=...)` only when it is called through the MCP facade. A direct
`BenchTestRunner` or ctx caller passing `"false"` or `1` is accepted. The rest of the batch
moved cases_aux, cases_lcd, cases_ota and operator to `is not True`, but this one was missed.

### LOW-2: new "running" guards fail open on any exception

Two new guards swallow every exception and allow the write:

- `mcp_server_debug._esp_profile_running_refusal` (`except Exception: return None`). It guards
  debug_reset, debug_halt and debug_write_memory.
- `mcp_server_wifi._wifi_write_refusal`'s running check.

An unreachable or 401-ing ESP therefore never blocks the action. This is documented in the
code, and a debug probe on a hung board must still work. But it means the guard only protects
a board whose HTTP happens to answer.

Gaps in coverage:

- The debug guard checks profile state 1 and 2 only; a running autotune is not covered.
- debug_step, debug_resume and `debug_read_*(leave_halted=True)` have no running guard at all.
  A halted core mid-firing stops the control loop the same way debug_halt does.

### LOW-3: read-backs that cannot fail, or fail only as a warning

The batch's purpose was "write tools verify by read-back". Several of the new read-backs still
let a failed or impossible verification read as success:

- `wifi_forget`: an unreadable saved-network list after the forget returns plain "ok", with no
  UNVERIFIED note. `wifi_add_network`'s equivalent path does carry one.
- `fixture_set_relay`: if the relay name is absent from the read-back (`got is None`), the
  result is a silent ok.
- `profile_live_decide`: the read-back checks only `not pending_decision`. That can already be
  false before the call. `working_id == -1` (the working copy really gone) would actually
  prove the decide landed.
- `debug_write_memory` (`_write_readback_note`) and `pico_gpio_write`: a mismatched read-back
  is appended as a WARNING, and the result still starts "wrote ..." / "ok".
- `profiles_save`:
  - an exception during the UART read-back yields "ok ... UNVERIFIED", where the caller sees
    an ok prefix;
  - the comparison covers only name and segment count, not segment contents.

### LOW-4: tests that do not pin the fix

[JS half FIXED 2026-10-10 in SHA_X: zones timeout, per-step wizard lock and omitBlankOptionalParams are now behavioural and negtest-CAUGHT; the Python/guardFieldHidden/kcHostRefusalFromText/source-regex items remain open.]

- `test_gate_flag_strictness_partb.py` unit-tests only `_is_gate_flag_name`. It never checks
  that a real registered tool (e.g. `flash_firmware(skip_backup="yes")`) is refused end to end
  through the registry.
- `test_web_review_fixes.js`: the zones Save abort path (LOW-1 second half) is untested.
  Negtest replaced the "may or may not have been applied" text and dropped the `loadCurrent()`
  call; this was MISSED by the test and by every `test_*.js` run under
  `check_js_host_tests.ps1`.
- `test_web_review_fixes.js`: the wizard double-submit guard (LOW-3) is tested only as the
  `lockSave` helper. Removing the guard from a step's Save handler (negtest: step 2) was MISSED
  by every JS test. No step's call site is pinned.
- Untested: `guardFieldHidden` (the DOM half of LOW-2), the zones `kcHostRefusalFromText`
  mapping (LOW-5, zones half), and step 3's new "NOT marked done" text.
- `test_web_review_fixes.js`: `assert(/blank optional|omitBlankOptionalParams\(params\)/ ...)`
  is vacuous. It matches the `function omitBlankOptionalParams(params)` definition line, so it
  always passes.
- Two assertions are source-text regexes ("step 11 read-back chain is returned",
  "stepStatusLine className reset"), not behavior. They would catch a plain revert but not an
  equivalent break.

### LOW-5: zones Save timeout reload discards the operator's unsaved edits [FIXED 2026-10-10 in SHA_X]

On the 30 s abort, `382c3936` now calls `loadCurrent()`, which repopulates the form from the
board. If the POST did not land, every edit the operator typed is silently replaced. The new
message says "check them before saving again", but does not say the edits were discarded.

The finding (LOW-1) asked for honest maybe-saved text. The reload is a reasonable choice for
the landed case, but it costs the not-landed case its edits. Consider keeping the form and
offering a "Reload from board" action instead.

### INFO-1: `PCTOOLS_WRITE_TOOLS_REVIEW_2026-10-09.md` has no record of the part B findings

The part A table is marked fixed. The part B findings fixed by `0c2846fb` are listed only in
the commit message, with no FIXED tags or SHA in the audit doc, so the doc cannot be used to
check closure.

### INFO-2: verified correct, no finding

- **20e1263f (F1-F3):**
  - `http_form_url_decode` returns -1 for `out_cap == 0`.
  - `http_form_parse_float` refuses any `x`/`X` (no decimal value contains one).
  - `zones_config_json_parse_float_field` now goes through `http_form_parse_float`.
  - Tests are in `test_http_form.c` and `test_zones_http.c`.
  - HTTP_PARSER F4-F9 remain open, as the doc says.
- **f333e9e7 (MED-1):** the login page carries its own `hostRefusalFromText`.
  `test_host_refusal_pages.js` executes the extracted inline function and asserts there is no
  `<script src>` and no `window.kc*` dependency. `lint_pages.js` now exits 2 on a missing
  argument.
- **382c3936:**
  - The app.js `__kcOnAuthPrompt` / `__kcAuthSignal` hooks are correct: they pause the timer
    for the modal, re-arm a fresh signal for the one retry, and callers without the hooks are
    unchanged.
  - Step 11's chain is returned, and auth-cancel is re-thrown to the outer catch.
  - `stepRefusalText` maps bad_host / cross_origin.
  - `stepStatusLine` class is reset.
  - Blank-guard labels are correct: every suffix in `ZONE_GUARD_BLANK_RE` has a label, so no
    blank is silently dropped. Every hideable suffix is in `ZONE_OPTIONAL_KEY_RE`, so a
    skipped hidden blank is omitted (stored value kept), not sent blank.
- **6b62a757:** every SHA the corrected docs now cite (`20e1263f`, `f333e9e7`, `382c3936`) is
  on origin/dev.
- **0c2846fb other items:**
  - `flash_firmware` / `fixture_flash` confirm gates are `is True` and run before any adapter
    access.
  - `fixture_flash` resolves its offset from partitions.csv.
  - `_maybe_reset_boot_guard` handles OSError and non-dict bodies.
  - The build-job RUNNING marker is written before the thread starts and is reported
    INTERRUPTED after a restart.
  - The profiles_save strip guard field names (`on_off_rules`, `seg_kind`) match
    `profiles_catalog_http.c`. A 404 is allowed, and any other read error fails closed.
  - The `coordinated_gpio_test` autotune check fails closed on None or unknown.
  - TOTP verify uses `password_override`, not `os.environ`.
  - An AST scan of remaining non-gate bool parameters found only benign ones (`dry_run`,
    `hard`, `verify`, `reset_boot_guard`, `apply_preset` behind `confirm`).

## Negative-test spot checks (`tools/negtest.ps1`)

| Mutation | Test | Result |
| --- | --- | --- |
| `flash_firmware`: `if confirm is not True` -> `if not confirm` | `test_flash_firmware_confirm_gate.py` | CAUGHT |
| `profiles_save` strip guard: `seg_kind == 1` -> `== 99` | `test_profiles_save_strip_guard.py` | CAUGHT |
| `http_form_parse_float`: `strpbrk(v, "xX")` -> `"qQ"` | kilnfw-host (`test_http_form.c:93`) | CAUGHT |
| app.js: drop `retryInit.signal = src.__kcAuthSignal()` | `test_web_review_fixes.js` | CAUGHT |
| zones abort text replaced (and still reloads) | `test_web_review_fixes.js`, all `test_*.js` | MISSED (LOW-4) |
| wizard step 2: remove `lockSave` from the Save handler | `test_web_review_fixes.js`, all `test_*.js` | MISSED (LOW-4) |

All runs: baseline passed, real tree unchanged, copies removed.

## Fix status

MED-1, LOW-1, LOW-2, LOW-3, LOW-4 (PcTools part) and INFO-1 fixed in batch C; see the closure table
in PCTOOLS_WRITE_TOOLS_REVIEW_2026-10-09.md for the commit ids. LOW-5 is a web JS item and was skipped.
