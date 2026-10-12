# Review TESTNETFX9 (2026-10-12)

Scope: the fix for `REVIEW_TESTNETFX8_2026-10-12.md` LOW-1, LOW-2 and INFO-5
on origin/dev: `464ebfeef` (code), `bc2795ef1` and `6d75ddfeb` (doc).
Reviewed at origin/dev `6d75ddfeb`.

## Verdict

0 HIGH, 0 MED, 2 LOW, 4 INFO.

- TESTNETFX8 LOW-2 and INFO-5 are closed.
- TESTNETFX8 LOW-1 is closed for the shapes it listed (S1-S5), and the
  token-based pin also catches `& { }`, `Invoke-Command`, `${env:...}`,
  `Environment::` provider paths and casing tricks.
- The pin still enumerates disarm forms instead of refusing everything it
  does not recognise, so other one-line disarms pass it (LOW-1).
- The tokenizer skips, not fails, when `powershell` is not on PATH, on
  Windows too (LOW-2).

Neither LOW is a live leak today: no current script contains a disarm.

## Findings

### LOW-1: env-write pin is a deny-list; nine verified one-line disarms still pass

`tools/PcTools/tests/test_network_guard.py:335` (`_ENV_CMDS`), `:352`
(`_env_write_problems`), `:383` (`_dot_sourced`), `:537-541` ("elsewhere"
check).

`_env_write_problems` flags a write only when it sees one of these:

- an `$env:` variable followed by an `=` operator;
- a `SetEnvironmentVariable` **Member** token;
- a Command token from a fixed alias list, with `Env:`/`HKEY`/`Environment` in
  its arguments;
- `Invoke-Expression`/`iex`.

The "elsewhere" check then catches only lines containing the literal text
`GUARD_PROBE`. Each mutant below was put on the line before the web guard
(`web_commission_row.ps1:29`) and passes both pin tests. Each construct was
also confirmed, in a separate PowerShell process against a dummy variable, to
really delete the variable:

| Mutant | Line | Why the pin misses it |
|---|---|---|
| B02 | `$m = 'SetEnvironment' + 'Variable'; [Environment]::$m('KILNCTL_TEST_GUA' + 'RD_PROBE', $null)` | method name is a Variable token, not a Member |
| B03 | `[Environment].GetMethod('SetEnvironment' + 'Variable', ...).Invoke($null, ...)` | reflection; the name is a String token |
| B04 | `$ExecutionContext.SessionState.InvokeProvider.Item.Remove('Env:\KILNCTL_TEST_GUA' + 'RD_PROBE', ...)` | provider API, no Command token |
| B05 | `Push-Location Env:; Remove-Item KILNCTL_TEST_GUARD*; Pop-Location` | `Remove-Item` argument has no `Env:` once the location is the Env drive |
| B06 | `Microsoft.PowerShell.Management\Remove-Item Env:KILNCTL_TEST_GUARD*` | module-qualified command name is not in `_ENV_CMDS` |
| B07 | `rmdir Env:KILNCTL_TEST_GUARD*` | `rmdir` (a built-in Remove-Item alias) is missing from `_ENV_CMDS` |
| B10 | `[scriptblock]::Create(@'...Remove-Item Env:KILNCTL_TEST_GUARD*...'@)` then `& $sb` | the disarm is inside a here-string String token |
| B16 | `$ExecutionContext.InvokeCommand.InvokeScript('Remove-Item Env:...')` | eval via member, not `iex` |
| B19 | `New-PSDrive -Name Q -PSProvider Environment -Root ''`; `Remove-Item Q:KILNCTL_TEST_GUARD*` | second drive name for the Env provider |
| B20 | `$ExecutionContext.SessionState.PSVariable.Set('env:KILNCTL_TEST_GUA' + 'RD_PROBE', $null)` | variable API with a provider-qualified name |

The same B05 line placed before the hotspot guard (H05) also passes.

The dot-source scan has three holes:

- It only recognises a `.` operator at the start of a statement
  (`:390`). A dot-source inside a block, such as
  `if ($true) { . (Join-Path ...) }` or a function body, is not resolved, and
  the sourced file is never scanned (M13).
- The call operator on a `.ps1` (`& (Join-Path ... 'x.ps1')`) runs the file
  in a child scope, but environment changes are process-wide. This form is
  not scanned (M14).
- `Import-Module x.psm1` is not scanned (harness only, below).

All of these forms delete the variable outright, so they disarm the probe
and the exit-87 backstop together. With such a disarm, the `hs-start`
control would read the User-scope hotspot credentials and start real
tethering, and in a tree with a venv the web control would load the real
web credentials. The `make_release` controls stay protected by
`_NOFETCH_ENV` (TESTNETFX8 INFO-1).

Rated LOW: every form needs someone to write a deliberate env deletion
before the guard of a test-guarded script, and the threat model is accidental
regression, not an adversary.

Fix: stop enumerating disarm shapes. Before the probe, allow only an
allow-list of token shapes. Fail on:

- any Command token not on a short allow-list (`Write-Error`, `Join-Path`,
  `Resolve-Path`, `Test-Path` and the like; make_release's functions are
  definitions and could be allowed by name);
- any `&` or `.` invocation operator at any depth, unless it is the one
  resolved `lib_pctools_python.ps1` dot-source;
- any Member token from `Create`, `Invoke*`, `InvokeScript`, `GetMethod`,
  `InvokeProvider`, `PSVariable`, `SessionState`;
- any `Env:`/`Environment` text, and any Variable token used as a method
  name after `::`.

A simpler alternative covers this whole family: make the probe the first
executable statement after `param()` and the guard in web and hotspot,
which have nothing before it. Then pin "no executable token between
`param()` and the guard except function definitions."

### LOW-2: tokenizer skip-if-absent applies on Windows too

`test_network_guard.py:303-305`: `_tokens` calls
`pytest.skip("needs powershell (PSParser tokenizer)")` when
`shutil.which("powershell")` and `shutil.which("pwsh")` both return None.
This skip runs on every OS. The two runtime tests (`:462`, `:484`) use the
same `skipif(_ps_exe() is None)`.

On Windows, `powershell.exe` is always present at
`%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe`. It is missing
from `PATH` only when PATH has been stripped, for example in a minimal
sandbox env or a CI runner launched with a reduced environment. In that case
the static pin, both backstop tests and both runtime tests all report `s`.

`mcpkit/pytest_verdict.py:90` (`pytest_output_problems`) refuses skipped slow
tests and shortfalls, but it does not refuse these skips. So a run where
every guard pin silently stopped running would still read as a pass. No CI
workflow exists today (`.github/workflows` is absent), so this is latent.

Fix: in `_ps_exe()`, on `sys.platform == "win32"` fall back to the
`%SystemRoot%` absolute path. Make `_tokens` and both `skipif` decorators
`pytest.fail` on Windows when that is also missing. Keep the skip only on
non-Windows hosts.

### INFO-1: exit 87 backstop is correct and distinct

`bench_hotspot.ps1:98`. The script's exits are:

- `1`: missing credentials, `:106`; an unhandled `throw` under `-File` also
  gives 1;
- `3`: the guard, `:93`;
- `86`: the probe, `:95`;
- `87`: the backstop;
- `0`: normal completion.

87 is unique to the backstop. It sits after the probe, before `switch ($Action)`
and so before every credential read, the HKLM write in
`Disable-AutoOffIfPossible` and every tethering call. Those functions are
only defined earlier, not invoked. The script has no programmatic callers. The
backstop is truthy for any non-empty value, like the web one (TESTNETFX8
INFO-4), which fails safe.

`test_credential_backstop_precedes_credential_load[hotspot]` pins the exact
line, that it follows the probe, and that only blank lines separate it from
the first action. Mutants K1-K3 are CAUGHT. No runtime test exercises the
backstop firing. Testing it would need a disarmed probe, so a static pin is
the right level.

### INFO-2: TESTNETFX8 fix SHA in that review doc is not on origin/dev

`6d75ddfeb` ("correct TESTNETFX8 fix sha after rebase") changed `9ab6925e0`
to `d8157efb6`. Neither commit is on any remote branch
(`git branch -r --contains d8157efb6` is empty). The landed fix is
`464ebfeef`. Corrected in this commit.

### INFO-3: code comments cite "testnetfx9 LOW-1/LOW-2" for TESTNETFX8 findings

The new comments in `bench_hotspot.ps1:96`, `test_network_guard.py:523` and
`:553` label the fix as `testnetfx9 LOW-1` and `LOW-2`. The findings are
TESTNETFX8 LOW-1/LOW-2; this doc's LOW-1/LOW-2 are different findings.
Cosmetic. Rename to `testnetfx8` on the next touch.

### INFO-4: smaller notes on the token pin

- `_tokens` discards the PSParser error list (`$e`). A file with a parse
  error does not run at all under `-File`, so this is not a bypass. Asserting
  `$e.Count -eq 0` would still make a tokenizer mis-read loud.
- `_dot_sourced` resolves by basename only, in the script's own directory
  and then `tools/`. A dot-source of a same-named file elsewhere would scan
  the wrong file. It does not recurse into nested dot-sources.
  `lib_pctools_python.ps1` has none today.
- B12 (`. $p`, a dot-source through a variable path) fails closed as
  intended.

## Negative tests

All runs used `tools\negtest.ps1 -Preset pytest -PresetArg
"tests/test_network_guard.py -k 'precedes or backstop'"` at base `6d75ddfeb`.
Before running, I confirmed every mutant was static-only:

- The selection runs only `test_guard_probe_precedes_real_action` (six
  cases) and `test_credential_backstop_precedes_credential_load` (two cases).
- Those tests only read the scripts and tokenize them with
  `PSParser.Tokenize` in a separate `powershell -Command`.
- No guarded script is executed, no credential is read, and the hotspot is
  never touched.

Each mutation's `expect` was `^E\s+AssertionError` (negtest matches it per
output line), so a CAUGHT is a pytest assertion failure, never a collection or
import error. An earlier run with a `^FAILED ... - AssertionError` pattern
scored every mutant MISSED because `-rfE` on a narrow terminal drops that
suffix; its logs showed the same assertions, and the rerun below is the
verdict of record.

| # | Mutation | Expected | Result | Evidence |
|---|---|---|---|---|
| S1 | web: TESTNETFX8 S1 (`<#` in a line comment hides `$env:` disarm) | CAUGHT | CAUGHT | `env write before the probe` (`:526`) |
| S3 | web: `Remove-Item Env:KILNCTL_TEST_GUARD*` | CAUGHT | CAUGHT | same (`:526`) |
| H-S3 | hotspot: same | CAUGHT | CAUGHT | same, hs-start/stop/status |
| B01 | web: `[SYSTEM.ENVIRONMENT]::setenvironmentvariable(...)` | CAUGHT | CAUGHT | same (`:526`) |
| B08 | web: `& { Remove-Item Env:... }` | CAUGHT | CAUGHT | same (`:526`) |
| B09 | web: `Invoke-Command -ScriptBlock { ... }` | CAUGHT | CAUGHT | same (`:526`) |
| B12 | web: `. $p` (dot-source through a variable) | CAUGHT | CAUGHT | `dot-sourced file cannot be resolved (fail closed)` (`:404`) |
| B17 | web: `${env:KILNCTL_TEST_GUARD_PROBE} = $null` | CAUGHT | CAUGHT | `env write before the probe` (`:526`) |
| B18 | web: `Remove-Item 'Environment::KILNCTL_TEST_GUARD*'` | CAUGHT | CAUGHT | same (`:526`) |
| M00 | lib_pctools_python.ps1: disarm in the dot-sourced file | CAUGHT | CAUGHT | `env write in dot-sourced file before the probe` (`:530`) |
| K1 | hotspot backstop `exit 87` -> `exit 0` | CAUGHT | CAUGHT | `backstop must appear exactly once` (`:560`) |
| K2 | hotspot backstop `if ($false -and ...)` | CAUGHT | CAUGHT | backstop assertion + `executable anchor not found` (hs-*) |
| K3 | hotspot backstop commented out | CAUGHT | CAUGHT | same as K2 |
| B02 | web: dynamic method name `[Environment]::$m(...)` | MISSED | MISSED | 8 passed (LOW-1) |
| B03 | web: reflection `GetMethod(...).Invoke` | MISSED | MISSED | 8 passed (LOW-1) |
| B04 | web: `InvokeProvider.Item.Remove` | MISSED | MISSED | 8 passed (LOW-1) |
| B05 | web: `Push-Location Env:` + relative `Remove-Item` | MISSED | MISSED | 8 passed (LOW-1) |
| H05 | hotspot: same as B05 | MISSED | MISSED | 8 passed (LOW-1) |
| B06 | web: module-qualified `...\Remove-Item` | MISSED | MISSED | 8 passed (LOW-1) |
| B07 | web: `rmdir Env:...` | MISSED | MISSED | 8 passed (LOW-1) |
| B10 | web: `[scriptblock]::Create(@'...'@)` + `& $sb` | MISSED | MISSED | 8 passed (LOW-1) |
| B16 | web: `InvokeCommand.InvokeScript(...)` | MISSED | MISSED | 8 passed (LOW-1) |
| B19 | web: `New-PSDrive` on the Environment provider | MISSED | MISSED | 8 passed (LOW-1) |
| B20 | web: `SessionState.PSVariable.Set('env:...')` | MISSED | MISSED | 8 passed (LOW-1) |
| M13 | make_release: dot-source moved into `if ($true) { }`, disarm in lib | MISSED | MISSED | 8 passed (LOW-1) |
| M14 | make_release: `& (Join-Path ... lib_pctools_python.ps1)`, disarm in lib | MISSED | MISSED | 8 passed (LOW-1) |

Expected MISSED rows are the LOW-1 findings. Run summary: 13 CAUGHT, 13 MISSED,
baseline passed, `real_tree_unchanged: true`, `copies_removed: true`, base
`6d75ddfeb`, logs `%TEMP%\negtest_logs\20261012_094202_36wy`.

A static harness was also run, outside negtest, with the PcTools venv
interpreter. It imports the worktree's test module and points `_repo_file`
at mutated copies in a temp dir. It agreed with every row above. It also
showed:

- `Import-Module` of a `.psm1` holding the disarm passes;
- `& (Join-Path $PSScriptRoot 'evil.ps1')` passes;
- a nested `if ($true) { . evil.ps1 }` passes.

Mutation JSON and harness are in the session scratchpad
(`rvtestnetfx9/muts/neg.json`, `rvtestnetfx9/harness/`).
