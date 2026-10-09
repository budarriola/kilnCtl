# Re-review: safety tc_type fixes (0d70f190, 917ca784, b9e1683c)

2026-09-15. Adversarial, read-only re-review against
`review_safety_tc_type_rework_2026-09-15.md` (commit c722a442), findings B1, H1, H2, M1-M3,
L1 and L2. No flashing.

Owner rule: while ARMED, a tc_type change is accepted ONLY when heat-enable is off AND no
firing is running. The Pico stays armed. Every other parameter is refused. The commissioning
page owns tc_type and the ESP only reads it back.

## Verification (clean worktree `C:\wt\tcrev` at origin/main b9e1683c, removed afterwards)
- SaftyFW host tests: 259/259 pass.
- SaftyFW target build: PASS (slot A and slot B ELFs linked).
- KilnFW host tests: 46/46 executables built and passed. Two SKIPs are gitignored captures.

B1 is closed: HEAD builds and tests without the dirty tree.

## Verdict

**Not closed.** The build blocker, M1 (persisted record) and L2 (honest reason) are fixed.
H1 is only partly fixed. The new "heat requested" input is still an instantaneous relay
state, not heat-enable, and three real heating modes read as "safe". The new post-write
re-check creates a Pico-side divergence of its own. On the wire, a heat-on refusal is reported
as a flash failure.

## Findings

### N1 (HIGH): CONTEXT_FLAG_HEAT_REQUESTED is the PWM relay instant, not heat-enable, so H1 is only partly closed
`link_task_heat_is_safe_for_tc_type_change()` (`SaftyFW/src/tasks/link_task.c:1533-1570`)
now refuses when the context is stale, never received, or DEGRADED_NO_CONTEXT, or when
HEAT_REQUESTED or PROFILE_RUNNING is set. The staleness handling is correct. A link
re-init clears `s_context_published`, and a 5 s age limit applies, so a stale or reset link
fails closed.

The facts themselves are still ESP-reported, and they do not encode what the owner rule
needs:
- **HEAT_REQUESTED** is set in `KilnFW/App/drivers/safety/safety_link_frames.c:484-502`
  only when some zone's `pstat.zones[i].relay_commanded_on` is true. That is the executor's
  per-tick chopped relay command (`profile_executor_relay_io.c:108`), the same PWM instant
  as `relay_now_mask`. It reads false in every off-window.
- **PROFILE_RUNNING** is set only for `PROFILE_EXEC_RUNNING` (`safety_link_frames.c:494`).
- The Pico still keeps no heat-enable state of its own. `link_task_handle_request_enable()`
  (`link_task.c:1339`) forwards the request and stores nothing.

Scenarios accepted while ARMED today, each in a relay-off instant:
1. **Autotune.** The engine holds `heat_enable_acquire(HEAT_ENABLE_CLAIMANT_AUTOTUNE)`
   (`autotune_engine.c:1348`). It drives relays directly through
   `kiln_io_owner_command_set_relay_mask_authorized()` (`autotune_engine_guard.c:120`),
   not through the executor, so neither flag is ever set. Between relay pulses at
   temperature all three checks read "safe".
2. **A PAUSED firing.** The state is `PROFILE_EXEC_PAUSED`, so PROFILE_RUNNING is clear and
   no relay is commanded. The firing is not over (`profile_executor_run.c:220` treats PAUSED
   as live), and a resume heats on the new type.
3. **Danger-mode K4 enable** (`danger_mode.c:98`). Heat-enable is explicitly requested, but
   no executor relay is commanded, so the flags are 0.
4. **The deferred heat_enable release window** (1c8d7f6e). See the interaction note below.

**Fix:** report heat-enable itself. `heat_enable_is_granted()` or any claimant held is a
suitable source (`heat_enable.h:131`). Treat any executor state other than IDLE, DONE or
FAULTED as "firing running", and add an autotune-running flag. Better still, have the Pico
record the last REQUEST_ENABLE value itself and require it to be false. This function has no
test: 917ca784's own negative test caught nothing, as its message admits.

### N2 (HIGH): "reapply skipped" commits the new type to flash while the sensor keeps running the old one
In SET_CONFIG and COMMIT_CONFIG, the post-write re-check (`link_task.c:1625-1632` and
`:2338-2345`) runs after `config_store_write_ex()` has already persisted the record. If the
re-check fails, only a WARN is logged. The consequences:
- `config_store_get_tc_type()`, the flash record, and the 0x0105 value that the ESP now
  mirrors and displays (917ca784) all say the NEW type.
- The MAX31856 keeps the OLD CR1 until an unrelated reconfigure. The one-shot
  `s_force_tc_reconfigure` is never set. The retry in `thermo_task.c:418-430` fires only while
  unverified, and the old type is verified.
- Nothing retries the reapply once heat drops again.
- The next boot, or any retry-driven reconfigure, silently switches the type.

This is the "Pico believes X, runs Y" state that 917ca784 set out to stop showing. It is also
a reset-one-side pair: the persisted tc_type and the part's live CR1.

The "re-check before the flash write" is also not a second check. It is the same single
sample passed as `heat_safe` into the write.

**Fix:** keep a sticky "reapply pending" flag that is serviced (and logged) once heat is safe
again, and expose it in status. Alternatively, sample immediately before the write and refuse
the write rather than committing a type the part is not running.

### N3 (MEDIUM): a COMMIT refused for heat reaches the page as "flash write failed"
`link_task.c:2355-2359` maps a refusal to `KILNLINK_COMMIT_CONFIG_REJECT_ARMED` only by
`strcmp` against the plain REFUSED_ARMED sentence. The new HEAT_ON reason (and HEAT_UNKNOWN)
strings differ, so they map to `KILNLINK_COMMIT_CONFIG_REJECT_STORAGE`. The consequences:
- `safety_cfg_write.c:314` renders "the safety processor's flash write failed".
- The commissioning page's `/ARMED/i` tests do not match (`safety_commissioning_page.html:2066`,
  `commissioning_shared.js:163`).
- `reject_reason_to_refusal_class()` classifies the refusal as a storage fault, not ARMED.

So the expected, correct refusal ("heat is on, try again when it is off") reads to the
operator as a hardware fault.

**Fix:** map on the decision enum, not on a string, and add a wire reason
(e.g. `REJECT_ARMED_HEAT_ON`) with page text.

### N4 (MEDIUM): prior M3 still open, and a mixed COMMIT gets only the generic ARMED reason
b9e1683c extended `test_config_params_commit_refused_while_armed()`. A tc_type change
bundled with any other staged field, or with a byte changed by finalize or by the recomputed
`calibration_missing` (`link_task.c:2310-2312`), still returns plain REFUSED_ARMED with
`CONFIG_PARAMS_NO_PARAM_ID`. The operator is not told that another field blocked the tc_type
change, or which field.

The review asked for a specific reason, and none was added. This is fail-safe, so it is not
HIGH.

### N5 (MEDIUM): zones GET silently falls back to the stale ESP copy when the mirror is unknown
`zones_http_get.c:241-242` initialises `live_pico_tc_type = s_zones.cfg.safety_tc_type` and
keeps that value when `zones_get_safety_pico_tc_type()` returns false. The comment says this
is deliberate ("never left blank").

The accessor's own contract says an unknown mirror must NOT be shown as a real value. After
boot, and before the first 0x0105 fetch, the page shows the stale ESP value (never updated
since F3, and possibly set by an old backup import) as the Pico's type. `renderSafetyTcType()`
(`zones_page.html:1817`) already supports "UNSET".

**Fix:** emit null or unknown, and render "unknown (not yet read from safety processor)".
The rest of H2 is fixed: `backup_import.c:1207` no longer writes the field, and GET otherwise
reads the mirror.

### L1 (LOW): heat-safety sampling path is untested
No harness reaches `link_task_heat_is_safe_for_tc_type_change()` or the skip branch. N1 and
N2 would both have been caught by a link_task-level test with a context frame whose flags are
0 while autotune or pause is active, or while the context flips after the write.

### L2 (LOW): static comparator buffers, prior L1, unchanged
`config_store_only_tc_type_differs()` gained a caller in `config_store_write()`
(`config_store_flash.c:1046`). All three store-write call sites are still on link_task
(`link_task.c:1604`, `:1696`, `:2316`). It is still single-writer in practice, but nothing
enforces it.

### L3 (LOW): leftover echo of safety_tc_type
`safety_config_page.html:291` still posts `safety_tc_type` back to `/api/zones`. The POST
handler ignores it, so this is harmless, but it is dead UI and suggests the ESP owns the value.

## Interaction with the deferred heat_enable release (1c8d7f6e)
The review worried that a tc_type change right after Stop might be refused briefly. It will
not be, because no Pico input reads heat_enable (N1). As soon as the executor leaves RUNNING
and commands the relays off, the next context frame (well inside 5 s) reads "safe". The
change is then accepted even while REQUEST_ENABLE(false) is still pending in
`heat_enable_service_pending_release()`, so heat-enable is still granted.

The owner rule is therefore violated in a narrow window, rather than the change being refused.
Once N1 is fixed so that heat-enable is reported, a brief refusal after Stop becomes the
expected behaviour. N3 must be fixed first, or that refusal will read as "flash write failed".

## Prior findings, status
- **B1** (build): CLOSED (0d70f190). Clean-worktree builds and tests pass.
- **H1** (heat-safe inside the PWM off-window): PARTIAL. Staleness and profile RUNNING are
  closed. Heat-enable, autotune, PAUSED and danger mode are open (N1).
- **H2** (ESP read-back shows its own copy): MOSTLY CLOSED. The unknown-mirror fallback is
  still open (N5).
- **M1** (volatile fields persisted): CLOSED. `s_persisted_record` is seeded at boot load
  (`config_store_flash.c:695`) and updated only after a confirmed flash write (`:1175`).
  A failed or refused write leaves it untouched, and the volatile install (`:1299`) touches
  only the RAM cache. Both sides are covered, and a test was added.
- **M2** (check and apply not atomic): REPLACED by N2. The re-check now exists, but its
  failure branch leaves a divergence.
- **M3** (mixed COMMIT reason): OPEN (N4).
- **L1** (static buffers): unchanged (L2).
- **L2** (misleading HEAT_ON reason from `config_store_write()`): CLOSED. HEAT_UNKNOWN is
  added, and its only caller is SET_CT_CAL (`link_task.c:1696`). That path does a
  read-modify-write of the committed record and never changes tc_type, so the real
  commissioning flow (SET_PARAM + COMMIT_CONFIG through `config_store_write_ex`) cannot hit
  it. tc_type does not become unsettable in practice.

## Checked and OK
- **The Pico stays ARMED.** No new path disarms.
- **Other parameters are still refused while ARMED.** The comparator is unchanged and compares
  against the persisted record.
- **abs_max_temp_c.** None of these commits touches the ceiling equality or divergence logic.
- **Protocol test literal.** `test_uart_version_independence.py` now expects 12 on both sides,
  which is consistent.
