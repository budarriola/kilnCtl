# ROADMAP truth-up, 2026-09-14

A roadmap survey flagged ten stale/duplicate/misnamed items. Each was
independently re-verified against code, git history, or a live board read
before any doc was changed — the survey's own claims were not assumed
correct. No firmware was flashed, no heating run was performed, and no
`.kicad_*` file was touched.

## Closed (done but marked open)

1. **Pico seqlock config-store fix is flashed.** Live
   `safety_get_fw_version()`: `Pico build: d957d5fd (dirty) ... commissioned`
   — far newer than either landing commit
   (`ae23aba4` 2026-09-09, `b88ea6ba` 2026-09-10). `git cat-file -t` confirmed
   `b202fe56`/`5671ee03`/`cb1ba325` all exist as commits. ROADMAP's summary
   table row (the LittleFS/`cfg` row) said "none of the three seqlock commits
   have been flashed to the bench Pico yet" — corrected in place; the
   sweep-log entries elsewhere in ROADMAP already said FLASHED and were left
   alone.

2. **S8 rate guard is ARMED, not DORMANT.** Live `safety_get_rate_guard()`:
   `max_rate_c_per_min=20C/min (ARMED) | rate_window_s=60`. Corrected the two
   ROADMAP rows that said "guard remains DORMANT (0)". **Left open, per
   instruction: whether 20 C/min (vs. the documented 33.3 C/min 2x-fastest
   rule, or an auto-derived value) is right for a real kiln is an owner
   decision, not resolved here** — flagged in both corrected rows that 20 is
   tighter than the documented rule and could nuisance-trip a 900 C/hr zone.

3. **`autotune_baseline_k_dc` IS emitted on `GET /api/zones`.**
   `firmware/KilnFW/App/drivers/http/zones_http_get.c:606`:
   `APPEND("\"autotune_baseline_k_dc\":%.4f,", ...)`, added by `0dbd7c6d`
   ("Expose autotune_baseline_k_dc on GET /api/zones (was write-only)"),
   confirmed via `git log`/`git cat-file -t 0dbd7c6d` (commit exists).
   Corrected `docs/audits/mcp_zone_model_fields_2026-09-13.md`'s claim that
   this is "a real firmware gap." **Left alone: the matching hardcoded
   string in `tools/PcTools/src/kilnctrl/mcp_server_control.py`** — that
   file is owned by another concurrent session per this task's
   instructions, and was not edited. Read-only inspection found it already
   checks the key's presence dynamically (`if "autotune_baseline_k_dc" not
   in z`) rather than hardcoding "not exposed," so it likely already renders
   correctly against current firmware, but this was not verified live and no
   claim to that effect was added anywhere.

4. **`iter_tune` steps 3, 4 and step 7 are shipped.** `git cat-file -t`
   confirmed `e0d2e006`, `225d4b91`, `f3fcd597` all exist; `git show --stat`
   confirmed their content (sim-harness G1/G3/G4 gaps, sec 6.5 credibility
   gate reporting FAIL, `check_iter_tune_write_surface.ps1`).
   `docs/ITER_TUNE_REDESIGN_PLAN.md` itself already carries a 2026-09-10
   "steps 1, 2, 3, 4, 5 ... landed" status update. Corrected ROADMAP's two
   stale rows (both said "steps 3-4 ... open/design-only") and consolidated
   the second into a pointer to the first (see Duplicates below) since they
   had drifted to different, both-wrong step lists.

5. **E-stop / SaftyFW Phase 0.8 is closed.** Confirmed
   `firmware/SaftyFW/test/test_estop_deenergizes_relay.c` exists and
   `firmware/SaftyFW/README.md` documents `estop_verified` /
   bench-verification. ROADMAP's own E-stop row already recorded the
   2026-09-10 owner decision ("pole 1 stays permanently unwired ... consider
   it closed so long as the signal is checked and acted on"). The
   `firmware/SaftyFW/TODO.md` line 189 checkbox was the only place this
   closure was never recorded — checked it off with a citation.

6. **The boot-hang blocker is resolved.** `docs/FILESYSTEM_PLAN.md` itself
   documents, further down the same file ("`cfg` partition re-flashed after
   stack-overflow fix, 2026-09-07"), that `3c36b7e1` was built and flashed
   from a clean detached worktree, host tests 31/31 passed, and the board
   came up normally. `git cat-file -t 3c36b7e1` confirmed the commit exists.
   The file's own opening banner still presented the original 2026-09-08
   panic warning as a live, current blocker — added a resolution note above
   it and relabelled the original text "history only," per instruction, kept
   rather than deleted.

## Retracted / superseded (contradicted)

7. **`FUZZY_CONTROLLER_PLAN.md` §4.1** concluded "(iv) [delete the fuzzy
   layer] as the honest disposition of the fuzzy layer." The file's own
   header already carries a 2026-09-14 "OWNER DECISION: SHIP the
   band-derivation half of this plan" note, and ROADMAP's header (line 11)
   confirms `2c49465a` shipped `rate_band_c_per_s`/`error_band_c` derived
   per-zone from the autotune model — fuzzy is KEPT, not deleted. Added a
   visible superseded-marker directly above the original §4.1 text rather
   than rewriting it, per `project_retraction_hid_the_stale_claim` (a prior
   incident in this repo where a silent rewrite hid a stale claim from
   review). The original conclusion is left intact below the marker as
   history.

8. **`FILESYSTEM_USER_DATA_PLAN.md`** said "nothing below is implemented."
   `docs/CONFIG_FILESYSTEM.md` documents zones/profiles/prefs dual-writing
   to the `cfg` partition live today (NVS stays authoritative). Added a
   stale-marker to the plan doc pointing at `CONFIG_FILESYSTEM.md` as the
   authoritative status doc, per this repo's "newest doc wins" convention —
   did not delete or rewrite the design content, which is still accurate as
   a design record.

## Convention cleanup

9. **`_PLAN` suffix removed from two status docs, all references fixed:**
   - `docs/UNIT_TEST_FIXTURE_PLAN.md` -> `docs/UNIT_TEST_FIXTURE_STATUS.md`
     (`git mv`). Verified zero unchecked boxes and "14 passing tests" claim
     both hold in the file's own text (`.venv/Scripts/python.exe -m pytest
     tests/test_fixture*.py -q` -> 14 passed, per the file's own
     "Verification run this session" section). Only remaining item is
     owner-gated bench validation, matching the "status not plan" call.
     References fixed in `ROADMAP.md` and (read-only-owned files reverted,
     see below) confirmed via repo-wide grep.
   - `firmware/KilnFW/docs/DRAM_PSRAM_PLAN.md` ->
     `firmware/KilnFW/docs/DRAM_PSRAM_STATUS.md` (`git mv`). Verified zero
     `- [ ]` unchecked boxes remain in the file. References fixed across
     `ROADMAP.md`, `docs/audits/executor_panic_stack_overflow_2026-09-09.md`,
     `docs/audits/profile_executor_panic_2026-09-10_root_cause.md`,
     `firmware/KilnFW/docs/FLASH_BUDGET.md`,
     `firmware/KilnFW/docs/WEB_UI_RESPONSIVE.md`, `firmware/KilnFW/TODO.md`,
     and the `check_*`/`build_host_tests.ps1` comment citations in
     `firmware/KilnFW/App/test/` and top-level `tools/`.
   - **Important correction to my own first pass**: a repo-wide sed for both
     renames initially also touched files under `tools/PcTools/src/kilnctrl/`
     and `tools/PcTools/tests/` (comment-only citations of the old
     filenames). That directory is explicitly owned by another concurrent
     session per this task's instructions and off-limits. All such edits
     were identified and reverted by hand (confirmed via `git diff` showing
     zero content difference afterward) before anything was staged. The
     handful of references that live in that owned directory
     (`fixture.py`, `mcp_server_fixture.py`, `mcp_server_flash.py`,
     `serial_link.py`, `mcp_server_info.py`, `devices_info.py`,
     `dashboard_http_client.py`, `stack_margin_baseline.py`, and their
     tests) still say the **old** filenames and are left as an outstanding
     item for whichever session next touches that directory, or for the
     owner to do in one pass.
   - `ON_OFF_ZONE_PLAN.md` left untouched (one open step, per instruction).

10. **Duplicate ROADMAP rows consolidated to one each:**
    - CT commissioning: three rows (the detailed M-size row, and two
      near-identical "owner-dependent items" rows) collapsed to one detailed
      row plus two short pointers back to it, re-verified against a live
      `safety_get_commissioning()` read (S14/S15 confirmed still DORMANT,
      `i_normal_a not measured`, matching all three rows' conclusion, so no
      row was actually wrong here — just redundant).
    - Field updates / Pico OTA: two near-identical rows (M8 area and the
      L-size row) collapsed to one, pointer added at the second location.
    - `iter_tune`: two rows with different, both-stale step lists
      consolidated — the L-size row now points at the corrected M-size row
      instead of repeating (and disagreeing with) it.

## Checks

`tools/check_doc_hash_citations.ps1` and `tools/run_all_checks.ps1` results
are recorded in this session's final report; see the commit message / PR
description for the tally. (Filled in below once the full suite completed.)
