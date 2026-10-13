# Review: jsfx1, test_zones_type_toggle.js loads the real kg helpers (2b20295ee, 8e37d48b6)

Date: 2026-10-12. Reviewer: Claude (Opus). This was a read-only review with negative tests. There was no board access.

Commits reviewed (origin/dev, base 99f81431b):

- `2b20295ee`: `test_zones_type_toggle.js` adds the real `kgR6` definition from
  `zones_page.html` to the VM sandbox that renders the `settingsStackHtml`/`zoneTypeHtml`
  fragment. `ace9ccbf5` made that fragment call `kgR6()`, so the test died with a
  ReferenceError.
- `8e37d48b6`: the same extraction also loads `kgRawAttrs` and `kgPost`. The fragment
  calls `kgRawAttrs` too.

The whole change is one line, line 247:

```js
const KGR6_SRC = ['function kgR6(', 'function kgRawAttrs(', 'function kgPost('].map((p) => LINES.find((l) => l.startsWith(p)).replace(/\r$/, '')).join('\n');
```

**Verdict: no HIGH or MED findings.** There are two LOW findings and five INFO findings. The
fix is correct. Every way the extraction can break makes the test fail loudly, as the
negative tests below show. No other page JS test has the same gap.

## Q1: is the extraction robust? Does it fail loudly?

Yes. Each way it can break, checked with a real mutation (Q3):

| Break | What happens | Loud? |
|---|---|---|
| Helper renamed (`function kgR6v(`) | `LINES.find` returns `undefined`, so `.replace` throws a TypeError at module load and node exits 1 | yes (M5) |
| Helper moved or indented (`  function kgR6(`) | `startsWith` fails, giving the same TypeError | yes (M3) |
| Helper split over several lines | only the first line (`function kgPost(el) {`) is extracted, so the sandbox script throws `SyntaxError: Unexpected end of input` | yes (M4) |
| Sandbox lacks a helper the fragment calls | `ReferenceError: kgRawAttrs is not defined` when the fragment renders | yes (M6) |
| Fragment starts calling a new `kg*` helper | the sandbox is a fresh context with no page globals, so the call throws a ReferenceError | yes, same as M6 |
| Helper body changed (logic) | the real body runs, so the test cannot drift from the page | yes, through real assertions (M2) |

A sandbox that lacks a helper cannot pass. Nothing catches the throw:
`renderZoneFragment` calls `vm.runInContext` without a try block, and the extraction runs at
module top level. `check_js_host_tests.ps1` judges each test by its node exit code.

## Q2: do other page JS tests have the same gap?

The search covered all 58 `firmware/KilnFW/App/test/test_*.js` files.

- **kg\* helpers (zones_page.html):** only five page functions call a `kg*` helper outside
  the kg family itself: `zoneBlock` (`kgR6`, `kgRawAttrs`), `renderKilnGraphicHtml`,
  `drawKilnGraphic`/`clearKilnGraphic`, `renderRga`, `pollFiringStatsHistory` and
  `pollAdaptiveTune` (all `kgEsc`/`kgPanel`/...). Only `test_zones_type_toggle.js` extracts
  code from any of them: the `zoneBlock` fragment this fix covers. `test_zones_kgr6.js`
  extracts the helpers themselves. The other zones-page tests (`test_zones_inheritance`,
  `test_zones_group_frames`, `test_zones_page_visibility_gates`,
  `test_zones_ceiling_message`, `test_tuning_recommendation`, `test_aux_outputs_ui`) extract
  functions that call no `kg*` helper. **No other test has the gap.** See INFO-4 for the
  untested renderers.
- **kc\* helpers (app.js):** many tests put hand-written stand-ins for `window.kc*` in their
  sandbox rather than the real helper. This is a deliberate seam, not this gap. A page that
  starts calling an unstubbed `kc*` helper throws a TypeError or ReferenceError, which is loud
  in the same way. The risk is drift: a stub can disagree with the real helper. The tests
  that stub rather than extract:
  - `kcEscapeHtml` as identity: `test_advanced_commissioning`, `test_aux_outputs_ui`,
    `test_guided_flow`, `test_lag_banner`, `test_live_profile_page`,
    `test_on_off_rules_editor`, `test_profile_limit_warning`, `test_profiles_page_filter`,
    `test_profiles_page_row_layout`, `test_safety_tc_diagnostics`, `test_setup_wizard`,
    `test_zones_inheritance`
  - `kcConfirm`/`kcIsAuthCancelled`: `test_advanced_commissioning`,
    `test_backup_restore_flow`, `test_dualwrite_window_card`, `test_fetch_auth_ack`,
    `test_live_profile_action_errors`, `test_live_profile_guard`,
    `test_profile_picker_auth_status`, `test_settings_reset_flows`
  - `kcUnit`: `test_firing_chart`, `test_lag_banner`, `test_safety_diag_card`,
    `test_safety_relay_pill`, `test_safety_tc_diagnostics`
  - others: `kcFetchWithSafetyAck` (`test_backup_restore_flow`), `kcOtaAuthedFetch`
    (`test_settings_reset_flows`), `kcPageIsDashboard`/`kcRequestIsUserInitiated`
    (`test_fetch_auth_ack`, `test_login_bootstrap_gate`, `test_page_login_gate`),
    `kcSafetyTcIsSeparate` (`test_safety_relay_pill`)

  Some tests load the real helper instead: `test_safety_diag_card` (`kcEscapeHtml`),
  `test_safety_tc_diagnostics` (`kcSafetyTcIsSeparate`), `test_kcunit_conversion`
  (`kcUnit`), `test_forgot_password_modal` (`kcModalStack`) and `test_login_auth_wrapper`
  (the wrapper range).
- A rough static scan (scratch script, not committed) listed page functions that extracted
  bodies call but that tests neither extract nor stub. Examples: `ctChannelZones` in
  `test_current_display`, and `isFiringActive`/`renderExec` in `test_lag_banner`. These sit
  on paths the tests do not reach today. If a page edit makes one of those paths reachable,
  the test fails loudly with a ReferenceError, which is what happened with `ace9ccbf5`. It
  does not pass silently. The one way such a failure could hide is a page `.catch` that
  absorbs the ReferenceError into a rendered error state, with a test that asserts only
  that error state. I did not find a case today.

## Q3: negative tests

Tool: `tools\negtest.ps1 -Preset check -PresetArg firmware\KilnFW\App\test\check_js_host_tests.ps1`,
run sequentially in throwaway copies at `8e37d48b6`. Every unmutated baseline passed (58/58).
The real tree was unchanged and the copies were removed. Mutation JSON was kept in session
scratch (`scratchpad\rvjsfx1\mutations*.json`). Each CAUGHT below was confirmed from the log
inside the named test's own failure block:

| # | Mutation | Verdict | Evidence |
|---|---|---|---|
| M1 | `kgR6` returns the unrounded `n` | CAUGHT (assertion) | `test_zones_kgr6.js`: `FAIL: rounds float noise`, `FAIL: 6 significant digits`, `FAIL: raw+disp attrs`. `test_zones_type_toggle.js` passes (INFO-1). |
| M2 | `kgRawAttrs` emits an extra `class="x"` attribute | CAUGHT (assertion) | `test_zones_type_toggle.js`: `FAIL: on/off zone: max ramp rate field is hidden`. The real helper body reaches a toggle-test assertion. |
| M3 | `kgR6` definition indented | CAUGHT (loud crash) | toggle block: `TypeError: Cannot read properties of undefined (reading 'replace')` |
| M4 | `kgPost` split onto two lines | CAUGHT (loud crash) | toggle block: `SyntaxError: Unexpected end of input`; matched on the toggle-only source line `result = settingsStackHtml + zoneTypeHtml;` |
| M5 | `kgR6` renamed (definition only) | CAUGHT (loud crash) | toggle block: the same TypeError as M3 |
| M6 | test drops `kgRawAttrs` from its list | CAUGHT (loud crash) | toggle block: `ReferenceError: kgRawAttrs is not defined` |
| M7 | test drops `kgPost` from its list | MISSED (expected) | 58/58 pass: nothing the sandbox runs calls `kgPost` (INFO-2) |

M1 and M2 are real assertion failures. M3 to M6 are the extraction-break cases: there, a
nonzero exit with a named error is the correct signal, because the requirement is "fails
loud", not "an assertion fires". The first M3 to M6 run reported MISSED only because my
expect regexes spanned several lines, and negtest matches one line at a time. The rerun used
single-line patterns. The first run of M1 ended with a negtest ERROR because the machine ran
out of disk space. That M1 log was already complete and its verdict is shown above.

## Findings

### LOW-1: extraction errors do not name what is missing

A rename or move gives `TypeError: Cannot read properties of undefined (reading 'replace')`.
A multi-line reformat gives a bare `SyntaxError: Unexpected end of input`. Neither says which
helper or why. The same file's `extractRange()` throws `start marker not found: "<marker>"`,
and `test_zones_kgr6.js`'s `pick()` throws `not found: <prefix>`. The failure is loud but
slow to diagnose.

Fix: a small `pickLine(prefix)` that throws `'helper not found at column 0: ' + prefix`, and
that also checks the line is a complete function (balanced braces). If it is not, it throws
`'<prefix> is no longer a one-line function; extend the extraction'`.

### LOW-2: the one-line-helper extraction is duplicated in two tests

`test_zones_kgr6.js` and `test_zones_type_toggle.js` each implement a "find the line that
starts with `function kgX(`" extraction. Their error behavior differs (LOW-1). Any change to
the helpers' shape needs two edits. A shared helper in `_drivers_layout.js` or a sibling
module would keep them in step.

### INFO-1: no toggle-test assertion depends on kgR6's rounding

The fixture values that pass through `kgR6` (cal 0, ramp 100, sanity 0, max 0, min -20) all
round to themselves. M1 is therefore caught only by `test_zones_kgr6.js`. That is the right
place for it. Note it so nobody assumes the toggle test also covers rounding.

### INFO-2: kgPost is extracted but never called

Nothing `test_zones_type_toggle.js` runs calls `kgPost`. The six-field round-trip slice reads
`.value` directly (M7 survives). Extracting it is harmless, but it makes the test fail if
`kgPost` is renamed or reformatted even though the test never uses it. The name `KGR6_SRC`
and its comment ("Real kgR6 helper") also undersell the three helpers it holds.

### INFO-3: first match wins

`LINES.find` takes the first `function kgR6(` line. If a duplicate definition were ever
added, the browser would use the last one and the test would use the first. This is
unlikely, and `check_duplicate_*`-style tooling does not cover page JS.

### INFO-4: the kiln graphic renderers have no JS host test

`renderKilnGraphicHtml`/`drawKilnGraphic`/`clearKilnGraphic` (`kgPanel`, `kgBadge`,
`kgAnnotation`, `kgMask`, `kgExtraRelays`, `kgDeviceType`, `kgChannelHealth`), `renderRga`,
`pollFiringStatsHistory` and `pollAdaptiveTune` are not extracted by any test. Only
`test_web_xss_fixes.js` checks two `kgEsc` call sites, by regex. This is outside this
change's scope. Recorded for whoever picks up zone-graphic test coverage.

### INFO-5: check_js_host_tests.ps1 writes output to fixed shared %TEMP% paths

Each test writes to `%TEMP%\jshosttest_<name>_out.log`/`_err.log`. Two concurrent runs can
read each other's output, for example `negtest -Parallel` copies or two sessions. The
verdict comes from the process exit code, so PASS/FAIL is not affected. But a negtest
`expect` pattern matched against printed output could match another run's text. These runs
were sequential, and each CAUGHT was confirmed inside the right test's block. The issue
existed before this change. Fix: per-run file names (PID or GUID).

## Not changed

This review changes no code. LOW-1 and LOW-2 are small test-side edits for a later fixer.
