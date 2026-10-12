# Review TESTNETFX8 (2026-10-12)

Scope: the fixes for `REVIEW_TESTNETFX7_2026-10-12.md` on origin/dev:
`f8b761565` (code) and `6e7837610` (doc). Reviewed at origin/dev `3904de2ac`.

## Verdict

0 HIGH, 0 MED, 2 LOW, 7 INFO. MED-1 (the DevDryRun fetch reaching
github.com) is closed. Both LOW findings are hardening gaps in the static pin
and the hotspot script. Neither is a live leak today.

## Findings

### LOW-1: static pin bypasses (comment/string-blind, outside-span disarms) -- FIXED in 464ebfeef (PSParser tokens, any KILNCTL_TEST_GUARD* env write refused before the probe incl. dot-sourced files; S1-S5 CAUGHT)

`tools/PcTools/tests/test_network_guard.py:284-288` (`_exec_lines`) strips
`<# ... #>` with a regex that ignores `#` line comments and string literals.
A line comment containing `<#` therefore opens a fake block comment. That
fake block hides the executable disarm on the next line, which ends in
`# #>`:

```powershell
# note <#
$env:KILNCTL_TEST_GUARD_PROBE = '0' # #>
```

The guard-to-probe span check (`:405-411`) never sees the disarm, and the
"elsewhere" check (`:412-415`) does not see it either. Mutants S1 (web),
S2 (make_release) and S5 (hotspot) all passed the pin.

Two disarms placed outside the span also evade the "elsewhere" substring
check, because neither line contains the literal `GUARD_PROBE`:

- S3: `Remove-Item Env:KILNCTL_TEST_GUARD*`, a wildcard.
- S4: `[Environment]::SetEnvironmentVariable('KILNCTL_TEST_GUARD' + '_PROBE', $null)`, a concatenated name.

In `web_commission_row.ps1`, S3 also clears the variable that the
credential backstop (`:60`) reads. In a tree with a venv, the control run
would then load the real User-scope credentials.

The pin also never reads `tools/lib_pctools_python.ps1`, which
`make_release.ps1` dot-sources between its two probes (about `:219`). A
disarm placed there is invisible to the pin.

The pin catches the shapes it was written for: a here-string (S6), a
function wrapper (S7) and `Start-Process git` (S8).

Fix: parse with `[System.Management.Automation.Language.Parser]::ParseFile`,
or tokenize with the PowerShell tokenizer, so comments and strings are real
tokens. Also refuse any `Env:` write, `Remove-Item Env:`, `Set-Item Env:` or
`SetEnvironmentVariable` that appears before the probe, whatever variable it
names. Scan dot-sourced files as well, or forbid dot-sourcing before the
last probe.

### LOW-2: bench_hotspot.ps1 has no credential/action backstop -- FIXED in 464ebfeef (exit-87 backstop, pinned)

`tools/PcTools/scripts/bench_hotspot.ps1:91` (guard), `:95` (probe). In
`web_commission_row.ps1`, a probe disarmed by one of the LOW-1 bypasses
still hits the second backstop (`:60`, exit 87) before any credentials
load. `bench_hotspot.ps1` has no second backstop. If its probe goes dead,
the `hs-start` control reads the User-scope `KILNCTL_HOTSPOT_SSID` and
`KILNCTL_HOTSPOT_PASSWORD` and starts real tethering, and `hs-stop` stops
it. The TESTNETFX7 claim that the hotspot control is "read-only status"
holds for `hs-status` only.

Fix: add a backstop like the web script's (any non-empty
`KILNCTL_TEST_GUARD_PROBE` means exit 87) right before the credential read
and before any tethering call.

### INFO-1: MED-1 verified closed

This was verified in a scratch repo whose origin was `https://127.0.0.1:1/...`,
using git 2.52.0.windows.1.

`GIT_ALLOW_PROTOCOL=file` refuses all of the following with
`fatal: transport 'https' not allowed`:

- `fetch`
- `ls-remote`
- `push`
- `submodule add`

The refusal also holds after a `url.insteadOf` rewrite from file to https,
and with `-c protocol.https.allow=always`.

The empty `GIT_CONFIG_VALUE_0` resets the multi-valued `remote.origin.url`.
With it set, `remote get-url --all` and `ls-remote --get-url` return only
the file URL. `git config --get-all` still lists the raw values, which is
expected.

The effective config on this machine has no `insteadOf`, `includeIf` or
`protocol.*` keys, and the environment has no `GIT_*` variable except
`GIT_EDITOR`. `make_release.ps1` itself does not clear `GIT_*` variables.
That is fine for the test, because `_NOFETCH_ENV` sets every variable it
relies on explicitly. `test_nofetch_env_is_network_dead` (about `:292`) pins
the reset and the `file`-only allow list. Negtest rows N1 and N2 below show
the pin is not vacuous.

Git for Windows maps `file:///nonexistent/...` to
`C:/Program Files/Git/nonexistent/...`. This is harmless: the path does not
exist and the protocol is local.

### INFO-2: datagram sendto is refused and surfaced, not silently sent

`tools/PcTools/tests/conftest.py:446-452`: `guarded_iocp_sendto` matches the
stdlib `IocpProactor.sendto(conn, buf, flags=0, addr=None)` signature. It
raises before `real_iocp_sendto`, so no bytes leave.

Here is how the failure surfaces. `_ProactorDatagramTransport._loop_writing`
catches the exception and calls `_fatal_error`, which reaches the loop
exception handler and then closes the transport. `_refuse` (`:384`) has
already appended the attempt, so the autouse fixture teardown fails the
test. Negtest rows A1 and A2 below confirm this.

### INFO-3: `_MCP_SERVER_PORTS` monkeypatch does not leak

`test_network_guard.py:505` and `:549`. The suite has no `tests/__init__.py`
and only one `conftest.py`, so `import conftest` returns the module pytest
loaded. `_is_hub_addr` (`conftest.py:331`) reads the global at call time.
monkeypatch undo always runs, even when the test fails. The yield fixture
closes its socket in teardown. xdist workers are separate processes.

While the patch is active, 8766 and 8767 are dropped from the refused set
for that one test only. HUB_PORT 8765 stays refused.

### INFO-4: web backstop is truthy for any non-empty value

`web_commission_row.ps1:60` exits 87 for any non-empty
`KILNCTL_TEST_GUARD_PROBE`, including `"0"`. A legitimate live run with a
stray value set would therefore stop before loading credentials. When the
value is `'1'`, exit 86 (`:35`) fires first.

The variable is unset in both User and Machine scope today. The script has
no programmatic callers, and it ends with `exit $LASTEXITCODE`, so nothing
treats 87 as success. This fails safe. Consider stating the behaviour in the
script header.

### INFO-5: stale control-test docstring -- FIXED in 464ebfeef

`test_network_guard.py:371` still says "closed localhost port + dummy
credentials". The dummy credentials were dropped in `f8b761565`.

### INFO-6: obfuscated network command inside the guard body is not caught

Mutant S9 adds `$g = 'gi' + 't'; & $g ('l' + 's-remote') origin` inside the
make_release guard body, and the pin misses it. The guard body runs only
when the guard trips, and the test runs it under `_NOFETCH_ENV`. The INFO-1
protocol refusal would still stop the call, so this is defence in depth
only. An AST-based pin (LOW-1 fix) could refuse any `&` invocation in the
span.

### INFO-7: TESTNETFX7 INFO-1 (connect patch not negtestable) is fixed

The new `_own_refused_port` fixture lets the test drop the connect patch
without reaching a real port. Mutant A3 is CAUGHT.

## Negative tests

All runs used `tools\negtest.ps1 -Preset pytest` at base `3904de2ac`. Every
baseline passed. Every run reported `real_tree_unchanged: true` and
`copies_removed: true`.

Before each run I confirmed by inspection that no mutant could reach the
network or real credentials:

- **Static run:** the selected tests only read files.
- **nofetch run:** local git only.
- **async run:** the tests use only their own ephemeral loopback ports.

| # | Mutation | Selection | Verdict | Evidence |
|---|---|---|---|---|
| S1 | web: line-comment `<#` hides disarm before probe | `-k 'precedes or backstop'` | MISSED | 7 passed (LOW-1) |
| S2 | make_release: same, before probe `:238` | same | MISSED | 7 passed (LOW-1) |
| S3 | web: `Remove-Item Env:KILNCTL_TEST_GUARD*` before guard | same | MISSED | 7 passed (LOW-1) |
| S4 | make_release: `SetEnvironmentVariable` with concatenated name | same | MISSED | 7 passed (LOW-1) |
| S5 | hotspot: line-comment `<#` hides disarm | same | MISSED | 7 passed (LOW-1) |
| S6 | web: probe inside here-string | same | CAUGHT | AssertionError "guard block must close with } right before the probe" (`:406`) |
| S7 | web: probe inside uncalled function | same | CAUGHT | same assertion (`:406`) |
| S8 | make_release guard body: `Start-Process git` | same | CAUGHT | AssertionError "disarm or network command between guard and probe" (`:411`) |
| S9 | make_release guard body: obfuscated `& $g ls-remote` | same | MISSED | 7 passed (INFO-6) |
| N1 | `GIT_CONFIG_VALUE_0` non-empty (appends instead of resets) | `-k nofetch` | CAUGHT | AssertionError: github.com in get-url output |
| N2 | `GIT_ALLOW_PROTOCOL=file:https` | same | CAUGHT | AssertionError `['file','https'] == ['file']` |
| A1 | drop `IocpProactor.sendto` patch | `-k 'refuses_asyncio or selector_loop'` | CAUGHT | `Failed: DID NOT RAISE NetworkGuardError` |
| A2 | sendto guard ignores registered ports | same | CAUGHT | `Failed: DID NOT RAISE NetworkGuardError` |
| A3 | drop `IocpProactor.connect` patch | same | CAUGHT | `Failed: DID NOT RAISE NetworkGuardError` |

A1-A3 are `pytest.raises` failures. These are real test failures, not
collection errors.

### Not run, for safety

- Full-file control runs for the S mutants. S3 would load real web
  credentials in a tree with a venv, and S5 would start the real hotspot.
  The static selection proves the pin misses these mutants without
  executing them.
- Any mutation whose effect would need a network reach.

Logs: `%TEMP%\negtest_logs\20261012_085139_2ai8` (static),
`20261012_085338_24u5` (nofetch), `20261012_085415_8kt6` (async).
