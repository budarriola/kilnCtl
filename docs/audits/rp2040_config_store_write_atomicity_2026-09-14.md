# RP2040 config_store write atomicity (D2) -- fixed, 2026-09-14

Finding D2 from `docs/audits/unreviewed_changes_review_2026-09-08.md`, carried
as open in `docs/CONFIG_FILESYSTEM.md` and named again in `CLAUDE.md`: the
RP2040 (SaftyFW) config store lacked write atomicity, in a way distinct from
the two already-fixed defects in the same file (the A/B sector erase-window
bug and the RAM-cache seqlock, both below). This pass verified the defect
from source, fixed it, and adds host-test coverage using the existing
power-cut injection harness (`firmware/SaftyFW/test/test_config_store_flash.c`).

**Bench verification is outstanding.** This pass is host-tests-only, per
explicit instruction -- another session was flashing the RP2040 concurrently.
Nothing here has been confirmed against real RP2040 flash.

## 1. The defect, verified from source

`config_store_next_write_slot()` (`firmware/SaftyFW/src/config_store.c:1101-1111`):

```c
size_t config_store_next_write_slot(size_t latest_slot_index)
{
    if (latest_slot_index == CONFIG_STORE_NO_SLOT) {
        return 0;
    }
    size_t next = latest_slot_index + 1;
    if (next >= CONFIG_STORE_SLOTS_PER_SECTOR) {
        return 0;
    }
    return next;
}
```

This is pure arithmetic: `latest_valid_slot + 1`, wrapped. It carries an
implicit assumption -- that a slot which has never held a *valid* record
since the sector's last erase is still physically blank (all `0xFF`). That
assumption is false whenever a previous write into that same slot was torn
by a power cut mid-program: the torn slot fails its own CRC check at the
next boot scan (`config_store_find_latest_multi_ex()`, `config_store.c`), so
`s_cached_slot` correctly lands on the last *good* slot before it -- but the
torn slot's bytes are still sitting in flash, non-erased. The very next call
to `config_store_next_write_slot()` computes `latest_good + 1`, which is
exactly that torn slot, and hands it back with no check that it is actually
erased.

`config_store_plan_write()` (`config_store.c:1067-1099`) calls
`config_store_next_write_slot()` on the "room left in this sector" path
(`plan.needs_erase == false`) -- the ordinary, non-8th-write append case.
`config_store_write()` (`firmware/SaftyFW/src/config_store_flash.c`, before
this fix at line 958) then programmed straight into that slot via
`hal_flash_program()` with no erase, because `plan.needs_erase` said none was
needed.

`hal_flash_program()`'s own documented AND-programming semantics
(`firmware/hwAbstraction/interface/hal_flash.h:249-260`) make this a real
corruption path, not merely wasted capacity:

> programming is NOT an overwrite ... any bit `buf` asks to be 1 that the
> flash had already cleared to 0 stays 0.

So a write landing on a torn slot silently produces `existing_bits &
new_bits` instead of the intended record. `hal_flash_program()` still returns
`HAL_OK` (the program instruction sequence ran fine), and -- before this
fix -- `config_store_write_cb()` (`config_store_flash.c`) reported `a->result
= HAL_OK` right after that call with no further check, so
`config_store_write()` returned `true` to its caller. **The caller believed
the write succeeded, the RAM cache (`s_cached_record`, updated via
`config_store_seqlock_write()`) was updated to the new value, and the
running board behaved as if the new value were committed -- while the actual
flash bytes are corrupted and will very likely fail their own CRC check at
the next boot, reverting to the last-good value with no distinguishing
signal.** This is exactly the "write reports success, then silently reverts
at next boot" defect named in `CLAUDE.md` and `docs/CONFIG_FILESYSTEM.md`.

The `NO_SLOT` case (`s_cached_slot == CONFIG_STORE_NO_SLOT`, i.e. no
CRC-valid record has ever been found anywhere) shares the same hazard: the
very first write is planned as sector 0 / slot 0 / `needs_erase = false`
("a fresh/blank sector may already be erased", `config_store.c:1071-1079`),
which is equally an unverified assumption -- a torn first-ever write at slot
0 leaves it non-erased with nothing recorded to say so.

## 2. How this differs from the two already-fixed config_store defects

Confirmed by reading `config_store_flash.c`'s own header comment and the two
seqlock blocks it documents -- **not the same defect, and neither was
re-fixed by this pass**:

- **A/B sector erase-window bug** (`flash_endurance_review_2026-09-07.md`
  R2): the original single-sector design had a real window, between the
  erase and the next successful program, where the sector held *zero* valid
  copies of the config at all. Fixed by adding sector B and the rule that a
  sector switch always erases the *other* sector from the one holding the
  current live record (`config_store_flash.c:29-77`). This fix is about the
  read side arbitrating between two sectors and never erasing the live one --
  D2 is about a torn slot being blindly reused for the *next* write inside
  a sector that already has room, an orthogonal failure mode the A/B design
  does not touch (the A/B design assumes the "room left" path always targets
  a genuinely blank slot -- exactly the assumption D2 shows is false).
- **RAM-cache seqlock** (2026-09-09, `config_store_flash.c:183-557`): a
  cross-core torn-*read* hazard on `s_cached_record`, fixed with a
  seqlock plus a writer-owned fallback double buffer. This is about a reader
  on the trip-path core observing a torn in-RAM snapshot mid-write. D2 is
  about torn *flash* bytes from an interrupted program, reused by a later
  write -- unrelated data (RAM vs. flash) and an unrelated hazard
  (concurrent read vs. reuse-after-tear).

D2 is a third, distinct defect that both of the above leave open: neither
fix ever inspects whether the *target slot itself* is actually erased before
programming into it.

## 3. The fix

Two parts, both in `firmware/SaftyFW/src/config_store_flash.c`:

**(a) Skip a torn/non-erased slot instead of reusing it.** In
`config_store_write()`, immediately after `config_store_plan_write()`
returns its plan: if the plan does not already call for an erase, read back
the target slot's bytes and check they are still all `0xFF`
(`config_store_flash_slot_is_erased()`, new). If they are not, treat it
exactly like the sector-full case -- switch to the *other* sector (never the
one holding `s_cached_sector`'s still-valid record, preserving the same
"never touch the sector holding the live record" atomicity guarantee the A/B
design already relies on) and force `needs_erase = true`. The `NO_SLOT` case
erases sector 0 directly, since by definition nothing valid exists anywhere
to protect. This check runs via `hal_flash_read()`, which carries no
execution-context restriction (`hal_flash.h`'s own doc comment), so it is
safe to call from ordinary task context before entering
`hal_flash_safe_execute()`.

**(b) Read-back verify after programming.** In `config_store_write_cb()`,
after `hal_flash_program()` reports `HAL_OK`, the callback now reads the
just-programmed slot back and `memcmp`s it against the exact bytes just
asked to be written. A mismatch sets `a->result = HAL_IO` instead of
leaving the prior (already-OK) status in place, so `config_store_write()`
reports failure to its caller and never advances `s_cached_slot`/
`s_cached_record` for a write that did not actually land as intended.

Precedent for (b) cited directly in the code comment: `boot_guard_mark_healthy()`
(`firmware/KilnFW/App/main_boot_early.c`) reported `HAL_OK` on an NVS write
whose persisted value never actually changed (`docs/audits/boot_guard_recovery_loop_2026-09-08.md`)
-- the fix there was also read-back verification with retry
(`verify_persisted_count()`). This confirms "a flash/NVS write's return code
alone cannot be trusted" is not a one-off finding in this codebase.

Limitation, stated plainly: (b) cannot catch a torn write caused by an
*actual* power cut in the same power cycle -- the MCU is off, so no code
(including this verify) runs again until the next boot, at which point the
existing CRC-scan-at-boot mechanism (unchanged by this pass) already
correctly rejects it. (b)'s value is catching any OTHER cause of a
program that silently fails to land bit-for-bit while every flash call
still reports success (e.g. a wear-out or write-disturb effect, not
necessarily a power event) within the SAME power cycle, so the caller and
RAM cache never diverge from flash even in that case. (a) is what actually
closes the "reused torn slot" reuse-after-tear path this finding is about.

## 4. Callers of `config_store_write()` -- no discarded result found

Three call sites, all in `firmware/SaftyFW/src/tasks/link_task.c`:
`link_task_handle_set_config()` (:1513), `link_task_handle_set_ct_cal()`
(:1578), and the `COMMIT_CONFIG` handler (:2158). All three assign the
return value to `bool written` and branch on it -- the success branch logs
"accepted" (and, for `COMMIT_CONFIG`, updates `s_staged_config`'s own local
baseline and calls `current_task_reload_cal()`); the failure branch logs the
refusal reason and does nothing else. None of the three touch any config
cache directly -- `config_store_write()` itself is the sole writer of
`s_cached_record`/`s_cached_slot`/`s_cached_sector`, and it only updates them
after both `hal_flash_safe_execute()`'s status and `args.result` (now
including the read-back check) report success. **No caller discards the
write result; this is not a second instance of the "logging unchecked
success" class** (`docs/audits/...` / `project_safety_calls_logging_unchecked_success` per
this repo's memory index) -- the architecture already centralizes the
success/failure gate inside `config_store_write()` itself.

## 5. Test coverage added

`firmware/SaftyFW/test/test_config_store_flash.c`, using the existing
fake-flash-backed power-cut injection harness (`fake_flash.h`'s
`fake_flash_script_next_op_status()` / direct `hal_flash_program()` of a
partial page, the same technique the pre-existing mid-program power-cut test
uses):

- `test_torn_inline_slot_is_never_reprogrammed()` -- three good writes land
  in sector A slots 0-2; slot 3 is directly torn (first page only
  programmed, modeling a power cut mid an ordinary same-sector append, NOT
  the 8th-write switch case already covered). Asserts: the next write
  succeeds; it switches to (and erases) sector B rather than reusing slot 3;
  slot 3's torn bytes are byte-for-byte untouched after the write (proving
  it was never reprogrammed); the new value is visible immediately and
  survives a simulated reboot (`config_store_boot_load()` called again).
- `test_torn_first_slot_before_any_valid_record_is_never_reprogrammed()` --
  the `NO_SLOT` companion: slot 0 of sector A is torn before any record has
  ever validated. Asserts the next write succeeds by erasing sector A itself
  (nothing valid existed anywhere to protect), and the value survives a
  simulated reboot.

Both tests independently satisfy every point in the task's coverage list: a
torn write is detected (boot scan already did this; unchanged) and the
*next* write is proven not to reuse the torn slot; the post-reboot value
matches what was last successfully written; a successful write still
round-trips (asserted directly in both new tests, and unchanged by every
pre-existing round-trip test in this file, all of which still pass).

No dedicated fault-injection test exists for read-back-verify (2(b)) firing
on its own, because `fake_flash.h` has no primitive for "the program call
reports `HAL_OK` but the byte-for-byte result differs" independent of the
already-covered non-erased-target case -- only `fake_flash_script_next_op_status()`
(outright failure status) and the partial-page torn-write model exist. Every
existing and new test's successful-write assertions already exercise the
non-corrupting path of the read-back-verify code on every passing write in
this file (196 pre-existing + several per new test), so a regression that
made the verify itself falsely fail would have shown up as a mass test
failure -- confirmed it does not.

### Negative test (confirms the tests actually test something)

Temporarily disabled the slot-erased guard in
`config_store_write()` (`if (false && !plan.needs_erase && ...)`), rebuilt,
and reran: both new tests failed as expected (9 `TEST_CHECK` failures across
the two, `211/220` checks passed) -- confirming they exercise the fix rather
than passing vacuously. Restored the guard by hand (removed the injected
`false &&`), deleted `firmware/SaftyFW/test/build/`, and did a full clean
rebuild: `220/220` checks passed, all green.

## 6. Flash endurance impact

The read-back verify (2(b)) adds one `hal_flash_read()` per successful
write -- reads, not writes, so no endurance impact at all (this store
already has 100x-1000x margin per
`docs/audits/flash_endurance_review_2026-09-07.md`, unaffected either way).

The slot-skip fix (2(a)) DOES change write/erase patterns, but only in the
already-rare torn-write case: every *ordinary* write is completely
unaffected (the erased-check passes trivially on a genuinely blank slot, no
extra erase). Only a write immediately following a torn write pays an extra
full-sector erase-and-switch that the old (broken) code would not have paid
-- and the old code's alternative was silently corrupting the record, not a
cheaper legitimate outcome, so this is not a fair "wear cost" trade at all;
it is the cost of doing the operation correctly. This can happen at most
once per genuine power-cut-during-a-write event, which is already the rare
case every other test in this file is built around (not a hot-path event).
No change to the steady-state 1-erase-per-8-writes-per-sector cadence the
A/B design established.

## 7. Checks

`firmware/SaftyFW/test/build_host_tests.ps1` (via Bash, short worktree path
per this repo's own build note): full clean rebuild, `220/220` checks
passed (up from the pre-existing `196/196`; the 24 new checks are inside the
two new test functions above).

`tools/run_all_checks.ps1 -ExecutionPolicy Bypass`: `91/94` passed. All
3 failures are pre-existing and unrelated to this pass's files
(`firmware/SaftyFW/src/config_store*`, its host tests, and this doc):

- `firmware\KilnFW\App\test\check_sim_iter_tune_bars.ps1` -- A1 bar
  (`PINNED KNOWN-FAILURE CEILING <= 24/660`) failing at 3.64%; PID/fuzzy
  tuning area, owned by other in-flight sessions per this task's own
  concurrency notes (`pid_fuzzy.c`, `adaptive_tune_ki.c`, etc.) -- untouched
  by this pass.
- `tools\PcTools\check_zones_per_zone_field_drift.ps1` -- `zones_http_get.c`
  drift (`model_fit_ambient_c`/`model_fit_temp_c`), also explicitly another
  session's file per this task's concurrency notes -- untouched by this
  pass.
- `tools\PcTools\selfcheck.py` -- infra-level failure ("dst task 2 not
  registered, replying NACK"), unrelated to config_store.

`tools/check_doc_hash_citations.ps1`: both hashes cited above
(`51e1ef5`, `0b5d9dad`) verified as real commits via `git cat-file -t`
before writing this document.
