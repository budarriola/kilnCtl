# Review: "Review tools4 fixes" (AX-C03, seam marker, mask cap), 2026-10-10

Reviewer: Opus, adversarial. Worktree `C:\wt\rvauxc03_fopdv3` at origin/dev 672a99fcf (none of the
reviewed files changed after ea2ba3087). No board access.

Commits reviewed:
- `dfabdd951` Review tools4 fixes: AX-C03 expects 409 aux-ownership body, taints/restores
  possibly-applied writes, mask cap 0xFF; seam guards require scratch-repo marker; tests
- `ea2ba3087` Review tools4: mark fix status

## Verification run

- `uv run pytest tests/test_bench_test_cases_aux.py tests/test_mcp_server_control_zone_relay_mask.py`:
  **1 failed, 73 passed** in this owner shell (`test_env_opt_in`, see LOW-6); 74 passed with
  `KILNCTL_AUX_BENCH_CONFIRM` unset.
- `tools/check_land.ps1`: all cases passed. `tools/check_dev_promote.ps1`: all cases passed.
  `tools/test_check_submodule_pins_pushed.ps1`: all passed, including the three new schannel cases.
- `tools/negtest.ps1 -Preset pytest` over the two pytest files (env var unset):
  - M1 `"already owns" in text or "aux" in text.lower()` -> `"aux" in text.lower()`: **MISSED**
  - M2 any 409 passes (`if True:`): CAUGHT
  - M3 no restore on possibly-applied outcome: CAUGHT
  - M4 `ZONE_RELAY_MASK_MAX = 0x1FF`: CAUGHT

## Answers to the focus questions

- **Can AX-C03 PASS while firmware accepted the write?** No. PASS needs `status == 409`. The default
  writer gets that only from the tool's `refused by firmware (HTTP 409)` line, which the tool prints
  only when `post_zones` raised a `ZonesHttpError`. The firmware's aux check
  (`zones_http_post.c:657-667`) runs on the scratch `tmp` and returns before the ceiling raise and
  the commit section, so a 409 means nothing was committed. No path turns a 2xx into a 409.
- **Is the 409 body the real firmware text?** Yes. The tests use the exact string from
  `zones_http_post.c:663-664`. The matcher is looser than the text, though (LOW-2).
- **Can the case leave aux or zone config changed without a restore?** Not silently, but see
  LOW-3 (a dead-today path that skips taint and restore) and LOW-4 (only the mask is restored).
- **Marker guard.** Sound. The guard triggers only when a stub is passed, so the coordinator's
  `dev_promote.ps1 -Commit X -CheckLog <log> [-Push]` from `C:\wt\devbatch1` is unaffected: the
  common-dir probe with the default `-RepoPath` just yields "not scratch", and nothing reads that
  value without a stub. Only the two check fixtures create `kilnctl_scratch_repo`, both in temp
  clones. The real repo's `.git` has none. A bypass needs someone to create that file in the real
  `.git` on purpose, and `land.ps1` also still needs `-AllowStandaloneClone`. INFO: because a linked
  worktree resolves to the main `.git`, one stray marker there would open the seams for every
  worktree. That is acceptable for an accident guard.
- **Mask cap 0xFF.** Correct for the wire format. `zones_http_post_parse.c:175` parses
  `z%u_relay_mask` as a u8 over 0..0xFF. Lines 179-181 then refuse 400 any bit at or above
  `relay_count`, and `KILN_IO_RELAY_COUNT` is 4 (`AUX_OUTPUTS_COUNT 4u`, `aux_outputs_cfg.h:51`). So
  the real valid range is 0..0x0F, and the firmware enforces it. The aux numbering 8+(relay-1) is a
  profile-rule target id, not a relay_mask bit, so it does not argue for a wider cap.

## Findings

### HIGH / MED

None.

### LOW-1: pre-aux-check 409s are reported as FAIL (a false firmware-defect finding)

`_case_ax_c03` treats any 409 without the aux text as `FAIL "firmware answered 409 but not the
aux-ownership refusal"`. Three 409s that `zones_post_handler` sends **before** the aux check carry
no mode-gate marker (`firing or autotune run is active`). So the tool reports them as
`refused by firmware (HTTP 409)`, not as a precheck refusal:
- `backup_import_restore_in_flight()`: "a backup restore or configuration change is in progress"
  (line 237ff)
- `kiln_cfg_swap_zone_edits_at_risk()`: `KILN_CFG_SWAP_ZONE_EDITS_RISK_TEXT`
- the `ota_http_check_interlocks()` 409s that follow

A 409 from any of these says nothing about the aux guard. It should be INCONCLUSIVE. The 409s
sent **after** the aux check (safety_ceiling_raise_failed, zones_config_changed_concurrently,
zones_config_undecided) do prove the guard let the mask through, so FAIL is right for those. The
fix is to classify by body: aux text -> PASS; one of the post-check error keys -> FAIL; anything
else -> INCONCLUSIVE.

### LOW-2: the owner-text match is looser than the firmware text

`if "already owns" in text or "aux" in text.lower()`: the `"aux"` alternative accepts any 409 body
that contains "aux". `text` is the whole tool line, including the `(host=...)` suffix, so a host
name containing "aux" would turn every 409 into a PASS. No 409 on this route contains "aux" today
apart from the target one (mode gate, backup, rollback, ceiling, lost-update and undecided texts
all checked), so there is no false PASS now. But the specific text is not pinned: negtest M1 is
MISSED. The fix is to match `"already owns"` only, and add a test where a 409 body mentions "aux"
for another reason and must not PASS.

### LOW-3: a post-2xx outcome is classified as pre-POST (no taint, no restore)

`control_set_zone_relay_mask` returns `refused: POST /api/zones refused: <body>` when the POST
answered 2xx with a body other than `ok`. That line starts with `refused:`, so `pre_post` is true
and the case returns INCONCLUSIVE without tainting or restoring, even though the write may have
landed. This is dead today, because the only 2xx is `httpd_resp_sendstr(req, "ok")` (line 897). It
would become live if the success body ever changes, e.g. to JSON. The fix is to exclude the
`refused: POST /api/zones refused:` prefix from `pre_post`.

### LOW-4: the restore covers relay_mask only, and a failed restore is not retried

On `FAILED: ... other field(s) changed`, the restore posts the original mask but cannot put back
the collateral fields the first write changed. A restore that fails or is unconfirmed is attempted
once, and no teardown hook retries it. Both outcomes taint the run and say "restore NOT
confirmed", so the leftover change is loud, not silent.

### LOW-5: no precondition check that relay 4 is an enabled aux output

AX-C03 does not check that AX-C01's state (relay 4 ENABLED as aux) holds. Two cases break it:
running AX-C03 alone, or running it after AX-C01 FAILed (which does not taint). In both, the
firmware correctly accepts the mask, and the case reports FAIL plus taint as if it were a firmware
defect. The fix is to read `control_get_aux_outputs` first and return INCONCLUSIVE unless relay 4
is ENABLED.

### LOW-6: `test_env_opt_in` is environment-dependent and fails on the owner's machine

`KILNCTL_AUX_BENCH_CONFIRM=1` is set at User scope on this machine. The test's final
`assertFalse(C._confirmed({}))` runs after it restores the old value `"1"`, so it fails in every
session started from this profile. The fix note in `REVIEW_TOOLS4_2026-10-10.md` says this test
was "already mock-safe in its own body". It is not. The fix is to clear the variable inside the
test (for example `mock.patch.dict(os.environ, clear=...)` / `pop`) before the final assertion.

### INFO

- The three schannel cases are meaningful: the cert-failure case adds a reachable origin and
  expects exit 1, so a SKIP there would fail the test.
- The `dev_promote.ps1` OVERRIDE line now reports the stub's real exit code after it runs. Good.
