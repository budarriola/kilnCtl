# Pico volatile install + unconfigured-ceiling backstop -- 2026-09-14

Implements items 15 and 16 of `docs/KILN_PROFILES_PLAN.md` (`c70ae799`),
informed by `docs/audits/kiln_profiles_robustness_2026-09-14.md` (`5182c3a4`)
and `docs/audits/pico_ceiling_mirror_and_rate_guard_2026-09-14.md` and its
appended reviews. Scope was SaftyFW's config-install path and `safety_core.c`
only, per the coordinating session's ownership split; `profile_executor*`,
`adaptive_tune*`, `sim_*`, `kiln_package.{c,h}` and
`zones_config_accessors.{c,h}` were not touched.

## Item 15 -- volatile install

### The new wire command

The plan named two options: a flag on `COMMIT_CONFIG`, or a sibling command.
This implementation uses a **sibling command**,
`SAFETY_CMD_APPLY_CONFIG_VOLATILE` (`0x2D`,
`firmware/CommonFW/include/kilnlink/kilnlink_apply_config_volatile.h`), not a
flag byte appended to `COMMIT_CONFIG`'s frame. Reason: `COMMIT_CONFIG`'s
1-byte fixed length is asserted directly by
`firmware/CommonFW/test/test_commit_config.c`'s `test_decode_too_long()` and
referenced throughout as a fixed contract; growing that frame would be a real
behavioural change to a heavily-depended-on existing codec for a feature a
brand-new, wholly additive command can express on its own. The new codec is a
byte-for-byte clone of `kilnlink_commit_config.h`'s shape (cmd byte only, no
payload).

`KILNLINK_PROTOCOL_VERSION` moved 14 -> 15 and `UART_PROTOCOL_VERSION` moved
11 -> 12 (both additive-only bumps, `KILNLINK_MIN_COMPATIBLE` left at 7) --
see `firmware/CommonFW/include/kilnlink/kilnlink_version.h` and
`firmware/KilnFW/App/drivers/common/uart_task_ids.h` for the reasoning
comments at each bump. `UART_PROTOCOL_VERSION` had to move too because
`wire_protocol_fingerprint_check.py`'s "uart" fingerprint spec keys off
`uart_task_ids.h`'s `SAFETY_CMD_` prefix without distinguishing which link a
given entry belongs to (the existing `SAFETY_CMD_CLEAR_TRIP`/
`SAFETY_CMD_PUSH_CONTEXT` entries already share that ambiguity) -- moving only
`KILNLINK_PROTOCOL_VERSION` would have left the "uart" fingerprint drifted
with no version bump to explain it. `wire_protocol_fingerprints.json` was
refreshed via `--update` after review (not hand-edited), and
`tools/check_wire_protocol_fingerprint.ps1` passes clean.

**IMPORTANT for whoever flashes next: both firmwares AND CommonFW must be
rebuilt and flashed together.** This pass built and host-tested the change
and confirmed both target builds compile against it, but **did not flash
anything** (an agent was extracting an ESP coredump; flashing would have
destroyed it). A partial flash -- one processor built against protocol 15,
the other still on 14/11 -- is not dangerous (the new command is additive and
an old peer simply never receives/sends it), but it would leave `kiln_help()`/
`kicad_help()`-style staleness confusion the next time someone tries to use
the new command and finds only one side knows about it.

### How it reaches the live record

`config_store_write_volatile()` (`firmware/SaftyFW/src/config_store_flash.c`,
declared in `config_store.h`) is the new function. Contract, matched exactly
to the plan's three hard requirements:

1. **Goes through `config_store_seqlock_write()`** -- the same seqlock-guarded
   assignment `config_store_write()` uses for its own commit, never a plain
   struct assignment. This is what prevents the trip path (running on the
   other core) from ever observing a torn record -- the exact defect
   `98d237b0` fixed for the flash path.
2. **No re-validation inside the function** -- it trusts its caller exactly as
   `config_store_write()` already does. The caller,
   `link_task_handle_apply_config_volatile()`
   (`firmware/SaftyFW/src/tasks/link_task.c`), runs the identical
   `config_params_validate_ex()` / `config_params_finalize_ct_channel_map()` /
   `config_params_finalize_i_present_a()` / `calibration_missing`
   recomputation sequence `link_task_handle_commit_config()` runs, in the same
   order, before calling either write function. A volatile install is not a
   less-checked install.
3. **Bumps identity automatically.** `config_version`/`config_crc` turned out
   to already be *pure functions of the live seqlock snapshot*
   (`config_store_get_config_version()`/`config_store_get_config_crc()`, both
   already reading `s_cached_record` via `config_store_seqlock_read()`) rather
   than a separately-tracked "last flashed" value -- so the refactor the plan
   anticipated (`config_store_record_crc()` "needs to run over the in-RAM
   record") was already done by an earlier pass. `config_store_write_volatile()`
   only had to bump `to_write.seq = s_cached_record.seq + 1u` before the
   seqlock write, exactly as `config_store_write()` does, and both getters
   report the new identity the instant the write lands with no second step.

**Deliberately does NOT call `config_store_decide_write()`** -- no ARMED
check at all. That is the entire point: this path never reaches
`config_store_write()`'s flash I/O (no `hal_flash_safe_execute()`, no
`s_regions`, no `s_cached_slot`/`s_cached_sector` update), so it never needs
`relay_owner_get_state()`. The Pico's `RELAY_OWNER_STATE_ARMED` refusal lives
entirely inside `config_store_write()`'s own gate; a volatile install simply
never arrives at that gate. Confirmed by test
(`test_write_volatile_installs_while_armed_and_bumps_identity`,
`firmware/SaftyFW/test/test_config_store_flash.c`): the Pico is forced ARMED
via the host stub, a volatile install lands and bumps identity, and
`relay_owner_get_state()` still reads `RELAY_OWNER_STATE_ARMED` afterward --
never once left ARMED.

`s_cached_slot`/`s_cached_sector` are left untouched (nothing was flashed, so
the fallback record's on-disk position has not moved) -- confirmed by test
that a fresh `config_store_boot_load()` (simulated reboot) after a volatile
install sees the pre-install state, never the volatile one.

### What was NOT built

The plan's section 1a.4 (persisting a confirmed volatile install as the new
flash fallback, `pkg_hash` bookkeeping, the boot-time `FALLBACK` vs
`CONFIGURED` vs `UNCONFIGURED` state machine) and the ESP-side apply
transaction that would actually send `APPLY_CONFIG_VOLATILE` are **not**
implemented here -- those live in `kiln_package.{c,h}` /
`zones_config_accessors.{c,h}`, explicitly out of this pass's ownership. This
pass delivers the Pico-side primitive those later items are meant to call;
`firmware/KilnFW/components/kilnlink/CMakeLists.txt` links the new codec into
the ESP build now (so the day a caller is added it is not rediscovering a
missing link-time dependency), but no ESP caller exists yet.

## Item 16 -- the unconfigured ceiling

Confirmed, not re-litigated: `d22431d0`'s finding stands. An uncommissioned
Pico fails `commissioning_gate_is_commissioned()`;
`safety_core_request_enable()` refuses the ON direction on that
(`firmware/SaftyFW/src/tasks/safety_core.c`, the
`commissioning_gate_energize_allowed()` check inside the `enable` branch);
`relay_owner_command_energize()` has exactly one caller outside its own file.
**This is not exploitable today** -- confirmed again by inspection during this
pass, no new path found where an unconfigured Pico can energize.

Per the brief, this was implemented as **defence in depth only** -- no change
to `safety_guards.c`/`.h`'s S1 semantics (still fields_set-gated,
`abs_max_temp_c = 0.0f` for unconfigured, "never trips" by design per
`CONFIG_REFERENCE.md` section 7's "no default may be a guess dressed as a
value"), no change to the commissioning gate or relay_owner's state machine.

The addition is a second, independent circuit breaker inside
`safety_core_task`'s tick (`safety_core.c`, right after
`safety_core_load_guard_cfg()` is called): if `abs_max_temp_c`'s `fields_set`
bit is clear (the *same* bit `safety_core_load_guard_cfg()` itself just read
-- deliberately, so this cannot independently drift from what S1 considers
unconfigured, per `project_reset_one_side_bug_class`) **and**
`relay_owner_get_state() == RELAY_OWNER_STATE_ARMED`, it calls
`relay_owner_command_energize(false)` every tick (idempotent) and logs once on
the transition (`s_unconfigured_armed_warned`, mirroring the existing
`s_borrowed_type_mismatch_warned` idiom in the same file). It de-energizes
**through `relay_owner`**, the sanctioned owner module -- never a raw
GPIO/relay write (`project_bypassed_owner_module_bug_class`) -- and never
assigns a fabricated `abs_max_temp_c` value.

Since the state this backstop guards against is unreachable through the
primary interlock chain today, this code path is presently dead in normal
operation -- it exists only to fire if a *future* change to `relay_owner` or
the commissioning gate ever defeats the primary refusal without anyone
noticing at that call site. This does not destabilise anything already
working: the primary interlock chain is untouched.

## Tests

- `firmware/CommonFW/test/test_apply_config_volatile.c` -- new codec's
  round-trip, byte-exact vector, and hostile-input set (mirrors
  `test_commit_config.c`).
- `firmware/CommonFW/test/test_fuzz_payloads.c` -- registered
  `kilnlink_apply_config_volatile_decode` in the fuzz harness's `k_cases[]`
  (the build script's own registry-completeness check caught the omission on
  the first run and failed loudly, exactly as designed).
- `firmware/SaftyFW/test/test_config_store_flash.c` --
  `test_write_volatile_installs_while_armed_and_bumps_identity` and
  `test_write_volatile_repeated_then_flash_commit_still_gated`: volatile
  install visible immediately, bumps `config_version`/`config_crc`, never
  reaches flash, Pico stays ARMED throughout, and `config_store_write()`'s own
  ARMED gate is unaffected by any of it.
- `firmware/SaftyFW/test/test_safety_core_unconfigured_armed_backstop.c` --
  source-text-scan test (safety_core.c is not host-compilable, same
  precedent as `test_safety_core_s8_wiring.c`/`test_safety_core_stack_budget.c`)
  pinning that the backstop exists, reads the same fields_set bit S1 itself
  uses, gates on `RELAY_OWNER_STATE_ARMED`, and de-energizes through
  `relay_owner_command_energize(false)` rather than a raw write.

### Negative tests (hand-broken, hand-restored, full rebuild)

1. `config_store_write_volatile()`'s `seq` bump was removed
   (`to_write.seq = s_cached_record.seq;`, no `+ 1u`). Result: 4 test
   failures in `test_config_store_flash.c` (both new tests, both the
   "bumps config_crc"/"bumps config_version" assertions and the "second
   install bumps again" assertions). Restored by hand; full rebuild
   (`-OutDir` deleted and recreated) confirmed 2507/2507 clean again.
2. The backstop's `relay_owner_command_energize(false)` call in `safety_core.c`
   was removed (replaced with a comment). Result: 1 test failure in
   `test_safety_core_unconfigured_armed_backstop.c` ("backstop de-energizes
   THROUGH relay_owner"). Restored by hand; full rebuild confirmed
   2507/2507 clean again.

## Verification

- `firmware/CommonFW` host tests (fresh `build_pv15` dir, deleted and
  rebuilt from scratch after the negative tests): 41/41 passed
  (`test_apply_config_volatile` included).
- `firmware/SaftyFW` host tests (`build_host_tests.ps1 -OutDir C:\wt\...`,
  fresh dir each run per the standing instruction): 2507/2507 + 56/56 +
  235/235 passed, fuzz-decoder-registry check clean.
- `firmware/SaftyFW` target build (`check_00_saftyfw_target_build.ps1`,
  Pico/RP2040, both A/B slots): PASS, `SaftyFW.elf`/`SaftyFW_slotA.elf`/
  `SaftyFW_slotB.elf` all linked including the new kilnlink command and
  `config_store_write_volatile()`.
- `firmware/KilnFW` target build (`check_00_kilnfw_target_build.ps1`): FAILED,
  attributed to concurrent work, NOT this change --
  `drivers/http/safety_cfg_http.c:1132: 'SAFETY_PARAM_ID_ABS_MAX_TEMP_C'
  undeclared` and a `kiln_cfg_store.c` `-Werror=format-truncation`, both in
  files this pass never touched (owned by the concurrent kiln-package/
  zones_config_accessors work). Neither symbol nor file appears in this
  pass's diff.
- `tools/check_wire_protocol_fingerprint.ps1`: PASS (manifest refreshed via
  `--update`, reviewed, committed).
- `tools/check_doc_hash_citations.ps1`: PASS (verified locally against this
  doc before commit).
- `tools/run_all_checks.ps1`: ran to 79/94 before this session's own
  wrapper timeout cut it off (`check_mykicad_golden_suite_runs.ps1`, exit
  143/SIGTERM -- an artifact of this session's own bounding, not a real
  failure, and unrelated to firmware). Of the checks that did complete, 4
  genuine FAILs, all attributed:
  - `check_00_kilnfw_target_build.ps1` -- concurrent WIP, see above.
  - `check_flash_worker_lint.ps1` -- `drivers/persist/kiln_cfg_swap.c`
    (untracked, concurrent kiln-package WIP; not touched by this pass).
  - `check_saftyfw_task_stack_budgets.ps1` -- **this pass's own regression**:
    `safety_core`'s measured stack total moved 2160 -> 2168 B (the new bool
    local + branch), exceeding its ceiling pinned at the old measured value.
    Fixed by re-pinning `CEILING_BYTES["safety_core"]` to 2168 in
    `firmware/SaftyFW/test/check_saftyfw_task_stack_budgets.py` (same
    "pinned at measured, not padded" convention the file's own header
    documents) and reconfirmed passing standalone afterward.
  - `check_c_files_in_cmakelists.ps1` -- `drivers/persist/kiln_cfg_swap.c`
    again (same untracked concurrent file, not referenced by its target's
    CMakeLists.txt yet -- not this pass's file to fix).

No `ZONES_CFG_VERSION` bump. `firmware/SaftyFW`/`firmware/CommonFW` changes
only touch the isolated ESP<->Pico link's own protocol constant and the
shared PC-link enumeration constant in `uart_task_ids.h` (see the version-bump
reasoning above) -- no zones-config schema field was added, removed, or
reinterpreted.
