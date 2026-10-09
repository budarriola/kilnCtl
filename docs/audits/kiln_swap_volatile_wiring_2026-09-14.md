# Wiring kiln_cfg_swap.c to the Pico's volatile install -- 2026-09-14

Connects `c2c9eff2` (the two-processor apply transaction,
`docs/audits/kiln_swap_transaction_2026-09-14.md`) to `17740e47`/`3d2c5413`
(SaftyFW's volatile RAM-only config install, item 15, and the unconfigured-
ceiling backstop, item 16 --
`docs/audits/pico_volatile_install_and_unconfigured_ceiling_2026-09-14.md`).
Those two passes were built in parallel against each other's specified
interfaces; this pass makes the swap transaction's Pico push actually use
item 15's `SAFETY_CMD_APPLY_CONFIG_VOLATILE` (0x2D) instead of the flash-
writing `COMMIT_CONFIG` path it was forced onto before that command existed.

Scope, per the coordinating session's ownership split: `kiln_cfg_swap.{c,h}`,
`safety_cfg_http.{c,h}`, and their host tests. Additionally, wiring an
ESP-side sender for 0x2D required a small, additive addition to
`safety_link.h`/`safety_link_commands.c` (a new function,
`safety_link_send_apply_config_volatile()`, byte-for-byte the same shape as
the existing `safety_link_send_commit_config()`) -- no other file in that
pair was touched. `firmware/KilnFW/App/test/build_host_tests.ps1` needed one
line added (`kilnlink_apply_config_volatile.c` to `test_safety_link_compile`'s
own source list, `$slExtra`) so that executable's real `safety_link_
commands.c` link cleanly against the new codec.

## 1. The swap now completes with the Pico ARMED throughout, never disarmed

Every Pico-touching call in `kiln_cfg_swap.c` was routed through the
volatile path:

- Step 4 (ceiling raise-first): `safety_cfg_http_set_and_confirm_f32_
  volatile()` (new function), not the flash-writing `safety_cfg_http_
  set_and_confirm_f32()` the standing ceiling-reconcile loop
  (`safety_ceiling_sync.c`, untouched, still flash) keeps using.
- Step 6/7 (bulk push of every other field): `safety_cfg_http_apply_
  package_and_confirm(link, pkg, /*volatile_install=*/true, ...)` -- the
  function gained a `bool volatile_install` parameter; its only caller is
  this module, so the signature change is contained.
- Rollback (both the in-transaction rollback and boot recovery's PICO_OPEN/
  PICO_DONE/ESP_DONE-failure paths, since they all share one `rollback()`
  implementation): also `volatile_install=true`, per task item 4 below.

Both new/changed calls reach `apply_pairs_ex()` (`safety_cfg_http.c`), which
now sends `SAFETY_CMD_APPLY_CONFIG_VOLATILE` instead of `COMMIT_CONFIG` when
`volatile_install` is true. `confirm_commit_landed()` needed **no changes**:
it only ever forces a live `GET_CONFIG_PAGE` re-fetch and compares values,
which is identical regardless of which command produced the live state --
exactly the reuse the brief asked for (field-by-field readback unchanged,
identity/hash check unchanged, no second detector).

`safety_link_send_apply_config_volatile()` is the ESP-side sender: same
`xact_lock` hold, same pre-send drain, same BROADCAST send (an ACK'd DATA
send is dropped by the Pico's frame handler, same as `COMMIT_CONFIG`'s own
comment explains), same `SAFETY_LINK_REPLY_TIMEOUT_MS` window for a possible
`COMMIT_CONFIG_REJECTED` reply -- `link_task_handle_apply_config_volatile()`
on the Pico reuses `link_task_send_commit_config_rejected()` verbatim for its
own validation-refusal reply, so no new rejection-decode path was needed on
either side.

Test: `test_swap_completes_with_pico_armed_never_disarmed`
(`firmware/KilnFW/App/test/test_kiln_cfg_swap.c`) forces the test fake's
`s_pico_armed=true` (every flash/`COMMIT_CONFIG`-shaped call in that fake is
then refused, mirroring `config_store_decide_write()`'s real ARMED gate) and
asserts the swap still succeeds, the ceiling lands at the target value, and
at least two calls went through the volatile path. **Confirmed: a swap
completes end-to-end with the Pico simulated ARMED for the whole transaction
and is never reported as disarmed at any point** (nothing in this module
ever calls anything resembling a disarm/re-arm primitive -- there is none in
its dependency surface).

## 2. What persists on the Pico, and when

- **Immediately, in RAM only:** everything the swap pushes (ceiling +
  every other field), via `SAFETY_CMD_APPLY_CONFIG_VOLATILE`. This is what
  the field-by-field readback and the ceiling/arming divergence check verify
  before the swap is ever reported successful -- unchanged from before this
  pass except for which wire command produced the state being verified.
- **Opportunistically, to flash, after the swap has already succeeded:**
  new step 13, `persist_pico_flash_fallback()` (`kiln_cfg_swap.c`), called
  once from `kiln_cfg_swap_apply()`'s own success path (right after step 12
  finalizes) and once from `kiln_cfg_swap_boot_recover()`'s `ESP_DONE`
  finish branch. It re-pushes the ceiling via the flash-writing `safety_cfg_
  http_set_and_confirm_f32()` and the rest via `safety_cfg_http_apply_
  package_and_confirm(..., volatile_install=false, ...)` -- i.e. ordinary
  `COMMIT_CONFIG`. **Allowed to fail, always**: an ARMED refusal here (the
  Pico's normal running state, so this is the expected outcome on most
  boards) is logged at `ESP_LOGI`, never alarmed, and never affects the
  swap's own already-decided result.
- **NOT built:** the plan's `pkg_hash` bookkeeping and boot-time `FALLBACK`/
  `CONFIGURED`/`UNCONFIGURED` state machine (section 1a.4). No field exists
  in `config_store_record_t` or on the wire to carry a package hash, and
  adding one is out of this pass's scope (no `ZONES_CFG_VERSION` bump, no
  new wire field, per the brief's own constraint). A successful flash
  fallback simply becomes the Pico's ordinary flashed record, indistinguishable
  from an operator pushing the same values while unarmed -- a reasonable
  bring-up fallback on its own, just without the hash bookkeeping the fuller
  plan describes.

Test: `test_flash_fallback_attempted_after_finalize_but_optional` (Pico NOT
armed in this one, so the fallback actually lands) confirms step 13 runs and
re-pushes the identical target values without corrupting the already-
verified state.

## 3. Rollback uses the same mechanism (task item 4)

`rollback()`'s Pico restore now calls `push_and_verify_pico(link, &p->
rollback_pico, /*volatile_install=*/true, ...)` -- the exact same command as
the forward push. Before item 15 existed, restoring R would have gone
through the same flash-writing path the forward push did, and would have
been refused while ARMED in exactly the situation most likely to need a
rollback (an ARMED board on which the forward push failed partway). Routing
only the forward path through 0x2D and leaving rollback on flash would have
left that case broken.

`rollback()` also now directly restores R's ceiling via `safety_cfg_http_
set_and_confirm_f32_volatile()` (best-effort; a failure here is logged, and
the pre-existing `safety_ceiling_sync_reconcile_on_link_up()` call right
after it runs as a second attempt) rather than leaving the ceiling entirely
to that standing reconcile loop's own flash-writing, ARMED-backoff-gated
cadence -- deterministic and immediate instead of dependent on a background
retry.

**Test result (task item 4's specific ask):**
`test_rollback_after_volatile_install_uses_volatile_too` forces
`s_pico_armed=true` and `s_zones_import_should_fail=true` (the Pico push at
step 6/7 succeeds via volatile install; the ESP-side commit at step 8 then
fails, triggering rollback). Assertions: the swap is refused (`ok==false`),
NOT reported as a divergence/alarm, the pending record is cleared (clean
rollback), the last push recorded by the fake used the volatile path, the
ceiling restore also used the volatile path, and the package actually pushed
back to the Pico was R (ceiling 1000), not P (ceiling 1300) or left
unrestored. **Confirmed: rollback succeeds on a simulated-ARMED Pico.**

**Negative test performed** (task instruction: negative-test this specific
case): `rollback()`'s `push_and_verify_pico(...)` call was changed BY HAND
from `volatile_install=true` to `volatile_install=false`, the suite was
rebuilt from a fresh `-OutDir`, and re-run. Result: 5 assertions failed in
`test_rollback_after_volatile_install_uses_volatile_too` -- the rollback
itself failed outright (the fake's ARMED simulation refused the flash-path
push), proving the guard is load-bearing, not vacuous. The change was then
reverted BY HAND, the build directory deleted, and a full clean rebuild
reconfirmed all 45 host-test executables green (`kilnctl_host_tests_kiln_
cfg_swap.exe`'s own 15 sections included).

## 4. Pico reboot between a verified volatile install and the flash fallback

A volatile install does not survive a Pico reboot -- `config_store_write_
volatile()` only ever touches RAM (`s_cached_record` via the seqlock), never
`s_cached_slot`/`s_cached_sector` or flash I/O (confirmed by `17740e47`'s own
test, `test_write_volatile_repeated_then_flash_commit_still_gated`, and by
inspection of `config_store_boot_load()`, which reads only the flashed
record on boot). So the sequence "step 6/7 volatile-installs and verifies
successfully" -> "Pico reboots (crash, watchdog, power glitch -- anything)
before step 13's flash fallback lands" -> "Pico boots back onto its OLD
flashed record" is a real window, and during it the ESP believes the swap
already succeeded (its own steps 8-12 already ran and finalized).

**This is caught by the EXISTING check, not a new one**, per the task's own
instruction: `safety_ceiling_sync_is_diverged()` (`safety_ceiling_sync.c`,
reused verbatim -- see this module's header comment on why no second
detector was added) is the standing, continuously-running divergence check.
A Pico reboot is a link-down/link-up transition; that transition's own
reconcile-on-link-up re-fetches the Pico's live ceiling and compares it
against the ESP's expectation. Since step 4 now volatile-installs the
ceiling as part of every swap (see section 1 above -- this is *why* step 4
had to move to the volatile path, not only for the "never disarm" goal), a
reboot that reverts the whole live record to its old flashed state also
reverts the ceiling, and the existing check reports a mismatch the next time
it runs after the reboot.

**Test result:** `test_pico_reboot_before_flash_fallback_caught_by_existing_
check` simulates this by forcing `s_diverged=true` at the point step 10/11
would consult it (representing "the Pico rebooted and its live ceiling has
reverted"), with the Pico still armed (so step 13 never had a chance to
land). Assertions: the swap does NOT report success, `out_diverged` is set,
`active_id` is NOT finalized, the pending record is left at `ESP_DONE` (so
boot recovery's "verify then finish" path, or a later reconcile once the
Pico is unarmed, gets another chance), and step 13 never runs (the swap
never reached finalize). **Confirmed: caught, not silently lost.**

**Negative test performed**: the `if (diverged)` guard at step 11 in
`kiln_cfg_swap_apply()` was changed BY HAND to `if (false && diverged)`, the
suite was rebuilt from a fresh `-OutDir`, and re-run. Result: 9 assertions
failed across BOTH `test_diverged_ceiling_does_not_finalize` (the
pre-existing test for this same gate) and the new
`test_pico_reboot_before_flash_fallback_caught_by_existing_check` -- with
the gate defeated, the swap wrongly reported success, finalized `active_id`,
and cleared the pending record despite the simulated divergence. Reverted by
hand, build directory deleted, full clean rebuild reconfirmed all 45
executables green.

### One named caveat, stated honestly rather than papered over

The existing check's coverage of "did the whole live record revert" is
*via* the ceiling field specifically, not a full-record identity compare
(`config_divergence.c`'s own identity set today has exactly one field,
`abs_max_temp_c`, per `safety_ceiling_sync.c`'s own caller). This means the
above coverage argument depends on the ceiling actually changing (or at
least being re-asserted) as part of the swap that just ran. A swap whose
target ceiling is numerically IDENTICAL to the pre-swap ceiling still
re-installs it volatile at step 4 (`raise_first` is true whenever `target_
ceiling >= current_ceiling`, which includes equality) -- so in practice
every swap this module performs does push the ceiling through the same
volatile/reboot-vulnerable path as everything else, and the existing check
therefore does have a live signal to react to after any reboot in the
danger window. The caveat is theoretical rather than a real gap given that
guarantee, but it is worth naming precisely: the coverage is "the ceiling is
always part of what a swap volatile-installs, and the existing check
watches the ceiling", not "the existing check watches the whole record".
Extending `config_divergence.c`'s identity set to cover more than the
ceiling would close this precisely, but that is `safety_ceiling_sync.c`/
`config_divergence.c` territory, outside this pass's file ownership.

## Constraints honoured

- No `ZONES_CFG_VERSION` bump.
- No weakening of the divergence enforcement or the readiness-gate
  interlock -- `apply_pairs_ex()`/`confirm_commit_landed()`'s verification
  logic is identical regardless of `volatile_install`; the field-by-field
  Pico readback and the byte-for-byte ESP readback are untouched.
- No HTTP/LCD wiring, no worker-task dispatch -- `kiln_cfg_swap_apply()`
  remains synchronous/blocking by design, unchanged from `c2c9eff2`.
- The bounded-disarm window the owner withdrew as unnecessary was never
  reintroduced; nothing in this module calls any disarm/re-arm primitive.

## Host test results

`firmware/KilnFW/App/test/build_host_tests.ps1 -OutDir <fresh dir>`: 45/45
executables built and passed, including `kilnctl_host_tests_kiln_cfg_
swap.exe` (15 sections, all green) and `kilnctl_host_tests_safety_cfg_
http.exe`/`kilnctl_host_tests_safety_link.exe` (both needed a matching fake/
link addition for the new `safety_link_send_apply_config_volatile()`
symbol, see above). The only non-zero-exit contributor is the pre-existing,
explicitly-non-blocking `sim_credibility_gate` informational tally
(unrelated to this work, per `docs/audits/sim_credibility_gate_real_cause_
2026-09-10.md`) -- run twice more after the two negative tests below, and a
third time clean after both were restored by hand.

**No blocker hit, unlike both prior passes:** `check_00_kilnfw_target_build.
ps1` was run directly by this pass (not just via `tools/run_all_checks.ps1`)
after wiring `kiln_cfg_swap.c` into `CMakeLists.txt`, and it now succeeds --
`Successfully created ESP32-S3 image.` -- with no Python-interpreter/CMake-
cache mismatch encountered (the environment issue both prior passes
reported appears to have been resolved separately in the meantime; a
`kiln_cfg_store.c` truncation-buffer regression noted mid-task by the
coordinator was already fixed by the check-suite triage pass, not by this
one). This is the first real target-build verification `kiln_cfg_swap.c`
has had; see the next section for the one genuine regression it surfaced
(a format-truncation warning this pass's own code introduced) and its fix.
Host tests remain the primary, most-exercised verification for this
module's logic, but the target build is no longer merely "not attempted".

## `tools/run_all_checks.ps1`

**First pass (before addressing the triage note below): 90 passed, 4
failed** -- all four pre-existing from `c2c9eff2` (the module was built and
host-tested but never wired into the target build), not introduced by this
pass:

- `firmware/KilnFW/App/test/check_01_kilnfw_pushed_build.ps1` -- fails
  checking out and building `origin/main` itself (a separate, already-pushed
  commit, `8d685bfe`), unrelated to any file this pass touched. Left as-is
  (out of this pass's ownership; a separate pre-existing issue).
- `firmware/KilnFW/App/test/check_flash_worker_lint.ps1` -- `kiln_cfg_
  swap.c` not on the allowlist.
- `firmware/SaftyFW/tools/check_link_impl_isolation.ps1` -- `kiln_cfg_
  swap.c`'s `pending_crc()` (and `test_kiln_cfg_swap.c`'s fake `safety_cfg_
  store_cached_crc()`) flagged as CRC implementations outside CommonFW.
- `tools/check_c_files_in_cmakelists.ps1` -- `kiln_cfg_swap.c` not
  referenced by `firmware/KilnFW/App/drivers/CMakeLists.txt`.

**A separate check-suite triage pass (`67a21e62`, `docs/audits/check_
suite_triage_2026-09-14.md`) correctly flagged the last three of those as
belonging to this pass rather than leaving them as permanent debt** -- an
unlinked module ships in no firmware image and cannot be verified on
hardware, which a later reader would not expect from "host tests pass".
Addressed here:

1. **`firmware/KilnFW/App/drivers/CMakeLists.txt`**: added `persist/kiln_
   cfg_swap.c` alongside its `kiln_cfg_store.c`/`kiln_package.c` neighbours.
2. **`firmware/KilnFW/App/test/flash_worker_lint.py`**: added a `kiln_cfg_
   swap.c` `ALLOWLIST` entry. `save_pending()`'s `hal_kv_set_blob()`/`hal_kv_
   commit()` calls are Pattern 2 (a local `hal_kv_write_safe_here()` guard,
   the SAME shared predicate `kiln_cfg_store.c`'s own entry cites -- not a
   re-derived copy), refusing the write outright if ever called from a
   PSRAM-stacked task; `kiln_cfg_swap_apply()` is documented to run only
   on a dedicated internal-SRAM worker task, so this is the defensive
   backstop, not the primary argument.
3. **`firmware/SaftyFW/tools/check_link_impl_isolation.ps1`**: added
   `kiln_cfg_swap.c` and `test_kiln_cfg_swap.c` to `$allowlistPaths`, same
   class as every other entry already there (this file's own header comment
   lists nine precedents) -- `pending_crc()` is `esp_crc32_le()` record-
   integrity CRC32 over the pending-swap NVS blob, a different algorithm
   entirely from the link's CRC16-CCITT-FALSE this check protects; the test
   file's fake `safety_cfg_store_cached_crc()` is a plain accessor
   returning a fixed test value, computing nothing, same shape as `test_
   safety_cfg_http.c`'s identical stub already allowlisted two lines below
   it. **Not** a second implementation of the wire CRC and does not risk
   silently disagreeing with the package hash the coordinator's note asked
   to check for -- `pending_crc()`'s own doc comment states it hashes the
   pending-swap record's OWN fields (marker/target_id/rollback blob/etc.),
   a wholly separate value from `kiln_package.c`'s package-identity hash or
   the link's frame CRC, never compared against either.
4. **A genuine regression surfaced once the module was actually linked
   into the target build**: `-Werror=format-truncation` on three `snprintf`
   calls in `kiln_cfg_swap.c` (`push_and_verify_pico()` and two sites in
   `rollback()`) where GCC can prove a fixed-size `KILN_CFG_SWAP_REASON_MAX`
   (200) local (`sub`/`push_reason`) is embedded via `%s` into another
   same-sized buffer with a literal prefix, and therefore cannot prove no
   truncation. Fixed with an explicit precision specifier on each (`%.181s`/
   `%.138s`/`%.136s`, sized to the exact "region of size N" GCC's own
   diagnostic reported) -- this makes the already-harmless worst case
   (`snprintf` always NUL-terminates; a cut-off reason string was never a
   safety issue) provable to the compiler instead of merely true in
   practice. `firmware/KilnFW/App/test/check_00_kilnfw_target_build.ps1`
   confirmed clean afterward (`Successfully created ESP32-S3 image.`) --
   **this is the first real target-build verification this module has ever
   had**, closing both prior passes' own "host tests only, target build not
   attempted" notes for this specific module.

**Re-run after all four fixes: 94 passed, 0 failed.**
`tools/check_wire_protocol_fingerprint.ps1` and `tools/check_doc_hash_
citations.ps1` both PASS throughout (untouched by this pass's own changes).

## Hash citations

- `c2c9eff2` (commit) -- the two-processor apply transaction.
- `17740e47` (commit) -- SaftyFW volatile config install (item 15).
- `3d2c5413` (commit) -- SaftyFW unconfigured-ARMED backstop (item 16).
- `docs/audits/kiln_swap_transaction_2026-09-14.md` -- prior audit, this
  pass's starting point for the swap side.
- `docs/audits/pico_volatile_install_and_unconfigured_ceiling_2026-09-14.md`
  -- prior audit, this pass's starting point for the Pico side.
- `67a21e62` (commit) -- the check-suite triage pass that flagged the three
  `kiln_cfg_swap.c` checks addressed above as this pass's own to clear.
