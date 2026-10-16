# Review: relsuitefx3 (release bench suite, REVIEW_RELSUITEFX2 fixes)

Date: 2026-10-13. Reviewer: Opus. No board access.

Scope: code commit `dedebcf8f` ("bench_test release suite: thread pinned host
through B01/D01, identity check before trip/stage/install, strict B01 diag, D01
cleanup taints, confirmed-reboot baseline") and its docs commit, both on
origin/dev. File: `tools/PcTools/src/kilnctrl/bench_test/cases_release.py`.

Fix status SHA in `REVIEW_RELSUITEFX2.md`: it cites `dedebcf8f`, which is an
ancestor of origin/dev and is the commit that last touched `cases_release.py`.
No correction needed.

Suite: `test_bench_test_cases_release.py`, `test_release_gates.py`,
`test_release_manifest.py`: 148 passed, 3 subtests passed.

Result: 0 HIGH, 2 MED, 3 LOW, 3 INFO.

## MED-1: the host pin is a no-op on the real code path (FX2 LOW-1 not fixed)

`_pin_identity()` pins only when the identity function returns a string:

```python
mism, ip = idfn(host)
...
if isinstance(ip, str) and ip:
    ctx["host"] = ip
```

The production identity function, `mcp_server_aux._board_identity_check()`,
returns `(None, uart_ips[0])`, and `uart_ips` comes from `_host_ips()`, which
returns `ipaddress` objects. Checked directly:
`type(_host_ips("192.168.1.50:80")[0])` is `ipaddress.IPv4Address`, so
`isinstance(ip, str)` is False. `ctx["host"]` therefore stays the unresolved
hostname for B01, every D01 update call and the install. Every MCP tool
re-resolves that name, so the DNS time-of-check/time-of-use window that FX2
LOW-1 described is still open. The identity checks themselves still run
(MED-1 of FX2 holds), but they check one resolution and the write can go to
another.

The tests miss this because every fake `_board_identity_fn` returns a `str`
(for example `lambda h: (None, "10.0.0.5")`), a shape the real function never
returns.

Fix: pin `str(ip)` for any non-empty value. Better, reuse
`mcp_server_aux._pin_host(resolved, ip)`, which also keeps the port (LOW-1).
Add one test that drives `_pin_identity` through the real
`_board_identity_check` (UART stub plus a literal-IP host) so the return type
is pinned.

## MED-2: a non-ok install result can still be followed by cancel and clear while the recovery image is applying (FX2 LOW-4 partly open)

`_install_and_verify()` returns `(FAIL, False)` for any result that does not
start with `ok` and did not raise. D01 then takes the cleanup path
(`update_status`, `update_fetch_cancel`, `update_stage_clear`). Of the
non-ok results `update_install_staged` can return, three come after
`uhc.start_install()` was sent:

- `UNKNOWN: board not confirmed ... the recovery image may still be applying`
- `FAILED: install recorded a failure ...`
- `FAILED: install refused or lost: ...` (an `UpdateHttpError` from the POST
  itself, which includes a lost reply after the board accepted it)

In the `UNKNOWN` case the case can cancel and clear the stage against a board
that is mid-apply in the recovery image. That is exactly what LOW-4 was meant
to forbid. Only `error: could not read ...` and `REFUSED: ...` (both before
the POST) prove the install never started.

Fix: treat a result as "install never ran" only when it starts with `error:`
or `REFUSED`. Anything else that is not `ok` should return
`install_may_have_run=True`, run the bounded reboot poll and skip cleanup.
Add a test with an `UNKNOWN:` install result that asserts no cancel or clear
is called.

## LOW-1: pinning a bare IP drops the port

If MED-1 is fixed as written (`ctx["host"] = str(ip)`), a host given as
`name:8080` becomes `10.x.x.x` and later calls go to port 80.
`mcp_server_aux._pin_host()` already handles this. The pinned value also
carries into E01 and F01, so the drop would affect the whole run.

## LOW-2: a post-install `judge_snapshot` FAIL is not test-pinned

Mutation M14 (`after install:` FAIL turned into PASS) was MISSED. No test
drives an install that reboots cleanly onto a snapshot that `judge_snapshot`
fails (crash present, low heap, liveness dead). Add one.

## LOW-3: `_stage_empty`'s `ok` prefix is not test-pinned

Mutation M11 (drop `text.startswith("ok")`) was MISSED. Without that prefix,
an `error:`/`UNKNOWN:` status text that happens to echo `staged=False`,
`busy=False` and `header=none` would count as empty, both at the pre-run check
and at the after-clear read-back. Add a test with such a text.

## INFO

- INFO-1: `judge_snapshot(after)`'s inconclusive list is discarded. The
  install can PASS while some post-reboot fields are unreadable. Only the
  three `_valid_baseline` fields and `fw_build` are enforced. Consider
  INCONCLUSIVE or at least reporting them in `observed`.
- INFO-2: the poll is bounded: a 300 s deadline on the injectable clock, each
  `_snapshot` bounded by its own HTTP/UART timeouts, so there is no hang. A
  transient snapshot failure before the board actually reboots sets
  `saw_down` and can accept the old board. The result is "fw_build
  unchanged", a false FAIL and never a false PASS; the uptime check usually
  catches the real reboot first anyway. The deadline value itself is not
  pinned (M3 MISSED).
- INFO-3: the missing pre-install `fw_build` refusal is not pinned (M15
  MISSED). Without it `before_build` is None, any readable build counts as a
  change, and an install could PASS without proof that the build changed.

Other areas checked, no finding:

- Every other board call in the module. The B01 clear and diag use UART
  (`wait_for_trip_clear` calls `srv.safety_clear_trip()`/`safety_get_diag()`
  with no host). `_read_snapshot`, `_heap_min`, `get_heap_status` and
  `get_boot_guard_status` read `ctx["host"]`. C01's profile start and teardown
  go over UART `srv._profiles`. All are bound to `ctx["host"]` and inherit
  MED-1's unpinned name.
- `install_ran` early return: it skips cancel and clear after an ok or raising
  install (M2 CAUGHT).
- B01 strict reason 4 and mask 0x0008 before the clear: correct.

## Negative tests

`tools\negtest.ps1 -Preset pytest -PresetArg "tests/test_bench_test_cases_release.py"`.
Baseline passed and real tree unchanged. None of these mutations was in FX2's
table. Each CAUGHT log has a `FAILED tests/...` assertion line (test named).

| # | Mutation | Verdict | Failing test |
|---|---|---|---|
| M1 | `_pin_identity` pin write removed | CAUGHT | test_b01_passes_host_and_pins_resolved_ip |
| M2 | `install_ran` early return removed | CAUGHT | test_install_raise_has_own_label_and_never_cancels_or_clears |
| M3 | poll deadline 300 s to 3000 s | MISSED | (INFO-2) |
| M4 | uptime-drop reboot clause removed | CAUGHT | test_install_uptime_drop_confirms_reboot_even_with_same_build |
| M5 | `_valid_baseline` drops heap_min_free | CAUGHT | test_install_unreadable_baseline_field_is_inconclusive[heap_min_free-None] |
| M6 | non-ok cancel not tainted | CAUGHT | test_d01_bad_cancel_taints[FAILED: x] |
| M7 | update_fetch_status drops host | CAUGHT | test_d01_every_update_call_carries_the_pinned_host |
| M8 | update_check drops host | CAUGHT | test_d01_every_update_call_carries_the_pinned_host |
| M9 | raised-but-rebooted install returns PASS | CAUGHT | test_install_raise_has_own_label_and_never_cancels_or_clears |
| M10 | unreadable-baseline INCONCLUSIVE turned FAIL | CAUGHT | test_install_unreadable_baseline_field_is_inconclusive[link_crc_errors-None] |
| M11 | `_stage_empty` drops `ok` prefix | MISSED | (LOW-3) |
| M12 | unchanged-build FAIL removed | CAUGHT | test_install_uptime_drop_confirms_reboot_even_with_same_build |
| M13 | D01 first identity refusal ignored | CAUGHT | test_d01_identity_mismatch_touches_nothing |
| M14 | post-install judge FAIL turned PASS | MISSED | (LOW-2) |
| M15 | missing pre-install fw_build refusal removed | MISSED | (INFO-3) |
| M16 | pre-run update_status drops host | CAUGHT | test_d01_every_update_call_carries_the_pinned_host |

M1 is CAUGHT only because the test fake returns a `str`; see MED-1.

## Fix status

Fixed in bbfa0712d (cases_release.py, test_bench_test_cases_release.py).

- MED-1 / LOW-1: `_pin_identity` pins the real `IPv4Address` through `mcp_server_aux._pin_host` (port kept). Test goes through the real `_board_identity_check`.
- MED-2: only `error:` / `REFUSED` results mean the install never started; every other non-ok result polls for the reboot and skips cancel/clear. Tests for `UNKNOWN`, `FAILED: install recorded a failure`, `FAILED: install refused or lost`.
- LOW-2 (M14), LOW-3 (M11), INFO-2 (M3), INFO-3 (M15): tests added.
- INFO-1: unreadable post-install fields give INCONCLUSIVE.

Negtest (`negtest.ps1 -Preset pytest`, 12 mutations, all CAUGHT with a `FAILED tests/...` assertion line, baseline passed, real tree unchanged): pin write removed, pin as `str(ip)` without port, pin only if str, UNKNOWN treated as pre-POST, FAILED treated as pre-POST, non-ok no longer may-have-run, non-ok reboot gives PASS, deadline 3000 s, stage-empty drops ok prefix, post-install judge FAIL ignored, missing fw_build refusal removed, post-install inconclusive ignored.
