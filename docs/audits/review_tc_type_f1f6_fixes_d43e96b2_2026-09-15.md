# Adversarial review: the F1-F6 fixes for the settable safety thermocouple type

2026-09-15. Adversarial, read-only review (no flashing).

Scope: the commit whose subject is "Fix F1-F6 from the tc_type adversarial
review" -- the tip of `origin/main` at the time of this review, its short hash
beginning `d43e96` and ending `b2`. Its parent's short hash begins `3ec087`
and ends `93`. Method: a clean worktree of `origin/main` under `C:\wt\`, plus a
second clean worktree at the parent for per-test comparison. Both were removed
afterwards. Review only; no production code was changed.

The owner's decisions on this feature are settled and were treated as
premises, not as subjects of review: the safety thermocouple type is settable;
while ARMED it may change only when heat-enable is off and no firing is
running, every other parameter keeping the existing ARMED refusal; the Pico
stays ARMED throughout and reapplies immediately; the commissioning page owns
the type and the ESP only reads it back; the Pico's `abs_max_temp_c` must
always equal the ESP's; and configuration divergence between the two
processors is a fault that must alarm rather than silently reconcile.

## Verdicts

| Finding | Verdict |
| --- | --- |
| F1 -- ARMED_HEAT_ON / ARMED_HEAT_UNKNOWN / ARMED_MIXED reject wording | CLOSED (landed in an earlier commit, confirmed independently) |
| F2 -- REFUSED_ARMED mapped to ARMED_MIXED on a mixed change | PARTIALLY CLOSED (defect 1 below) |
| F3 -- reentrancy trip-wire logs and fails closed instead of `abort()` | CLOSED (the stated reason for untestability is wrong; the conclusion is right) |
| F4 -- `tc_type_apply_in_progress` interlock on enable | CLOSED (verified real, not a no-op) |
| F5 -- pending-flag comment accuracy | CLOSED (comment-only) |
| F6 -- backup export reads the live Pico type | CLOSED, and the only load-bearing new test |

## F1: the "absorbed byte-identically by another session" claim is TRUE

The commit's path list deliberately omits
`firmware/KilnFW/App/drivers/http/safety_cfg_http.c`, on the claim that an
earlier commit from another session had already landed the identical hunks.
That is exactly the sort of claim that deserves an independent check, because
the failure mode it hides -- behaviour falling between two commits and
existing in neither -- is invisible from either commit alone.

It checks out. `commit_reject_reason_words()` on `origin/main` carries real
sentences for all three new reject reasons, and
`reject_reason_to_refusal_class()` maps all three to
`SAFETY_CEILING_REFUSAL_ARMED`. Each sentence contains the literal word
"ARMED", which is what the commissioning page's case-insensitive `/ARMED/`
match keys on. Asking git which commit introduced those lines names the commit
whose subject is "Fix HIGH1/HIGH2/MEDIUM3-6/LOW7-10 from
review_divergence_fixes ..." (short hash beginning `60d655`, ending `2f`), and
`git merge-base --is-ancestor` confirms that commit is an ancestor of this
commit's parent. The behaviour genuinely exists on `origin/main`; it did not
fall between two commits.

## Per-test results, parent versus child

Measured, not taken from the commit message. Both reported figures are correct
at the child.

Child (this commit), clean worktree:
- KilnFW host tests: `Built: 47/47 executables`, `all 47 host test executables
  built and passed`. Main executable `7902/7902 checks passed`;
  `safety_cfg_http` `219/219`; `heat_owner_active_decide` `6/6`.
- SaftyFW host tests: `259/259 checks passed`.

Parent, clean worktree:
- SaftyFW host tests, unmodified parent: `259/259 checks passed` -- identical
  to the child. This commit adds zero SaftyFW test coverage.
- KilnFW, with only the two changed test files overlaid onto the parent:
  `Built: 46/47 executables`, `BUILD FAILURES (1): heat_owner_active_decide`,
  main executable `7899/7902 checks passed`, `3 FAILURE(S)`.

| New test | Parent | Child | Load-bearing? |
| --- | --- | --- | --- |
| `test_export_emits_live_pico_tc_type_not_stale_esp_cache` (F6) | FAIL, 3 assertions | PASS | Yes |
| `test_apply_pairs_rejected_commit_new_tc_type_reasons_are_readable` (F1) | PASS | PASS | No -- characterization only |
| `test_heat_owner_active_decide.c` (F6) | BUILD FAILED | PASS, 6/6 | No -- see below |

The F6 export test is the one genuine regression detector. At the parent it
fails three assertions: that the live Pico value is emitted, that the stale
ESP-cached value is not, and that an unknown live value renders as 0 rather
than as the stale cache or the stub's own value. Those three account for
exactly the parent's `3 FAILURE(S)`.

The F1 wording test passes against the parent, because the wording landed in
the earlier commit named above, which is an ancestor of the parent. It is a
valid characterization test for behaviour that already shipped, not a
regression detector for this commit. That is not a defect -- it is the correct
consequence of F1 having been absorbed -- but it should not be counted as
evidence that this commit fixed anything.

`test_heat_owner_active_decide` fails to build at the parent purely because the
source file does not exist there. A "the API does not exist yet" failure is not
behavioural evidence. Separately, `heat_owner_active_decide()` is a
byte-for-byte extraction of the condition previously inline in
`safety_link_frames.c`, so the test characterizes existing behaviour rather
than detecting a change in it.

## Defect 1 (MEDIUM): F2's mixed detection reads the record the same fix deliberately stopped trusting

`link_task_handle_commit_config()` captures

```c
uint8_t prev_tc_type = config_store_get_tc_type();
```

before the write, and F2 uses it to choose between
`KILNLINK_COMMIT_CONFIG_REJECT_ARMED_MIXED` and the plain `..._ARMED`. The
added comment claims this is the "same 'tc_type differs from what's persisted'
test config_store_flash.c uses". It is not.

`config_store_get_tc_type()` returns `s_cached_record.tc_type` through the
seqlock. `config_store_write_ex()` and the MIXED sentence it logs both use
`s_persisted_record`. The distinction is not incidental: the comment block at
`config_store_flash.c:185-196` records that the earlier "item 15" fix moved
that comparison off the cached record precisely because a prior
`config_store_write_volatile()` install leaves the cached record carrying
values that are not on flash. F2 reintroduces a read of the cached record one
layer up.

Consequence: after a volatile install that changed tc_type while the relay was
not ARMED, `s_cached_record.tc_type` and `s_persisted_record.tc_type` disagree.
A later refused COMMIT_CONFIG can then send one wire reason while the Pico's
own log line asserts the other -- the operator is told "only thermocouple type
may change while ARMED" by one channel and plain "ARMED" by the other, for the
same refusal.

Severity is limited, and deliberately so: both branches still refuse the write.
This cannot persist anything, cannot leave the Pico unarmed, and cannot make
the ceilings unequal. It is a wrong-label defect on a refusal path, plus a
comment asserting an equivalence that does not hold.

**Fix:** compare against the persisted record, or have `config_store_write_ex()`
report which classification it actually used rather than having the caller
re-derive it from different state.

Coverage: none. `ARMED_MIXED` appears in no SaftyFW test file.

## F3: fail-closed is genuinely closed; the untestability reason is wrong

The trip-wire in `config_store_only_tc_type_differs()` replaces an `abort()` --
which on the RP2040 resets the chip and drops K4 -- with a log line and
`return false`.

Returning `false` means "this is not a tc_type-only change", which routes into
`config_store_decide_write_ex()`'s unconditional
`CONFIG_STORE_WRITE_REFUSED_ARMED`. So the failure path refuses, and in the
safety direction specifically:

- It cannot accept a write it should have refused. `false` is strictly the more
  restrictive answer; only `true` unlocks the ARMED carve-out.
- It cannot leave the Pico unarmed. Removing the `abort()` is what closes that
  hole, not what opens it -- the previous behaviour was a chip reset, the one
  outcome the owner ruled out.
- It cannot create a silent divergence between the two processors' views of the
  type, because nothing is written on a refusal, and the refusal reaches the ESP
  as a reject frame rather than as silence.

The re-entrant call returns before touching either scratch buffer and does not
clear the outer call's flag, so the outer comparison is not corrupted.

The claim that this is not host-testable is correct in its conclusion but wrong
in its stated reason. The reason given -- that a file-local static is
unreachable because `test_config_store.c` links `config_store.c` as an object
rather than including it as source -- is contradicted by this very commit:
`test_heat_owner_active_decide.c` demonstrates the house pattern of
`#include`-ing the production `.c` directly, and `test_config_store.c` could do
the same. The real reason is that there is no re-entry seam: everything executed
between setting and clearing the flag (`config_store_pack()`, the little-endian
accessors, `memcmp()`) is either same-translation-unit or libc, so no test can
interpose a call that re-enters. Reaching the trip-wire would require adding a
seam to production code, which is not worth doing for a defence-in-depth check.

## F4 / F5

Both verified as already landed and correct. `safety_core_request_enable()`
genuinely refuses the enable direction while `s_tc_type_apply_in_progress` is
set, so the interlock is not a no-op; it is redundant with task serialization,
which the code itself says, and redundancy here is appropriate. F5 was a
comment-accuracy correction: the pending flag is now documented as a "retry was
dispatched" latch rather than a "verified" latch, with the actual verification
living in thermo_task's own `max31856_tc_type_verified()` loop. The comment now
matches the code.

## Defect 2 (LOW): the parent commit cannot build its own KilnFW host tests

The runner wiring for `heat_owner_active_decide` -- its executable entry, its
`Invoke-HostTestExe` call, and the bump of `$totalExpected` to 47 -- landed in
the earlier commit named in the F1 section, while the test source file it
invokes landed in this one. Asking git which commit introduced each confirms the
split.

The parent commit therefore has a build script referencing a file that does not
exist there, and its KilnFW host-test suite builds 46 of 47 executables. That is
a bisect hazard: a future bisect across this range hits a commit that fails to
build for reasons unrelated to whatever is being bisected. It also shows the F1
absorption swept up more than `safety_cfg_http.c` and the wire-reason header --
it took build-script wiring too -- which the commit message does not mention.
Nothing about the shipped firmware is affected.

## Defect 3 (INFORMATIONAL): `calibration_missing` is inside the compared window, but the misclassification is unreachable

`config_store_only_tc_type_differs()` neutralizes exactly four things -- format
version, sequence number, the `tc_type` byte, and the
`CONFIG_STORE_SET_TC_TYPE` bit in `fields_set` -- then `memcmp`s up to the CRC.
`calibration_missing` (record offset 25) sits inside that window and is not
neutralized, so a write that flips it would be classified as not-tc_type-only
and, under F2, labelled as modifying a field other than the thermocouple type.

I pursued this as a candidate misleading-message defect and it does not stand.
The only realistic flip is a first-ever commissioning of the type taking
`calibration_missing` from true to false, because
`config_params_all_required_set()` includes the tc_type bit. But a board in that
state cannot be ARMED: `commissioning_gate_energize_allowed()` delegates to
`commissioning_gate_is_commissioned()`, which requires both
`!calibration_missing` and `config_params_all_required_set()`. An ARMED board has
therefore already committed a type and already has `calibration_missing` false,
so a subsequent type change leaves the byte untouched and the comparison behaves
correctly. Recorded so the next reviewer need not re-derive the same negative
result.

## Defect 4 (MEDIUM, process): F2 and F3 ship untested

Neither SaftyFW-side fix has a test. `ARMED_MIXED` appears in no SaftyFW test
file; `config_store_only_tc_type_differs()` is exercised at
`test_config_store.c:615` but nothing reaches the new trip-wire. Parent and child
both report `259/259`. For F3 that is defensible -- there is no seam. For F2 it
is not: the mapping is a pure function of two byte values and a decision enum,
and a test would have caught defect 1 immediately, since the cached-versus-
persisted split is exactly what such a test would have to set up.

## The two invariants that matter most

Neither is weakened by this commit.

**Nothing here can leave the Pico unarmed.** The only change touching an
arming-relevant control path is F3, and it strictly removes an `abort()` -- that
is, it removes the sole mechanism in the reviewed diff capable of resetting the
chip and dropping K4.

**Nothing here can make the ceilings unequal.** `abs_max_temp_c` sits at record
offset 17, inside the range `config_store_only_tc_type_differs()` compares, and
is not neutralized. Any write changing it while ARMED is therefore still
classified as not-tc_type-only and still refused by the unconditional ARMED
branch. The carve-out remains exactly as narrow as intended.

## Race and consistency review of the ARMED gate

`link_task_tc_type_gate_decide()` is fail-closed on every input it takes: NULL
context, relay energized, a recent enable request inside the safe window,
DEGRADED_NO_CONTEXT, context never received, context lock unavailable, context
invalid, context stale by age, any of HEAT_REQUESTED / PROFILE_RUNNING /
HEAT_OWNER_ACTIVE set, relay on continuous, and any current present. The window
between the gate check and the apply is closed from the other side as well, by
`safety_core_set_tc_type_apply_in_progress()`, which makes
`safety_core_request_enable()` refuse the enable direction for the duration of
the flash-write-plus-reconfigure span. Task serialization already prevents a
REQUEST_ENABLE frame from being processed inside that span; the interlock is the
second layer.

A reapply skipped because heat came on is not silent: it logs a DIVERGED line
naming the condition and latches a retry, with thermo_task verifying the chip's
actual configuration independently. That satisfies the owner's requirement that
divergence alarm rather than reconcile quietly.

## What could not be verified

Stated plainly rather than papered over.

- The KilnFW ESP32-S3 target build could not be exercised. It is broken on
  `origin/main` in `firmware/hwAbstraction/esp/sysinfo/hal_sysinfo_esp.c`
  (`TEMPERATURE_SENSOR_CLK_SRC_DEFAULT` undeclared), which is pre-existing and
  belongs to another area; not attributable to this commit, and not fixed here.
- `tools/run_all_checks.ps1` could not run in a clean worktree: it aborts early
  because `tools/PcTools/selfcheck.py` and its virtual environment are not
  provisioned in a fresh checkout. A known clean-worktree provisioning gap, not
  a finding against this commit.
- `check_all_task_stack_budgets` (the `gpio_probe` symbol) and
  `check_flash_worker_lint` (`ui_page_home_refresh.c:259`) fail on clean
  `origin/main` and belong to other areas.

## Recommended fixes

1. Defect 1: compare tc_type against the persisted record when classifying a
   refusal as MIXED, or return the classification from `config_store_write_ex()`
   instead of re-deriving it in the caller. Correct the comment either way -- it
   asserts an equivalence the surrounding code was specifically changed to break.
2. Add one SaftyFW test for the F2 mapping, including the case where a volatile
   install has moved the cached type away from the persisted one. That single
   test is what separates defect 1 from having been caught before it shipped.
3. Leave F3 untested and keep the comment, but correct the stated reason to "no
   re-entry seam exists", so a future reader does not believe the
   `#include`-the-source pattern is unavailable here when this very commit uses it.
