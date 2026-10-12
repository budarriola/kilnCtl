# Review: TESTNETFX7 network-guard fixes (origin/dev 11ac6638f, 2026-10-12)

Scope: `fc8e866bd`, `44d5411db`, `d478ebe67` (code) and `edbbbed50`, `11ac6638f` (docs).
Together they fix REVIEW_TESTNETFX6 MED-1, LOW-1, LOW-2 and INFO-1, and REVIEW_OLDPOOLB MED-1 and LOW-1.
The review is adversarial: every mutation ran in a `tools\negtest.ps1` throwaway copy. No board access and no subagents were used.
Nothing that could fetch or build was executed outside a static-only (`-k precedes`) run.

**Verdict: 0 HIGH, 1 MED, 3 LOW, 3 INFO.**

What holds up:

- The per-guard refusal needles and the site-specific `GUARD-PROBE-PAST-GUARD publish` marker work.
- `-SkipBuild` on the `-Publish` control is effective. A dead `:83` probe now refuses at `make_release.ps1:201`, before any gate or network step (F1).
- The static pin catches the forms TESTNETFX6 found: `#`-comment, deletion, `$false -and`, and a changed condition (S6).
- The Iocp patch closes the ProactorEventLoop TCP path, and the A2 equivalent-mutant claim is correct.

What does not hold up:

- The `GIT_CONFIG_*` override that is supposed to make the `-DevDryRun` control inert does nothing for `git fetch` (MED-1).
- The static pin has two blind spots: block comments, and code between the guard and the probe (LOW-1).
- The asyncio guard misses Proactor UDP `sendto` (LOW-2).

## Findings

### MED-1: the GIT_CONFIG override appends a second origin URL; fetch still uses GitHub

**Site:** `tools/PcTools/tests/test_network_guard.py:286-288` (`_NOFETCH_ENV`), used by the `devdryrun` case at `:310`. The fetch it should neutralize is at `tools/make_release.ps1:239`, with `ls-remote` at `:244`.

**Why it fails:** `remote.<name>.url` is multi-valued. Git reads config in scope order: system, global, local, then the `GIT_CONFIG_COUNT/KEY/VALUE` command-line scope. A command-line value is therefore *appended* after the repo-local URL. `git fetch` and `ls-remote` use the first URL.

Verified locally with git 2.52; no network was touched:

- `git remote get-url origin` with the test's env still prints `https://github.com/budarriola/kilnCtl.git`.
- `get-url --all` lists that URL and then `file:///nonexistent/...`.
- `git ls-remote --get-url origin` prints the GitHub URL.

**Failure scenario:** the `:238` probe becomes dead. The static pin misses this in, for example, a block comment (S2) or when code between the guard and the probe clears the env var (LOW-1). The `devdryrun` control then runs these steps:

1. the local gates
2. `git status`
3. a real `git fetch --quiet origin release --tags` against GitHub through the Windows git credential manager. `GIT_TERMINAL_PROMPT=0` does not stop GCM's own UI; `GCM_INTERACTIVE=never` would.
4. `Fail` at `:242`, because the tag does not exist

The fetch also rewrites the local `origin/release` and tag refs of whatever repo the test runs in. `-SkipBuild` keeps the builds from running, but only after the fetch, so it does not help here. No `gh` CLI or REST call is reachable before `:201`/`:239`.

**Consequence for the fixer's evidence:** the fix-status note claims "network probe commented/`$false -and`" was CAUGHT, apparently from a full-file run. In a negtest copy (a git worktree sharing the main `.git/config`), that control would have reached the real fetch before failing. The test's comment at `:283-285` ("origin URL overridden ... to a nonexistent local path") is false.

**Fix:** make the override actually replace the URL. Two options:

- Put an empty value first: `GIT_CONFIG_COUNT=2`, `KEY_0=remote.origin.url`, `VALUE_0=""` (git 2.46 and later reset the list on an empty value; verified on 2.52 through Python's `subprocess`), then `KEY_1/VALUE_1` = the file URL.
- More robust and version-independent: add `GIT_ALLOW_PROTOCOL=file`, which refuses every non-file transport locally. Optionally also add `url.file:///nonexistent/.insteadOf=https://github.com/`.

Then pin the result with a test. Under `_NOFETCH_ENV`, `git ls-remote --get-url origin` is purely local and must not contain `github.com`. Without that pin, a later git behaviour change silently re-arms the fetch.

### LOW-1: the static pin misses block comments and code between the guard and the probe

**Site:** `test_network_guard.py:357-377`.

**The pin checks two things:**

- whether the probe line *starts with* `if (`
- whether the lines between the probe and the anchor are blank or `#`

**Gap 1, block comments:** neither check understands `<# ... #>` block comments. Wrapping the probe in `<#` / `#>` passes:

- The `<#` line sits before the probe, and nothing checks that span.
- The `#>` line starts with `#`.

S1, S2 and S3 are all MISSED by the static test.

**Gap 2, guard-to-probe span:** nothing is checked between the guard line and the probe. S4 disarms the probe there with `$env:KILNCTL_TEST_GUARD_PROBE = '0'`. S5 inserts `git ls-remote origin` there. Both are MISSED statically, and the refusal test is blind to them as well, because the guard exits first.

**Who catches what today:**

| Site | What catches a dead probe | Safe? |
|---|---|---|
| `bench_hotspot` | the live control | yes, read-only `status` |
| `-Publish` (`:83`) | the live control, via the `-SkipBuild` refusal (F1) | yes |
| `web_commission_row` | the live control | only after real credentials load (LOW-3) |
| `devdryrun` (`:238`) | the live control | only after the real GitHub fetch (MED-1) |

**Fix:**

- Track `<#`/`#>` state and treat lines inside a block comment as non-executable.
- Require that the lines between the guard's closing `}` and the probe are blank or comments only. The guard body is the guard line, one or two refusal lines, and `}`; pin that span exactly.

### LOW-2: asyncio Proactor UDP sendto bypasses the guard

**Site:** `tools/PcTools/tests/conftest.py:419-442`.

`IocpProactor.connect` is patched, but `IocpProactor.sendto` is not, and Proactor UDP calls `WSASendTo` on the raw fd:

- `_ProactorDatagramTransport.sendto(data, addr)` on an unconnected endpoint goes through it.
- So does `loop.sock_sendto`.

**Probe:** a throwaway test, deleted before any negtest run, sent to the refused hub port `127.0.0.1:8767` both ways under the autouse guard on the default ProactorEventLoop (Python 3.14.6). Result: no exception, 0 attempts recorded. The same path reaches non-loopback addresses.

No PcTools source uses asyncio UDP today (grep for `create_datagram_endpoint`, `sock_sendto`), so this is latent. Connected UDP (`remote_addr=`) goes through `connect` and is guarded.

**Fix:** patch `IocpProactor.sendto` the same way. The address is the 4th positional argument or the `addr=` keyword. Add a refused-target test.

### LOW-3: dummy credentials are dead config; a dead web probe still loads the real admin password

**Sites:** `test_network_guard.py:289` (`_DUMMY_CREDS`) and `:283-285`; `tools/PcTools/scripts/web_commission_row.ps1:58-59`.

The script unconditionally overwrites `KILNCTL_WEB_USERNAME`/`PASSWORD` from User scope. The test's dummy values never reach the Python child, so the test comment ("dummy credentials") is misleading.

With the `:35` probe dead, the control behaves differently by tree:

- **Tree with a venv** (the main tree, any worktree with `.venv`): the child loads the real admin credentials and `http_auth.login` POSTs to `http://127.0.0.1:1`.
- **Negtest copy** (no venv): exit 2 at `:39-41`, before credentials load.

Is that acceptable? Mostly:

- Port 1 is normally closed, so the connect gets RST and no bytes leave.
- `http_auth._login` redacts the username and password from the raised error (`http_auth.py:212-221`), so nothing reaches pytest output or logs.
- `KC_REUSE_SID` is scrubbed by `conftest.py:55`.

The residual risk is local. Any unprivileged process can bind `127.0.0.1:1` on Windows and would then receive the plaintext admin password. The credentials also sit in the child's environment for the whole run.

**Fix:** it should fail before credential load. Add a backstop in the script immediately before `:58`:

```powershell
if ($env:KILNCTL_TEST_GUARD_PROBE) { [Console]::Error.WriteLine('GUARD-PROBE-CRED-BACKSTOP'); exit 87 }
```

Pin it statically. Have the control assert that `GUARD-PROBE-CRED-BACKSTOP` never appears when the real probe works. Drop `_DUMMY_CREDS` or correct the comment.

### INFO-1: the asyncio refusal test is not safe on failure

`test_network_guard.py:439-443` says "can never reach a live server". All three loopback ports are live on this machine:

- 8765, the link hub (owned by the kilnctrl server process)
- 8766, kicad
- 8767, kilnctrl

So if the Iocp patch regresses, the test opens real TCP connections to the hub and the MCP servers before failing. `192.0.2.1:80` sends a real SYN out of the NIC. The fixer's "dropping the Iocp patch is CAUGHT" run did exactly that. I did not repeat it.

**Suggested fix:** monkeypatch `_MCP_SERVER_PORTS` (or add a test-only port) to a port the test bound and closed. Then a regression hits a closed port.

### INFO-2: A2 is correct; the selector patch is redundant

`BaseSelectorEventLoop.sock_connect` → `_sock_connect` → `sock.connect`, which is already guarded at `conftest.py:415`. Dropping the selector patch is MISSED (both selector tests pass, 1 attempt recorded), confirming the equivalent-mutant claim. The patch is harmless defence in depth; it only moves the refusal earlier.

### INFO-3: other checks

- **Multi-line guard conditions:** the pin requires the guard as one exact line occurring once. A reformatted guard fails loud (a false positive), which is acceptable.
- **Quote normalization:** `"` becomes `'`, which matches the `make_release.ps1:78`/`:235` double-quoted guards correctly.
- **Loopback tests:** they keep working. The Iocp wrapper keeps the `(self, conn, address)` signature, and the proactor self-pipe uses `socket.socketpair`, which is loopback and allowed.
- **Other clients:** `urllib`, `http.client` and raw `socket` are covered by the existing patches. Subprocess children remain outside any in-process guard (OLDPOOLB INFO-1).
- **TESTNETFX6 INFO-1** (stale "exit 0" docstring) is fixed.
- **OLDPOOLB LOW-1** (SHA citation) is fixed.

## Negative tests

- All runs used `tools\negtest.ps1 -Preset pytest` from `C:\wt\rvtestnetfx7_18i569` at `11ac6638f`.
- Every baseline passed. Each run reported `real_tree_unchanged: true` and `copies_removed: true`.
- No edits were made in the worktree during the runs.
- Mutation JSON is in session scratch (`rv7\static.json`, `publish.json`, `sel.json`).
- S-rows ran with `-PresetArg "tests/test_network_guard.py -k precedes"`, which is static only, so no script executed.
- The F1 row used `-k publish`. A2 used `-k selector_loop`.

| # | Mutation | Verdict | Evidence |
|---|---|---|---|
| S1 | `web_commission_row.ps1:35` probe wrapped in `<# ... #>` | **MISSED** | static 6 passed |
| S2 | `make_release.ps1:238` probe wrapped in `<# ... #>` | **MISSED** | static passed |
| S3 | `make_release.ps1:83` probe wrapped in `<# ... #>` | **MISSED** | static passed |
| S4 | `$env:KILNCTL_TEST_GUARD_PROBE = '0'` inserted before the web probe | **MISSED** | static passed |
| S5 | `git ls-remote origin` inserted between `:235` guard and `:238` probe | **MISSED** | static passed |
| S6 | web probe condition `-eq '1'` → `-eq '2'` | CAUGHT | `AssertionError: expected exactly one executable probe`, `assert 0 == 1` |
| F1 | `make_release.ps1:83` probe block-commented, full `-k publish` | CAUGHT | control `AssertionError ... assert (1 == 86)`, `REFUSED: -Publish cannot be combined with -SkipBuild` (inert path) |
| A2 | drop the `BaseSelectorEventLoop.sock_connect` patch | MISSED (equivalent) | `2 passed`; refused by the `socket.connect` guard |

Not run, for safety:

- any `:238` dead-probe mutation under the full file, because the control would fetch from GitHub (MED-1)
- dropping the Iocp patch, because the test would connect to the live hub and MCP ports and SYN `192.0.2.1` (INFO-1)
- web mutations under the full file in a tree with a venv (LOW-3)
