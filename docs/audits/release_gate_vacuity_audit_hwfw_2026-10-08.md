# Release-gate vacuity audit, hwAbstraction and KilnFW build-artifact checks, 2026-10-08

Follow-up to the rows the previous pass left NOT AUDITED. Worktree
`C:\wt\vacgrpA_3bnpv6`. No board access, no `.kicad_*` file touched.

## compile_esp_backends.ps1 / compile_pico_backends.ps1

Producers: `compile_commands.json` (ESP) comes from the KilnFW target build
(`check_00_kilnfw_target_build.ps1`); `build.ninja` and `CMakeFiles/rules.ninja`
(Pico) come from the SaftyFW target build. No new build was run: the files
were copied from existing `C:\wt\checkbuild_8597e7a4f1` (compile_commands) and
`C:\wt\checkbuild_origin_saftyfw` (ninja) into the (gitignored) build dirs of
the audit worktree. Baselines: both exit 0.
Mutation: appended `int negtest_undeclared(void){ return not_declared_xyz; }`
to `esp/spi/hal_spi_esp.c` and `pico/spi/hal_spi_pico.c` respectively.
Result: `error: 'not_declared_xyz' undeclared`, `FAILED: <file>`, exit 1 for
both. Restored by hand from the original bytes; `git status` clean.

## check_embedded_pico_image_fresh.ps1

Real slot bins from `C:\wt\checkbuild_origin_saftyfw` (commit 613a9b72). Baseline
PASS. Fabricated inputs (copies in TEMP, via -SlotABin/-SlotBBin): identical
slots -> FAIL; truncated slotB -> FAIL (unequal length); commit byte altered in
both slots -> FAIL (does not match git); commit altered in slotB only -> FAIL;
config_format_version altered in slotB -> FAIL (slots disagree); missing file ->
SKIP exit 3. A flipped byte outside the identity record still PASSes, which is
the documented scope (record only).

## check_all_task_stack_budgets.ps1

Real ELFs from `C:\wt\checkbuild_origin_kilnfw` and `checkbuild_8597e7a4f1`.
Both exit 1 with `bx_flash_worker` over its ceiling (3808 B vs 3792 B): a real
FAIL on real data, so the check is not vacuous in the fail direction. Using its
own negative hook, `-ForceCeiling zone_sweep=1000`, the count goes from 1 to 2
over-budget tasks (zone_sweep named). The check-input mutation is not a source
mutation. The bx_flash_worker finding is reported to the owner separately.

## check_kilnfw_ccache_no_stale.ps1

`tools/negtest.ps1 -Preset check`: in `lib_kilnfw_ccache.ps1` replaced
`$env:CCACHE_NODIRECT = "1"` with a removal of that variable (direct mode back
on). Baseline PASS (124.7 s). Mutant CAUGHT: step 5 `got 'hit(direct)'`, stale
hit on the shadowing header, and `direct_mode is 'true', expected 'false'`.
negtest left the real tree unchanged.

## test_host_fakes.ps1

tools/negtest.ps1 -Preset check, mutation in fake_gpio.c: `num < FAKE_GPIO_NUM_PINS` -> `<=`. The baseline run timed out twice (10 and 25 min, load; all 12 fakes OK, stuck in the negative-test compiles), so the mutated run used -NoBaseline (3175 s). CAUGHT: `fake_gpio: FAILED (pass=36 fail=1)`; the other 11 fakes printed OK in that run. negtest confirmed the real tree unchanged.
