# Review ZPRECFX7: fixes for REVIEW_ZPRECFX6, 2026-10-12

Reviewer: opus (rvzprecfx7). Subject: the zprecfx7 commits on origin/dev:

- code: `6c0b12032` (fix), `712ae3fa0`, `e6f218d9b`, `28dfaf18f` (test hardening);
- doc: `2393c6977` (resolution SHAs and negtest table in `REVIEW_ZPRECFX6_2026-10-12.md`).

They answer `docs/audits/REVIEW_ZPRECFX6_2026-10-12.md` (LOW-A, LOW-1, LOW-B,
INFO-A/B/C). Reviewed in worktree `C:\wt\rvzprecfx7_el7ips` at origin/dev
`2393c6977`. No later dev commit touches the reviewed files. No SaftyFW file is
touched.

**Verdict: 0 HIGH, 0 MED, 0 LOW, 4 INFO.**

All three LOWs are fixed. Every negative test reruns CAUGHT with a compiled
mutant and a failing assertion, apart from one new boundary probe, which is
MISSED (INFO-3, not reachable).

## 1. Key question: ±0 is canonicalised on the ESP only. Can the two sides' identity hashes disagree?

**No. There is only one hash path, and it runs on the ESP for both sides.**

### 1.1 Where each identity is computed

- **ESP side.** `safety_ceiling_sync.c` builds `esp_fields[]` from the ESP's
  own zone config and the kiln-config package's expected Pico values.
  `kiln_cfg_store.c:~2990-3035` widens F32 params from raw `value_bits`, and
  `config_divergence_check()` hashes them (`safety_ceiling_sync.c:558`, `:608`).
- **"Pico side".** `pico_fields[]` holds values the ESP decodes from
  `safety_cfg_store`'s live cache, which is filled from the Pico's
  GET_CONFIG_PAGE reply. The Pico sends values only, never a hash. The ESP
  hashes them in the **same** `config_divergence_check()` call
  (`config_divergence.c:184-185`), through the **same**
  `config_identity_compute()` and `config_identity_normalize_f32()`.
- **SaftyFW** computes no config identity. A grep of `firmware/SaftyFW/src`
  for `fnv`/`identity`/`config_hash` finds only the build-image identity
  record and the CT identity map. Neither is compared with an ESP config hash.

Both identities therefore pass through the new `if (out == 0.0f) out = 0.0f;`.
"ESP-only" is the only place a hash exists, not one half of a pair. The
"reset one side of a pair" class does not apply: no paired Pico computation
exists that could go stale.

### 1.2 False divergence?

None. `0.0f` and `-0.0f` on either side, in any combination, now hash the same
bytes (`00 00 00 00`). Before the fix this case raised the never-clearing
"possible hash collision" warning (REVIEW_ZPRECFX6 LOW-A). It is pinned by
`test_signed_zero_is_canonical` (bytes, `config_identity_matches`, and
`config_divergence_check` all agree), and the `no_zero_norm` mutant is CAUGHT.

The two consumers of the normalised value now agree on every input:

- the hash (bytes);
- `format_first_field_difference()` (`ne != np`).

Before the fix, ±0 was the one input where they disagreed.

### 1.3 Hidden real divergence?

None found. The change merges only values that compare `== 0.0f`. Under IEEE
semantics that is exactly +0 and -0. A real divergence would be hidden only if
some Pico consumer behaved differently for -0.0 than for +0.0. A grep of
`firmware/SaftyFW/src` for `signbit`, `copysign` or division by a config field
finds nothing. The guards use the params in additions, comparisons and
multiplications, where ±0 behave the same.

The other ESP float compare of pushed against read-back values,
`kiln_cfg_swap.c:384` (`normalize_f32_like_wire(a) != normalize_f32_like_wire(b)`),
already treated ±0 as equal because it uses `!=`. So the swap verify and the
standing-divergence check now share one notion of equality.

The kilnlink wire and the persisted package are unchanged. The canonical
value is used only for comparison, never written or pushed.

### 1.4 Compiler

`if (out == 0.0f) out = 0.0f;` survives only while signed zeros are honoured.
`-ffast-math`/`-fno-signed-zeros` would fold it away. A grep of KilnFW and
CommonFW CMake/cmake files finds no fast-math, `-fno-signed-zeros`,
`-ffinite-math-only` or `-Ofast`, and `-Os`/`-O2` keep `-fsigned-zeros`. The
host build passes `test_signed_zero_is_canonical`. See INFO-2 for the target
caveat.

## 2. Truncation helper `config_divergence_append_suffix_truncating_head()`

Bounds checked by hand (`config_divergence.c:29-49`):

| Case | Bytes written | Verdict |
|---|---|---|
| `dst == NULL` or `cap == 0` | none, returns 0 | safe |
| `cap <= suffix_len` | `snprintf(dst, cap, "%s", suffix)` writes at most `cap` bytes including the NUL | safe; the ARMED mark may be cut (unreachable: cap 160, suffix 68) |
| `cap > suffix_len` | `head_cap = cap - suffix_len - 1 >= 0`, no underflow; `head_len <= head_cap`; the last write is `dst[head_len + suffix_len] <= dst[cap - 1]` | safe, exact fit at a 159-char result |

There is no off-by-one. The equality boundary (`cap == suffix_len + 1`) gives
`head_cap = 0`, which yields the suffix alone and a NUL at `dst[cap-1]`.
`head`/`suffix` must be non-NULL. The only production caller passes a local
buffer and a string-literal macro, and `dst` never aliases `head`
(`s_standing_warning_reason` against the stack `standing_reason`). So the
`memcpy` calls cannot overlap.

The call site (`safety_ceiling_sync.c:642`) is behaviour-identical to the
inline code it replaced. The suffix moved into `CONFIG_DIVERGENCE_ARMED_SUFFIX`
with the same text, 68 chars. The seam test keeps its own literal copy
(`test_safety_ceiling_sync_divergence.c:635`), which pins the macro's text
independently. That is a useful pin, not drift.

**Test hardening.** The first fixer negtest of `drop_head_clamp` crashed
rather than asserting, because the overrun went past a tight `dst[160]`.
`712ae3fa0`/`e6f218d9b` give the test a 256-byte `'X'` guard tail and an
explicit "nothing written past cap" assertion at `guarded[cap]` and at the
last byte. `28dfaf18f` shortens the LCD test's head to 60 chars so that its
own `full[160]` cannot be overrun by a mutant. With those changes,
`drop_head_clamp` and the new `head_cap_off_by_one` probe fail at an
assertion line (table below).

## 3. LOW-B: LCD notice

`config_divergence_lcd_notice()` keys on `CONFIG_DIVERGENCE_ARMED_MARK`
(`"relay is ARMED"`). The helper always appends the suffix in full when
`cap > suffix_len`, so the mark always survives the head clamp. No first-diff
or other producer text contains that phrase, so no non-ARMED reason can
trigger the short form.

The LCD refresh (`ui_page_home_refresh.c:416`) passes a 192-byte `notice_buf`.
The non-ARMED form is at most 24 + 159 = 183 bytes, so there is no truncation.
The strip stays fixed-height and `LV_LABEL_LONG_DOT`, which honours the
no-scroll rule.

## 4. INFO-A/B/C

- **INFO-A:** the comment now reads 83 (`test_safety_ceiling_sync_divergence.c`). Verified.
- **INFO-B:** recorded as a correction in the ZPRECFX6 resolution. The clamp is now unreachable through the seam, because ±0 was its only route and the hash-collision text needs a real hash disagreement with all fields equal. It is unit-tested directly. Verified.
- **INFO-C:** still deferred to the owner (REVIEW_ZPRECFX5 INFO-3). Unchanged, as stated.

## 5. Findings

- **INFO-1 (doc):** the resolution text in `REVIEW_ZPRECFX6_2026-10-12.md`
  calls the LCD string 48 chars. It is 49
  (`"Pico config mismatch: disarm, retry commissioning"`). `"disarm"` ends at
  char 28, so the test's two limits (55 for `"disarm"`, 60 for the length) hold.
- **INFO-2 (comment):** the header comment on `config_identity_normalize_f32()`
  (`config_divergence.h:100-104`) still says it normalises "exactly the way
  safety_cfg_http.c's f32 wire encode does". It now also canonicalises -0 to
  +0, which the wire encode does not (`%.9g` prints `-0`). The comment in the
  `.c` file is correct. A related caveat: if the target FPU ever flushed
  subnormals to zero in compares, `out == 0.0f` would merge a subnormal with
  0. That is numerically irrelevant for kiln parameters, and the hash and the
  first-diff compare would still agree, because both go through the same
  normalised value and the same `!=`.
- **INFO-3 (test gap, not reachable):** the `cap == suffix_len` boundary of the
  helper is not tested. The mutant `tiny_branch_lt` (`cap <= suffix_len` to
  `cap < suffix_len`) is MISSED. At `cap == suffix_len` that mutant
  underflows `head_cap` and would overrun `dst`. Production `cap` is the
  compile-time 160 against a 68-char suffix, so this is not reachable. A
  one-line case with `cap == strlen(suffix)` and a guard tail would pin it.
- **INFO-4 (behaviour, accepted):** in ARMED mode the LCD shows only the
  action and drops the culprit field. That is the form REVIEW_ZPRECFX6 LOW-B
  recommended, and the full text stays on the web and readiness pages.

## Negative tests (run by this review)

`tools/negtest.ps1 -Preset kilnfw-host` (the full KilnFW host suite) with
`-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`
and `-Parallel 3`, at base `2393c6977`. Every mutant used a fresh `{OUT}`.

The unmutated baseline passed in 800 s. The run reported
`real_tree_unchanged: true` and `copies_removed: true`, and printed no
"REAL TREE CHANGED" line.

No mutant log contains `BUILD FAILURES` or a compiler error. Every CAUGHT
below is a compiled mutant whose test failed at an assertion line.

The first three rows are the three negtests the fixer ran before the
hardening commits, rerun here on the final tip.

| Mutation | Verdict | Evidence (`test_config_divergence.c`) |
|---|---|---|
| `no_zero_norm`: `out = 0.0f;` changed to `(void)0;` | CAUGHT | `:191` -0.0 normalises to the same BYTES as +0.0; `:196` hashes match; `:198` not a divergence |
| `head_cap_minus_40`: `head_cap` made 40 bytes smaller | CAUGHT | `:214` result exactly fills the buffer (159 chars) |
| `no_lcd_hint`: `if (0 && reason != NULL && strstr(...))` | CAUGHT | `:235` 'disarm' within the first 55 chars; `:236` short enough for one strip line |
| `drop_head_clamp`: `head_len = head_cap;` changed to `(void)head_cap;` | CAUGHT | `:212` nothing written past cap; `:214` |
| `head_cap_off_by_one` (new): `cap - suffix_len - 1u` changed to `cap - suffix_len` | CAUGHT | `:212` nothing written past cap; `:214` |
| `tiny_branch_lt` (new): `cap <= suffix_len` changed to `cap < suffix_len` | MISSED | Exit 0. The `cap == suffix_len` boundary is untested; production cap 160 against a 68-char suffix cannot reach it (INFO-3). |

`drop_head_clamp` and `head_cap_off_by_one` now fail at the canary
assertion, not with a crash. That confirms the `712ae3fa0`/`e6f218d9b`/`28dfaf18f`
hardening does what it claims.
