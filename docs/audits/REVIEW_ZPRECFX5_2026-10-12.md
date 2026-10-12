# Review ZPRECFX5: exact Pico-ceiling sync (`6a8ba27b4`), 2026-10-12

Reviewer: opus (rvzprecfx5). Subject: `6a8ba27b4` on origin/dev, the fix for
`docs/audits/REVIEW_ZPRECFX4_2026-10-12.md` LOW-1/INFO-1/INFO-2/INFO-4. It:

- removes `SAFETY_CEILING_EPSILON_C`;
- makes `safety_ceiling_policy_guard_raise()` and `safety_ceiling_policy_apply_lower()`
  act on any exact float difference;
- prints the divergence alarm values at `%.9g` instead of `%.2f`;
- makes wizard steps 6 and 7 compare `kgR6(a)` against `kgR6(b)` instead of using 0.01 slack;
- adds tests.

Reviewed at worktree HEAD `828db5360`, which contains `6a8ba27b4`. No SaftyFW
file is touched, so SaftyFW host tests were not run.

**Verdict: 0 HIGH, 0 MED, 3 LOW, 4 INFO.** The safety logic is correct. The
representation chain is lossless end to end, so there is no write loop and no
permanent divergence halt. `apply_lower` never leaves the Pico ceiling tighter
than a zone max. All three LOWs are test or display gaps.

## 1. End-to-end round trip (ESP to Pico and back)

Every hop is either a bit copy or a `%.9g`/`strtof` round trip, and that round
trip is the identity for finite float32 values:

1. **ESP to text.** The ESP float is formatted `%.9g` into `value_text` (`safety_cfg_write.c`, `safety_cfg_write_set_and_confirm_f32`).
2. **Text to float.** `strtof` parses it back (`safety_cfg_write_parse_value_for_type`).
3. **Over kilnlink.** The value travels as f32 little-endian bits, copied through a union (`kilnlink_bytes.h:48-66`).
4. **Pico stores.** The Pico stores the raw f32 (`SaftyFW config_params.c:465` `rec->abs_max_temp_c = value.f32_val`), and the flash copy uses `put_f32_le` (`config_store.c:430`).
5. **Pico back to ESP.** `GET_CONFIG_PAGE` returns the f32 bits to `safety_cfg_store`.
6. **Confirm.** `confirm_commit_landed` compares with `param_value_equal`, which is a `memcmp` (`safety_cfg_write.c:117-126`).

A successful write therefore reads back bit-equal. The divergence compare
(`config_identity_normalize_f32`, `%.9g` then `strtof`) then finds equality, and
reconcile has nothing more to write. If any hop were lossy, the `memcmp`
confirm would already fail and report the write as failed. That was true
before this commit as well, so it could not produce a silent loop.

The lossy-read mutant in the negtest table shows that the tests model this: a
-0.001 read-back is caught by the zones-HTTP and divergence tests.

## 2. Can `apply_lower` leave the Pico tighter than a zone max?

No. `safety_ceiling_policy_target_c()` is the exact maximum of the positive
zone maxima, with no headroom. `apply_lower` writes that target only when it is
strictly below the current ceiling (`safety_ceiling_policy.c:135`), and it is
called post-commit with the live maxima (`zones_http_post.c:41-58`,
`backup_import.c:3067`). Raises go through `guard_raise` before the commit
(`zones_http_post.c:700-760`, `backup_import.c:2350`), and a failure there
refuses the save with a 409.

A partial failure, such as a lower that fails after the commit, leaves the Pico
looser. That is the fail-safe direction: the exact divergence check halts heat
until the next save heals it. This behaviour predates the commit. The owner rule
(same-or-looser, now "always equal") holds.

Removing the epsilon is a strict improvement. Previously a raise smaller than
0.01 was skipped, which left a zone max above the Pico ceiling that the exact
divergence check then halted on forever.

## 3. `%.9g` alarm text bounds and parsers

The `esp_side`/`pico_side` buffers are 32 bytes each
(`config_divergence.c:106-128`). The longest `%.9g` float is
`-3.40282347e+38` (15 chars), so it fits. The reason buffer is
`CONFIG_DIVERGENCE_REASON_MAX` (160). Every consumer bounds the reason with
`snprintf` at that cap or at its own cap:

- `readiness_http.c:684`
- `kiln_cfg_http.c:277`
- `kiln_cfg_store.c:2800`
- `kiln_cfg_swap.c:1014`

No tool, test or page parses `ESP=<%.2f>`. Two side effects are LOW-1 and
LOW-2 below.

## 4. Wizard `kgR6` compare against firmware

`kgR6(v) = parseFloat(Number(v).toPrecision(6))` (`setup_wizard_page.html:597`).

- **Step 6** (`:921`) refuses when `kgR6(zone) > kgR6(absMax)`. This is stricter
  than the firmware, which would simply raise the Pico ceiling. That is acceptable.
- **Step 7** (`:1649`) refuses when `kgR6(absMax) < kgR6(maxZone)`. It can accept
  an `abs_max` up to about 0.005 below the zone float, because
  `kgR6(1260.295) == 1260.3`. The commissioning POST has no firmware-side
  "tighter than zone" refusal, but reconcile's `guard_raise` then raises the Pico
  ceiling to the exact target in one confirmed write and converges (INFO-3).

## Findings

### LOW-1: the ARMED-suffix truncation test no longer reaches truncation -- RESOLVED AS UNREACHABLE (zprecfx6): worst-case reason through the sync seam is 82 chars < head_cap 91; version-mismatch/unknown reasons cannot be produced there (ESP fields always known, shared format_version). Clamp is defensive; test comment corrected, suffix-in-full still pinned.

`firmware/KilnFW/App/test/test_safety_ceiling_sync_divergence.c:596-640`
(comment at :605-611, value at :616).

The test relies on the `%.2f` rendering of `3.4028235e38f` (about 44 chars) to
push the reason past `head_cap` (91). With `%.9g` the value prints as
`3.40282347e+38`, and the reason is about 74 chars, under 91. So the truncation
branch at `safety_ceiling_sync.c:649-651` is no longer exercised.

The branch is still reachable in production, through the format-version-mismatch
and unknown-field reasons at `config_divergence.c:181-200`, so the code is still
needed. Only its pin is gone. The vacuity probe below, which removes
`head_len = head_cap;`, was MISSED.

**Fix:** drive the test with a reason that still exceeds 91 chars, such as the
format-version-mismatch reason, and update the `%.2f` comment.

### LOW-2: the Pico-side `%.9g` is not pinned -- FIXED (zprecfx6)

`firmware/KilnFW/App/test/test_config_divergence.c:84`.

The check `strstr(reason, "Pico=1260.3")` also matches the `%.2f` text
`Pico=1260.30`, so reverting `pico_side` to `%.2f` (`config_divergence.c:122`)
passes. The negtest `pico_fmt_2f` was MISSED. Only the ESP side is pinned
(CAUGHT at :83).

**Fix:** make the Pico value need more than 2 decimals (for example Pico=1260.305,
ESP=1260.3), or assert that the following character is a space.

### LOW-3: the wizard tests do not tell `kgR6` apart from the old 0.01 slack -- FIXED (zprecfx6)

`firmware/KilnFW/App/test/test_setup_wizard.js:537-542,974-982`.

Restoring the pre-fix expressions exactly passes the whole suite:

- `z.max_temp_c > absMaxTempC + 0.01`
- `fields.absMaxTempC + 0.01 < maxZone`

The negtests `step6_slack001` and `step7_slack001` were both MISSED. The new
0.05 cases separate a 0.1 slack, not the 0.01 slack that the commit actually
removed.

Two inputs do tell the old and new code apart:

- step 6: zone 1260.305 against ceiling 1260.3. The old code accepts it; `kgR6` refuses it (1260.31 > 1260.3).
- step 7: `abs_max` 1260.294 against zone 1260.3. The old code accepts it; `kgR6` refuses it (1260.29).

Not a safety issue: the firmware heals either way. But the fix's wizard half is unpinned.

### INFO-1: the new header comment overstates reconcile -- FIXED (zprecfx6)

`firmware/KilnFW/App/drivers/safety/safety_ceiling_policy.h:115-123` says
"reconcile heals a divergence only through guard_raise()/apply_lower()".

Reconcile (`safety_ceiling_sync.c:906`) calls only `guard_raise`. A Pico ceiling
above the zone max is healed by the next zone save, backup import or swap
(`apply_lower`), not by reconcile. The comment should say reconcile raises and
saves lower.

### INFO-2: stale comment naming the deleted macro -- FIXED (zprecfx6)

`firmware/KilnFW/App/drivers/safety/safety_ceiling_policy.c:5-8` still says
"SAFETY_CEILING_EPSILON_C is declared in safety_ceiling_policy.h". The macro is gone.

### INFO-3: wizard step 7 tolerances (pre-existing design)

The `kgR6` print-precision band lets step 7 accept an `abs_max` up to about 0.005
below the zone float. One reconcile raise heals it, as described in section 4.

Step 7 also accepts an `abs_max` above the zone max. Under the always-equal rule
that becomes a standing divergence halt, which reconcile never lowers. Only the
next zone save heals it. This predates the commit and is noted for the owner rule.

### INFO-4: LCD `%.130s` can cut the ARMED hint, and refusal text prints `%.1f` -- LCD part FIXED (zprecfx6); %.1f left as is

`firmware/KilnFW/App/drivers/ui/ui_page_home_refresh.c:414` prints
`"Config mismatch (Pico): %.130s"`. A realistic `%.9g` reason plus the 68-char
ARMED suffix is about 142 chars, so the LCD can now lose the tail of the hint.
Under `%.2f` the same message was about 128 chars. The web, readiness and API
paths show it in full.

Separately (pre-existing), `kiln_cfg_store.c:1651` and `:2353` compare exactly
but print `%.1f`, so a refusal for a sub-0.05 difference shows two identical numbers.

## Negative tests (run by this review)

All runs used `tools/negtest.ps1` from `C:\wt\rvzprecfx5b_is4cg0` at
`828db5360`. For every run:

- the baseline passed;
- `real_tree_unchanged` was true;
- the copies were removed;
- there was no `REAL TREE CHANGED`.

The two runners were:

- **Host tests:** `build_host_tests.ps1 -OutDir {OUT} -Only "ceiling|divergence"` with `-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`. All host mutants compiled: no `BUILD FAILURES` and no compiler error in any log.
- **Wizard tests:** `node test_setup_wizard.js` with `-RequireAssertion`.

| Mutation | Runner | Verdict | Evidence |
|---|---|---|---|
| `guard_raise`: `<= effective_current + 0.01f` | host | CAUGHT | `test_safety_ceiling_policy.c:98: zone max 0.005 above the Pico ceiling must raise`, `RUN FAILURES (1)` |
| `apply_lower`: `>= current_pico_ceiling_c - 0.01f` | host | CAUGHT | `test_safety_ceiling_policy.c:105: Pico ceiling 0.005 above the zone max must be lowered to match` |
| `esp_side` back to `%.2f` | host | CAUGHT | `test_config_divergence.c:83: ESP value printed with enough decimals` |
| `pico_side` back to `%.2f` | host | **MISSED** | LOW-2 |
| Lossy ceiling read-back (`f32_val - 0.001f`) | host | CAUGHT | `test_zones_http.c:2108/2289` (got 99.9990, want 100.0000), `test_safety_ceiling_sync_divergence.c:482-485,565-569,626` |
| Vacuity probe: drop `head_len = head_cap;` | host | **MISSED** (expected) | LOW-1 |
| Step 6: `z.max_temp_c > absMaxTempC + 0.01` (pre-fix) | node | **MISSED** | LOW-3 |
| Step 6: raw exact compare, no `kgR6` | node | CAUGHT | `FAIL: step6: raw 1260.30005 vs printed ceiling 1260.3 is accepted` |
| Step 6: `kgR6` + 0.1 slack | node | CAUGHT | `FAIL: step6: 1260.35 vs 1260.3 (0.05 over, past kgR6) is refused -- no 0.1 slack` |
| Step 7: `absMaxTempC + 0.01 < maxZone` (pre-fix) | node | **MISSED** | LOW-3 |
| Step 7: raw exact compare | node | CAUGHT | `FAIL: step7: typed 1260.3 vs raw zone max 1260.30005 is accepted` |
| Step 7: `kgR6` + 0.1 slack | node | CAUGHT | `FAIL: step7: 1260.25 vs zone max 1260.3 (0.05 tighter, past kgR6) is refused -- no 0.1 slack` |

The firmware half of the fix (both epsilon removals, the ESP alarm format) is
pinned by real assertions. The Pico alarm format, the truncation pin and the
wizard half are not (LOW-1..3).
