# Review: TESTNET5 L1-L4 and JSFX1 LOW-1/2 fixes (4c4e76efe, 2026-10-12)

Scope: origin/dev `4c4e76efe`, which fixes `REVIEW_TESTNET5_2026-10-12.md` L1-L4
and `REVIEW_JSFX1_2026-10-12.md` LOW-1/LOW-2. Adversarial review with negative
tests, run in a worktree at `4c4e76efe`. No board access, no subagents.

What the commit does:

- The four guard probes now write `GUARD-PROBE-PAST-GUARD` to stderr and
  `exit 86`: `bench_hotspot.ps1:95`, `web_commission_row.ps1:35`,
  `make_release.ps1:83` (`-Publish` only) and `make_release.ps1:238`.
- `test_network_guard.py:301-336`: the probe control is parametrized over four
  invocations (`_PROBED`), `bench_hotspot` uses `-Action status`, and a new
  static test `test_guard_probe_precedes_real_action` requires a probe line
  within 3 lines before an anchor line.
- `settings.py:35` writes a stderr notice when `KILNCTL_TEST_SETTINGS_PATH` is
  set; `test_settings_override_prints_notice` covers it.
- `_drivers_layout.js:108` adds a shared `pickLine()`, used by
  `test_zones_kgr6.js` and `test_zones_type_toggle.js`, with a new
  `test_pickline.js`.

**Verdict: 0 HIGH, 1 MED, 2 LOW, 4 INFO.** The exit-86/stderr change, the
settings notice and `pickLine` are correct, and their negative tests are real
assertion failures. The static pin is not: it accepts a commented-out or
`$false -and`-neutralized probe (5 of 6 static mutations MISSED). When that
happens, the `make_release -Publish` control still passes in a committed tree,
and two other controls fall through to real network actions before they fail.

## Findings

### MED-1: the static pin accepts a dead probe, and two controls then reach real network code

**Site:** `tools/PcTools/tests/test_network_guard.py:331-336`.

`probes` is every line that contains both `KILNCTL_TEST_GUARD_PROBE` and the
marker text. Nothing checks that the line executes. So a probe line that is
commented out (`# if (...)`) or neutralized (`if ($false -and $env:...)`)
still passes:

- the 3-line distance check (`:334`), and
- the `[Console]::Error.WriteLine(...)`/`exit 86` text check (`:336`).

The static test was meant to stop a broken probe from reaching real code.
It does not catch this. What runs instead depends on the site, because only
the live control is left:

| Site | Control with the probe dead (committed tree) |
|---|---|
| `bench_hotspot.ps1:95` | runs the real `-Action status` (read-only; prints `ssid_env_set`/`password_env_set` booleans only), then fails `rc == 86`. Safe, as intended by TESTNET5 L2. |
| `web_commission_row.ps1:35` | reads `KILNCTL_WEB_USERNAME`/`KILNCTL_WEB_PASSWORD` from User scope (`:58-59`) and runs `web_commission_row.py W28 --host 192.0.2.1`, whose `_login_once()` (`web_commission_row.py:568`) POSTs the real admin password over plain HTTP to `192.0.2.1`. The pytest socket guard covers only the pytest process, not this PowerShell-launched grandchild. The test fails only after that. |
| `make_release.ps1:238` | `-DevDryRun` runs `git fetch origin release --tags` and `ls-remote` through the git credential manager (`:239-245`), then starts both firmware target builds (`:264-276`). `subprocess.run(timeout=120)` kills only the direct `powershell` child, so the build children can outlive the test. |
| `make_release.ps1:83` | falls through to the `:238` probe and exits 86, so the control **passes** (LOW-1). |

Negative tests S1-S5 confirm that the static test passes for each of these.
F4 shows the web control "catches" a dead probe only because the negtest copy
has no `tools\PcTools\.venv`, so `web_commission_row.ps1:39-41` fails (exit 1) before
it reads any credentials. In the main tree or any worktree with a venv, the
same mutation reaches the login POST. I did not run the `:238` mutation under
the full file, because it would fetch from origin and start the target builds.

**Fix:**

- Make the static check reject non-executing probes. Require the stripped
  line to start with `if (`, and the condition to be exactly the guard's own
  condition with `KILNCTL_TEST_NO_HUB` replaced by `KILNCTL_TEST_GUARD_PROBE`.
  Also assert that the guard line precedes the probe.
- Make the control safe when the probe is dead:
  - Point `web_commission_row` at `127.0.0.1` on a closed port, and set
    `KILNCTL_WEB_USERNAME`/`KILNCTL_WEB_PASSWORD` to sentinels. Better still,
    have the script read User scope only after the probe.
  - Give the `-DevDryRun` control `-SkipBuild` together with a nonexistent
    `-BuildDir`, and point it at a remote that cannot resolve (for example
    `GIT_CONFIG_*` overriding `remote.origin.url` to a `file:` path that does
    not exist). The fetch then fails locally.

### LOW-1: the `-Publish` control cannot prove the `make_release.ps1:83` probe is live

**Site:** `test_network_guard.py:305` (`_PROBED[2]`).

With the guard condition off, `-Publish` runs these steps, all local:

1. the gates check (`:220-223`; `v0.0.0-test.1` is a prerelease, so it passes)
2. `git status`
3. the unconditional `:235` guard (not triggered, because the control unsets
   the variable)
4. the `:238` probe, which exits 86

So the control passes whether or not `:83` is live. That makes `_PROBED[2]` a
duplicate of the `-DevDryRun` row, and TESTNET5 L3 stays open for this site.

**Evidence (F2, F3):** the negtest CAUGHTs are artifacts. Both logs fail with
`REFUSED: working tree is dirty (1 change(s), first:  M tools/make_release.ps1)`
(`log_full\m02`, `m03` line 75). The mutation itself made the copy dirty. In
a committed regression, the tree is clean and the control passes.

**Related:** the `-Publish` refusal test (`:272`, `:286-293`) accepts any
refusal that contains "refused" and `KILNCTL_TEST_NO_HUB`. Suppose the `:78`
publish guard regresses while `:83` is dead. Then the `:235` network guard's
message satisfies the assertions, and the test stays green. No token is read
before `:235`, so this hides a regression but leaks nothing.

**Fix:**

- Have the `:83` probe print a site-specific marker, such as
  `GUARD-PROBE-PAST-GUARD publish`, and assert that marker in the control.
- Assert the `-Publish` refusal text (`-Publish refused`) in the refusal test.

### LOW-2: the static anchor is a comment, and the pin does not order the guard before the probe

**Site:** `test_network_guard.py:301-306`, `:333-334`.

- **The publish anchor is a comment.** It is the line
  `# ---...--- publish helpers`. Code inserted between `:83` and the function
  definitions would run before the anchor, and the pin would not see it.
- **"First real action" is asserted, not derived.** For the `-DevDryRun` row,
  the gates check and `git status` (`:222`, `:225`) already run before the
  anchor. They are local, so this is benign today. But the pin only proves
  the probe sits near a chosen string.
- **Guard-before-probe is not checked statically.** Only the refusal test
  covers it (TESTNET5 N12).

**Fix:** same as MED-1. Pin the triple guard line, probe line, anchor line,
in that order, using executable-line matching.

### INFO-1: stale docstring

`test_network_guard.py:280` still says the probe makes every script "exit 0".
It now exits 86.

### INFO-2: the settings notice assumes `sys.stderr` exists

`settings.py:35` calls `sys.stderr.write` at import. Under `pythonw.exe`,
`sys.stderr` is `None`, so importing `kilnctrl.settings` with the variable set
would raise `AttributeError`. No repo launcher uses `pythonw`: `kilnctrl-gui`
is a console script (`pyproject.toml:50`). The notice also prints in every
pytest child that imports `kilnctrl`, because `conftest.py` sets the variable;
that is harmless noise. TESTNET5 L4 also suggested gating the override on
`KILNCTL_TEST_NO_HUB`, which was not taken. Acceptable.

### INFO-3: `pickLine` counts braces naively

`_drivers_layout.js:111-113` counts every `{`/`}`, including those inside
string literals. A helper that contained `'{'` would be rejected as "no longer
a one-line function". That fails loud and never passes silently, so it is
fine. JSFX1 INFO-3 (first match wins) is unchanged.

### INFO-4: fixer's 11 mutations not listed in the commit

The commit message does not name the 11 mutations or their logs, so I could
not re-check them one by one. I re-derived the key classes below instead.

## Questions from the brief

- **Can a probe still be bypassed?** Yes, by commenting it out or making its
  condition false. The static test accepts both (S1-S5 MISSED).
  - `bench_hotspot`: the control catches it, safely.
  - `web_commission_row` and `make_release:238`: the controls catch it only
    after real network or credential activity (MED-1).
  - `make_release:83`: nothing catches it in a committed tree (LOW-1).
- **Is the 3-line pin vacuous or easily fooled?** It catches deletion (S6)
  and a changed exit code or stream (F6, F7). It is fooled by a comment or a
  false condition, and it does not order the guard before the probe (LOW-2).
- **Could a control path still do a real network or hardware action?** Not
  while the probes are live. With a dead probe:
  - `web_commission_row` POSTs the real admin password to `192.0.2.1`.
  - `-DevDryRun` runs `git fetch`/`ls-remote` and starts both target builds.
  - `bench_hotspot status` is read-only, so it is acceptable.
- **Do the CAUGHTs hold up?**
  - F1, F5, F6, F7 and J1-J3 are real assertion failures.
  - F2 and F3 are dirty-tree artifacts.
  - F4 is caught only because the copy has no venv.

## Negative tests

All runs used `tools\negtest.ps1`, in throwaway copies at `4c4e76efe`.

- Every baseline passed.
- After each run, `real_tree_unchanged=true` and `copies_removed=true`.
- Mutation JSON is in session scratch (`rvfx6\static.json`, `full.json`,
  `js.json`).
- The pytest preset runs `tests/test_network_guard.py`; S-rows add
  `-k precedes`.
- The JS rows use the `check` preset with `check_js_host_tests.ps1`, and a
  per-mutation `expect` naming a `test_pickline.js` assertion label.
- Each verdict below was read from its per-mutation log.

| # | Mutation | Verdict | Evidence |
|---|---|---|---|
| S1 | `bench_hotspot.ps1:95` probe commented out | **MISSED** (static) | 4 passed |
| S2 | `web_commission_row.ps1:35` probe commented out | **MISSED** (static) | 4 passed |
| S3 | `make_release.ps1:83` probe commented out | **MISSED** (static) | 4 passed |
| S4 | `make_release.ps1:238` probe commented out | **MISSED** (static) | 4 passed |
| S5 | `make_release.ps1:238` probe `if ($false -and ...)` | **MISSED** (static) | 4 passed |
| S6 | `bench_hotspot.ps1:95` probe deleted | CAUGHT | `:332 AssertionError: no probe line` |
| F1 | `bench_hotspot.ps1:95` probe commented out (full file) | CAUGHT | control `assert (0 == 86)`; real read-only `status` ran (booleans only) |
| F2 | `make_release.ps1:83` probe commented out | CAUGHT (**artifact**) | `REFUSED: working tree is dirty`; a committed tree reaches `:238`, exits 86 and passes (LOW-1) |
| F3 | `make_release.ps1:83` probe `$false -and` | CAUGHT (**artifact**) | same as F2 |
| F4 | `web_commission_row.ps1:35` probe commented out | CAUGHT (**environment**) | `PcTools venv python not found`, exit 1 != 86; with a venv it reaches the login POST (MED-1) |
| F5 | `settings.py:35` notice removed | CAUGHT | `test_settings_override_prints_notice` AssertionError |
| F6 | `bench_hotspot` probe `exit 86` -> `exit 0` | CAUGHT | control and static tests both fail |
| F7 | `web_commission_row` marker to stdout | CAUGHT | control (`_PROBE in r.stderr`) and static tests both fail |
| J1 | `pickLine` brace-balance check disabled | CAUGHT | `FAIL: multi-line helper names the problem (did not throw)`, `FAIL: brace-less line rejected` |
| J2 | `pickLine` accepts an indented match | CAUGHT | `FAIL: indented helper is not found at column 0 (did not throw)` |
| J3 | `pickLine` error omits the prefix | CAUGHT | `FAIL: missing helper names itself [helper not found at column 0]` |

Not run, for safety:

- the `make_release.ps1:238` probe dead under the full file, because the
  `-DevDryRun` control would fetch from origin and start both target builds
- any `web_commission_row` mutation in a tree that has a venv
