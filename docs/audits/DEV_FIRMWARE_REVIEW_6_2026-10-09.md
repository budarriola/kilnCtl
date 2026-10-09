# Dev firmware review 6 (2026-10-09)

Scope: every commit on `origin/dev` after `01b16066` (review 5 doc) through `52502d56`: `8b193746`,
`3a240a3a`, `7551d44a`, `f419141c`, `39231ca2`, `9a9bd654`, `52502d56`. `8c287553` was asked for too,
but it is an ancestor of `01b16066` and review 5 already covered it; nothing in this range touches it.

This was a code review only, with no builds and no board access. One finding (M2) was confirmed with a
negative test, run as a scratch script against the worktree. That script was not committed.

## HIGH

None.

## MED

### M1. A failed first migration erases the legacy Wi-Fi credential, and the "will retry" logs are false (7551d44a)

**Fixed in @@SHA@@.**

- Where: `firmware/KilnFW/App/drivers/net/wifi_prov_nvs.c:531-543` (`adopt = !found_in_wifi_nvs`, plus the
  stale-copy erase); `:561` (`nvs_save_mode()` runs first); `:582`, `:587`, `:621`, `:629` (the "will
  retry" messages).

What happens: the old rule `adopt = !found || (default_has_legacy && !wifi_nvs_has_legacy)` is what
allowed a partial migration to be retried. The new rule adopts only when the `wifi_nvs` namespace is
absent. The migration's first write, `nvs_save_mode()`, opens `wifi_nvs` READ_WRITE, and that creates
the namespace. It does so even if the later writes fail.

So on the next boot `found_in_wifi_nvs` is true, and the `!adopt` branch erases the legacy keys as
"stale". `s_legacy_single` then comes from `wifi_nvs`, which never holds the legacy single keys, so it is
empty. `nvs_load_saved_nets()` has nothing to migrate.

Scenario:

1. A board provisioned before the split boots this build for the first time.
2. `nvs_save_mode()` succeeds. Then one of three things happens: `nvs_save_saved_nets()` fails (for
   example, the partition is full), the saved_nets or mode read-back differs, or power is lost before
   `nvs_load_saved_nets()` runs.
3. The log says "will retry next boot".
4. On the next boot the legacy copy is erased with nothing migrated. The board comes up with zero saved
   networks in AP fallback and needs re-provisioning.

The same outcome follows if anything else creates an empty `wifi_cfg` namespace in `wifi_nvs` before the
first migration runs.

Nothing tests this path. The three new tests in `test_wifi_prov.c` cover only the success path.

Fix: base the stale-copy decision on a migration marker, not on namespace presence. Write a
`migrated=1` key in `wifi_nvs` only after both read-backs pass, then erase the legacy keys. Without the
marker, treat the legacy copy as pending, whatever `found_in_wifi_nvs` says. Add a host test that fails
the saved_nets write on boot 1 and checks that boot 2 still migrates the network.

### M2. The fail-closed write-gate test passes with the gate removed for 12 of its 15 cases (8b193746, 3a240a3a)

- Where: `tools/PcTools/tests/test_bench_test_suite_gate.py:38-41` (the fake `get` returns `{}` for
  every path except the executor and autotune), and `:69-79` (`_check` asserts only that nothing was
  posted and the verdict is not PASS).

What happens: every judge reads its own precondition (`relay_type`, `temp_unit`, `/api/profiles`,
`/api/zones`, and so on) before writing. Against `{}`, that read fails, and the judge returns FAIL,
INCONCLUSIVE or SKIP before it reaches any POST. Both assertions therefore hold whether or not the
`write_refusal()` gate exists.

Negative test: with `board_lock.write_refusal` monkeypatched to always return `None`, the test caught
only three cases. WEB-SEC-03, WEB-SEC-04 and WEB-LOG-02 raised through `_Boom` on `get_config`. The other
12 cases still posted nothing and returned a non-PASS verdict, so the test still passed: COMM-07,
WIZ-03, WIZ-10, DISP-02, DASH-13, DIAG-07, DIAG-08, DIAG-09, KCFG-02, PROF-02, PROF-08 and ZONE-05.

The hand-written `WRITING_IDS` list also omits writers that the gate covers, so a regression in those
would go unnoticed too: WEB-DASH-05 (`/api/zones/pid`) and WEB-PROF-05, -06 and -07 (via `_prof_preflight`).

The production gates themselves are present in every writing path inspected. Only the test is
vacuous.

Fix: give the fake board a well-formed GET body for each case, so that each judge reaches its write
point when the suite is `web`. Assert that it does post under `suite="web"`, then assert that it posts
nothing when the suite is absent or `smoke`. Derive the ID list from the judges that call a POST seam,
rather than keeping it by hand.

**Fixed in 0a6e8c75** (test only). Every writing judge now reaches its gate and must report the refusal;
WRITING_IDS adds DASH-05/07, PROF-03..07 and ZONE-09 and is checked against a code-derived writer scan;
a permanent test proves that removing the gate fails all 23 cases. Gap found, not changed here:
WEB-ZONE-10 POSTs `current_sweep/abort` with no `write_refusal()` gate (listed in EXEMPT_WRITERS).

## LOW

### L1. Comments still say the legacy Wi-Fi copy is never deleted (7551d44a)

**Fixed in @@SHA@@.**

- Where: `firmware/KilnFW/App/drivers/net/wifi_prov_nvs.c:237`, `:487-491`, `:523-529` (still describes
  the removed `default_has_legacy && !wifi_nvs_has_legacy` clause), and `:597`.

The old comments give the rollback rationale: a rollback to pre-split firmware still finds its network.
That rationale is no longer true. The new trade-off (a rollback past the split now boots unprovisioned)
is not recorded anywhere, so the next reader will reason from the wrong invariant.

Fix: rewrite the comments to describe the one-shot erase and its rollback consequence.

## Review 5 fix status

- M2 (legacy credential resurrection): closed for the success path. The failure path regresses; see M1
  above.
- Review 4 M3 / review 5 L6 (write gates fail-open on suite): closed in production code by
  `board_lock.write_refusal()` in every `_mutating_gate` / `mutating_gate`, and in the
  `cases_web_rw.py` writers. The test that is meant to guard it is vacuous; see M2.
- M1 and L1-L5 of review 5: not in this range. They were addressed by `dcd67f54` and `f19b7c62`, which landed on dev while this review was being written and were not reviewed here.

## Checked, no defect found

- `8b193746`: the `cases_web_diag.mutating_gate` autotune allow-list no longer accepts a missing
  `state`. Firmware always emits one of `idle`, `settling`, `stepping`, `relay_*`, `done`, `aborted` or
  `unknown` (`dashboard_json.c:282-293`), so the gate does not refuse an idle board. RDY-04's ungated
  `profile_exec/start` probe posts no `id`, and the module documents it as unable to start a firing.
  COMM-07 now prefixes `ERROR:` on a restore mismatch, which is a FAIL verdict because the registry has
  no ERROR verdict.
- `52502d56`: the wave3b ordering test still fails when a board-touching case is scheduled after
  WEB-SEC-05. Only `depends_on == "WEB-SEC-05"` aliases may trail it.
- `7551d44a`: the migration's mode/AP read-back restores `s_wifi` from the snapshot whether or not the
  comparison passes. `erase_keys()` probes READ_ONLY before it opens READ_WRITE, so it never creates a
  default-partition namespace.
