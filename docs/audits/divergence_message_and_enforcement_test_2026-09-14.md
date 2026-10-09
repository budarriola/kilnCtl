# Divergence message and enforcement test -- 2026-09-14

Follow-up to the opus review at
`docs/audits/pico_ceiling_mirror_and_rate_guard_2026-09-14.md` (commit
`d22431d0`, reviewing `7adf191b`), closing its three named findings (A/B, C,
and the summary table's #A/#B/#C). Owner scope for this pass: `config_
divergence.{c,h}`, `safety_ceiling_sync.{c,h}`, `main_control_bringup.c`'s
hook installation, and their tests. Concurrent agents own `adaptive_tune*`,
`kiln_cfg_store.{c,h}`/`kiln_package.{c,h}`, and the board -- none of those
were touched here.

## Defect 1 -- the self-contradicting all-unknown message (FIXED)

`config_divergence.c`'s fallback branch (reached when every individual field
agrees yet the two identities still fail to match -- the case that can only
happen when `format_version` differs, or when **every field is unknown on
both sides**) used to print:

```
config divergence: identity mismatch (format_version 1 vs 1, hash 0xBE20868A vs 0xBE20868A)
```

for the all-unknown case -- an ALARM that disables heaters while naming
IDENTICAL versions and IDENTICAL hashes. An operator has no way to act on
that and would reasonably read it as the system being broken, not the
config. Latent today because the one-field identity set
(`abs_max_temp_c` only) never reaches this branch from its real caller
(`safety_ceiling_sync.c`'s `target_known` gate short-circuits first), but a
concurrent session is adding fields to this same identity for the multi-kiln
profile feature, which makes an all-unknown (or partially-unknown) hash
collision reachable imminently.

**Fix:** `config_divergence_check()`'s fallback now distinguishes the three
distinct causes that can reach it, and reports each honestly:

1. **Format version actually differs** -- reports the two version numbers
   and says outright that values cannot be compared across versions.
2. **One or more fields unknown on either side** (the all-unknown case, and
   the general N-field case) -- names EVERY field that is unknown on either
   side, e.g.:

   ```
   config divergence: one or more fields not yet confirmed on both sides (abs_max_temp_c, max_rate_c_per_min) -- cannot confirm the configs match
   ```

   Measured via `test_config_divergence.c`'s new
   `test_all_unknown_message_is_honest()` (two unknown fields on both
   sides): **142 of 160 bytes (`CONFIG_DIVERGENCE_REASON_MAX`)** -- fits
   with 18 bytes to spare. The length is measured with `strlen()` and
   asserted (`TEST_CHECK(len < sizeof(reason) - 1, ...)`) against a
   poison-filled buffer, not assumed to fit -- the same discipline the
   existing `test_reason_buffer_is_never_truncated()` test already used,
   extended to this new branch. The production code itself also asserts:
   the field-name-listing branch calls `abort()` if `snprintf()`'s return
   value ever indicates truncation, rather than trusting the return value
   unchecked (the repo's own prior incident: a message silently truncated
   at 162 bytes into a 96-byte buffer went unnoticed until this same
   review pass).
3. **Neither version nor known-ness differs, yet the hash still disagrees**
   (an FNV collision, or an internal inconsistency) -- reports the hash
   values with an explicit "possible hash collision" framing, which is what
   the old message was honestly describing in the one case it could ever
   legitimately fire.

Files: `firmware/KilnFW/App/drivers/safety/config_divergence.c`.

## Defect 2 -- logging an action not taken (FIXED)

`safety_ceiling_sync_set_disable_heat_hooks()` used to be called in
`main_control_bringup.c` right before `safety_link_set_context_sources()`,
well after `safety_link_start()` (which starts `safety_poll_task`, the task
that can observe link-up and call `safety_ceiling_sync_reconcile_on_link_up()`
the moment the link comes up). Everything in between --
`danger_mode_init()`, `heat_enable_init()`, `kiln_io_owner_start()`, the
flash-safe worker, `relay_cycles_init()`, `profile_executor_start()` -- ran
with the hooks still `NULL`. A divergence found in that window logged
`"ALARM: ... -- heaters disabled (all relays forced off, any run halted)"`
while `s_disable_all_relays_off`/`s_disable_halt_run` were both `NULL` and
neither call happened -- a truthfulness defect (the verdict
`safety_ceiling_sync_is_diverged()` reads was never wrong, and the window is
short and entirely pre-HTTP, so no safety hole), but exactly this repo's
"logging unchecked success" shape.

**Fix chosen: install the hooks as early as they can be installed**, rather
than teach `safety_ceiling_sync.c` to log accurately about an absent hook.
`safety_ceiling_sync_set_disable_heat_hooks(main_control_bringup_all_relays_
off_void, profile_executor_halt)` now runs immediately after the
`safety_link_start()` block (right before `danger_mode_init()`), closing the
window to nothing. This is safe because both real actions already tolerate
being called before their own subsystem starts:
`kiln_io_owner_command_all_relays_off()` fails closed through
`post_and_wait()`'s NULL-queue check the same way every other
`kiln_io_owner_command_*()` call does before `kiln_io_owner_start()` runs,
and `profile_executor_halt()` is documented to tolerate being called
before/without `profile_executor_start()` (the RECOVERY MODE call site
already relied on this). The rejected alternative (an `ESP_LOGE` once on a
NULL hook) was rejected because it would still let real enforcement no-op
during bring-up, merely announce it.

Files: `firmware/KilnFW/App/main_control_bringup.c`.

## Defect 3 -- the cited test did not exist (FIXED)

`safety_ceiling_sync.h`'s doc comment on
`safety_ceiling_sync_set_disable_heat_hooks()` named
`test_safety_ceiling_sync_divergence.c` as the place a host test observes the
hooks -- that file did not exist. The enforcement path (hooks firing on
divergence, the latch, the `target_known` exclusion, the uninstalled-hook
case) had **no test at all**.

**Fix:** `firmware/KilnFW/App/test/test_safety_ceiling_sync_divergence.c`,
its own executable (`build_host_tests.ps1`'s fortieth), linking the REAL
`safety_ceiling_sync.c`/`safety_ceiling_policy.c`/`config_divergence.c` and
supplying this file's own fakes for `zones_config_is_valid()`/
`_get_temp_limits()`, `safety_cfg_store_param_count()`/`_get_by_index()` and
`safety_cfg_http_set_and_confirm_f32()` (same "fake the seam, link the real
file under test" convention `test_zones_http.c`/`test_safety_ceiling_policy.c`
already use). Covers, using the real production functions:

1. **Uninstalled hooks are a safe no-op** -- with no hooks installed (this
   module's actual default, and the real boot window before this defect-2
   fix used to leave it in), a real divergence still sets the verdict
   (`safety_ceiling_sync_is_diverged() == true`, non-empty reason) but calls
   nothing. Runs first in the process, since the module has no
   unset-hooks-again entry point.
2. **Hooks fire on divergence** -- both hooks called once on a real
   mismatch, reason names the diverged field.
3. **Latch persists** -- a divergence `guard_raise()` cannot resolve (Pico
   already wider than the ESP's target, so there is nothing to raise) keeps
   firing both hooks on every subsequent tick, three ticks checked.
4. **Latch clears on agreement** -- once the Pico's cache reports the same
   value the ESP holds, the very next tick clears `is_diverged()` and the
   reason, and the hooks are NOT called again.
5. **`target_known` exclusion** -- an all-zero ESP config (no zone has ever
   had a positive `max_temp_c`) is never reported as a divergence, checked
   against both an unconfirmed AND a real-valued Pico side.

**Negative test (2026-09-14, run and reverted in this same session):**
commented out the `if (s_disable_all_relays_off) { s_disable_all_relays_
off(); }` call in `enforce_ceiling_divergence()`
(`safety_ceiling_sync.c`). Result:

```
RUN FAILURES (2):
  adaptive_tune
  safety_ceiling_sync_divergence
```

with the new test file naming six failed checks (`all-relays-off hook fired
once`, `hooks fire on the first diverged tick`, three persistence checks,
and `all-relays-off hook NOT called again once agreement is reached`) --
RED reproduced exactly, and the accompanying `adaptive_tune` failure is the
pre-existing concurrent-work failure below, unrelated to this edit. Restored
BY HAND (re-added the two removed lines verbatim); `git diff` on
`safety_ceiling_sync.c` around that block came back empty, confirming
byte-identical restoration. `App/test/build` was then deleted and the whole
suite rebuilt from scratch (`build_host_tests.ps1 -OutDir <private dir>`,
to avoid a separate concurrent-session collision on the shared
`App/test/build` directory -- see "Build tooling note" below): both
`config_divergence` (27/27, including the new Defect-1 message test) and
`safety_ceiling_sync_divergence` (25/25) came back green.

Files: `firmware/KilnFW/App/test/test_safety_ceiling_sync_divergence.c`
(new), `firmware/KilnFW/App/test/build_host_tests.ps1` (new executable
entry), `firmware/KilnFW/App/test/test_config_divergence.c` (new
`test_all_unknown_message_is_honest()` covering Defect 1's fixed message).

## Recorded, not fixed -- two deliberate couplings

Per the review's item D and the task's own instruction to write these down
rather than silently accept them as bugs later:

1. **`enforce_ceiling_divergence()` runs on every tick of
   `safety_ceiling_sync_reconcile_on_link_up()`**, which runs on every tick
   of `safety_poll_task` -- the ESP->Pico liveness heartbeat. A
   permanently-diverged board issues one relays-off/halt-run owner RPC per
   tick, forever, for as long as the divergence persists. Deliberate:
   edge-triggering the disable call instead would leave a window after some
   other path re-energizes relays mid-divergence with nothing to
   immediately re-disable them.
2. **`profile_executor_halt()` (like `kiln_io_owner_command_all_relays_
   off()`) runs from inside `safety_poll_task`'s own call chain**, so a
   persistently diverged board blocks that task on whatever lock
   `profile_executor_halt()` takes for as long as it is held elsewhere --
   and since `safety_poll_task` is also the ESP side of the safety link's
   liveness heartbeat, a late heartbeat during that window can trip the
   Pico's own S6b (link-dead) guard.

Both judged deliberate and fail-safe: heat is already forced off by the
divergence doing the blocking, so a late heartbeat (or an independent S6b
trip) cannot make anything less safe -- it can only add a second reason heat
stays off. Written into `safety_ceiling_sync.c`'s own doc comment on
`enforce_ceiling_divergence()` (a "RECORDED, NOT A DEFECT" block) so a future
reader who sees S6b trips or heartbeat warnings alongside a diverged board
does not chase them as a new symptom.

## Verification run and check tally

`build_host_tests.ps1` (private `-OutDir`, to avoid another session's
concurrent write to the shared `App/test/build` directory -- see below):
**all of this commit's own executables green** (`config_divergence` 27/27,
`safety_ceiling_sync_divergence` 25/25, plus every other previously-green
executable). One run failure present in every run this session, unrelated to
this work: `adaptive_tune` -- owned by the concurrent `adaptive_tune*`
rewrite (`ADAPTIVE_TUNE_DUTY_WINDOW_NUM_BUCKETS`-sized array fields moved
between structs mid-edit).

`tools/run_all_checks.ps1`: **93 passed, 0 skipped, 1 failed.** The one
failure, `check_00_kilnfw_target_build.ps1`, is entirely the concurrent
`kiln_cfg_store`/`adaptive_tune` work, not this commit's:

- `firmware/KilnFW/App/drivers/persist/kiln_cfg_store.c:686` and `:1222` --
  `-Werror=format-truncation` on two `snprintf()` calls whose format strings
  exceed their destination buffers (175 bytes into <=131, 112 bytes into a
  95-byte hole).
- `firmware/KilnFW/App/drivers/control/adaptive_tune_internal.h:422-423` --
  `duty_win_bucket_min`/`duty_win_bucket_max` are "variably modified at file
  scope" (a VLA-shaped array member reached from a non-constant macro during
  this rewrite).

Neither file is in this task's scope (`config_divergence.*`,
`safety_ceiling_sync.*`, `main_control_bringup.c`'s hook installation) and
neither was edited here. `tools\check_c_files_in_cmakelists.ps1` **passed**
in this run (the `kiln_package.c`-not-in-CMakeLists shape the task
anticipated was not present at the time of this run -- the concurrent work
had moved past it).

**Build tooling note:** two earlier attempts to run
`build_host_tests.ps1` against the shared `App/test/build` directory in this
same session failed with a cascade of unrelated `cl : Command line error
D8022 : cannot open '...\host_tests_common_flags.rsp'` errors and, in one
run, transient CMakeLists/redefinition errors in files this task does not
own -- consistent with another concurrent session writing into the same
shared build directory at the same time (see CLAUDE.md's "Concurrent
sessions git race" note; `build_host_tests.ps1`'s own `-OutDir` parameter
exists for exactly this). Re-running with a private `-OutDir` immediately
produced a clean, reproducible result with only the `adaptive_tune` failure
remaining -- confirming those transient errors were shared-directory
contention, not a defect in this change.

## Constraints honored

No `ZONES_CFG_VERSION` bump. The divergence verdict and the readiness-gate
interlock are unchanged -- `readiness_gate.h` still blocks with
`READINESS_GATE_BLOCK_CEILING_MISMATCH` naming the ceiling mismatch
specifically (`test_readiness_gate.c`'s existing 70/70 checks, including "a
safety config divergence alone refuses a start", still pass unmodified).
Float normalisation (`config_identity_normalize_f32()`) was not touched.
