# Review: devbreakfx9 (autotune host-test stub fix), 2026-10-12

Adversarial review of one dev commit:

| SHA | Subject |
|---|---|
| `e5c057fe5` | autotune host test: link-up stub reports a fresh DIAG so the start gate passes |

Background: `docs/audits/REVIEW_SAFETYFX7_2026-10-12.md` MED-1. Since `7592416a0`, `autotune_begin_run_locked()` (`autotune_engine.c:1102`) goes through `relay_authority_start_blocked()`. That gate now needs a DIAG received since the Pico's last reboot, no older than `SAFETY_LINK_DIAG_FRESH_MS` (`relay_authority_pico_diag_fresh()`, `relay_authority.h`). The stub `safety_link_get_status()` in `test_autotune_engine_prestart.c` never reported a DIAG, so every positive-control autotune start was refused and the KilnFW host suite was red.

The fix makes the stub report `diag_ever_received = diag_since_reboot = true` and `diag_frame_age_ms = 0`, in link-up mode (`s_stub_link_mode == 0`) only.

The review was done in worktree `C:\wt\rvdevbreakfx9_vsgnum` at `origin/dev` = `e5c057fe5`.

**Verdict: 0 HIGH, 0 MED, 1 LOW, 2 INFO.** The fix is correct and turns the suite green. It does not weaken any test that was meaningful before `7592416a0`. The DIAG-freshness rule is still pinned, but only in `test_profile_executor_prestart.c`, not at the autotune call site.

## Full suite

`build_host_tests.ps1` with no `-Only`, private `-OutDir`, at `e5c057fe5`: `Built: 99/99 executables`, `all 99 host test executables built and passed`, exit 0. No `FAIL` line, no `RUN FAILURES`, no `BUILD FAILURES`.

## Question 1: is the autotune DIAG-freshness gate now untested?

### LOW-1: no autotune-level test refuses a start on a stale, pre-reboot or never-received DIAG

- **Where:** `test_autotune_engine_prestart.c:280-300`. The stub has only three modes: 0 = up with a fresh DIAG, 1 = never up, 2/3 = error return. No mode reports "link up, DIAG stale" or "link up, DIAG not since reboot". So no test in this executable can reach the gate's `!relay_authority_pico_diag_fresh()` branch.
- **Proof (negtest, `-Only "autotune_engine_prestart|relay_authority"`):** that selects `autotune_engine_prestart`, `link_watchdog` and `main_reset_fence`.
  - M2: `diag_frame_age_ms <= FRESH` changed to `(... || 1)`. **MISSED.** The mutant compiled, and all three executables passed.
  - M3: `relay_authority_pico_diag_fresh()` body changed to `return (st != NULL) || true;`. **MISSED.** The mutant compiled, and all three executables passed.
- **Mitigation:** the rule is still pinned elsewhere.
  - The helper is a `static inline` in `relay_authority.h`, and both engines call the same helper.
  - `test_profile_executor_prestart.c` pins it directly: lines 13782-13796 test the age boundary at FRESH and FRESH+1, no DIAG since reboot, and a DIAG from before the reboot. Lines 2359-2365 test the resume path.
  - The same M2 and M3, run with `-Only profile_executor_prestart`, are both **CAUGHT**. These were compiled mutants with failing assertions at `:2359`, `:2361`, `:2365`, `:13785`, `:13786`, and for M3 also `:13790`, `:13791`, `:13795`, `:13796`.
  - The autotune call site itself is pinned too. M1 (`if (0 && relay_authority_start_blocked(...` in `autotune_engine.c`) is **CAUGHT** by `test_autotune_engine_prestart.c:3844-3846` and `:3877-3879`.
- **What stays unpinned:** an autotune-only regression that keeps the link check but drops the DIAG check would pass every test. Examples are autotune replacing the shared helper with its own inline link check, or passing a status struct that bypasses the fresh check.
- **History:** `REVIEW_SAFETYFX7` MED-1's fix list asked for "one negative case: an autotune start with a stale DIAG is refused". That case was not added.
- **Fix:** add a stub mode such as 5 = "link up, DIAG stale", driven by a `diag_frame_age_ms` / `diag_since_reboot` / `diag_ever_received` override. Add one test that calls `autotune_engine_run()` with each of the three not-fresh shapes. Each must return false, make no heat-enable request, and give an `errbuf` containing "waiting for the safety processor".

## Question 2: do the "link down" refusal tests still refuse for the intended reason?

Yes. With the fix, mode 1 reports `link_up = false` and no DIAG, and modes 2/3 return an error. In `relay_authority_start_blocked()`, the `link_up` / return-code check runs before the DIAG checks, so these tests stop there.

- Both tests pin the reason text:
  - `test_begin_run_refuses_on_down_link_with_latched_link_source` checks for "heat is blocked" (`:3846`).
  - `test_begin_run_refuses_unlatched_link_down` checks for "safety link is down" (`:3879`) in modes 1-3.
- The fix applies the fresh DIAG to mode 0 only, so it cannot make a link-down mode pass.
- Every other autotune refusal test was spot-checked: lines 3335, 3380, 3424, 3463, 3515, 3575, 3607, 3639, 3672, 3720 and the helper at 1212. Each one pins its own reason with a `strstr` on the error text, such as "thermocouple", "on/off", "monitor-only", "sweep", "update", "zones configuration changed", "factory reset in progress", "danger mode is active" or "profile". So none of them can pass on an incidental DIAG refusal.

### INFO-1 (pre-existing): mode 4 (NULL `s_at.safety`) asserts only `!ok`

`test_autotune_engine_prestart.c:3879` skips the reason check when `mode == 4`. With `s_stub_blocked_real_semantics`, the refusal comes from `relay_authority_on_blocked()` ("heat is blocked", `SAFETY_FAULT_SRC_APP`). It is not pinned. This is not introduced by `e5c057fe5`.

## Question 3: any other test with a similarly stale stub hidden by `-Only` runs?

No.

- Only two host-test executables compile a caller of `relay_authority_start_blocked()`:
  - `test_autotune_engine_prestart.c`, which includes `autotune_engine.c`.
  - `test_profile_executor_prestart.c`, which includes `profile_executor_run.c` and `profile_executor_status.c`.
- Eleven other test files stub `safety_link_get_status()`: `ct_leak_alarm_service`, `danger_mode`, `dashboard_exec_http_handlers`, `dashboard_http_relay`, `dashboard_safety_ready`, `diagnostics_http`, `ota_http`, `safety_cfg_http`, `safety_link`, `safety_link_compile` and `zones_http`. None of them reaches the start gate or `relay_authority_pico_diag_fresh()`.
- `test_profile_executor_prestart.c`'s stub already defaults to a received, since-reboot, age-0 DIAG (`g_stub_diag_received` / `g_stub_diag_since_reboot` / `g_stub_diag_frame_age_ms`, lines 690-718).
- The full 99/99 run above confirms nothing else is red.

### INFO-2: the stub's DIAG state is implicitly 0

The fix relies on `memset(out, 0, ...)` leaving `diag_state` at 0, which is not `SAFETY_LINK_DIAG_STATE_TRIPPED`. The comment says so. A future enum reorder that made 0 mean TRIPPED would turn every autotune positive control red rather than green, so this fails loudly, not silently.

## Negative tests

All were run with `tools\negtest.ps1`, a `-Command` of `build_host_tests.ps1 -OutDir {OUT} -Only <regex>`, and `-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`. Every baseline passed. Each run reported the real tree unchanged and its copies removed. Every CAUGHT below is a compiled mutant with failing `FAIL file:line` assertions, not a build failure.

| Mutation | File | `-Only` | Verdict |
|---|---|---|---|
| M1 drop the start-gate call (`if (0 && relay_authority_start_blocked(...`) | `autotune_engine.c` | `autotune_engine_prestart\|relay_authority` | CAUGHT (`:3844-3846`, `:3877-3879`) |
| M2 DIAG age check always true | `relay_authority.h` | `autotune_engine_prestart\|relay_authority` | **MISSED** (LOW-1) |
| M3 `relay_authority_pico_diag_fresh()` always true | `relay_authority.h` | `autotune_engine_prestart\|relay_authority` | **MISSED** (LOW-1) |
| M2 (same) | `relay_authority.h` | `profile_executor_prestart` | CAUGHT (`:2359`, `:2361`, `:2365`, `:13785`, `:13786`) |
| M3 (same) | `relay_authority.h` | `profile_executor_prestart` | CAUGHT (adds `:13790-13796`) |

The first attempt at the last two rows used `-Only "^profile_executor_prestart$"`. That matched no executable, so the baseline errored and the run was discarded. The rerun with an unanchored selector is the one reported.
