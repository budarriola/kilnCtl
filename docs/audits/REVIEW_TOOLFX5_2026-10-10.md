# Review: toolfx5 (REVIEW_TOOLFX3 T-1..T-6 fixes), 2026-10-10

Reviewer: Opus. Scope: `1fdb15bdd` (the fix) and `96e4d56d9` (marks T-1..T-6 fixed) on
origin/dev. Files: `tools/check_submodule_pins_pushed.ps1`, `tools/land.ps1`,
`tools/dev_promote.ps1`, `tools/check_land.ps1`, `tools/check_dev_promote.ps1`,
`tools/test_check_submodule_pins_pushed.ps1`, `tools/PcTools/src/kilnctrl/pico_gpio_probe.py`
(docstring only, no finding). Line numbers are at origin/dev `43cf6c767`.

Verdict: no HIGH or MED findings. The four questions asked:

1. **Can `-PinCheckScript` be abused or left set?** It cannot be "left set": it is a plain
   parameter with no env var or persisted default, so every real land runs the real check
   unless the caller passes it on that command line. It is not refused outside tests, though,
   and the JSON verdict does not show that a stub ran (L-1). Same exposure `-ChecksScript`
   already had.
2. **Can the retry skip miss a needed re-check after a rebase changed the gitlinks?** No. The
   skip key is every `160000` entry of `ls-tree -r <new HEAD>` plus the `.gitmodules` blob at
   that commit, recomputed after each rebase; the check depends only on those (plus remote
   state). A skip needs a prior `pass` on an identical key. But no test exercises it (L-2).
3. **Quoting of the appended BatchMode with spaces in paths:** correct. git always runs
   `GIT_SSH_COMMAND` through `sh -c '<cmd> "$@"'` (confirmed with `GIT_TRACE`), and
   ` -o BatchMode=yes` is appended outside any quotes, so a quoted
   `"C:/Program Files/.../ssh.exe" -i "C:/x y/key"` stays intact. Two edge cases are wrong
   (L-3, L-4).
4. **Does FAIL on auth wrongly block offline landing?** No. Offline DNS/connect errors
   ("Could not resolve host", "Could not resolve hostname", "Failed to connect", "Connection
   timed out") match `$netPattern` and stay SKIP (exit 3). A missing origin remote also stays
   SKIP. Pushing needs origin anyway. Some transient errors that are not auth do
   FAIL, though (L-5).

## Negative tests (tools\negtest.ps1 -Preset check)

Baseline passed and the real tree was unchanged in both runs.

| Mutation | Check | Verdict |
|---|---|---|
| P1 drop T-2 origin-probe FAIL (line 131) | test_check_submodule_pins_pushed | CAUGHT |
| P2 core.sshCommand branch drops `-o BatchMode=yes` (line 33) | same | CAUGHT |
| P3 core.sshCommand branch never taken (line 32) | same | CAUGHT |
| P4 env `GIT_SSH_COMMAND` branch never appends BatchMode (line 31) | same | **MISSED** |
| P5 T-4 fetch-timeout branch removed (line 113) | same | MISSED (expected: exit code identical) |
| P6 no-origin sentinel `TimedOut=$false` (line 127) | same | CAUGHT |
| L1 retry skip ignores gitlink key (land.ps1:349) | check_land | **MISSED** |
| L2 empty-HEAD guard removed (land.ps1:347) | check_land | MISSED (guard is unreachable, see L-6) |
| L3 skip on first attempt (sanity) | check_land | CAUGHT |

## LOW

**L-1. The `-PinCheckScript` seam is accepted on a real land/promote, and the verdict does not show it.**

**Fixed in ac2b1b217:** Seams refused unless -AllowStandaloneClone (land) / foreign -RepoPath (dev_promote); verdict shows submodule_pins=pass(stub:<path>) and checks_script_override; negtest CAUGHT.
`tools/land.ps1:76,353`, `tools/dev_promote.ps1:33,154`.
Scenario: an agent copying a test command line, or one that wants to get past a failing pin check, runs
`land.ps1 -PinCheckScript stub.ps1` (stub exits 0) from a real worktree. The real check never
runs. The verdict says `submodule_pins=pass` with nothing marking it as a stub, and a commit pinning an unpushed
mykicadMcp sha lands. The same holds for `dev_promote.ps1 -Push` against main.
Fix: refuse `-PinCheckScript` (and `-ChecksScript`) unless `-AllowStandaloneClone` is set
in land.ps1, or `-RepoPath` is not this repo in dev_promote.ps1. Both are already true in
every test. At minimum, add `pin_check_script` to the JSON verdict and print a yellow
OVERRIDE line.

**L-2. The T-3 retry skip has no test. A regression that drops the gitlink comparison would ship green.**

**Fixed in ac2b1b217:** check_land counter-stub cases (gitlink racer: 2 pin runs, plain-file racer: 1); negtest CAUGHT (two mutations).
`tools/land.ps1:349`. Negtest L1 (`if ($script:subPins -eq 'pass')`) was MISSED.
Scenario: attempt 1 passes the pin check. The push loses a race to a commit that bumps the
`tools/mykicadMcp` gitlink to an unpushed sha. With the comparison gone, attempt 2 skips the
check and pushes the bad pin. Today's code is correct. The risk is a future regression.
Fix: in check_land's existing racer case (`check_land.ps1:120`), use a pin stub that appends
a line to a counter file per call. Assert two calls when the racer commit adds or changes a gitlink
(`git update-index --add --cacheinfo 160000,<sha>,m` plus a `.gitmodules` entry). Assert
one call when the racer changes only a regular file. Worth adding, because this is the one
behavior T-3 introduced.

**L-3. When both `GIT_SSH` and `core.sshCommand` are set, the script leaves git's actual ssh unbatched. The precedence comment is inverted.**

**Fixed in ac2b1b217:** Real precedence comment; core.sshCommand batched even with GIT_SSH set; test sshcommand-beats-GIT_SSH; negtest CAUGHT.
`tools/check_submodule_pins_pushed.ps1:28,32`.
The comment says `GIT_SSH_COMMAND > GIT_SSH > core.sshCommand`. git's real order is
`GIT_SSH_COMMAND > core.sshCommand > GIT_SSH`: `get_ssh_command()` reads the config before
falling back to `GIT_SSH`. Verified here: with `GIT_SSH=C:/nonexistent/...` and
`core.sshCommand="echo CFGSSH"`, `GIT_TRACE` shows git running `echo CFGSSH`. With both set,
the `elseif ($cfgSsh -and -not $env:GIT_SSH)` branch is skipped, and git runs the user's
core.sshCommand with no BatchMode. A passphrase or host-key prompt then hangs until the
60 s timeout, which ends as SKIP or FAIL, not a quick FAIL.
Fix: drop `-and -not $env:GIT_SSH` from line 32 and correct the comment.

**L-4. Appending `-o BatchMode=yes` breaks a PuTTY plink or TortoisePlink ssh command.**

**Fixed in ac2b1b217:** Add-SshBatch: ssh -> -o BatchMode=yes, plink/TortoisePlink -> -batch, other programs untouched; negtest CAUGHT (2 mutations).
`tools/check_submodule_pins_pushed.ps1:31,33`.
Scenario: `core.sshCommand` or `GIT_SSH_COMMAND` is `plink` or `"C:/Program Files/PuTTY/plink.exe"`.
plink has no `-o` option, so every ssh ls-remote fails with a message that does not match
`$netPattern`. The result is FAIL (exit 1), and land.ps1 refuses every land with ssh submodule URLs.
(The current `.gitmodules` uses https, so this repo is unaffected today.)
Fix: append BatchMode only when the command's first word is ssh or ssh.exe (git's own
variant test), or when `GIT_SSH_VARIANT` is `ssh`. Otherwise leave the command alone, or add `-batch`
for plink.

**L-5. Some transient network errors that are not auth turn the origin probe into FAIL instead of SKIP.**

**Fixed in ac2b1b217:** netPattern gains Connection was reset, errno 10054, schannel handshake, SSL_connect, HTTP 5xx; test transient-tls-reset-is-skip; negtest CAUGHT.
`tools/check_submodule_pins_pushed.ps1:56,131`.
`$netPattern` misses Windows schannel/OpenSSL wording such as `SSL_connect: Connection was reset`
("Connection was reset" does not contain "Connection reset"), `errno 10054`,
`schannel: failed to receive handshake`, and GitHub 5xx (`The requested URL returned error: 502`).
Scenario: the submodule host SKIPs on DNS while origin fails with one of these. The land is
refused as "auth?". That fails closed and a retry recovers it, so it does not wrongly allow a push, but the message
misleads.
Fix: add those strings to `$netPattern`. Or invert the rule: classify FAIL only on a
positive auth/not-found marker (`Authentication failed|Permission denied|Repository not
found|does not appear to be a git repository|could not read Username|returned error: 40[134]`)
and SKIP everything else.

**L-6. The T-6 empty-HEAD guard cannot fire.**

**Fixed in ac2b1b217:** Guard now rev-parse --verify -q plus exit code and sha shape; negtest MISSED by design (HEAD cannot be unborn at that point in the rebase flow, defensive only).
`tools/land.ps1:346-347`.
In a repo where HEAD is unborn or unreadable, `git rev-parse HEAD` prints the literal `HEAD`
on stdout (verified: `out=[HEAD] exit=128`), so `$headSha` is never empty. It still fails
closed further on: the pin check gets `-Commit HEAD`, `ls-tree` fails, it exits 2, and Finish 1
runs. So this is a wording and dead-code issue, not a hole. Negtest L2 was MISSED for this reason.
Fix: `$headSha = (& git rev-parse --verify -q HEAD 2>$null | Out-String).Trim(); if ($LASTEXITCODE -ne 0 -or $headSha -notmatch '^[0-9a-f]{40}$') { Finish 1 ... }`.

**L-7. When `GIT_SSH_COMMAND` is set, its BatchMode append has no test.**

**Fixed in ac2b1b217:** Test env-ssh-command-batchmode; negtest CAUGHT.
`tools/check_submodule_pins_pushed.ps1:31`. Negtest P4 was MISSED.
The T-1 test covers only the `core.sshCommand` branch. It clears `GIT_SSH_COMMAND`/`GIT_SSH`
and does not restore them afterwards. That is harmless because the test runs in its own
process.
Fix: add a second case that sets `$env:GIT_SSH_COMMAND` to the fake ssh, with no core.sshCommand,
and asserts the marker contains `BatchMode=yes`.

## INFO

**I-1. The T-4 fetch-timeout message needs no test.** `tools/check_submodule_pins_pushed.ps1:113`.
Both the timeout branch and the generic branch `$failed++` and exit 1. Negtest P5 MISSED
only because the exit code is the same. Reaching the branch needs an ls-remote that succeeds
followed by a fetch that hangs, which needs a fake transport. A message-only assertion is not
worth that fixture.

**I-2. A `BatchMode=no` already in the user's ssh command is kept.**
`-notmatch 'BatchMode'` skips the append, so an explicit `-o BatchMode=no` can still prompt.
The 60 s bound covers it. This is deliberate respect for the user's config.

**I-3. The retry skip trusts remote state for the length of one land.**
A submodule remote force-pushed between attempts is not re-detected. That window is negligible
and the check is advisory against fresh clones anyway.

**I-4. A stale negtest copy was found.** `C:\wt\negtest_fdjlv9` (owner pid 19440, dead) could not
be swept by negtest during this review. It is not this session's copy. Leave it to `tools/wt_status.ps1 -Prune`.
