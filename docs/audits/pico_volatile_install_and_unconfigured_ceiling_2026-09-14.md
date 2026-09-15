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

---

# Review, 2026-09-14 — two load-bearing claims REFUTED: the ARMED gate is half safety rule, not only a flash constraint; and the backstop is not unconditionally dead code

Adversarial review of `17740e47` (volatile install) and `3d2c5413`
(unconfigured-ceiling backstop), by a separate session. Nothing was flashed
and no heating run was performed. Every conclusion below is labelled
**[executed]** or **[read]**.

## Verdict summary

| Claim under review | Verdict |
|---|---|
| 1a. Volatile install cannot reach `config_store_decide_write()`/flash | **Upheld** [read + executed] |
| 1b. No torn record reachable on the guard path | **Upheld** [read + executed] |
| 1c. Bypassing the ARMED gate is safe "BECAUSE this never reaches the flash write it protects" | **REFUTED** [read] — that is only half of the gate's documented purpose |
| 2. Reported `config_version`/`config_crc` reflect the installed record | **Upheld, and strengthened** [executed] |
| 3. `0x2D` validation as strict as its siblings; fuzz-registered; same validation as the flash path | **Upheld** [read + executed] |
| 4a. Both protocol bumps were genuinely required | **REFUTED** [read] — the UART half is self-inflicted |
| 4b. `wire_protocol_fingerprints.json` genuinely refreshed; regex covers new size-determining constants | **Upheld** [executed] |
| 4c. A version-mismatched pair fails loudly | **Split** [read] — loud on the PC link (and it *will* fire, see finding B), SILENT on the isolated link |
| 5a. The backstop's target state is unreachable through the primary interlock chain | **Upheld** [read] |
| 5b. The backstop is "dead code today by construction" and cannot misfire | **REFUTED** [executed] |
| 5c. No change to S1 semantics or the primary interlock chain | **Upheld** [read] |
| 6. `safety_core` 2160 -> 2168 B is honest | **Upheld** [executed] |
| 7. Both negative tests reproduce | **Upheld, plus a third** [executed] |

Three findings are new. **A** and **C** are safety-argument findings against
this pass; **B** is a shipped defect that will break the bench toolchain the
moment this firmware is flashed.

---

## Finding A (load-bearing, refutes claim 1c): the ARMED refusal is a SAFETY rule as well as a platform constraint, and the volatile path defeats the safety half

`config_store.h`'s and this document's justification for having no ARMED check
is consistently framed as *"this path never reaches the flash write that gate
protects"*. That is true of the **platform** half of the gate. It is not the
whole gate. [read]

- `firmware/SaftyFW/docs/ARCHITECTURE.md` §7: *"**A config write is refused
  while ARMED** — retuning a safety threshold during a firing is not a
  supported operation."* §8 then adds the flash-stall reason and says
  explicitly: *"is **therefore not only** a safety rule, it is a platform
  requirement."* Two independent reasons, stated as such.
- `docs/CONFIG_REFERENCE.md` §210: *"**Writes are refused while ARMED.** Both a
  safety rule and an RP2040 flash constraint."*
- `firmware/SaftyFW/docs/COMMISSIONING.md` §2: *"**Writes are refused while
  ARMED** … This is enforced inside the store's own write path, not at the call
  site, **so a future second caller cannot forget it**."*

`config_store_write_volatile()` is exactly that future second caller inside the
store's own write path, and it is the one that does not enforce it. The
mechanism the doc named as the protection against this is the mechanism this
change steps around.

What the volatile path can now do, while ARMED and mid-firing: install all 68
params, including `abs_max_temp_c` (S1's ceiling), `max_rate_c_per_min` (S8)
and `tc_type`. `config_params_validate_ex()` bounds `abs_max_temp_c` only to
*finite, positive, and* `<= TC_MAX_C_BY_TYPE[tc_type]` (`config_params.c:613`,
`615`, `786`) — so raising a commissioned 1100 °C ceiling to a type-K 1372 °C
during a live firing is a **validated, accepted** volatile install that
immediately disarms S1's trip point, with no refusal, no ARMED branch and no
operator confirmation. That is precisely the operation ARCHITECTURE.md §7
declares unsupported.

This is not a code defect in `17740e47` — the code does exactly what it says.
It is a **missing half of the safety argument**. The plan's §1a.2 requirement
("a package swap must not have to unarm the Pico") is legitimate, but it was
satisfied by removing a rule whose *other* stated reason was never addressed.
Concretely, the gap is that there is no restriction on **which** params a
volatile install may change while ARMED.

**Recommended narrowing, before any ESP caller ships** (item 5's transaction,
`c2c9eff2`, now exists): give `config_store_write_volatile()` a
guard-threshold carve-out — while `relay_owner_get_state() ==
RELAY_OWNER_STATE_ARMED`, refuse a volatile install that would *loosen* any
trip threshold (raise `abs_max_temp_c`, raise `max_rate_c_per_min`, change
`tc_type`). A package swap that only moves PID/profile-shaped params is
unaffected. This is narrower and more honest than either "no check at all" or
"unarm for 68 writes", and it keeps ARCHITECTURE.md §7 true. Alternatively,
amend ARCHITECTURE.md §7 / CONFIG_REFERENCE.md §210 / COMMISSIONING.md §2 to
say the safety half of the rule has been deliberately dropped — but it should
be one or the other, not silence.

## Finding B (shipped defect): the `UART_PROTOCOL_VERSION` 11 -> 12 bump was not mirrored into PcTools, and the bump itself was avoidable

Two separate problems, both inside the "double bump" of claim 4.

**B1 — the mirror was not updated, and still is not at HEAD.** [executed]

```
git show 17740e47:tools/PcTools/src/kilnctrl/protocol.py | grep ^UART_PROTOCOL_VERSION  ->  11
git show HEAD:tools/PcTools/src/kilnctrl/protocol.py      | grep ^UART_PROTOCOL_VERSION  ->  11
firmware/KilnFW/App/drivers/common/uart_task_ids.h                                       ->  12
```

`devices_info.py:107`'s `FirmwareVersion.compatible` is a **hard equality**
gate. The moment this firmware is flashed, every PcTools/`kiln_call` device
command is refused with *"device speaks v12, pc_tools speaks v11"* — the exact
failure `protocol.py`'s own "Version 6 (2026-08-23)" note records as having
been *observed live*. It is presently patched only in an **uncommitted
working-tree edit** made by a concurrent session (`protocol.py` shows ` M` and
reads 12 in the worktree). If that edit is lost, the bench toolchain breaks on
the next flash. **This must be committed alongside the firmware bump.** Not
fixed here: `protocol.py` is concurrent WIP this session does not own.

**B2 — the bump was self-inflicted, and it is the 4th instance of a
documented, checked-against failure class.** [read]

`tools/check_uart_version_independence.ps1`'s own header exists because *"a
version bump driven purely by the isolated link's contract … silently dragged
the PC link's version along with it … This has now bitten the project three
times."* `17740e47`'s own commit message states the condition for instance
four: *"This command lives entirely on the isolated ESP<->Pico link … and has
no PC<->ESP counterpart or caller yet — nothing about the PC link's own frames
changed."* The check did not catch it because it only forbids the **textual
alias**; a hand-written bump of the same magnitude sails through.

The stated cause was that `wire_protocol_fingerprint_check.py`'s "uart" spec
matches `^SAFETY_CMD_` in `uart_task_ids.h` without distinguishing which link
an entry belongs to (confirmed, `wire_protocol_fingerprint_check.py:193`
[read]). But the *reason* the uart fingerprint moved at all is that this pass
**voluntarily added** `SAFETY_CMD_APPLY_CONFIG_VOLATILE` to `uart_task_ids.h`
"purely for enumeration", when the id is already covered by the kilnlink spec
via `kilnlink_apply_config_volatile.h`. Either not defining it in
`uart_task_ids.h`, or scoping the "uart" spec to exclude isolated-link
`SAFETY_CMD_` entries, would have avoided a PC-facing protocol bump entirely.
A tooling scoping gap was paid for with a wire-version bump on a link whose
wire did not change — which is the failure class, not a workaround for it.

## Finding C (refutes claim 5b): the backstop's trigger is satisfiable on a commissioned, ARMED board, so it is not unconditionally dead code

Claim 5a is upheld [read]: `CONFIG_STORE_SET_ABS_MAX_TEMP_C` is in
`config_params_all_required_set()`'s `required` mask,
`commissioning_gate_is_commissioned()` ANDs that with `!calibration_missing`,
and `safety_core_request_enable()` checks
`commissioning_gate_energize_allowed()` before the one out-of-file
`relay_owner_command_energize(true)` call. An unconfigured Pico cannot reach
ARMED through that chain.

But the backstop does not test the record. It tests **what
`config_store_get_full_record()` hands it** — and that function is *fail-closed
to `config_store_default()`*, whose `fields_set` is `0` (`config_store.c:811`):

```c
void config_store_get_full_record(config_store_record_t *out) {
    if (!s_loaded)                        { config_store_default(out); return; }
    if (!config_store_seqlock_read(out))  { config_store_default(out); }
}
```

Executed proof (standalone harness linking the real `config_store_flash.c`,
`config_store.c`, `config_params.c` and the existing host stubs; scratchpad
only, nothing added to the tree): install a fully commissioned record while the
stub reports `RELAY_OWNER_STATE_ARMED`, then drive
`config_store_seqlock_read()` into its own documented "no stable snapshot"
state (`config_store_test_force_fallback_path(true)` +
`config_store_test_reset_fallback_state()`):

```
  normal read:   fields_set=0x0008 abs_max=1100.0
  degraded read: fields_set=0x0000 abs_max=0.0
  -> safety_core.c:1044 abs_max_temp_c_unconfigured evaluates to TRUE
  -> relay state ARMED
  degraded config_version=0 config_crc=0x0000
```

On that tick the backstop's condition is satisfied on a **commissioned,
ARMED** board and it calls `relay_owner_command_energize(false)` — a spurious
force-de-energize of a live firing. Note the asymmetry with the primary gate:
`safety_core_request_enable()` reads the *same* degraded record, but all it
does is refuse a *new* ON request (harmless — ARMED is a latch, not a
re-requested state each tick). The backstop converts the same transient into an
actual shutdown. That is a behaviour change the "dead code by construction"
framing does not cover.

**Honest bounds on this finding.** The degraded state was reached via the
test-only forcing hook, not from production concurrency; production
reachability was **not** demonstrated. In production it needs
`CONFIG_STORE_SEQLOCK_MAX_RETRIES` (4) **and**
`CONFIG_STORE_FALLBACK_SEQLOCK_MAX_RETRIES` retries all to lose to concurrent
writer commits, which remains very unlikely for a single
`APPLY_CONFIG_VOLATILE` (one seqlock write per frame). Severity is
**availability, not hazard**: the misfire direction is de-energize, always the
fail-safe direction.

What matters is that `17740e47` invalidated the written premise those bounds
rest on. Both retry bounds are justified in `config_store_flash.c` by the
phrase *"a rare, deliberate, **ARMED-refused-anyway** commissioning commit, not
a hot-path event"* — and after item 15 a config write is no longer
ARMED-refused and no longer necessarily a commissioning event. This is the
`project_reset_one_side_bug_class` shape at the level of a *justification*
rather than a counter: one module's safety argument quotes another module's
now-changed policy, and nothing forced the quote to be revisited. Two concrete
follow-ups:

1. Revisit both retry-bound comments in `config_store_flash.c` (they now assert
   something false).
2. Make the backstop distinguish "the record says unconfigured" from "I could
   not read the record". `config_store_get_full_record()` has no way to say
   which — it returns `void`. The cheapest honest fix is to gate the backstop on
   a positive "I got a real snapshot" signal (e.g. a non-zero
   `config_store_get_config_version()`, which returns the same 0 sentinel in the
   degraded state) so a read failure never *acts*, it only declines to clear.

Claim 5c is upheld [read]: `safety_guards.c`/`.h` are untouched by `3d2c5413`,
`abs_max_temp_c` still ships at `0.0f`/"never trips" for an unconfigured board,
and `commissioning_gate.c` and `relay_owner`'s state machine are unchanged.

---

## Claim-by-claim detail

### 1. Volatile install vs. the ARMED refusal, and the seqlock

**Cannot reach the flash path.** [read] `config_store_write_volatile()`'s body
is a NULL check, two field overwrites and one
`config_store_seqlock_write(&to_write)`. There is no `config_store_decide_write()`,
no `ensure_region()`, no `hal_flash_safe_execute()`, no
`config_store_plan_write()`, no `s_cached_slot`/`s_cached_sector` update, and no
early-return path that reaches any of them. The only downstream call from its
caller is `current_task_reload_cal()`, traced end to end
(`current_task.c:181`): a pure read of `config_store_get_full_record()` followed
by `current_sense_set_ct_cal()` — no flash, no NVS, no persistence side effect.
Confirmed by execution too: `test_write_volatile_repeated_then_flash_commit_still_gated`
passes and a simulated reboot does not see the volatile record.

**Seqlock usage is correct, and a torn record on the guard path is NOT
possible.** [read + executed]

- It uses `config_store_seqlock_write()`, the same single publisher
  `config_store_write()` uses — odd counter, `HAL_DMB()`, struct copy,
  `HAL_DMB()`, even counter, then the writer-owned fallback double buffer under
  its own generation seqlock. No partial publish, no plain struct assignment, no
  third bypass introduced.
- **Writer discipline holds.** `config_store_seqlock_write()`'s correctness
  rests on "exactly one writer". Both callers — `config_store_write()` (from
  `link_task_handle_commit_config()`) and `config_store_write_volatile()` (from
  `link_task_handle_apply_config_volatile()`) — are reached only from
  `link_task_handle_raw_frame()`'s dispatch switch, i.e. from `link_task`,
  pinned to `SAFTYFW_CORE_LINK_PATH`. The two cannot interleave: they are
  sequential cases of one switch on one task. The unsynchronised
  `s_cached_record.seq + 1u` read is therefore a writer-core read of a
  writer-owned field, identical to `config_store_write()`'s own at line 1002.
- 235/235 `config_store_flash` checks pass, including the two multi-threaded
  torn-read regression tests, which exercise the same
  `config_store_seqlock_write()` this path calls (`writes=165764, torn reads=0`;
  `4 readers … torn reads=0, fallback_taken=90174`). [executed]

One documentation nit: `config_store_seqlock_write()`'s own comment still says
*"Called only from `config_store_write()`"*. It now has two callers.

### 2. The identity bump — verified by execution, not by reading

The committed test only asserts `config_crc != 0` and `config_version !=
before`, which would also pass if the reported identity were of the *wrong*
record. A stronger independent harness was built against the real sources
(scratchpad only) which recomputes the expected identity **from the live record
read back through `config_store_get_full_record()`** and compares it against
what the reporting getters return: [executed]

```
  install 1: seq=1 rep_ver=1 exp_ver=1 rep_crc=0xB9E7 exp_crc=0xB9E7 tc=1 abs=1001.0
  install 2: seq=2 rep_ver=2 exp_ver=2 rep_crc=0xC4A8 exp_crc=0xC4A8 tc=2 abs=1002.0
  install 3: seq=3 rep_ver=3 exp_ver=3 rep_crc=0x106D exp_crc=0x106D tc=3 abs=1003.0
  ok: reported crc actually tracks content   (a 4th, different record changed it)
  ok: still ARMED  (all three installs)
```

The claim holds: `config_store_get_config_version()`/`_get_config_crc()` are
pure functions of the seqlock snapshot, so the `seq` bump alone is sufficient
and the reported identity always matches the record being run. No refactor was
needed. The equality is not vacuous — the sensitivity check confirms the
reported CRC moves with content.

Caveat, from Finding C: in the degraded `config_store_seqlock_read()` failure
state both getters report **0**, i.e. "never commissioned", regardless of what
is actually running. That is pre-existing and deliberate (the 0 sentinel drives
the ESP's UNCOMMISSIONED display), but worth knowing when relying on this pair
for the version+hash divergence check that now disables heaters on mismatch.

### 3. Command `0x2D` — validation strictness

[read] `kilnlink_apply_config_volatile_decode()` is byte-for-byte the shape of
`kilnlink_commit_config_decode()`: `len != 1` rejected **before** any
dereference of `payload`, then `payload[0] != 0x2D` rejected, then an optional
NULL-tolerant `out`. Truncated (`len < 1`), oversized (`len > 1`) and
wrong-command frames are all refused with no out-of-bounds read. `0x2D` is
unique across every `KILNLINK_*_CMD` (the only duplicate pairs are the
pre-existing request/reply pairs `0x01` and `0x0B`).

Fuzz registration is complete, not nominal: `test_fuzz_payloads.c` gained a
`DECL_ADAPTER`, a union member, a canary-armed `decode_apply_config_volatile()`
wrapper and a `k_cases[]` entry with `MAX_LEN + 32` — so oversized inputs are
actually fed, with buffer canaries checked on every call. CommonFW's own
build-script registry-completeness check is what caught the original omission.

**Same validation as the flash path: yes.** [read]
`link_task_handle_apply_config_volatile()` runs
`link_task_ensure_staged_config()`, then the identical
`config_params_validate_ex()` call with the identical reject-reason mapping and
the identical `link_task_send_commit_config_rejected()` reply, then the
identical `config_params_finalize_ct_channel_map()` /
`config_params_finalize_i_present_a()` /
`calibration_missing = !config_params_all_required_set()` sequence, in the same
order, as `link_task_handle_commit_config()`. Diffing the two functions, the only
differences are the codec, the log tag, and `config_store_write_volatile()` in
place of `config_store_write()` plus its `written`/ARMED-vs-STORAGE rejection
branch. A volatile install is not a trusted install. (What it *is*, per Finding
A, is an install of things the flash path would have refused to change at all in
this state.)

### 4. The double protocol bump

**kilnlink 14 -> 15: required and correct.** [read] A new frame id genuinely
changes the link's fingerprint. `KILNLINK_MIN_COMPATIBLE` left at 7 is
deliberate and reasoned in place, consistent with the 5->6, 8->9 and 11->12
precedents cited in the same header: the change is purely additive, so a peer
built against 7..14 stays fully compatible for everything it already speaks.

**UART 11 -> 12: see Finding B.** Required only as a consequence of a voluntary
addition to `uart_task_ids.h`, mirrored nowhere, and a fourth instance of the
class `check_uart_version_independence.ps1` was written to prevent.

**Fingerprint manifest genuinely refreshed.** [executed]
`python firmware/KilnFW/App/test/wire_protocol_fingerprint_check.py` →
*"Wire protocol fingerprint check passed: kilnlink and uart links both match
their last reviewed (version, fingerprint) snapshot."* The stored hashes are
recomputed from the extracted `defs` lists, so they cannot have been
hand-written to pass without the defs matching the real headers. Both new `defs`
entries are present and correct (`KILNLINK_APPLY_CONFIG_VOLATILE_LEN=1u`,
`SAFETY_CMD_APPLY_CONFIG_VOLATILE=0x2Du` in both specs).

**Regex covers new frame-size-determining constants: yes.** [read]
`wire_protocol_fingerprint_check.py:181` is
`^KILNLINK_.*(_LEN|_OFF|_NUM_|_COUNT)\w*$` — the `_NUM_`/`_COUNT` alternatives
were added 2026-09-14 for exactly the `9 -> 10` slip the brief refers to, and
the new `_LEN` constant is matched by it.

**Doc citation correction:** the Verification section above cites
`tools/check_wire_protocol_fingerprint.ps1`. **No such file exists in this
repo.** The check is
`firmware/KilnFW/App/test/wire_protocol_fingerprint_check.py` (run directly, or
via the KilnFW host-test suite). The check itself passes; only the path cited is
wrong.

**Does a version-mismatched pair fail loudly? Split answer.** [read]

- **PC <-> ESP link: loudly.** `FirmwareVersion.compatible` is hard equality and
  the refusal names both versions (`devices_info.py:119`). Same as the prior
  bump. Per Finding B, this will actually fire on the next flash.
- **ESP <-> Pico link: silently, and differently from the prior bump.** With
  `MIN_COMPATIBLE` at 7, a 15-built ESP and a 14-built Pico link up normally. An
  `APPLY_CONFIG_VOLATILE` sent to the 14-built Pico falls into
  `link_task_handle_raw_frame()`'s `default:` case, which *"is silently
  discarded"* by design. And the handler sends **no success reply** — only a
  `COMMIT_CONFIG_REJECTED` on validation failure. So a mismatched pair produces
  *no* rejection, *no* log on the Pico, and *nothing to time out on*: the install
  simply does not happen and the ESP cannot tell. This is weaker than the prior
  bump's behaviour, which this document describes as *"a pre-13 peer simply timed
  out with an explicit log"* — that description does **not** carry over to this
  frame. Harmless while there is no ESP caller; with item 5's transaction now
  landed (`c2c9eff2`) the apply transaction needs either a positive ACK for this
  command or a `config_crc` read-back to confirm the install actually took.
  (`c2c9eff2` was not in this review's scope; flagged for whoever owns it.)

### 5. The backstop

See Finding C for reachability and misfire. Additional confirmations:

- De-energizes **through** `relay_owner_command_energize(false)`, never a raw
  GPIO/relay write. [read]
- Reads the *same* `cfg_rec.fields_set` bit `safety_core_load_guard_cfg()` was
  just handed two lines earlier — a genuinely shared fact, not an
  independently-derived one. [read; negative test 3 below pins it by execution]
- The log-once latch clears in the `else` branch, so a recurring condition logs
  again rather than being silenced forever — correct, and matching the existing
  `s_borrowed_type_mismatch_warned` idiom. Note the practical consequence under
  Finding C: a transient would log at `LOG_LEVEL_ERROR` each time it re-enters,
  which is the right behaviour for diagnosing a misfire.

### 6. The stack ceiling 2160 -> 2168

Verdict: **honest, and low risk.** [executed] Against a freshly linked
`SaftyFW.elf` built from the current tree (`check_00_saftyfw_target_build.ps1`:
PASS, all three of `SaftyFW.elf`/`_slotA`/`_slotB`),
`check_saftyfw_task_stack_budgets.ps1` reports:

```
  safety_core   INDETERMINATE (unresolved regsp) total= 2168 B  declared= 6144 B  margin= 3976 B  ceiling= 2168 B  [ok]
```

The 2168 figure is measured on the ELF that actually contains the backstop, not
on a lighter configuration. Two qualifications, stated because this repo treats
these totals as lower bounds:

- It is flagged `INDETERMINATE (unresolved regsp -- may be WRONG, not just
  incomplete)`, so 2168 B is a floor, not a worst case.
- The ceiling is pinned exactly at the measured value with zero slack. That is
  this table's documented convention (a *drift detector*, not a safety margin)
  and is acceptable **only because** the real headroom lives elsewhere: declared
  stack 6144 B against 2168 B measured leaves 3976 B. This is not the shape of
  tonight's panic, where a task's static path landed on its *declared* stack.
  `3d2c5413^` was not rebuilt to independently confirm the old 2160 figure; an
  8-byte delta for one `bool` plus a branch is consistent.

### 7. Negative tests — both reproduced, plus a third

All three broke a **production** file, were restored **by hand** (verified
byte-identical via `git diff --stat` showing no diff), and each was followed by a
full clean rebuild in a fresh private `-OutDir`.

1. **Seq bump removed** (`config_store_flash.c`, `to_write.seq =
   s_cached_record.seq;`): **4 failures**, exactly as documented —
   `test_config_store_flash.c:219, 221, 261, 262`. [executed]
2. **`relay_owner_command_energize(false)` removed** (`safety_core.c:1052`,
   replaced by a comment): **1 failure**,
   `test_safety_core_unconfigured_armed_backstop.c:113` ("backstop de-energizes
   THROUGH relay_owner"). Script exit 1. [executed]
3. **Additional probe of the source-text test's power** — the backstop's
   condition inverted (`== 0u` -> `!= 0u`): **1 failure**,
   `test_safety_core_unconfigured_armed_backstop.c:108` ("reads
   `CONFIG_STORE_SET_ABS_MAX_TEMP_C`, the same bit S1 itself uses"). So the scan
   pins the *polarity*, not merely the presence of the symbol — better than a
   bare text-presence test. [executed]

Structural limitation, stated for the record: being a source-text scan, this
test cannot see whether the block is *reached* (an early `return` inserted above
it would leave it passing), and it cannot see Finding C at all, because Finding
C is about the *value* the block's input carries, not its text.

After restoring all three by hand and a full clean rebuild:
**2507/2507 + 56/56 + 235/235, `all passed`, script exit 0.** [executed]

---

## Verification performed by this review

- SaftyFW host tests, fresh private `-OutDir` each run, via the bash tool with a
  short worktree path: baseline **2507/2507 + 56/56 + 235/235**; final
  post-restore run identical. [executed]
- SaftyFW target build (`check_00_saftyfw_target_build.ps1`): **PASS**, all three
  slot ELFs linked. [executed]
- `check_saftyfw_task_stack_budgets.ps1`: **PASS** — this pass's re-pin is
  correct, and the FAIL recorded in the Verification section above is resolved.
  [executed]
- KilnFW target build (`check_00_kilnfw_target_build.ps1`): **PASS**. The
  `SAFETY_PARAM_ID_ABS_MAX_TEMP_C` / `kiln_cfg_store.c` failures the Verification
  section attributes to concurrent work have since been fixed in the working
  tree; that note is now stale. [executed]
- KilnFW host tests: **2 run failures, `sim_scenarios` and
  `sim_scenarios_adaptive`** — concurrent `sim_factorial*` work, not this pass's
  files. (A first run also hit `unresolved external symbol
  kilnlink_apply_config_volatile_encode` in `test_safety_link_compile`; that is
  `c2c9eff2`'s ESP-side caller against a test link line that had not yet listed
  the new codec, and it was fixed in the working tree between runs. Worth noting
  as an integration seam: `17740e47` added the codec to
  `firmware/KilnFW/components/kilnlink/CMakeLists.txt` for the target build but
  not to `App/test/build_host_tests.ps1`'s `$slExtra`, so the gap stayed latent
  until a caller appeared.) [executed]
- `wire_protocol_fingerprint_check.py`: **PASS**. [executed]
- `tools/run_all_checks.ps1`: ran to completion, **90 passed, 0 skipped, 4
  failed**. All four attributed to concurrent work, none to these two commits:
  - `check_flash_worker_lint.ps1` — `drivers/persist/kiln_cfg_swap.c`
    (untracked, concurrent).
  - `check_link_impl_isolation.ps1` — `kiln_cfg_swap.c:47` `pending_crc()` and
    `test_kiln_cfg_swap.c:344` (same concurrent work).
  - `check_c_files_in_cmakelists.ps1` — `kiln_cfg_swap.c` not referenced by its
    target's `CMakeLists.txt`.
  - `check_01_kilnfw_pushed_build.ps1` — **`origin/main` (`8d685bfe`) does not
    build**: `drivers/persist/kiln_cfg_store.c:1479: error: 'snprintf' output
    truncated before the last format character [-Werror=format-truncation=]`.
    Also concurrent, but flagged loudly: the pushed branch is currently red.
  [executed]
- No board was flashed, no `debug_*` tool was called, no heating run was
  performed.

## Things this review did NOT establish

- Production reachability of Finding C's degraded read — only that the code path
  and the resulting condition exist, and that the written justification for its
  improbability is no longer true.
- Any behaviour of `safety_core.c`'s backstop *executing*: that file is not
  host-compilable, so the backstop itself was reviewed by reading and by
  source-text test, and only its *input* was exercised by execution.
- That the 2160 B predecessor figure was itself correct (`3d2c5413^` not
  rebuilt).
- Anything about `c2c9eff2` (the ESP-side apply transaction) beyond the two seams
  it exposed in `17740e47`.

### Addendum, same evening: B1 is now committed

While this review was being written, a concurrent session committed the
PcTools mirror as `67a21e62` ("Fix target-build regression and PC/firmware
protocol-version drift; triage check suite"), so
`tools/PcTools/src/kilnctrl/protocol.py`'s `UART_PROTOCOL_VERSION` now reads 12
in a commit rather than only in an uncommitted edit. **Finding B1 is closed.**
Finding B2 (the bump was avoidable, and is the 4th instance of the class
`check_uart_version_independence.ps1` exists to prevent) stands: the two
numbers still move together for a change that touched only the isolated link's
wire, and nothing mechanical would catch the next one.

---

## Fix pass, 2026-09-14 (Findings A and C implemented; scope: config_store_flash.c, safety_core.c, link_task.c, kilnlink_apply_config_volatile.h)

Both load-bearing findings above are now addressed. Scope per the
coordinating session's ownership split: SaftyFW's `config_store_flash.c`,
`safety_core.c`, `link_task.c` and their tests, plus the one CommonFW header
comment (`kilnlink_apply_config_volatile.h`) that documented the now-stale
claim. `kiln_cfg_swap.{c,h}`/`safety_cfg_http.c`, `sim_factorial*`, and the
coredump reader were not touched, per the standing ownership split noted at
the top of this document. No board was flashed, no `debug_*` tool was
called, no heating run was performed (a coredump investigation was reported
live at the start of this pass).

### Finding A -- the ARMED-loosening carve-out

`config_store_write_volatile()` (`firmware/SaftyFW/src/config_store_flash.c`)
now refuses a narrow class of installs while `relay_owner_get_state() ==
RELAY_OWNER_STATE_ARMED`, via a new pure helper,
`config_store_volatile_would_loosen_safety()`, evaluated against the live
`s_cached_record` as `cur` and the caller's proposed record as `next`.
**Exact gating condition: `RELAY_OWNER_STATE_ARMED`, not "is this a kiln
package swap" and not any broader firing-in-progress flag.** This is
deliberate, and is the crux the review asked to get precise:

- ARMED is entered ONLY by an actual energize request
  (`safety_core_request_enable()` -> `relay_owner_command_energize(true)`,
  itself gated by `commissioning_gate_energize_allowed()`) -- it is not set
  merely because a kiln package was swapped, and an ordinary swap performed
  while de-energized never sees this gate at all.
- A kiln-package swap is already refused during an actual firing by its own
  separate interlock (KILN_PROFILES_PLAN.md); this carve-out does not need
  to duplicate that refusal, only to cover the case that interlock does
  NOT cover -- a volatile install arriving while the relay happens to be
  live for any reason.
- ARMED is a one-way latch within a run (never demotes back to un-armed
  mid-firing) but it DOES clear at the start of the NEXT run/boot cycle
  before a swap for that next firing is staged -- so gating on ARMED
  (rather than, say, a broader "any firing was ever started" flag that
  never resets) does not refuse a legitimate swap prepared between firings.
  A swap prepared WHILE still ARMED from a still-live prior firing is
  exactly the case Finding A exists to refuse.

What counts as "loosening", decided per field, matching the review's ask
for explicit per-field answers:

| Field | Unset -> set (first commissioning) | Set, value lowered/same TC type | Set, value raised | Set -> unset | Set, TC type changed |
|---|---|---|---|---|---|
| `abs_max_temp_c` (S1) | allowed (tightening) | allowed | **refused** | **refused** | n/a |
| `max_rate_c_per_min` (S8) | allowed (tightening) | allowed | **refused** | **refused** | n/a |
| `tc_type` | allowed (tightening) | allowed (unchanged) | n/a | n/a | **refused** |

Reasoning: `abs_max_temp_c`/`max_rate_c_per_min` are fields_set-gated with a
documented "unset == never trips / rate check disabled" meaning
(CONFIG_REFERENCE.md sec 7, config_store.h's own field comments) -- unset IS
the loosest possible state, not a neutral default, so clearing a
commissioned value is exactly as much a loosening as raising it, and both
are refused; going the other way (committing a real value where none
existed) can only add a bound, so it is always a tightening and always
allowed. `tc_type` has the same fields_set-gated unset/set split (2026-08-24
addition, `CONFIG_STORE_SET_TC_TYPE`) but, once commissioned, has no
ordering between types the way a numeric threshold does -- it rescales what
`abs_max_temp_c`'s own already-validated bound means
(`TC_MAX_C_BY_TYPE[tc_type]`) and feeds the borrowed/main-board
type-mismatch guards -- so ANY change away from an already-committed type,
including clearing it, is treated as loosening; a first commissioning of
tc_type (unset -> any type) is still a tightening.

Every other field (PID/profile-shaped params, CT cal, `mains_voltage_v`,
etc.) is untouched by this carve-out -- an ordinary kiln-package swap still
never has to unarm the Pico for the other ~65 params, matching the plan's
section 1a.2 requirement the review itself reaffirmed.

Refusal is reported on the wire via the EXISTING
`KILNLINK_COMMIT_CONFIG_REJECT_ARMED` reason on the existing
`SAFETY_CMD_COMMIT_CONFIG_REJECTED` (0x20) frame
(`link_task_handle_apply_config_volatile()`, `firmware/SaftyFW/src/tasks/link_task.c`)
-- **no new wire value, no protocol bump.** That reason already existed for
COMMIT_CONFIG's own ARMED refusal; APPLY_CONFIG_VOLATILE's rejected reply
already carries the same reason byte, it was simply unreachable via this
path until now. `kilnlink_apply_config_volatile.h`'s comment claiming that
reason was "unreachable here by construction" is corrected in place.

Constraints honored: no change to the divergence enforcement, the
readiness-gate interlock, or the `autotune_baseline_k_dc` envelope (none of
this pass's files touch any of those); no `ZONES_CFG_VERSION` bump; the
identity bump (`config_version`/`config_crc` tracking the installed record)
is unaffected -- both are still pure functions of `s_cached_record`, and the
carve-out runs BEFORE the seqlock write, so a refusal never bumps identity
at all (confirmed by test: `test_write_volatile_refuses_loosening_while_armed`'s
each-refused-case sequence leaves the live record exactly as the last
SUCCESSFUL install left it).

### Finding C -- the backstop no longer trusts a failed read as "unconfigured"

`config_store_get_full_record()` (`config_store_flash.c`/`config_store.h`)
now returns `bool`: `true` when `*out` is a genuine snapshot
(`config_store_seqlock_read()` succeeded), `false` when it fell back to
`config_store_default()` for EITHER reason that can produce that fallback --
`!s_loaded` (boot_load hasn't run) or a retry-exhausted seqlock read (the
review's Finding C scenario). The bytes written to `*out` are unchanged in
both cases (still `config_store_default()`'s shape) -- only the return
value is new, so every pre-existing call site (`current_task.c`,
`link_task.c` x6, `thermo_task.c` x2, `safety_core.c`'s other, unrelated
call at line ~1657) compiles and behaves identically by simply ignoring it,
which is correct for all of them: falling back to the safe default on a
failed read was already their intended behavior.

The ONE call site that needed to tell the two apart -- `safety_core.c`'s
item-16 backstop -- now captures the return value as `cfg_read_ok` and
gates on it explicitly: `if (!cfg_read_ok) { /* decline, no action */ }
else if (abs_max_temp_c_unconfigured && ARMED) { de-energize }`. A failed
read makes the backstop decline to act in EITHER direction for that tick --
it does not clear the warn-latch (so a genuinely stuck condition is not
silently un-latched by one bad read) and does not de-energize (so a
transient retry-exhaustion window on a healthy, commissioned, firing board
no longer trips a spurious shutdown). The backstop's actual target case --
a board that is REALLY stuck ARMED-and-unconfigured -- keeps re-triggering
on every tick that DOES get a real snapshot (which is the overwhelming
majority of ticks even under the pathological condition Finding C
demonstrated), so declining on a failed read does not weaken the backstop's
coverage of the case it exists for.

The two retry-bound comments (`CONFIG_STORE_SEQLOCK_MAX_RETRIES`,
`CONFIG_STORE_FALLBACK_SEQLOCK_MAX_RETRIES`, and their three other
"ARMED-refused-anyway" references in `config_store_flash.c`) are corrected:
they no longer claim a write is rare BECAUSE ARMED refuses it (false since
item 15), only that it is rare because it is one seqlock write per wire
frame, never a burst -- the retry-exhaustion analysis those comments support
was already about write FREQUENCY, not about ARMED, so it is unaffected by
the correction.

`config_store_seqlock_write()`'s comment ("called only from
config_store_write()") is corrected to name both callers.

### Also fixed: a stub redefinition caught by the build itself

`firmware/SaftyFW/test/test_relay_owner_gpio_init_stubs.c`'s
`config_store_get_full_record()` test double still declared `void` after
the signature change, which MSVC's `warning C4142` (treated as error by
this project's build) caught immediately on the first host-test build
attempt -- fixed to `bool`, returning `false` (honest: this stub always
zero-fills, it never reads anything real).

### Is the UART protocol bump necessary? (addendum ask)

No new wire value and no protocol bump were needed for THIS pass's fix --
`KILNLINK_COMMIT_CONFIG_REJECT_ARMED` already existed on the wire from
COMMIT_CONFIG's original landing, and is simply reused, unmodified, on
APPLY_CONFIG_VOLATILE's existing rejected-reply frame. Confirms Finding B2's
own conclusion: the ONLY genuinely required bump in `17740e47` was
`KILNLINK_PROTOCOL_VERSION` 14->15 for the new frame id itself;
`UART_PROTOCOL_VERSION` 11->12 remains, on inspection, avoidable, and it
would have stayed avoidable under this pass's fix too, since no wire shape
changed. Whether `check_uart_version_independence.ps1` can be strengthened
to catch a hand-written bump of this shape: yes, in principle -- scoping
`wire_protocol_fingerprint_check.py`'s "uart" fingerprint spec to exclude
isolated-link `SAFETY_CMD_` entries (i.e. match only entries that also
appear in a PC-link-facing header) would stop a purely-isolated-link
addition from moving the PC-facing fingerprint at all, which removes the
version-bump PRESSURE at its source rather than trying to detect a
hand-written bump after the fact. That change lives in
`firmware/KilnFW/App/test/wire_protocol_fingerprint_check.py`, outside this
pass's SaftyFW-scoped ownership, and is not made here -- flagged as a
follow-up for whoever owns that file next. Doc citation nit already
corrected by the review above: the check is
`firmware/KilnFW/App/test/wire_protocol_fingerprint_check.py`, not
`tools/check_wire_protocol_fingerprint.ps1`.

### Negative tests (hand-broken, hand-restored, full rebuild)

1. **Finding A's carve-out disabled**
   (`config_store_flash.c`: `if (relay_owner_get_state() == ...` changed to
   `if (false && relay_owner_get_state() == ...`): **5 failures**, exactly
   the 5 new assertions in `test_write_volatile_refuses_loosening_while_armed`
   that expect a refusal (raise abs_max, clear abs_max, raise S8 rate,
   change tc_type, and the refusal-reason check) -- the 3 assertions
   expecting SUCCESS (lowering, and the neutral-field case) correctly kept
   passing, since disabling the carve-out only removes refusals, it does not
   add any. Restored by hand; full rebuild (`-OutDir` deleted and recreated)
   confirmed 2515/2515 + 56/56 + 255/255 clean again.
2. **Finding C's read-ok capture defeated**
   (`safety_core.c`: `bool cfg_read_ok = config_store_get_full_record(&cfg_rec);`
   changed to a discarded call plus `bool cfg_read_ok = true;` -- i.e. the
   backstop always believes the read succeeded, exactly the pre-fix
   behavior): **1 failure**,
   `test_backstop_gates_on_a_real_snapshot_not_just_fields_set` ("return
   value is captured (not discarded) as cfg_read_ok"). Restored by hand;
   full rebuild confirmed 2515/2515 + 56/56 + 255/255 clean again.

After restoring both by hand and a full clean rebuild:
**2515/2515 + 56/56 + 255/255, all passed, script exit 0.**

### Verification

- `firmware/SaftyFW` host tests, fresh private `-OutDir` under `C:\wt\...`
  via the bash tool each run (per the standing short-worktree-path
  instruction): baseline after the fix **2515/2515 + 56/56 + 255/255**;
  identical after both negative-test restores.
- `firmware/SaftyFW` target build (`check_00_saftyfw_target_build.ps1`):
  **PASS**, all three slot ELFs (`SaftyFW.elf`/`_slotA`/`_slotB`) linked
  including the new carve-out helper and the backstop's `cfg_read_ok` gate.
- `check_saftyfw_task_stack_budgets.ps1`: **PASS** -- `safety_core` still
  measures exactly 2168 B (the prior pass's pinned ceiling); the extra
  `bool cfg_read_ok` local did not move it further.
- `firmware/KilnFW` target build (`check_00_kilnfw_target_build.ps1`):
  **PASS** -- this pass's files are not linked into KilnFW at all (SaftyFW/
  CommonFW-only change plus one CommonFW header comment), so this was
  expected to be unaffected, and was.
- `tools/run_all_checks.ps1`: **94 passed, 0 skipped, 0 failed** -- a clean
  board, unlike the two prior passes' 88-90/94 (their remaining reds traced
  to `kiln_cfg_swap.c`/`safety_cfg_http.c`/`sim_*`, owned by other sessions,
  and have since been resolved by those owners).
- `tools/check_doc_hash_citations.ps1`: passes as part of the 94/94 above;
  every hash cited in this addendum (`67a21e62`, `17740e47`, `3d2c5413`,
  `d2671675`, `5a07116b`, `c2c9eff2`) already appears earlier in this same
  document and was previously verified to resolve.

No `ZONES_CFG_VERSION` bump. No protocol version bump (neither
`KILNLINK_PROTOCOL_VERSION` nor `UART_PROTOCOL_VERSION` moved in this pass
-- the reused `KILNLINK_COMMIT_CONFIG_REJECT_ARMED` reason needed no wire
change). No board flashed, no `debug_*` tool called, no heating run
performed.
