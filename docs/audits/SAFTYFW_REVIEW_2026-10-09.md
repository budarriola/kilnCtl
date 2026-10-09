# SaftyFW / CommonFW review, 2026-10-09

Source-only review of every commit touching `firmware/SaftyFW/` or
`firmware/CommonFW/` on origin/dev after `86415191` (the end of the range
covered by `recovery_bootloader_audit_2026-10-02.md`) up to `de296633`.
Nothing was run on hardware. Host tests were run once at `de296633`:
`firmware/SaftyFW/test/build_host_tests.ps1` passed in full (main exe
3579/3579, hal_spi_pico 56/56, config_store_flash 333/333, bootloader
recovery 72/72, fuzz all passed).

## Scope

37 commits are in range. **No firmware source behavior changed.** The only edit
under `src/`, `bootloader/` or CommonFW `src/`/`include/` is a comment fix in
`firmware/SaftyFW/src/board/board_pins.h:24-25` (`651bcbcd`, ADuM1201 channel
naming). The rest are:

- Tests: `9fb7bc1b` (frozen v1/v2 config_store blobs), `a5447ec4` (Frame C
  pack pinned against the CommonFW codec, with an ESP-side mirror in
  `firmware/KilnFW/App/test/test_safety_link_compile.c:1371`).
- Build and check tooling: `build_host_tests.ps1` parallel pool (`613a9b72`,
  `d7e232c9`, `d1c700b5`), CommonFW ctest floor (`648b1a87`, `f920711b`),
  SKIP-FAST relabels (`1b21e90f`), link-isolation allowlists, build-gate
  plumbing.
- Docs only: UPDATE_PROTOCOL.md, HARDWARE.md, CURRENT_SENSE.md,
  THERMOCOUPLE.md, ARCHITECTURE.md, BOOTLOADER.md, TODO.md, CommonFW README.

Checked with no finding: guard reachability, `trip_mask`/`trip_reason`
(`1 << (reason-1)`), link-protocol drift (no wire change; the new Frame C test
is mirrored on the ESP side), stack/ISR hazards, the reset-one-side bug class,
and the owner decisions (no thermal-guard disable added; abs_max not
tightened. The new migration test asserts a v2 `abs_max_temp_c` is carried
bit-exact and a v1 record leaves it unset rather than inventing a value).

## Findings, ranked

No high or medium findings.

### Low

1. **Known limitation (accepted, documented in `config_store.c`): pre-existing, not introduced in range: v1 odd slots are invisible to the
   migration scan.** v1 firmware wrote 256 B records, 16 slots per sector
   (`config_store.h:26-27`, `bootloader/flash_layout.h:85-86`).
   `config_store_find_latest_ex()` walks the sector at the current 512 B
   stride (`config_store.c:1139-1140`), so it reads only v1 slots 0, 2, 4, and
   so on. If a v1 sector's newest record sits in an odd slot, the migration
   loads an older even-slot record (stale `tc_type`/`ct_cal`) or, with only
   slot 1 written, nothing (defaults). The write path is safe: it refuses to
   program a non-erased slot (`config_store_flash.c:1482`). Impact is limited
   because migration forces `calibration_missing` true, so commissioning is
   required again anyway. Only a board still holding an unrewritten
   pre-2026-08-21 v1 sector is affected. The new frozen-blob sector test
   (`test_config_store.c:1389`) places the v1 blob at slot 0 only, so it cannot
   see this. Nothing documents the gap. Fix: either scan v1 records at a
   256 B stride when slot 0's format_version is 1, or record the gap as
   accepted.
2. **FIXED: wrong provenance commit in the frozen-blob comment.**
   `firmware/SaftyFW/test/test_config_store.c:1221` says the v1 layout is
   `config_store_pack()` at `901256e0`. That commit introduced v2 (512 B
   records). The v1 layout is at `901256e0^`. The bytes themselves are right:
   they match `901256e0^`'s pack (tc_type @12, calibration_missing @13, ct_cal
   @16, CRC @248), and both CRCs were recomputed independently (v1 `0xAAB0FE42`
   over [0,248), v2 `0x53E67DB5` over [0,504), both equal to the stored
   values). Fix the comment to `901256e0^`.
3. **FIXED (torn-data-byte case added): the truncation test only exercises an erased CRC.**
   `test_frozen_blob_truncated_rejected()` (`test_config_store.c:1389`, cuts
   at `:1394`) erases from byte 100 (v1) and 300 (v2) onward, which also erases
   the stored CRC. For v1 the cut changes no payload byte at all (bytes 43-247
   are already 0xFF), so both cases reduce to "CRC field erased". That is the
   real torn-write shape, because the CRC is written last, so this is not a
   protection gap. Still, the test name overstates what it covers. Consider
   adding a case that corrupts the payload but keeps the old CRC.
4. **FIXED: fuzz failure lost its reproduction hint.** Before `613a9b72`, a fuzz
   failure threw "rerun with KILNLINK_FUZZ_SEED set to the seed printed
   above". The pooled path (`build_host_tests.ps1:392` and the exit chain at
   the end) now exits with the exe's code and no hint. The seed is still
   printed by the exe itself, so this is cosmetic only.

### Informational

- `check_no_sim_plant_guard_disable.ps1` (`1b21e90f`) now reports SKIP-FAST,
  which is non-fatal, when `build/saftyfw_build_info.h` is absent under
  `-Fast`. A `-Fast` run therefore never proves the no-guard-disable property.
  Only a full run, where `check_00_saftyfw_target_build.ps1` produces the
  header, enforces it. This is intended and the label is exact-reason only.
