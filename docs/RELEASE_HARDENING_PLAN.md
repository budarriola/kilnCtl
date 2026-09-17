# Release Hardening Plan — what has to be true before this controls a real kiln

> **Status:** plan · **Opened:** 2026-09-16. Nothing here is implemented. No
> board was flashed, no heating run was performed, and no `.kicad_*` file was
> touched while writing it.
>
> This plan starts once `docs/KILN_PROFILES_PLAN.md` is finished. It does not
> re-plan anything that plan, `docs/SETUP_WIZARD.md`,
> `firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md`, `docs/ON_OFF_ZONE_PLAN.md`
> or `docs/ITER_TUNE_REDESIGN_PLAN.md` already owns; where release depends on
> one of those, it is named as a dependency rather than duplicated.

## 0. What "release" means here, and why that changes the bar

Everything this repository has been verified against so far is a roughly 4 W
bench fixture at 120 V, run attended, for minutes to a few hours, with a
developer on the keyboard and a second session usually watching. Release means
the opposite of every one of those: kilowatts of real element, an enclosure
that stores enough heat to keep climbing after every relay opens, twelve-hour
firings, and nobody in the room. Two consequences follow, and they set the
shape of this whole plan.

First, the failure that matters is no longer "the feature did the wrong
thing". It is "the board stopped, or lied, or drifted, at hour seven, and the
elements stayed on". Nearly all of this repository's mechanical coverage is
static analysis and host tests that execute in milliseconds. That coverage is
genuinely good — see section 6 for what it already closes — but time is the
axis it does not cover at all, and time is the axis release is defined on.

Second, the safety argument has to stand on its own without the bench. The
bench fixture physically cannot exercise several of the guards the safety
argument leans on, and no amount of bench time will change that. A release
plan that quietly counts bench hours as safety evidence is making the exact
mistake `docs/SAFETY_CASE.md` §4 was written to prevent. Section 4 below
separates what the bench can close from what it structurally cannot, and says
what the second category costs.

The items below are ordered by risk. Each says what the gap is, why it blocks
*release* specifically rather than merely being desirable, how you would know
it is closed, and a rough size on this repository's existing S/M/L/XL scale.
**Release blockers are marked BLOCKER.** Everything else is explicitly not.

---

## 1. BLOCKER — the `profile_executor` panic is unresolved, and it is the
## release-defining defect

**The gap.** `profile_executor` has panicked four times with the same
signature: `IllegalInstruction`, `exc_pc = 0xfffffffd` (a saved PC of exactly
zero, i.e. a return through a smashed `a0`), `backtrace_corrupted = true`,
crash task `profile_executo`. The history is in
`docs/audits/cplval75_aborted_executor_panic_2026-09-09.md`,
`docs/audits/profile_executor_panic_2026-09-10_root_cause.md`,
`docs/audits/profile_executor_panic_recurrence_2026-09-14.md` and
`docs/audits/profile_executor_coredump_2026-09-15.md`. The 2026-09-10 root
cause was real and its fix held; the 2026-09-14 and 2026-09-15 events are
*different events wearing the same signature*, and the last of these could not
be symbolized at all because no coredump could be read off the board. A
deferred-release fix has since landed and been adversarially reviewed
(`docs/audits/review_executor_panic_fix_1c8d7f6e_2026-09-15.md`), which found
further ordering defects in the fix itself. The honest state is: **the task
that runs every firing crashes, on the real board, under exactly the
conditions release consists of, and nobody can currently read the evidence.**

**Why it blocks release.** Every other item in this plan is conditional on
this one. A multi-hour unattended firing is precisely the workload that
produced all four crashes. The crash takes down the task that owns setpoint
progression and relay commands; what the elements do afterwards depends on
paths that have themselves never been exercised from this particular entry
state. Shipping with a known, recurring, unsymbolizable crash in the firing
task is not a risk trade — it is an unbounded one.

**How to know it is closed.** In order, and none of these substitutes for the
next:

1. ~~Coredump readback has to work first~~ — **done, `4af518ca`.** The prior
   failure was two real bugs: wrong `espcoredump` CLI args (positional path
   instead of `--core`/`--core-format`, no `--chip`) and the wrong
   interpreter (defaulted to the MCP server's own venv, which lacks
   `esp_coredump`), whose `ModuleNotFoundError` was misreported as "very
   likely an ELF/coredump mismatch" — an environment failure dressed as a
   substantive verdict. Fixed and confirmed live: `espcoredump` now runs far
   enough to report its own genuine verdict. Host-tested:
   `tools/PcTools/tests/test_coredump_symbolize_and_archive.py`. Also adds a
   durable, content-addressed coredump archive
   (`firmware/KilnFW/coredump_archive/`) so a fetched dump is never
   overwritten. The existing 748032-byte on-board dump is still permanently
   unsymbolizable — its matching ELF (build `Sep 14 2026 23:55:17Z`) was lost
   before the ELF-archive durability fix (`a347e726`) landed, and nothing
   recovers lost ELFs after the fact. **Residual gap, confirmed still open:**
   `find_crash_elf()` (`mcp_server_flash.py:970`) resolves against the
   board's *currently running* `fw_build`, not the stored dump's origin
   build — a dump fetched before a reflash will be matched to the wrong ELF
   unless the caller passes the dump's own `fw_build` by hand. Full detail:
   `docs/audits/release_hardening_plan_verify_1_2_5_7_8_2026-09-16.md`.
2. **A stack-margin measurement taken during a firing, not at idle.** The idle
   baseline is a floor, not a worst case — that is already recorded as a
   standing caveat in `tools/PcTools/scripts/stability_soak.py`'s own
   docstring. `check_executor_task_stack_budget.ps1` computes a static deepest
   path; the last review of it reported honest headroom around 27%, classified
   LOW. Static analysis of the deepest *known* path and a runtime high-water
   mark under a real firing are different measurements and this defect lives
   in the gap between them. **Size: M.**
3. **Two consecutive clean firings of at least the intended unattended
   duration, on the release candidate build, with the crash report read and
   confirmed absent at the end of each.** Not "no crash observed" — an
   explicit `GET /api/crash_report` read showing `present=false`. Note that a
   panic-and-clean-reboot reads as healthy to everything except that endpoint,
   which is why `get_heap_status` was fixed to surface it. **Size: L, and
   mostly wall-clock.**

**Trap to avoid.** Do not declare this closed on the basis of a build that
fixed *a* stack path. The 2026-09-14 pass established that the static stack
story which explained the first two events does not explain the third. A fix
without a symbolized backtrace is a hypothesis, and this defect has already
survived one confident fix.

---

## 2. BLOCKER — long-duration behaviour has never been measured over the
## duration release is defined by

**The gap.** `tools/PcTools/scripts/stability_soak.py` exists and is good: it
samples heap free and min-free, per-task stack margin, safety link counters,
guard warn/trip masks, reset reason and unacknowledged crash reports on a
cycle, appends to CSV, and uses *flat counters* rather than absolute values as
its pass criterion — which is the right choice for a link whose counters never
reset. What does not exist is a run of it that covers the release duration
**with a firing active**, and a defined verdict for what the resulting series
must look like.

Four specific quantities have no long-duration evidence:

- **Internal DRAM.** The documented failure threshold is real and measured:
  below roughly 11.9 kB free the board starts resetting HTTP sockets. Recent
  readings sit near 32–34 kB min-free. That is comfortable but it is a
  *snapshot*; nothing establishes the slope over twelve hours with a web
  client polling, LVGL rendering, and a profile logging.
- **Stack headroom under load**, per item 1 above.
- **Flash wear and config churn.** The endurance review concluded there is no
  wear problem, and that conclusion is believed — but the dual-write to the
  `cfg` LittleFS partition alongside NVS changed the write pattern after that
  review, and `docs/CONFIG_FILESYSTEM.md` carries its own open items.
- **Link stability.** `bench_link_health.py` measures per-command latency and
  catches both the "stopped answering" and "answers but slowly" failure
  shapes. It is a spot check, not a duration measurement.

**Why it blocks release.** Every one of these is a slow leak, and a slow leak
is exactly the defect class that passes an attended bench session and fails an
unattended firing. The board being demonstrably healthy at minute ten is not
evidence about hour eight; this is the same "a snapshot is not a trend" error
the idle-stack-baseline caveat already names.

**How to know it is closed.** ~~Extend `stability_soak.py` with an explicit
verdict rather than only a CSV~~ — **partly done already.** `main()` already
returns a real PASS/FAIL verdict with a non-zero exit (1 on any accumulated
problem, 2 on a precondition failure), and already flags a heap-floor breach,
a DOWN trend on heap-free or stack-min-headroom%, and a positive delta on
`crc_errors`/`timeouts`/`broadcast_dropped` (landed across `95ca7e6e`,
`e84a2db5`, `3780030f`). Three things are still genuinely open:

1. **The trend test is not a slope test.** `_trend_direction()` compares only
   the first and last sample against a 5% threshold — noisy middle samples
   are invisible to it. Needs a real monotonic-degradation/slope test on
   heap min-free and stack high-water, per the original ask.
2. **The harness itself has never been negative-tested.** No test file
   references `stability_soak.py` or `_trend_direction()`. Feed it a
   synthetic degrading series and confirm it goes RED before trusting it —
   this repo has been burned by exactly this omission eight times before.
3. **No run of the intended release duration exists**, with or without a
   firing active — no `stability_soak_*.csv` artifact or audit record exists
   anywhere in the tree.

**Size: S for the slope test + negative test (the verdict scaffolding is
already there), L for the runs.** Full verification detail:
`docs/audits/release_hardening_plan_verify_1_2_5_7_8_2026-09-16.md`.

**Already partly covered, and worth saying:** the metric *selection* problem
is solved. `stability_soak.py` already samples the right things, already
knows why flat counters beat absolute ones, and already carries the
idle-baseline caveat in its own docstring. This item is about duration and a
verdict, not about instrumentation.

---

## 3. BLOCKER — test coverage that survives this repository's own failure modes

**The gap.** This repository has a documented, repeated history of tests that
pass for the wrong reason. The known instances are worth restating as a
checklist because they are the acceptance criteria for any coverage this plan
adds:

- A check that passes against the *unfixed* code (vacuous), caught only by
  negative-testing — and eight shipped that way before negative-testing became
  standing practice.
- A check that read a sibling process's output file instead of producing its
  own.
- A harness printing a PASS-shaped verdict while exiting 0 on a regression.
- A verdict measured from a prebuilt binary that an earlier negative test had
  poisoned, where an empty `git diff` was taken as proof and the stale `.exe`
  in the build directory was not.
- A guard that skipped its own body (`if not path.is_file(): skipTest(...)`)
  and reported green with zero coverage — the reason `run_all_checks.ps1` now
  treats SKIP as a failure by default.
- A mirror-drift test written against a test-local copy of the production
  table, which proves nothing about production.

**Why it blocks release.** Not because the existing suite is weak — it is
unusually strong for a project this size — but because the *release decision*
will be made by reading a green suite. If any gate in that suite is green for
a reason unrelated to the code being correct, the release decision is made on
a forgery. The cost of this class is already measured: one poisoned-binary
verdict reached the project owner before it was root-caused.

**How to know it is closed.** A release-gate audit, executed once, over every
check that the release decision will cite:

1. **Negative-test every gate that has not been negative-tested.** Break the
   thing it claims to guard, confirm RED, restore **by hand** (never
   `git checkout --`, which discards other sessions' WIP), and then **force a
   full rebuild** before measuring anything — an empty `git diff` proves the
   source is restored and says nothing about build artifacts.
2. **Confirm each gate produces its own inputs.** For every check that reads a
   file, establish who wrote that file and when. A check reading an artifact it
   did not cause to be built is the poisoned-binary shape.
3. **Confirm each gate's failure exit actually propagates.** Non-zero on
   failure, exit 3 with a stated reason on a genuine missing prerequisite, and
   nothing else — the contract already written in `tools/run_all_checks.ps1`'s
   header. Verify by injection, not by reading.
4. **Record the result as a table of gate → negative-test evidence**, so the
   next session does not have to re-derive which gates are trustworthy.

**Size: L.** It is mechanical but there are roughly ninety-four checks plus
the host-test suites, and the ones that matter for release are a subset —
scope it to the safety, config, stack-budget and build gates first.

**Already covered, name the evidence:** the two `check_01_*_pushed_build.ps1`
scripts are the strongest single piece of process coverage in the repo. They
build `origin/main`'s actual content in a clean worktree rather than the local
tree, which is the only thing that catches a push whose dependency closure was
not fully committed — a failure that happened at least four times in one day.
Their own header is honest about the one hole (`sdkconfig` is gitignored and
copied in from the main tree, so they prove "origin/main's source builds
against this machine's board config", not "a fresh clone builds"). Closing that
hole is **desirable, not a blocker**, and is item 11.

---

## 4. BLOCKER — a safety argument that does not depend on the bench

**The gap.** The rollup in `docs/SAFETY_CASE.md` §4 is the number that matters:
of roughly twenty tracked guard-level claims, twenty are host-tested and
**three** are hardware-verified. The three are S5's hardware fit, S5's
masking-before-fit finding, and KilnFW's thermal_guard guard 6. Everything
else — all of S1–S4 and S6–S14's trip logic, and KilnFW guards 1, 2, 3, 4, 5,
7 and 9 — has never been provoked on real silicon. Separately, several guards
are commissioned off or structurally inert on this fixture: S9 and S14 depend
on current sensing whose commissioning is incomplete, S11's gate runs through
the same current path, and S6a cannot be provoked by any host fixture at all
now that `virtual_dut` and SimFW are gone.

**Why it blocks release.** The dual-processor design *is* the safety argument.
If the safety processor's guards have only ever been proven by host tests, the
argument reduces to "the logic is right", which was never the claim — the
claim is that the wiring, the sensors, the trip path and the relay actuation
all work together on this board. As `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`
puts it in its own header: the host tests prove the logic, the hardware tests
prove the wiring, and most of the failures worth catching are wiring.

**How to know it is closed.** Split the guards into three buckets and treat
each differently. This split is the actual deliverable of this item.

**Bucket A — provokable on the bench today, and must be provoked.** S1's real
ceiling trip, S2's overshoot-sustained path, S5's fault-injection cases (a
reading outside the commissioned band, a deliberate `tc_type` mismatch — only
the healthy in-band reading has ever been observed live), S6b's link-dead
tiers, S7's press-to-open path, S11's frozen-sensor trip once its gate is
reachable, and KilnFW's per-zone guards. Each needs a provocation, an observed
trip, and a confirmed relay state afterwards. Record results in
`GUARD_TEST_MATRIX.md` §3.4, which is currently entirely unexecuted.
**Size: L.**

**Bucket B — needs commissioning the bench has not completed.** S3, S4, S9,
S14 and S15 all sit behind current sensing. `CT_COMMISSIONING_PLAN.md` steps 0
and 6 are the dependency, and both need the owner. Until those land, five
guards are inert and the safety case must say so in plain language rather than
listing them as coverage. **Size: M, and owner-blocked.**

**Bucket C — cannot be closed on this fixture at all, ever.** S9's genuine
welded-contactor escalation needs a jig that injects real AC current through
the CT loop while the K4 drive line is confirmed de-energized; no such jig
exists and firmware simulation cannot substitute, because the guard latches on
an analog current-transformer signal, not on a GPIO. S8's real threshold
cannot be set until a full-power ramp is measured, and a 4 W fixture cannot
produce one. The E-stop's pole 1 is permanently unwired here by owner
decision, so the E-stop on this fixture is firmware-mediated only. And the
whole class of thermal behaviour — overshoot magnitude, ramp rates, guard
nuisance thresholds — is measured against a plant with essentially no stored
energy.

**What bucket C means for release, stated plainly.** These are not items to
schedule; they are the boundary of what the bench can sign off. A
release-credible position on them is one of three, chosen deliberately: build
the jig, defer the guard to commissioning-on-the-installed-kiln with the
safety case saying openly that it ships unproven, or accept the risk with the
owner's explicit sign-off. Picking silently is the failure mode. **Size: S to
decide, XL if the answer is "build the jig".**

---

## 5. BLOCKER — failure injection and recovery, exercised rather than argued

**The gap.** The recovery paths are designed, largely implemented, and in
several cases host-tested. Almost none has been made to happen on real
hardware. Specifically:

- **Power loss mid-write.** The RP2040 `config_store` A/B sector scheme is
  implemented and flashed, and its in-RAM cache race is fixed. ~~Its
  `next_write_slot` torn-slot reprogramming defect is flagged and untouched~~
  — **the defect is fixed, 2026-09-14** (`config_store_next_write_slot()` now
  verifies the target slot is actually erased before programming, and
  read-back-verifies after every program; full detail
  `docs/audits/rp2040_config_store_write_atomicity_2026-09-14.md`), host-tested
  (`test_config_store_flash.c`, torn-slot-reuse cases) and **partially
  bench-verified** (`88bb4333`, 2026-09-14: a real field round-tripped
  correctly across two live Pico reboots, all safety-relevant config read
  back unchanged). What remains genuinely open, by deliberate safety choice
  rather than oversight: that bench pass explicitly did not tear a real flash
  program mid-write (judged unsafe/irreversible on the only bench Pico), so
  the mismatch-detected and slot-skip-and-switch branches are proven only by
  the host-test power-cut-injection harness, never on real hardware. **The
  plan's original ask — cut power during a write, repeatedly, on real
  hardware, confirming intact read-back every time — is still open.** Verify
  detail: `docs/audits/release_hardening_plan_verify_1_2_5_7_8_2026-09-16.md`.
- **A Pico reboot mid-firing.** The reset-one-side bug class has already
  produced four confirmed silent instances in this codebase, one of them
  exactly this scenario (`trip_seq` restarting on the Pico while the ESP's
  dedup cursor did not). That one is fixed. The general exercise — reboot the
  Pico during a live firing and confirm the ESP blocks heat, recovers the
  link, and re-establishes dedup state correctly — has not been run.
- **A dead link.** The 1.5 s staleness ceiling is bench-verified. The 30 s
  firing-abort is host-test-pinned only; `bench_firing_abort_stopwatch.py`
  exists and is written specifically to close it, and the only missing
  ingredient is a running firing. **This one is cheap and should be taken the
  next time a firing runs.** Size: S.
- **A thermocouple fault.** Guard 6 is hardware-verified on the KilnFW side —
  genuinely, with the trip firing after exactly three bad reads and the relay
  staying off. The safety processor's equivalent injection cases are not.
- **A welded contactor.** Bucket C above.
- **Corrupted or downgraded config.** Section 7.

**Why it blocks release.** Unattended operation is defined by what happens
when something goes wrong and nobody intervenes. Every item above is a
scenario a real installation will eventually produce. An argued recovery path
is a design intent; a hardware-exercised one is a property.

**How to know it is closed.** One scripted injection session per scenario,
each with a defined expected end state (relays open, fault surfaced, firing
aborted or resumed as designed), each recorded. Build them as scripts in
`tools/PcTools/scripts/` next to the two bench scripts that already exist, so
they are repeatable against the next build rather than being one-off session
logs. **Size: L overall; the link-abort stopwatch alone is S.**

---

## 6. BLOCKER — OTA, rollback, recovery mode and the first-boot path

**The gap.** Field-update mechanics are the one area where a defect is
unrecoverable without physical access, and this system has three known, sharp
hazards in it:

1. **`flash_firmware()` writes the factory partition and never touches
   `otadata`.** After any OTA, the bootloader keeps booting `ota_0`/`ota_1`
   while every later flash reports success and the board runs old code. The
   automatic post-flash verification now catches this loudly, which is a real
   mitigation — but the hazard itself is structural and an operator doing a
   field update needs to not be able to hit it.
2. **Rolling back past a `zones_cfg` schema bump silently runs on
   firmware-default PID gains.** The older firmware refuses the
   newer-than-it-knows blob, flash is untouched so reflashing restores
   everything, and there is *no separate warning* — a firing started between
   the rollback and the reflash runs on defaults. On a 4 W fixture that is a
   bad graph. On a real kiln it is a ruined load or worse.
3. **Recovery mode has bricked this board into a permanent loop three times.**
   Both 2026-09-08 findings are fixed — `boot_guard_mark_healthy()` now
   verifies its own NVS write rather than trusting a return code, and
   `boot_guard_reset_counter()` exists for a tool that knows it just
   deliberately flashed. That second function is now wired into
   `flash_firmware()`'s verify step (`b09294fb`, 2026-09-09): an opt-in
   `ap_password` parameter makes `flash_firmware()` call the new
   `POST /api/ota/esp/boot_guard_reset` route ONLY after post-flash
   verification confirms full, unambiguous success, and reports the
   counter's before/after values and whether the clear actually verified —
   see CLAUDE.md's `boot_guard_reset_counter()` paragraph for the full
   wiring. A caller who omits `ap_password` gets the pre-existing behavior
   unchanged, so this is closed for a caller that opts in, not yet closed
   as a default every flash gets automatically.

The Pico half of field updates has additionally never completed a transfer:
the 2026-09-06 attempt was refused by a genuine Pico-side interlock before any
bytes crossed the wire.

**Why it blocks release.** An OTA that leaves a kiln in an unknown state is
hazard H8 in `docs/SAFETY_CASE.md`, and every mitigation for it is currently
classed argued or host-tested. A field unit cannot be recovered by walking
over to it with a JTAG probe.

**How to know it is closed.**

- ~~Wire `boot_guard_reset_counter()` into `flash_firmware()`'s verify step and
  negative-test it~~ — **done, `b09294fb`** (see item 3 above and CLAUDE.md).
  Host-tested (`tools/PcTools/tests/test_flash_firmware_verify.py`'s
  `BootGuardResetWiringTest`, and `test_boot_guard.c`/`test_ota_http.c` on the
  firmware side) covering the central negative case (a hard verification
  failure must never clear the counter) and the lying-write/unreachable-
  endpoint paths. Residual, not yet closed: the call is opt-in
  (`ap_password` must be passed) rather than the flash-tool's default, so an
  ordinary `flash_firmware()` call with no `ap_password` still gets none of
  this protection.
- Make the schema-downgrade hazard impossible to hit silently: on boot, if the
  persisted config version is newer than this firmware understands, refuse to
  start a firing and say so on every surface, rather than running on defaults.
  **Size: M.** This is a blocker; the current behaviour is a silent wrong
  answer with heat attached.
- Complete a real Pico update over the wire, end to end, including whatever
  bootloader/metadata gap `firmware/SaftyFW/docs/BOOTLOADER.md` and the update
  protocol's completion checklist imply for a finished one. **Size: L.**
- Write and rehearse the **first-boot-on-a-real-kiln checklist** — section 8.

**Already covered, name the evidence:** post-flash verification is genuinely
solid. `flash_firmware()` polls the board's own API for the running partition
and compares the reported build timestamp against the binary's embedded
`esp_app_desc_t`, fails loudly on disagreement, tries an ordered candidate host
list rather than assuming the AP-fallback address, and treats "was reachable
before, unreachable after" as a hard failure. It also records git provenance
and refuses a dirty tree that touches config-schema, migration or safety code.
That is a better flash path than most projects of this size have, and it does
not need work.

---

## 7. Config, migration and the two-processor agreement — mostly owned
## elsewhere, one release-specific addition

**Already owned:** `docs/KILN_PROFILES_PLAN.md` is the in-progress plan and it
already covers the central defect (the stored blob is ESP-only; the Pico's
sixty-eight commissioning parameters do not travel with a kiln config), the
standing cross-processor agreement invariant with alarm-and-disable-heaters on
divergence, upload as untrusted input, the missing-field rule, and atomic
swap across two processors. Do not re-plan any of it here.

**What release adds on top.** Two things that plan does not have to solve but
release cannot ship without:

- **A migration test that runs every historical schema version forward.**
  `ZONES_CFG_VERSION` is in the low twenties; each bump has a converter. There
  is no test that takes a v1-through-current corpus of real persisted blobs and
  runs each forward to current, asserting the resulting config is sane. This
  matters for release because a field unit that has been through several
  updates is exactly the case nobody has. **Size: M. Blocker.**
- **Import of a deliberately hostile config.** Truncated, wrong CRC, valid CRC
  with out-of-range values, a version number from the future, a file that is
  valid for a *different* kiln. The kiln-profiles plan covers untrusted upload
  for its own surface; the release gate is that no such input can produce a
  bootable state that will command heat. **Substantially more coverage
  already exists than this bullet implied**: `test_backup_import.c` already
  rejects a malformed/truncated body, wrong `kind`, a too-new version,
  out-of-range model values and overlong names; separately, at the NVS-blob
  level, bad-CRC and refused-newer-than-firmware blobs are covered and
  preserved un-overwritten (`test_nvs_load_from_bad_crc_is_rejected`,
  `test_zones_http_start_refused_newer_blob_not_overwritten`, among others).
  What is still missing is a single test stating the release-gate property
  directly rather than inferring it from several unit tests. **Size: S
  (down from M).** Verify detail:
  `docs/audits/release_hardening_plan_verify_1_2_5_7_8_2026-09-16.md`.

---

## 8. ~~BLOCKER~~ DONE — what must be verified on the installed kiln, and the
## first-boot checklist

**Closed.** `docs/FIRST_FIRING_CHECKLIST.md` now exists: an ordered,
owner-followable sequence covering pre-power wiring/continuity, E-stop
function (both poles, including the durable `estop_verification` attestation
for pole 1, which no software check can see), safety-link-up and trip
clearing, per-zone thermocouple identity and type on both processors
(including the zone-2-is-bottom/zone-0-is-top swap check and the
`s_tc_type_verified` distinction from a bad reading), Pico/ESP
`abs_max_temp_c` equality, a low-temperature dry run proving the contactor
before any real load, CT calibration (with the import-forces-uncalibrated
caveat), and an explicit per-guard accounting of what the bench already
proved versus what still needs the installed kiln or a supervised firing.
It ends by handing off to the low-temperature/full-temperature/unattended
firing sequence below rather than claiming to close that sequence itself.
The remaining bullets below (S8's real threshold, autotune/coupling matrix,
thermal overshoot/cool-down, relay wiring under full load) still require an
actual firing on the installed kiln to measure — the checklist gets the
kiln to the point those are safely attemptable, it does not substitute for
running them.

**The gap (as originally written).** There is no document that says, in order, what an operator does
between "the controller is bolted to a kiln that has never run under it" and
"it is safe to leave this firing unattended". `docs/SETUP_WIZARD.md` covers
configuration thoroughly and is the right backbone, but configuration is not
commissioning: the wizard gets the numbers in, and this checklist proves the
kiln behaves the way those numbers claim.

**Why it blocks release.** Several things are *only* knowable on the installed
kiln, and every one of them is a safety input:

- **`abs_max_temp_c` must be raised Pico-first, then ESP**, and the Pico's
  ceiling must never end up tighter than the ESP's — that ordering is a
  standing rule, and the bench's 80 °C value is meaningless on a real kiln.
- **S8's rate-of-rise threshold** cannot be set until a full-power ramp has
  been measured on the actual kiln. The bench's hand-set 20 °C/min is a bench
  number, tighter than the documented rule, and would nuisance-trip a fast
  kiln.
- **Current-sensing commissioning under real load**, which is what unlocks
  S3/S4/S9/S14/S15 from bucket B.
- **Autotune per zone and the coupling matrix**, which are multi-hour, need a
  rested kiln, and must never ship values tuned against a different one. The
  standing requirement that nothing ship guessed for, or tuned to, a kiln
  other than the installed one applies to the fuzzy layer's bands too.
- **Thermal overshoot and cool-down behaviour** — a real kiln keeps climbing
  after the elements open, and every guard threshold tuned against a 4 W
  fixture with no stored energy is untested against that.
- **Relay and contactor wiring actually proven**, including that dropping K4
  actually opens the contactor. Today that claim is schematic-derived, not
  bench-proven.

**How to know it is closed.** Write `docs/FIRST_FIRING_CHECKLIST.md` (or fold
it into the setup wizard as a final gated stage, which is better because the
wizard already persists progress in NVS where a filesystem problem cannot lose
it): an ordered, signed-off sequence ending in a low-temperature attended
firing, then a full-temperature attended firing, then the first unattended
one. Each step names its refusal condition. The deliverable is closed when an
operator who is not the author can follow it. **Size: M to write, L including
the rehearsal.**

---

## 9. Desirable, not blocking — a shared-state review pass

The reset-one-side bug class has four confirmed instances, all silent, all
expensive. It was evaluated for a mechanical check and rejected for good
reasons: the four have no unifying syntactic shape and any rule general enough
to catch all four would flag the large majority of correct one-sided resets.
The standing practice — when code resets a counter, window, timestamp or seed,
ask who else holds a copy or a derived expectation of it — is the right
answer and needs no plan.

What is worth doing once before release is a **deliberate sweep with that
question**, over the cross-processor state specifically: sequence numbers,
dedup cursors, boot IDs, config revision counters, seqlock generations, and
the `cfg`-versus-NVS revision comparison. Not a check, a reading pass, with
findings written down. **Size: M. Desirable.**

## 10. Desirable, not blocking — the `cfg` partition is inert and should
## either be finished or explicitly parked

Zones config, profiles and several preferences dual-write to the `cfg`
LittleFS partition, but that partition is unformatted on the bench board and
not mounted at boot, so the whole path is inert today. NVS remains
authoritative and unconditional, so this is not a hazard. It is, however, a
significant amount of live code that has never run. Before release, either
finish it (format, mount, exercise the tie-break and fallback paths) or gate it
off behind something that makes its inertness explicit. Shipping a dormant
write path that will wake up on the first field unit whose partition happens to
be formatted is the bad third option. **Size: M. Desirable.**

## 11. Desirable, not blocking — prove a fresh clone builds

`check_01_kilnfw_pushed_build.ps1` copies the main tree's `sdkconfig` into its
clean worktree because `sdkconfig` is gitignored, and says so honestly in its
own header. The result is that it proves origin/main's source builds against
*this machine's* board config, not that a fresh clone builds. A Kconfig default
change would sail through. Closing this means committing a
`sdkconfig.defaults`-derived configuration the check can generate from, which
is a real piece of work and not a release blocker on a single-machine project
— but it is the gap between the check's name and what it proves, and that gap
should not be discovered by someone else. **Size: M. Desirable.**

## 12. Desirable, not blocking — adaptive tuning should not ship enabled and
## unproven

The fuzzy layer has no demonstrated benefit under matched conditions on this
bench — that scope qualifier is load-bearing and travels with the finding.
Fixed-gain fuzzy at strength 50 measured as net harmful; the adaptive variant,
which is the one the owner has repeatedly said is intended, has not been
evaluated at all, and `docs/ADAPTIVE_FUZZY_EVALUATION_PLAN.md` specifies the
run that would decide it. `fuzzy_strength_pct` is 0.0 on the live board, so it
is inert today. The release position writes itself: **it ships at zero strength
unless and until the adaptive evaluation shows benefit on the installed kiln**,
and the band derivation from each zone's own autotune model is what makes that
defensible rather than fixture-trained. No work, one recorded decision.
**Size: S. Desirable.**

---

## 13. Order of work

The dependency structure is simple enough to state in a paragraph. Coredump
readback (item 1.1) comes first because it changes the information content of
every subsequent hardware run. The gate audit (item 3) comes second, because
everything after it is measured by gates whose trustworthiness is currently
assumed. The soak harness with a real verdict (item 2) is third and then runs
continuously alongside everything else. Guard provocation (item 4, buckets A
and B) and failure injection (item 5) are the bulk of the hardware work and can
interleave. OTA and recovery (item 6) is independent and can be done by a
different session in parallel. Config migration (item 7) depends on the
kiln-profiles plan landing. The first-firing checklist (item 8) is written last
because it is the summary of everything the other items established, and it is
the document the owner actually uses.

The three bucket-C decisions in item 4 should be put to the owner early rather
than late, because one possible answer ("build the current-injection jig") is
an XL that would otherwise be discovered at the end.
