# Release-gate vacuity audit, 2026-10-07 (twelfth pass)

Continues `docs/audits/release_gate_vacuity_audit_2026-10-02.md`; BLOCKER 3 of
`docs/RELEASE_HARDENING_PLAN.md` item 3.

Work was done in a dedicated worktree, `C:\wt\vac12_o50bic`, minted at
origin/main (50fca908). No hardware was touched, no MCP tool called and no
`.kicad_*` file opened.

## Scope and method

Scope: every `check_*.ps1` added after 2026-10-02 that no earlier pass covered
(`check_app_image_size`, `check_aux_relay_conflict_sites`,
`check_kiln_scope_cfg_mirrors`, `check_monocypher_vendored`,
`check_ota_esp_refuses_running_target`, `check_persist_scratch_malloc_caps`
(extended), `check_release_manifest`, `check_release_version_regex`,
`check_safe_remove_junction`) and the `firmware/KilnFW_recovery/main/
check_recovery_*.ps1` family, which the previous pass excluded.

For each check the smallest source change that violates exactly the claim was
made, only that check was run, the failure and its reason were read, and the
source was restored by hand from a backup and verified with `git hash-object`
against the original (never `git checkout`). Discovery floors (missing scan
directory, too few files) were tested as well. A mutation that was caught for
the wrong reason (a different, later gate failing) counts as a miss.

One harness trap, recorded so nobody repeats it: a `\b` regex written through a
shell heredoc into a `.ps1` became a literal backspace (0x08) and silently
turned a new guard blind (0 files found). It was caught only because the
guard's own floor (`< 8 adopters` fails) fired, which is the reason the floor
exists.

## Results

| Check | Mutation(s) | Result | Verdict |
|---|---|---|---|
| check_app_image_size | image 1 byte over the bound; app row shrunk below the image; app row grown past the 0x400000 ceiling; missing CSV; missing/duplicate `app` row | each FAIL with the right message; no image yet is SKIP (SKIP-FAST under `-Fast`) | OK. By design it does not grade anything until `build/KilnCtrl.bin` exists, so under `-Fast` it is a SKIP-FAST, not coverage |
| check_aux_relay_conflict_sites | delete the `zones_config_json_aux_conflict_mask` call from the ordinary `POST /api/zones` commit (`zones_post_apply`) | PASS (vacuous) | WEAK, FIXED: the file-level `$mustCall` was satisfied by the restore function's call alone. `$mustCallInFunc` now pins the call inside `zones_post_apply`; same mutation now FAILs. Other call sites, the backup-import path and the unlisted-site scan: FAIL as expected |
| check_kiln_scope_cfg_mirrors | scope-list versus cfg-mirror drift mutations in `kiln_scope_cfg_files.c`, `unit_pref.[ch]`, `kiln_package.c`, `cfg_fs.c` | each FAIL, names the item | OK |
| check_ota_esp_refuses_running_target | variants of the `ota_http_esp_target_usable()` guard in `ota_http_esp.c` that keep the call text but defeat the refusal | PASS (the check located the call by position only) | WEAK, FIXED: the match is now the whole `if (!ota_http_esp_target_usable(target, esp_ota_get_running_partition())) { ... goto cleanup; }` guard. The Pico handler (`ota_http_pico.c`) mutations FAIL as before |
| check_persist_scratch_malloc_caps | plain `malloc` in `zones_http_post.c`, `zones_config_store.c`, `zone_aux_convert_http.c` (each already calls `persist_scratch_alloc`) | PASS (vacuous): none of the three files was in the scan list | WEAK, FIXED: they are listed, and a new adoption guard fails if any `firmware/KilnFW/App` `.c` that calls `persist_scratch_alloc()` is missing from the list (with its own floor of 8 adopters). Listing them exposed two real plain mallocs (`zones_http_post.c` ceiling scratch, `zones_config_store.c` `zones_config_persisted_equals_ram`), now `persist_scratch_alloc()`. Re-run: all three mutations, and an unlisted adopter, FAIL |
| check_release_version_regex | cap 32 -> 33; drop the cap; drop `-` from the prerelease charset; neutralise the leading-zero reject; allow `+` in the prerelease charset; remove/add tags in the C `badtag[]` table; rename the table | all FAIL except the `+` charset, which PASSed | WEAK (minor), FIXED: no table tag exercised `+` after a prerelease dash. `v1.2.3-rc+1` is now in both the check's table and the C `badtag[]`, and the `+` mutation FAILs |
| check_release_manifest | size gate 0x400000 -> 0x500000; dirty-tree refusal off; sha256 compare off (validator and publish re-download); draft flag off; open gates not refused; provenance refusal off; token forwarded on hop 2; semver gate off | all FAIL except the semver gate off, which PASSed | WEAK, FIXED: step 2 only asked for exit 1, and a later gate also exits 1 on tag `1.0`. It now requires the `is not semver` refusal text; the mutation FAILs |
| check_safe_remove_junction | unlink step skipped in `Remove-TreeSafe`; recursive delete through links; do not descend in `Remove-ReparsePointsUnder`; drop the unlink in `worktree_mint.ps1` and in `check_00_kilnfw_target_build.ps1`; rename the command so the pattern goes blind | descend, and both call-site drops FAIL; the "skip unlink" mutant PASSed, the blind mutant PASSed | WEAK, FIXED twice. (1) Windows PowerShell 5.1 `Remove-Item` does not follow junctions on this box, so the sentinel alone cannot prove `Remove-TreeSafe` unlinks; the check now also requires its `unlinked reparse point` report. (2) The site counter matched the *message string* `git worktree remove failed` in `worktree_mint.ps1`, so the floor of 2 was met with one real site; the pattern now requires a command line (`git`, `& git`, `$x = git`). The "recursive delete" mutant stays equivalent on this PowerShell version (documented, cannot be distinguished here) |
| check_monocypher_vendored | flip a byte in a vendored file; edit a README hash; drop a README row; delete the directory | FAIL for each; an extra, unhashed `extra.c` PASSed | WEAK, FIXED: any `.c/.h/.S` in the directory that the README table does not list now fails |
| check_recovery_* | see the next section | | |

## Recovery family

`check_recovery_page_crc` was mutated against real source (three extra mutants, all RED, for the right reason). The rest of the family (apply, boot_verify, health, hold, image_check, lcd_policy, passphrase, pico_proto, upload, wifi_policy) carries its own built-in mutant battery (each check injects violations into a scratch copy and requires a failure); no extra real-source mutants were run for these because the build gate was saturated by other sessions for the whole pass. In the full `-Fast` run page_crc, image_check, boot_verify and hold passed. `check_recovery_health` and `check_recovery_pico_proto` were not graded: their direct reruns sat in the build gate (`pico_proto` ended "build gate: timed out after 3600s waiting for a light-lane build slot"), so they remain unverified rather than green. Verdict for the family: page_crc OK; the others UNVERIFIED beyond their built-in batteries.

Verification gap: the host tests, the KilnFW target build (including the `.dram0.bss` figure against 101000 B) and `check_01_kilnfw_pushed_build` did not run to completion either (gate timeouts), so the two `persist_scratch_alloc` conversions are unbuilt and untested here.

## Findings and fixes

All fixes are to the check scripts, plus two firmware changes that were not
audit targets but fell out of the persist-scratch coverage fix:

- `tools/check_aux_relay_conflict_sites.ps1`: `$mustCallInFunc` entry for `zones_post_apply`.
- `tools/check_ota_esp_refuses_running_target.ps1`: anchored guard match.
- `tools/check_persist_scratch_malloc_caps.ps1`: three files listed, adoption guard with floor.
- `firmware/KilnFW/App/drivers/http/zones_http_post.c`, `.../persist/zones_config_store.c`: one `malloc` each became `persist_scratch_alloc` (ordinary `free()` still pairs; the helper falls back to the internal heap).
- `tools/check_release_version_regex.ps1` and `firmware/KilnFW/App/test/test_update_url.c`: `v1.2.3-rc+1` pinned as refused on both sides.
- `tools/check_release_manifest.ps1`: semver refusal text required.
- `tools/check_safe_remove_junction.ps1`: unlink report required; site pattern tightened.
- `tools/check_monocypher_vendored.ps1`: unhashed files in the directory fail.

## Standing caveat

`check_app_image_size` and the host-test builds in the recovery family grade
nothing on a machine that has not built the artifact (`SKIP`/`SKIP-FAST`), so a
green `-Fast` run in a fresh worktree says nothing about them.
