# Review: testnetfx10 network-guard pin fix (2026-10-12)

Reviewed: `3d6c58efa` (code: "network guard pin: allow-list pre-probe tokens, probe first after
param(), absolute powershell path, fail on tokenizer errors") and `d0b069126` (doc: marks
REVIEW_TESTNETFX9 LOW-1, LOW-2, INFO-3, INFO-4 fixed). Base for every run below: `d0b069126`
(origin/dev), worktree `C:\wt\rvtestnetfx10_k2sj5q`.

Threat model, as in the earlier rounds: the guard only has to stop a test from reaching the
network or the bench by accident. It is not a sandbox against a hostile edit.

## Verdict

The fix is sound for what it claims. All 13 bypasses REVIEW_TESTNETFX9 recorded as MISSED are now
CAUGHT by a real `AssertionError` from the new allow-list (`token outside the pre-probe
allow-list`). They are caught at their original sites and again when re-placed in front of
`make_release.ps1`'s publish guard, a site with no first-statement pin, so the allow-list carries
them on its own and is not leaning on the first-statement pin. `_ps_exe()` and `_tokens` do what
FX9 LOW-2 and INFO-4 asked. The full `test_network_guard.py` passes (48 passed), and the loopback
allowances are untouched.

No HIGH or MED findings. Each of the three LOWs below is a hole in the new allow-list's own claim
("only listed token shapes execute before the probe"). Reaching any of them needs an unusual edit.

## Findings

### LOW-1: `& $python` is allowed by variable *name*, including in param() defaults

`tools/PcTools/tests/test_network_guard.py:489-492` accepts the call operator `&` whenever the next
token is a variable named `python`, whatever that variable holds. The pin does not check where
`$python` was assigned. So a script can set `$python` to `Remove-Item` and call it:

- in a `param()` default, which runs before the first statement, so `_assert_probe_is_first_statement`
  (`:526`) does not see it. Affects `web_commission_row.ps1:18-23` and `bench_hotspot.ps1:19-23`.
- anywhere before `make_release.ps1`'s publish probe (`:83`) or devdryrun probe (`:238`).

Proven at runtime in a standalone script using a dummy variable
(`[string]$python='Remove-Item', [string]$Z=(& $python ('En'+'v:KILNCTL_DUMMY_X'))` deleted the
variable before the first statement ran). Negtests N01, N02 and N03 are MISSED.

Fix options:
- Require that the only pre-probe assignment to `$python` is `$python = Resolve-PcToolsPython ...`.
- Or refuse any `param()` default that contains `&`, `.` or a subexpression `(`.

### LOW-2: function-body scan resolves the wrong definition (last-def-wins, lib overrides script)

`_split_defs` (`:421`, `defs[name] = ...` at `:441`) keeps only the last definition of a name, and
`defs.update(ldefs)` (`:564`) then lets the dot-sourced lib's definitions replace the script's own.
PowerShell does not resolve calls that way: a call runs whichever definition is current at that
point. Two cases follow:

- N04: an evil `function Gate` defined and called before the probe, with the benign `Gate` (`:207`)
  defined later. The pin scans only the later, benign body. Runtime-proven with a dummy variable;
  negtest MISSED.
- N06: a script-level `function Resolve-PcToolsPython` defined after the lib dot-source (`:211`).
  At `:212` the script's shadowing copy runs, but the pin scans the lib's body. Negtest MISSED.

Fix: refuse a name defined more than once across the script and its lib. Or scan every definition
of each called name.

### LOW-3: Saleae gRPC connects bypass the socket patches; `SALEAE_AUTOMATION_HOST` is not scrubbed

`kilnctrl/logic_capture.py:37-38` reads `SALEAE_AUTOMATION_HOST` and `SALEAE_AUTOMATION_PORT` at
import time. The connect at `:69` (`automation.Manager.connect`) goes through grpc's C-core, which
never calls Python `socket.connect`. A child-pytest probe with the conftest guard active reached
`192.0.2.1:10430` and got a gRPC `_InactiveRpcError` (UNAVAILABLE), not the guard's refusal.

`conftest.py:58` scrubs only `KILNCTL_*` and `_EXTRA_SECRETS`, so a developer shell that has
`SALEAE_AUTOMATION_HOST` set keeps it. The default host is loopback and no test drives a real
capture today, so the risk is small.

Fix: add `SALEAE_*` to the scrub, or monkeypatch `logic_capture.DEFAULT_HOST` in the fixture. Note
that the module reads the env var at import, so a scrub alone may run too late.

### INFO-1: `trap` blocks are hoisted, so a trap after the probe can run before it

A `trap` statement applies to its whole scope, wherever it sits in the text. The pin scans tokens
only up to the probe line. A trap written after the probe can therefore run on any error raised
before the probe, and the pin never scans it. Shown with a dummy variable: a script that throws
before the probe, with a trap at the end that removes the variable, printed
`DISARMED-REACHED-ACTION`.

None of the four pinned scripts uses `trap` today (grep). The web and hotspot guards are first
statements, so only an error in a `param()` default or in `make_release`'s pre-probe code could
trigger such a trap. Cheap fix: refuse the `trap` keyword anywhere in a pinned script.

### INFO-2: `getaddrinfo` and raw `_socket` are not guarded (known)

The probe confirmed `socket.getaddrinfo` still sends a real DNS query, and `_socket.socket.connect`
still bypasses the patches. Both were recorded in earlier rounds (TESTNET, TESTNET2, FX7 INFO). DNS
alone sends no test payload. They are listed here for completeness and are not new.

The same probe confirmed these are refused by the guard:
- IPv6 and IPv4-mapped IPv6 connects
- urllib, with proxy env removed
- asyncio datagram `remote_addr`
- selector `sock_sendto`

### INFO-3: fix comments cite a finding id that does not exist

`bench_hotspot.ps1:25`, `web_commission_row.ps1:25` and `test_network_guard.py:527` and `:693` say
"testnetfx10 LOW-1". The finding is REVIEW_TESTNETFX9 LOW-1. This is the same class as FX9 INFO-3,
which was fixed by making comments name the review that raised the finding.

### INFO-4: FX9 FIXED notes cite a pre-rebase SHA

`REVIEW_TESTNETFX9_2026-10-12.md:26`, `:103`, `:160` and `:169` say "FIXED in 4c44501df". That
commit is not on any remote branch; the landed code commit is `3d6c58efa`.

## (c) Legitimate localhost use

The code commit did not touch `conftest.py`. `test_loopback_still_allowed`,
`test_udp_sendto_loopback_allowed` and `test_loopback_classification` pass, along with the rest of
the file:

`python -m pytest tests/test_network_guard.py` (run in the worktree): 48 passed in 154 s.

## Negtest table

Run with `tools\negtest.ps1 -Preset pytest -PresetArg "tests/test_network_guard.py -k 'precedes or
backstop' -rfE" -Mutations neg.json`. Every mutation expects `(?m)^E\s+AssertionError`.

- Base: `d0b069126`.
- Baseline: PASS (40.7 s).
- `real_tree_unchanged: true`, `copies_removed: true`.
- Logs: `%TEMP%\negtest_logs\20261012_130138_o4qe\`.

Every CAUGHT row below was checked in its log. Each failing assertion is the allow-list's
`token outside the pre-probe allow-list` with the expected reason, never a tokenizer parse error.

| # | Mutation | Site | Verdict | Assertion reason (abridged) |
|---|---|---|---|---|
| 1 | B02-dynamic-method-orig | web, before probe | CAUGHT | type not on allow-list [Environment]; `::` on non-Console type |
| 2 | B03-reflection-orig | web | CAUGHT | type [type[]]; member Invoke |
| 3 | B04-invokeprovider-orig | web | CAUGHT | variable ExecutionContext; member Remove |
| 4 | B05-push-location-env-orig | web | CAUGHT | command Push-Location / Remove-Item |
| 5 | H05-hotspot-push-location-orig | hotspot | CAUGHT | command Push-Location / Remove-Item |
| 6 | B06-module-qualified-orig | web | CAUGHT | command Microsoft.PowerShell.Management\Remove-Item |
| 7 | B07-rmdir-alias-orig | web | CAUGHT | command rmdir |
| 8 | B10-scriptblock-create-orig | web | CAUGHT | type [scriptblock]; `&` not on $python/git |
| 9 | B16-invokescript-orig | web | CAUGHT | variable ExecutionContext; member InvokeScript |
| 10 | B19-new-psdrive-orig | web | CAUGHT | command New-PSDrive |
| 11 | B20-psvariable-set-orig | web | CAUGHT | variable ExecutionContext; member PSVariable/Set |
| 12 | M13-dotsource-in-block-orig | make_release | CAUGHT | dot-sourced lib has top-level code |
| 13 | M14-call-operator-ps1-orig | make_release | CAUGHT | `&` not on $python/git |
| 14-23 | B02..B07, B10, B16, B19, B20 `-mr` | make_release, before publish guard `:78` | CAUGHT (10/10) | same reasons as rows 1-11, line 78 |
| 24 | P05-web-param-default-pushloc | web param() default | CAUGHT | command Push-Location (line 20) |
| 25 | N01-web-param-python-callop | web param() default | **MISSED** | LOW-1 |
| 26 | N02-hs-param-python-callop | hotspot param() default | **MISSED** | LOW-1 |
| 27 | N03-mr-python-callop | make_release before publish guard | **MISSED** | LOW-1 |
| 28 | N04-mr-gate-redefined-later | make_release, evil Gate first | **MISSED** | LOW-2 |
| 29 | N06-mr-lib-function-shadowed | make_release, shadow after lib | **MISSED** | LOW-2 |

The negtest selection is static-only (`-k 'precedes or backstop'`), so no mutated script was
executed. The MISSED rows' runtime effect was shown separately with a dummy env var
(`KILNCTL_DUMMY_X`), never with the real guard variables.

## Counts

HIGH 0, MED 0, LOW 3, INFO 4.
