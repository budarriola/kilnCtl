# Review: LD-01/LD-02 start gate and Wi-Fi/web fix batch (2026-10-10)

This is a review only; nothing was fixed. It was done on origin/dev at `f1d9d2567`.

| Batch | Commits | Scope |
|---|---|---|
| A. LD-01/LD-02 | `804f67e85`, `5b794fd7b` | `relay_authority_start_blocked()` start gate (`docs/audits/HOST_TEST_CAMPAIGN_FINDINGS_2026-10-09.md`) |
| B. Wi-Fi/web fixes | `2cd02753c`, `843f4e7d0`, `8ce01a41b`, `45d1efa9f`, `125f44157` | F1-F6, F8 of `docs/audits/REVIEW_WIFI_WEB_BATCHES_2026-10-10.md` |

## Verdict

- There are no HIGH findings.
- The new helper is correct where it is called:
  - It fails closed on a NULL safety pointer: `relay_authority_on_blocked()` treats NULL as `SAFETY_FAULT_SRC_APP`.
  - It fails closed on an uninitialised link (`ESP_ERR_INVALID_STATE`) and on a lock failure (`ESP_FAIL`).
  - It adds no new lock order. `safety_link_get_status()` takes the same safety lock that `safety_link_get_fault_sources()` has just taken and released, at the same point, and neither is called with the executor or autotune lock held differently than before.
- One start path, `profile_executor_resume()`, is not gated (MED-1).
- The test-fidelity gaps and the Wi-Fi status buffer size are LOW.

## Findings

### MED-1. `profile_executor_resume()` has no start gate (LD-01 class on the resume path)

- `profile_executor_status.c` `profile_executor_resume()` checks neither `relay_authority_on_blocked()` nor `relay_authority_start_blocked()`.
- It sets `PROFILE_EXEC_RUNNING` (`profile_executor_status.c:409`, one of only two places that set RUNNING) and calls `heat_enable_acquire_since()`.
- On a link that is down or not yet confirmed, the acquire fails. The executor still reports RUNNING with heat pending, and `heat_enable_reconcile()` keeps retrying.
- So a resume issued during a link outage succeeds as an operation, and heat starts silently whenever the link comes back. A fresh start is refused in exactly the same situation.
- Every resume entry reaches this same function: UART `uart_bridge_ext_control.c`, HTTP `dashboard_exec_http.c`, LCD `ui_page_home_actions.c`, and the internal call in `profile_executor.c`. Gating it once covers all of them.
- Heat is still never energized without the Pico granting K4, so this is a fail-safe gap in operator expectation, not a safety bypass.
- Suggested fix: call `relay_authority_start_blocked(..., "resume")` before the state change, and add a prestart test that mirrors `test_run_refuses_unless_link_positively_up`.

### LOW-1. The `safety_link_get_status()` return-code check is not covered by any test (negtest A2 MISSED)

- The new stubs in both `test_autotune_engine_prestart.c` and `test_profile_executor_prestart.c` memset `*out` even when they return an error.
- So a mutation that ignores the return code still reads `link_up=false` and refuses (A2 below: exit 0, MISSED).
- The real `safety_link_get_status()` leaves `*out` untouched on `ESP_ERR_INVALID_STATE` and on `ESP_FAIL`. The caller's `st` is an uninitialised stack variable, so dropping the rc check on target would read garbage and could pass the gate.
- Fix: make the stubs leave `*out` untouched (or fill it with `0xFF`/`link_up=true`) on the error path.

### LOW-2. Gate tests miss two refusal causes and one heat-request assertion

- `test_run_refuses_unless_link_positively_up` (profile) asserts the refusal and its text. It does not assert "no heat-enable request", which the autotune twin does.
- Neither suite exercises a NULL safety pointer or the `ESP_FAIL` lock-failure path.

### LOW-3. The other relay-ON paths still gate on fault sources only

- These paths gate on `relay_authority_on_blocked()` only, not on positive link-up:
  - aux outputs (`profile_executor_relay_io.c:633`)
  - manual relay writes (`kiln_io_owner.c:264/279`)
  - the UART safety enable (`uart_bridge_safety.c:93`, a direct `safety_link_request_enable()`)
- They are relay writes, not heat starts, and K4 still comes only from the Pico, so this is plausibly by design. The LD-01 finding names only starts.
- Record the decision so a later reviewer does not re-raise it.

### LOW-4. No commit-time recheck between the gate and RUNNING

- `profile_executor_run.c` gates at line 397 but commits RUNNING at line 1534, after validation and plan work, without re-checking the gate.
- A link drop inside that window gives the same state as MED-1 (RUNNING, heat pending). `heat_enable` refuses on a down link, so the effect is minor.

### LOW-5. `GET /api/wifi/status` buffer not grown for the new fields (F3)

- `wifi_provision_http.c:365` `json_cap` is still 680 B. The comment above it records a measured worst case of 677 B + NUL before this change.
- F3 appends `,"saved_nets_refused":false` (+26 B). When refused it appends `true` plus `,"recovery_hint":"<~105 B>"` (about +150 B).
- Worst case is now about 704 B unrefused and about 830 B refused. The truncation path (`n = json_cap - 1`) then sends a body that is not valid JSON.
- This needs every escaped field at maximum length, so it is unlikely in practice. But the refused case is exactly when the operator needs the hint.
- Grow `json_cap` by about 160 B and update the measurement comment.

### LOW-6. F2 is partial: a 429 still discards a reset token the firmware has not spent

- `app.js` `onStep2Submit` now keeps `retryToken` on a 400 `reason:weak_password`.
- But every TOTP failure, including the weak-password refusal, takes the per-IP backoff, and the first step is 5 s.
- A user who retries within 5 s gets a 429, and the 429 branch still calls `forgotBackToStep1()`, which drops the token the firmware has not spent. The new message invites a retry "if it says to try again later".
- Fix: keep the token on 429 as well.

### INFO-1. The weak_password reason is a narrow oracle on an OPEN route

- The new 400 `{"reason":"weak_password"}` makes a weak-password refusal distinguishable from a bad token. The `totp_http_client.py` docstring itself states that the plan "never distinguishes" these.
- The only extra fact it leaks is whether the candidate equals the AP password or fails the strength rules, behind the TOTP backoff and a valid code.
- The usability gain probably outweighs this, but the plan text and the docstring should be reconciled.

### INFO-2. F1: a transient open error at boot now skips legacy adoption for that boot

- With the tri-state probe, an UNREADABLE `saved_nets` record (an open or get error other than NOT_FOUND) makes `migrate_from_default_partition()` return early.
- So a transient error on the migration boot leaves the board unprovisioned for that boot. It may expose the open WIFI_SETUP tier until the next boot retries.
- This is correct fail-closed behaviour for the data and is self-healing. Noted only.

### INFO-3. The bounded read retry has no delay

- `nvs_load_saved_nets_from()` retries 3 times back to back. A transient condition that lasts longer than three NVS calls still latches refused for the boot.
- A few ms of delay would make the retry meaningful.

### INFO-4. F8 is partial: fake_kv still diverges on non-string types, and its header comment is stale

- `fake_kv.c do_get` now refuses a get_blob on a string-written key and a get_str on a blob, matching target NVS.
- u8/u32 values are still stored as non-string blobs, so `get_blob` on a u8 key, or `get_u8`/`get_u32` on a blob key, still succeed in the fake. On target NVS keys are typed and these reads fail.
- `fake_kv.h` lines 38-43 still say get_blob accepts a key "regardless of whether it was written via set_blob or set_str", which now contradicts the code.

F4, F5 and F6 look correct. F4's `wifi_prov_static_ip_config_problem()` texts are covered rule by rule. F5's dirty-listener addition is covered by `test_web_review_fixes.js`.

## Negative tests (`tools/negtest.ps1 -RequireAssertion`)

The A runs used `-Command build_host_tests.ps1 -OutDir {OUT} -Only _prestart` (both prestart suites) and `-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`. The baseline passed. Every CAUGHT below matched `FAIL <file>.c:<line>: <assertion>` lines, not a build failure.

| # | Mutation | Verdict | Assertion that fired |
|---|---|---|---|
| A1 | helper drops `\|\| !st.link_up` | CAUGHT | `test_autotune_engine_prestart.c:3845` "start refused while the link is not positively up", `:3846` "no heat-enable request"; `test_profile_executor_prestart.c:2818` |
| A2 | helper ignores the `safety_link_get_status()` rc | **MISSED** | none: the stubs zero `*out` on error (LOW-1) |
| A3 | autotune site gated on `relay_authority_on_blocked()` only | CAUGHT | `test_autotune_engine_prestart.c:3845-3847` |
| A4 | profile site gated on `relay_authority_on_blocked()` only | CAUGHT | `test_profile_executor_prestart.c:2818` "the refusal is decoded and names the safety link" |
| A5 | helper skips the fault-source check | CAUGHT | `test_autotune_engine_prestart.c:3812-3813`; `test_profile_executor_prestart.c:2791` |
| F8 | `fake_kv.c do_get`: `want_str != is_str` back to `want_str && !is_str` | CAUGHT (see note) | `test_fake_kv.c:142: hal_kv_get_blob(&h, "name", strbuf, &len) == HAL_NOT_FOUND` |

F8 used `-Command firmware\hwAbstraction\test\test_host_fakes.ps1` with the unique `-ExpectPattern 'FAIL .*test_fake_kv\.c:\d+: hal_kv_get_blob\(&h, .name., strbuf'`.

A first F8 run reported MISSED only because the double quotes in the pattern were stripped from the command line, so the pattern never matched. That run's log already showed the `test_fake_kv.c:142` failure under `=== FAILURES ===` (`fake_kv: FAILED (pass=437 fail=1)`). The rerun with the quote-free pattern is the one recorded. The baseline passed and the mutation was CAUGHT. negtest's overall verdict was ERROR "REAL TREE CHANGED" only because this review doc was written into the reviewing worktree while the run was going. `git status` showed that untracked doc as the only change.

The author's own negtests in `804f67e85` matched on exit code only. A1, A3, A4 and A5 show that the new tests catch the gate's removal by assertion. A2 shows the rc branch is untested.
