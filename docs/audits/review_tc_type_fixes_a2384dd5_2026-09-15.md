# Adversarial review: settable safety tc_type, fixes a2384dd5 / 6137b4b5 / b40c894b / a986f170 / 2cc69ebb

2026-09-15. Read-only adversarial review of the five commits that answer
`review_safety_tc_type_fixes_2026-09-15.md` (commit 7251c220, findings N1-N5 and the LOWs).
No flashing, no board access.

Owner rules held fixed for this review: the safety TC type must be settable; while ARMED a change
is accepted only when heat-enable is off AND no firing is running; the Pico stays armed and
reapplies immediately; every other parameter keeps the ARMED refusal; the commissioning page owns
the type and the ESP only reads it back; **there must never be a way that the Pico is not armed.**

## Verdict

**Not closed.** N1 is genuinely and well closed. N2 is closed in substance. N3 is closed on the
Pico and still open on the ESP. N4 is not closed at all for the operator. N5 is closed for the
page and still open for backup export. One of the LOW "fixes" introduced a new, higher-severity
problem: an `abort()` on the safety processor.

Open: F1 (MEDIUM), F2 (MEDIUM), F3 (MEDIUM), F4 (LOW), F5 (LOW), F6 (LOW).

## Finding-by-finding status

### N1 (was HIGH) — CLOSED
`link_task_heat_is_safe_for_tc_type_change()` no longer rests on the ESP's instantaneous relay
flag. The gate is now a pure decision (`SaftyFW/src/tasks/link_task_tc_type_gate.c`) over five
inputs, all ANDed, all fail-closed. Each of the four scenarios the prior review named is now
covered, and I checked each producer rather than the commit message:

1. **Autotune** — `autotune_engine.c:1348` holds `HEAT_ENABLE_CLAIMANT_AUTOTUNE`; the producer
   ORs `heat_enable_is_held(HEAT_ENABLE_CLAIMANT_AUTOTUNE)` into `HEAT_OWNER_ACTIVE`
   (`safety_link_frames.c:508-511`). The claimant enum has exactly two members
   (`heat_enable.h:58-60`), and both are tested, so there is no third claimant left uncovered.
   The only other relay-authority call sites (`kiln_io_owner_command_set_relay_mask_authorized`)
   are profile_executor, autotune_engine_guard, and `uart_bridge.c:469` — the last only ever
   commands relays OFF.
2. **PAUSED firing** — `pstat.state == PROFILE_EXEC_PAUSED` is tested explicitly.
3. **Danger-mode K4** — `danger_mode_active()` is folded in. `danger_mode_set_heat_enable_request()`
   refuses unless the window is open, so no K4 request can exist with `danger_mode_active()` false.
4. **Deferred heat-enable release** — `heat_enable_release()` clears `held_mask`/`granted`
   synchronously, so the ESP-side flags go quiet before the wire `REQUEST_ENABLE(false)` is
   serviced. That window is now covered from the other side: check 1 is
   `safety_core_get_output_status()`'s `relay_energized` = `relay_owner_is_energized()`, the
   Pico's own GPIO ground truth, which stays true until K4 actually opens. This is the right
   shape — the Pico owns that pole, exactly as the owner rule asks.

Unknown state refuses, as required: `degraded_no_context`, never-received context, context lock
unavailable, `!context_valid`, `age >= max_age` and a NULL input all return false. A rebooted or
silent ESP therefore cannot produce an acceptance — the context simply goes stale (5 s) or the
link degrades. The Pico-local heuristics (`s_relay_on_continuous`, `current_task_any_current_present()`)
are retained on top. Boundary behaviour (`>=`, not `>`) is asserted by the new test.

The gate reads the snapshot under `s_context_lock`, copies four scalars, gives the lock back, and
only then decides — no lock is held across the decision or across any producer call.

### N2 (was HIGH) — closed in substance; the comments overstate what the code does (see F4)
`s_tc_type_reapply_pending` makes the skipped reapply a retried condition instead of a one-shot
WARN, logged at ERROR with an explicit "DIVERGED" message, and driven every poll from
`link_task_fn()`. Both handlers (`link_task_handle_set_config`, `link_task_handle_commit_config`)
set and clear the apply-in-progress bracket with no early return in between — I read both
bodies specifically for a `return` between `set(true)` and `set(false)`; there is none, so the
flag cannot be stranded true (which would otherwise have wedged heat-enable off permanently).
A Pico reboot with a pending divergence self-heals, since the boot path configures the part from
the same flash record.

### N3 (was MEDIUM) — closed on the Pico, OPEN on the ESP → **F1**
### N4 (was MEDIUM) — NOT closed for the operator → **F2**
### N5 (was MEDIUM) — closed for the page; backup export still stale → **F6**
`zones_http_get.c` now emits `safety_tc_type_known`, `renderSafetyTcType(code, known)` renders
UNSET when it is false, and `zones_http_client.py` mirrors the new key. The ESP-side push of
`safety_tc_type` stays removed: the only remaining `safety_link_send_set_config()` caller is
`uart_bridge_safety.c` (an explicit PC command), so nothing auto-pushes the ESP's copy on
reconnect. `zones_http_post.c:239` still overwrites any submitted value with its own cache, and
`safety_config_page.html` no longer posts the dead field.

### LOWs
- "heat check untested" — closed. The new `test_link_task_tc_type_gate.c` links the real
  production decision function and passes deliberately different flag bit values, so a decision
  function that hardcoded the real constants would be caught.
- "static buffers single-writer but unenforced" — enforced, but by `abort()` → **F3**.
- "`safety_config_page.html` still posts tc_type" — closed.

## New findings

### F1 (MEDIUM): the two new wire reject reasons have no consumer on the ESP
`KILNLINK_COMMIT_CONFIG_REJECT_ARMED_HEAT_ON (5)` and `_ARMED_HEAT_UNKNOWN (6)` are emitted by
`link_task.c:2460-2480` but appear nowhere on the ESP side:
- `commit_reject_reason_words()` (`safety_cfg_http.c:828-837`) has cases only for
  RANGE/CONTRADICTION/ARMED/STORAGE; 5 and 6 fall to `default: "refused (unrecognised reason)"`.
- `reject_reason_to_refusal_class()` (`:848-856`) likewise defaults to
  `SAFETY_CEILING_REFUSAL_OTHER`.
- The commissioning page's `/ARMED/i` test (`commissioning_shared.js:163`,
  `safety_commissioning_page.html:1984`) matches on the rendered sentence, so it still fails to
  recognise the refusal as ARMED-family.

So the correct, expected refusal ("heat is on right now, try again with heat off") now reads to
the operator as *"refused (unrecognised reason)"*. That is better than N3's "the safety
processor's flash write failed" — it is no longer a lie about a hardware fault — but the prior
review asked for a wire reason **and page text**, and only the wire half landed. Side effect:
`safety_ceiling_reconcile_record_result()` classifies these as OTHER and uses the generic retry
backoff rather than the ARMED fixed backoff. That retries sooner and never gives up, so ceiling
equality is not weakened, but it is not the intended classification.

**Fix:** add both cases to `commit_reject_reason_words()` (with sentences containing "ARMED") and
to `reject_reason_to_refusal_class()` (→ `SAFETY_CEILING_REFUSAL_ARMED`).

### F2 (MEDIUM): N4's mixed-refusal explanation never leaves the Pico
`config_store_write_ex()` now builds a specific sentence for a mixed ARMED refusal
(`config_store_flash.c:1069-1082`), but the COMMIT_CONFIG_REJECTED frame carries only
`param_id` plus a one-byte reason enum (`kilnlink_commit_config_rejected_encode()`: `out[0]`,
`OFF_PARAM_ID`, `OFF_REASON` — no string). The mixed case still maps to
`CONFIG_STORE_WRITE_REFUSED_ARMED` → plain `KILNLINK_COMMIT_CONFIG_REJECT_ARMED` with
`CONFIG_PARAMS_NO_PARAM_ID`. The improved sentence exists only in the Pico's console log, which
the operator at the commissioning page cannot see.

N4 asked that the operator be told another field blocked the tc_type change, and which one. That
is still not true. The prior review's M3, re-raised as N4, remains open.

**Fix:** either a third wire reason (`REJECT_ARMED_MIXED`) with page text, or report the
offending `param_id` instead of the NO_PARAM_ID sentinel.

### F3 (MEDIUM, new — introduced by the LOW fix): `abort()` added to the safety processor's config path
`config_store_only_tc_type_differs()` now calls `abort()` on re-entry
(`config_store.c:1187-1194`), deliberately written as `abort()` rather than `assert()` so it
**fires in the NDEBUG RP2040 target build**. On the Pico an `abort()` resets the processor; a
reset drops K4 and leaves the system with a safety processor that is not armed until it reboots
and clears GRACE. That is precisely the state the owner's hard line forbids, chosen as the
response to a condition that is, by the code's own reasoning, not currently reachable (single
caller, single task).

Trading "a silently wrong comparison" for "the safety processor resets" is the wrong direction
here, and the trip-wire is not race-safe anyway (a plain `volatile bool`, no lock), so it cannot
reliably detect the concurrent case it is written against — only the straight-line one.

**Fix:** on re-entry, log at ERROR and return `false` (i.e. "not a tc_type-only change"), which
fails closed into the existing unconditional ARMED refusal, and leave the processor running.

### F4 (LOW): two of N2's claims are stronger than the code
- **The REQUEST_ENABLE interlock cannot fire for its stated purpose.** The only caller of
  `safety_core_request_enable()` is `link_task_handle_request_enable()` (`link_task.c:1349`),
  dispatched from the same `link_task_handle_raw_frame()` switch, on the same task, that runs the
  SET_CONFIG/COMMIT_CONFIG handlers. No REQUEST_ENABLE frame can be processed between the flash
  write and the reapply — task serialization already closes that window.
  `s_tc_type_apply_in_progress` is harmless defence in depth, but the header comment ("a
  REQUEST_ENABLE landing mid-apply can never race…") describes a race that cannot occur on this
  code path today; it would only matter if a second caller appeared.
- **"keep retrying until it verifies" is not what happens.** `link_task_retry_pending_tc_type_reapply()`
  clears `s_tc_type_reapply_pending` as soon as `thermo_task_request_tc_type_reapply()` has set
  `s_force_tc_reconfigure`, not when CR1 read-back verifies. `thermo_task.c:426-436` consumes the
  flag "once, regardless of what the attempt below finds". The outcome is still acceptable — a
  failed `max31856_configure()` leaves `max31856_tc_type_verified()` false, and
  `max31856_reconfig_retry_should_attempt()` then takes over — but the ERROR log's promise and the
  code comment are inaccurate about which mechanism finishes the job.
- `s_tc_type_reapply_pending_value` is written and then explicitly discarded (`(void)`), i.e. dead.

### F5 (LOW): the new context flag's PRODUCER has no test, and the one test that compiles it is blinded
All new coverage is on the consumer side. On the ESP, `test_safety_link_compile.c` is the only
executable that compiles `safety_link_frames.c`, and a986f170 satisfied its linker with
`fake_danger_mode_for_safety_link.c`, which returns fixed `false` for both `danger_mode_active()`
and `heat_enable_is_held()`. That is a reasonable stub choice for a wire-decode test, but it means
nothing anywhere asserts that `HEAT_OWNER_ACTIVE` is actually set for a PAUSED firing, a held
autotune claim, or danger mode — the exact four cases N1 exists to cover. A small pure helper
(state + two held bools + danger bool → flag) would be host-testable the same way the Pico-side
gate now is.

### F6 (LOW): backup export still records the ESP's possibly-stale copy
`backup_export.c:401-403` still writes `zones_config_get_safety_tc_type()` — the ESP's own cached
value, not the Pico mirror `zones_get_safety_pico_tc_type()` that N5 taught the GET path to
prefer. A backup taken before the first Pico read-back captures the stale value. Import no longer
writes the field to the Pico, so the consequence is a misleading backup file, not a wrong device.

## Specifically assessed

- **Can any interleaving leave the Pico unarmed?** Not through the tc_type feature itself:
  `config_store_decide_write_ex()` keeps the Pico ARMED throughout and no new path disarms,
  drops to GRACE, or reboots. The one new way to lose the armed state is **F3's `abort()`**.
- **`abs_max_temp_c` equality.** Untouched by these five commits. The ceiling sync path still
  goes through SET_PARAM + COMMIT_CONFIG, and a ceiling change is never a tc_type-only change, so
  it keeps the unconditional ARMED refusal and its existing reconcile loop. The only behavioural
  delta is F1's backoff classification, which retries sooner rather than later.
- **Is the reapply immediate, and what if it fails halfway?** Immediate in the accepted case
  (flag set in the handler, consumed on thermo_task's next cycle, and any in-flight conversion
  under the old CR1 is discarded). Half-failed cases: heat became unsafe → flash holds the new
  type, chip holds the old, logged at ERROR and retried (F4's caveat about which mechanism
  finishes); `max31856_configure()` itself fails → `tc_type_verified()` stays false, every
  snapshot is published invalid, and the reconfig-retry path keeps trying. A reboot mid-divergence
  self-heals from flash. No path leaves a silent, unreported flash/chip split.
- **`UART_PROTOCOL_VERSION`.** 12 on both sides, no drift: `uart_task_ids.h:191`
  `#define UART_PROTOCOL_VERSION ((uint16_t)12)`; `test_uart_version_independence.py:127-128`
  asserts the firmware literal is 12 and `protocol.UART_PROTOCOL_VERSION == 12`. The separate
  ESP↔Pico `KILNLINK_PROTOCOL_VERSION` is 15 and was not bumped — the new context flag bit and
  the two new reject reason values are purely additive, and an older peer ignores both, so this
  is defensible, but it is worth noting that the reject-reason enum did widen without a bump.
- **Constraint checks.** `ZONES_CFG_VERSION` untouched (26). No new NVS keys. No `xTaskCreate`
  changes (no stack words/bytes confusion introduced). No lock held across a producer or blocking
  call in any new code — the gate releases `s_context_lock` before deciding, and
  `heat_enable_is_held()` takes only its own short lock inside the context builder.

## Are the new tests load-bearing?

**The SaftyFW gate test cannot be run against the parent commits, and it is important to say so
plainly: it is new-code coverage, not a regression catcher for the old code.**
`link_task_tc_type_gate.c`/`.h` and `test_link_task_tc_type_gate.c` are all new in a2384dd5; the
function under test does not exist at 7432f6c9, so "how many of these fail against the parent" has
no meaningful answer — the executable does not build there. The honest equivalent question is
whether the test would fail against the *old behaviour*, and the answer is yes for the one check
that names the N1 fix, which is what my own poison test reproduces below. The other checks
(staleness, boundary, NULL, the two Pico-local heuristics) restate behaviour the old inline
function already had; they are useful lock-in, not new evidence.

The KilnFW-side change (a986f170) adds a fake, not a test — see F5.

## Verification evidence

Clean detached worktree `C:\wt\rvtc` at `origin/main` = `2cc69ebb`, `git submodule update
--init --recursive` run. All builds from that worktree; never from the main dirty tree; never from
a prebuilt binary.

| Check | Result |
| --- | --- |
| SaftyFW host tests (`build_host_tests.ps1`) | **2560/2560**, plus 56/56 and 259/259 auxiliary executables, all passed |
| SaftyFW target build (`check_00_saftyfw_target_build.ps1`) | **PASS** — `SaftyFW.elf`, `SaftyFW_slotA.elf`, `SaftyFW_slotB.elf` linked (confirms b40c894b's CMakeLists registration) |
| KilnFW host tests (`App/test/build_host_tests.ps1`) | **46/46 built and passed** (2 documented SKIPs: gitignored `logs/coupling/*.jsonl` captures). Confirms a986f170 |
| KilnFW target build | **Not attempted** — known broken at origin/main (`safety_ceiling_sync.c` `-Werror=format-truncation`, `safety_cfg_http.c` implicit `hal_time_now_us`), owned by another agent. Not caused by these five commits |
| `tools/run_all_checks.ps1` | **NOT RUN** — the fresh worktree has no `tools/PcTools/.venv`, so the ~94-check suite could not be run there. The two checks that matter most for these commits were run individually, above |
| PcTools `selfcheck_zones_fields.py` (2cc69ebb) | **Not re-run** for the same venv reason; the change is a one-key addition to `_TOP_READONLY_OR_STRUCTURAL_KEYS` and was read instead |

### My own negative test (poison → fresh full rebuild → hand restore → fresh full rebuild)

1. **Poison v1**: deleted `context_flag_heat_owner_active` from `unsafe_mask`. Full build into
   fresh `C:\wt\rvtc_negbuild`: **build failed**, `link_task_tc_type_gate.c(9): error C2220`
   (unused parameter, warning-as-error). That is itself evidence the parameter is load-bearing,
   but it is not a test failure, so I restored by hand and re-poisoned.
2. **Poison v2**: `(void)context_flag_heat_owner_active;` plus the same mask deletion — i.e. the
   N1 fix removed, everything else identical. Full build into fresh `C:\wt\rvtc_negbuild2`:
   **2559/2560, 1 FAILURE(S)** —
   `test_link_task_tc_type_gate.c:128: HEAT_OWNER_ACTIVE set refuses -- this is the N1 fix
   itself...`. Exactly one check failed, and it is the one that names the finding.
3. **Restored by hand** (scripted text replacement of the exact original three lines; never
   `git checkout --`, `git restore` or `git stash`). `git diff` = **0 lines**, `git status
   --porcelain` shows only pre-existing untracked build dirs.
4. **Forced full rebuild into a fresh directory** `C:\wt\rvtc_negbuild4`: **2560/2560, all
   passed**, plus 56/56 and 259/259. No poisoned object survived into the final measurement —
   each of the three builds used its own `-OutDir`.

## Summary by severity

| ID | Sev | Status |
| --- | --- | --- |
| N1 heat-owner gate | HIGH | **CLOSED** — all four scenarios covered, fail-closed on unknown, negative-tested |
| N2 flash/chip divergence | HIGH | **CLOSED in substance** (see F4 for two inaccurate claims) |
| F1 new reject reasons unhandled on the ESP | MEDIUM | **OPEN** (N3 half-closed) |
| F2 mixed-refusal reason never reaches the operator | MEDIUM | **OPEN** (N4 not closed) |
| F3 `abort()` on the safety processor | MEDIUM | **OPEN, newly introduced** — the only new way to leave the Pico unarmed |
| F4 N2's interlock is a no-op; "retries until verified" is inaccurate | LOW | OPEN (documentation/dead code) |
| F5 `HEAT_OWNER_ACTIVE` producer untested, its one compiling test blinded by a fake | LOW | OPEN |
| F6 backup export still records the ESP's stale copy | LOW | OPEN (N5 otherwise closed) |
