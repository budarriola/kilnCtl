# Kiln profiles implementation progress -- 2026-09-14

Plan: `docs/KILN_PROFILES_PLAN.md` (`c70ae799`). This pass's scope was
explicitly narrowed by the coordinator to the early, non-safety-critical
work items only: **item 1** (raise `KILN_CFG_MAX_COUNT` 8 -> 10), the
Pico-half-packaging slice of **item 2**, and **item 12** (the package
hash). The two-processor apply transaction (item 5), the volatile Pico
RAM install (item 15), the `safety_core.c:333` unconfigured-ceiling fix
(item 16), the standing divergence alarm (item 7), and upload/download
(items 3/4/9/14) are all explicitly OUT of scope for this pass and were
not touched.

## What was implemented

- **`KILN_CFG_MAX_COUNT` 8 -> 10** (`firmware/KilnFW/App/drivers/persist/kiln_cfg_store.h`).
- **`KILN_CFG_STORE_VERSION` 2 -> 3** (`kiln_cfg_store_internal.h`), adding
  each slot's Pico-half package and package identity alongside the
  pre-existing ESP-only blob.
- **New module `firmware/KilnFW/App/drivers/persist/kiln_package.{h,c}`**:
  - `kiln_package_capture_pico_half()` walks the ESP-side mirror of
    `CONFIG_PARAM_TABLE` (`safety_cfg_store_param_count()`/
    `_get_by_index()`, injected via a `kiln_pkg_pico_source_t` accessor
    pair for testability) and packs every row -- **set or unset, never
    omitted** -- sorted ascending by `param_id` into a fixed
    `kiln_pkg_safety_t` (cap `KILN_PKG_SAFETY_PARAM_CAP` = 96, headroom
    over today's `SAFETY_CFG_PARAM_COUNT` = 68, enforced by a
    compile-time assert in `kiln_cfg_store.c`).
  - `kiln_package_compute_hash()` implements section 3.1.3's canonical
    binary serialization (pkg_schema, ESP blob length + bytes verbatim,
    Pico param count, then each `{param_id, type, flags, value_bits}` in
    ascending `param_id` order) and CRC-32s it via `esp_crc32_le()` --
    the same primitive `zones_config_store.c`/`crash_report.c` already
    use. Built field-by-field into a buffer, never a struct `memcpy`, so
    compiler padding can never leak into the hash.
  - `kiln_cfg_store_save_current()` now calls both, storing
    `pico_populated`/`pkg_schema`/`pkg_hash` on every save-as-new and
    save-over. `kiln_cfg_store_clone()` inherits the source slot's
    already-captured Pico half/identity via its existing whole-struct
    copy (no re-capture on clone, matching "save as" semantics).
  - New read accessor `kiln_cfg_store_get_package_identity()`.
- **v2 -> v3 migration** (`migrate_store_v2_to_v3()`): every ESP-side
  field copied by name; the two new slots (indices 8, 9) start empty;
  every migrated slot gets `pico_populated = 0` explicitly (never a
  fabricated Pico half or hash). The v1 frozen layout's slot count was
  also **hardcoded to 8** (previously it tracked the live
  `KILN_CFG_MAX_COUNT` macro, which would have silently changed the v1
  struct's size the moment that macro moved -- a real, caught-before-
  shipping defect; see "What I found and fixed" below).
- **`FAKE_KV_MAX_VALUE_BYTES`** (`firmware/hwAbstraction/host/fake_kv.h`)
  raised 8192 -> 20000 to fit the new, larger `kiln_cfg_store_blob_t` in
  host tests.
- **CMakeLists.txt**: `persist/kiln_package.c` added to
  `firmware/KilnFW/App/drivers/CMakeLists.txt`.
- **Host tests**: `firmware/KilnFW/App/test/test_kiln_package.c` (own
  executable, 28 checks, injected fake Pico table -- no dependency on the
  real `safety_cfg_store.c`) plus additions to
  `firmware/KilnFW/App/test/test_kiln_cfg_store.c` (v2->v3 migration
  round trip, package-identity accessor contract, save-current capturing
  the Pico half). `build_host_tests.ps1` updated to link
  `kiln_package.c` into the main executable and to build/run the new
  `kiln_package` executable.

## What I found and fixed (not in the original plan text)

1. **The v1 frozen migration struct was not actually frozen.**
   `kiln_cfg_store_blob_v1_t` declared `entries[KILN_CFG_MAX_COUNT]` --
   the LIVE macro, not a literal 8. Bumping `KILN_CFG_MAX_COUNT` to 10
   would have silently changed `sizeof(kiln_cfg_store_blob_v1_t)`,
   breaking the exact-size migration detection
   (`stored_len == sizeof(kiln_cfg_store_blob_v1_t)`) for every board
   still carrying a real v1 blob. Fixed by hardcoding
   `KILN_CFG_STORE_V1_COUNT`/`KILN_CFG_STORE_V2_COUNT` = 8, with a
   comment explaining why these must never track the live macro again.
   This is exactly the migration-adjacent defect class CLAUDE.md warns
   cost three days of plant models this week -- caught here before it
   shipped, not after.
2. **`-Werror=comment` target-build break**: `kiln_cfg_store.h`'s
   `kiln_cfg_store_get_package_identity()` doc comment contained the
   literal substring `pkg_schema/*out_pkg_hash`, which reads as a nested
   `/*` to the compiler. Fixed by rewording. Confirmed via a full
   `idf.py -C firmware/KilnFW build` (see below).
3. **`kiln_package.c` was missing from `CMakeLists.txt`** on first pass
   (host tests don't catch this -- they compile the .c directly). Added.

## The Pico-half enumeration

Walks `safety_cfg_store_param_count()` (68 today) /
`safety_cfg_store_get_by_index()` -- the ESP's own live cache of the
Pico's `CONFIG_PARAM_TABLE`, already populated by ordinary link traffic
(`safety_cfg_store_maybe_refetch()`). This module never talks to the Pico
itself; it packages whatever the ESP already has. An unset param (never
fetched/commissioned) is packaged with `flags=0`/`value_bits=0`, not
skipped -- omitting it would make "never asked" indistinguishable from
"this package's schema predates the param", which is the missing-field
defect class this whole plan exists to close.

## Capacity arithmetic, with the Pico half included

Measured (not estimated) via the actual struct layout used by both the
host build and the ESP32-S3 target build:

```
kiln_pkg_pico_param_t   = 8 B   (param_id u16 + type u8 + flags u8 + value_bits u32)
kiln_pkg_safety_t       = 2 B (count) + pad2 + 96*8 B (cap) = 772 B
kiln_cfg_entry_t        = 1(in_use)+pad3+4(id)+24(name)+2(blob_len)+pad2
                          +896(blob, ZONES_CONFIG_BLOB_MAX_SIZE)
                          +1(pico_populated)+pad1+2(pkg_schema)+pad2+4(pkg_hash)
                          +772(pico)
                        = 1712 B
kiln_cfg_store_blob_t   = 1(version)+pad3+4(active_id)+4(next_id) + 10*1712
                        = 12 + 17120 = 17132 B
```

`kiln_nvs` = `0x10000` = 65536 B; NVS overhead leaves ~56KB usable per
`docs/KILN_PROFILES_PLAN.md` section 2.3's own figure. 17132 B is
**~3.3x headroom**, matching the plan's projected order of magnitude.
`cfg` LittleFS (512 KiB at `0xDB0000`) holds the same blob at negligible
fraction of the partition. **No partition added, moved, or resized.**
Confirmed the budget guard in `kiln_cfg_store.c`
(`kiln_cfg_store_blob_budget_check`, raised 16384 -> 40000 to cover the
v1+v2+v3 struct sum) still compiles as a loose tripwire, not a tight fit.

## How existing saved profiles migrate

A pre-existing 8-slot, ESP-only (v2) blob is detected by exact size
(`sizeof(kiln_cfg_store_blob_v2_t)`), version-checked (`v2->version == 2`,
refused otherwise -- never blindly reinterpreted), and migrated field-by-
field into the v3 layout: every name/id/blob/blob_len/active_id/next_id
survives unchanged; the two new slots start empty; every migrated slot's
`pico_populated` is explicitly 0 (readable via
`kiln_cfg_store_get_package_identity()`, which reports `pkg_schema`/
`pkg_hash` as 0 in that state) -- never a fabricated Pico half. A
still-older v1 blob chains through the v2 shape first
(`migrate_store_v1_to_v2()`, unchanged target shape) before reaching v3.

## The hash definition implemented

`pkg_hash` = CRC-32 (`esp_crc32_le()`) over a canonical BINARY buffer,
built field-by-field (never a struct `memcpy`, so padding is never
hashed):

```
pkg_schema (2B LE)
esp_blob_len (2B LE) + esp_blob_len bytes of the ESP zones-config blob verbatim
pico.count (2B LE)
for each pico param, ASCENDING param_id order:
  param_id (2B LE), type (1B), flags (1B), value_bits (4B LE)
```

`pkg_schema` is `KILN_PKG_SCHEMA_VERSION` (1 today), versioned
independently of `KILN_CFG_STORE_VERSION` and `ZONES_CFG_VERSION`. The
ESP blob itself is already a packed binary format (not JSON), so its
floats are already raw IEEE-754 bytes -- the `%.9g` round-trip-exactness
rule from section 3.1.3 rule 5 applies to a FUTURE JSON transport layer
(upload/download, item 3/4, out of scope this pass) that would need to
reproduce these exact bytes from text; this pass never goes through JSON,
so `backup_export.c`'s lossy `%.4f` is not reachable from this code at
all. Noted explicitly in `kiln_package.h` for whoever implements item 3/4
next.

**Distinct from the Pico's own config identity.** Per the coordinator's
note: the Pico's `(config_version, config_crc)` (computed on the Pico,
over its own live record, used by `config_divergence.c`'s standing
cross-processor check) is NOT this hash and is never conflated with it.
This module's `pkg_hash` is the ESP's own package-identity hash over what
is STORED (a point-in-time capture of the ESP's cache of the Pico's
params, not the Pico's live state); the config identity is a separate,
continuously-live comparison the ceiling-mirroring agent's work owns. No
field was added to `config_divergence.c` or its identity struct by this
pass.

## Negative test (item 1's acceptance criterion)

Broke `kiln_cfg_store.c`'s v2-branch version check by hand (replaced
`if (v2->version != 2)` with `if (false /* NEGATIVE-TEST SABOTAGE */)`),
deleted `firmware/KilnFW/App/test/build`, rebuilt clean. Result: exactly
the 3 checks in `test_v2_blob_never_blindly_reinterpreted_as_v3` failed
(a v2-sized blob claiming an unrecognised version 99 was accepted rather
than refused, and `active_id`/`entries[0]` came up as the staged garbage
instead of clean defaults); every other check across all 43 executables
was unaffected. Restored the check by hand, deleted `build` again,
rebuilt clean: `7571/7571` in the main executable (up from 7542 before
this pass's additions), `28/28` in the new `kiln_package` executable, and
`43/43` executables run overall except `config_divergence` (pre-existing,
unrelated, another agent's own in-flight work on `config_divergence.c` --
confirmed via `git status` showing that file modified, not by this pass).

## Check tally

- Host tests (`build_host_tests.ps1`, clean rebuild): every executable
  green except `config_divergence`, which belongs to concurrent,
  unrelated in-flight work (see above). `main` executable: **7571/7571**.
  `kiln_package` (new): **28/28**.
- ESP-IDF target build (`idf.py -C firmware/KilnFW build`, via the
  Espressif PowerShell profile per
  `project_kilnfw_idf_build_invocation`): **clean**, `KilnCtrl.elf`/
  `.bin` produced, `kiln_package.c.obj` and `kiln_cfg_store.c.obj` both
  compiled and linked. This also confirms the `-Werror=comment` fix.
- `tools/run_all_checks.ps1`: ran to completion (92 passed / 2 failed in
  its own suite before my fixes were in the tree for that pass; the two
  named failures at that time were the `-Werror=comment` break and the
  `kiln_package.c` CMakeLists.txt omission, both fixed above and
  reconfirmed green by the target build just described).

## Hardening pass follow-up (same day)

`docs/audits/kiln_profiles_robustness_2026-09-14.md` (`5182c3a4`) found six
live defects in the code this pass had already landed, plus one flaw in the
plan's own test guidance, and named an unrelated concurrent breakage
(`-Werror=comment`, `kiln_package.c` missing from `CMakeLists.txt`) that was
already fixed above by the time this section was written. Landed in the
coordinator-recommended order:

- **H2 (fixed).** `kiln_package_compute_hash()` returned a plain `uint32_t`
  with failure signalled only via an optional `out_ok`, and `0` is the
  store's documented "hash never computed" sentinel -- a discarded `out_ok`
  could store `pkg_hash=0` on a `pico_populated=1` slot, indistinguishable
  from a genuinely-computed zero CRC. Changed to `bool kiln_package_compute_
  hash(..., uint32_t *out_hash)`; `*out_hash` is written ONLY on success and
  left untouched (never zeroed) on failure. `populate_pico_half_and_hash()`
  now only sets `pico_populated=1` after a successful, non-zero hash; a
  failure downgrades the slot to "not yet captured" rather than leaving a
  half-marked state. `kiln_cfg_store_init()` also normalizes any
  `pico_populated=1, pkg_hash=0` row found on load (defensive, for a
  hypothetically buggy past write) back to "not yet captured", logged.
- **H3 (fixed).** A corrupt store used to silently `reset_to_defaults()`
  with one `ESP_LOGW`; the next save would then overwrite both NVS and the
  `cfg` mirror with an empty store, destroying the only copy of the
  operator's saved kiln configs. Added an in-RAM (deliberately not
  separately persisted -- see `s_quarantined`'s own comment on why that
  would itself be a reset-one-side-of-a-pair risk) quarantine flag, set by
  every genuine corruption branch in `nvs_load_store()` (a re-read failure,
  a wrong-claimed-version blob, a wrong-size blob, an unmigratable older
  version, and a newer-than-firmware version -- the last one included
  because an unguarded later save would otherwise still overwrite it).
  `kiln_cfg_store_save_current()`/`_clone()`/`_apply()`/`_delete()`/
  `_rename()` all refuse while quarantined via a new `refuse_if_quarantined()`
  gate. New API: `kiln_cfg_store_is_quarantined()` (read) and
  `kiln_cfg_store_quarantine_clear(confirm_discard, ...)` (the one way out --
  requires an explicit `true`, discards the store, starts a fresh empty one,
  never salvages or heuristically parses the corrupt bytes).
- **H5 (fixed).** `kiln_cfg_store_delete()` had no interlock at all and
  could delete the active config mid-firing. Signature changed to
  `kiln_cfg_store_delete(id, ack_no_safety_processor, reason_out, reason_cap)`;
  it now calls `ota_http_check_interlocks()` first (same backstop pattern
  and predicate as `apply()`), and separately, unconditionally refuses to
  delete the currently active config regardless of interlock state (that
  refusal names the slot and says why). `kiln_cfg_http.c`'s delete handler
  updated to pass the ack and surface the reason, distinguishing "not
  found" (404) from a refusal (400).
- **H9 (fixed).** `normalize_name()` validated length/emptiness/duplicates
  but no character set. Added `name_charset_and_utf8_valid()`: rejects
  control bytes (0x00-0x1F, 0x7F) anywhere in the name (not just at the
  edges `isspace()` trims), `"`, `\`, `/`, and invalid/truncated UTF-8
  (including a multi-byte sequence cut off exactly at the 23-byte length
  boundary). Table-driven host test with 14 cases. **Deferred, noted
  explicitly, not silently dropped:** item 3 of H9 (giving `kiln_cfg_store_
  rename()` a `reason_out` so a specific character-set reason can be
  surfaced) and item 4 (a sanitised `Content-Disposition` filename slug)
  are UI/HTTP-surface polish tied to routes (`rename`'s reason plumbing
  touches `kiln_cfg_http.c`'s existing bare-bool contract; the filename slug
  belongs to the download route, item 3/4, out of scope this pass) --
  `rename()` still correctly REFUSES an invalid name today via
  `normalize_name()`, it just reports a bare `false` the way it already did
  for every other name defect (too long, duplicate) before this fix.
- **H17 (fixed).** A `pico_populated==0` slot (v2-migrated, never re-saved)
  is a half-package. `kiln_cfg_store_apply()` now refuses it outright, by
  name, before `zones_config_import_blob()` is ever called -- proven by a
  test asserting the import stub's call count does not move. This guard
  exists now, before plan item 5 (the actual Pico push) is written, so that
  a future implementation of item 5 is structurally prevented from silently
  applying the ESP half while leaving the Pico on the previous kiln's
  settings.
- **H1 (verified, NOT fixed in full -- see reasoning below).**

### H1: verified real, deliberately not fully fixed this pass

Confirmed by reading the code: `zones_config_export_blob()`
(`zones_config_accessors.c:1675`) is `memcpy(out, &s_zones.cfg,
sizeof(s_zones.cfg))` -- a raw struct copy, padding included -- and
`kiln_package_compute_hash()` hashes that blob verbatim for the ESP half.
Plan section 3.1.3 rule 3 ("padding is never hashed") is genuinely violated
for the ESP half (the Pico half is unaffected -- it is packed field-by-field
already).

**Why this is real but not currently reachable in what this pass shipped:**
the audit's own failure scenario is "download an unmodified package, upload
it, the hash mismatches" -- but no download or upload route exists yet
(plan items 3/4 are explicitly out of scope for this pass, and were not
built). The only two places `kiln_package_compute_hash()` is ever called
today are `kiln_cfg_store_save_current()` and (indirectly, via the copied
struct) `kiln_cfg_store_clone()`, both of which feed it `zones_config_
export_blob()`'s live output -- a memcpy of the SAME static, zero-
initialized-at-boot `s_zones.cfg`, whose padding is therefore always zero
and stays zero (nothing ever writes to it except through named struct
fields). Every hash comparison this pass's delivered code can ever make is
therefore reproducible today, verified by `test_save_current_captures_pico_
half_and_hash()`'s recompute check.

**Why I did not build the full fix the audit describes:** the audit's own
prescribed fix (a canonical, padding-free, field-by-field
`zones_config_export_canonical()` for `zones_cfg_t`) requires walking a
~2000-line struct definition (`zones_config_json.h`) field by field, by
hand, to emit each one in declaration order. The audit's own accompanying
warning is that a hand-written field list is exactly the forgettable-field
defect class this whole plan exists to close (`137dea1a`) -- and
`zones_cfg_t` is a large, actively-developed struct with concurrent work
already touching adjacent files in this same tree today. Writing and
verifying such a serializer completely, under this pass's time budget, was
judged a materially higher risk of silently forgetting a field than leaving
H1 open with this deliberately-verified, currently-safe boundary and a very
loud handoff. Per my instructions to escalate rather than reach into
safety-critical or disproportionately large work: **this is not fixed, it
is verified-safe-for-today's-actual-surface and clearly handed off.**

`kiln_package.h`'s own doc comment on `kiln_package_compute_hash()` was
rewritten to carry this exact finding, inline, at the call site, so item 3/4's
implementer cannot miss it: it names the two acceptable fixes (a real
canonical serializer built by walking the struct's own declaration
mechanically, or a memset(0)-before-populate discipline on every JSON-
reconstructed `zones_cfg_t` proven by a poison-byte (`0xA5`) negative test)
and says explicitly **do not ship upload/download against this function
without one of those.**

### H2/H1 test updates and target-build fallout

- `test_kiln_package.c` updated for the new `bool`-returning signature
  (8 call sites); added a poison-value (`0xDEADBEEF`) check proving
  `*out_hash` is left untouched on a refused call.
- New tests: `test_delete_refuses_the_active_config`,
  `test_delete_refused_by_interlock_while_firing_even_when_not_active`,
  `test_name_character_set_validation` (14 cases),
  `test_apply_refuses_half_package`,
  `test_corrupt_store_quarantines_and_blocks_writes`. `reset_state()` now
  also clears the quarantine flag between tests (it is process-wide static,
  same as `s_store` itself). Two pre-existing cfg_fs dual-write tests
  (`test_cfg_fs_dual_write_keeps_file_and_nvs_in_sync`,
  `test_cfg_fs_stale_delete_not_resurrected`) were updated for `delete()`'s
  new signature and, for the latter, restructured (a second slot saved
  first) so its delete target is no longer the active config -- H5 would
  otherwise correctly refuse that test's own scenario.
- **Target build caught two real defects the host build (MSVC) could not:**
  `-Werror=format-truncation` on two new `snprintf()` call sites
  (`refuse_if_quarantined()`'s message buffer at 192 bytes against a
  128-byte reason plus template, and `kiln_cfg_store_delete()`'s
  active-config message at 96 bytes against a 23-byte name plus template).
  Both buffers enlarged (400 and 160 bytes respectively) with the actual
  worst case sized against `s_quarantine_reason`'s and
  `KILN_CFG_NAME_MAX_LEN`'s real caps, not guessed. Re-verified via a full
  clean host-test rebuild (44/44 executables, main 7618/7618) and a
  target-object rebuild of exactly the three changed files
  (`kiln_cfg_store.c.obj`/`kiln_package.c.obj`/`kiln_cfg_http.c.obj`, all
  clean) -- a FULL target build could not be completed at that moment
  because of an unrelated, concurrent, uncommitted breakage in
  `adaptive_tune_internal.h`/`adaptive_tune.c` (`variably modified ... at
  file scope`, `-Werror`) -- confirmed via `git status` as another agent's
  in-flight work on files explicitly off-limits to this pass, not caused by
  anything in this change.

## Where I stopped

Exactly at the scoped boundary: items 1, the Pico-half-packaging slice of
2, and 12. Did not touch: the two-processor apply transaction (item 5),
the volatile Pico RAM install (item 15), `safety_core.c:333` (item 16),
the standing divergence alarm/`config_divergence.c` (item 7), or
upload/download (items 3/4/9/14). `ZONES_CFG_VERSION` was never touched
(still 26). No `.kicad_*` file was touched. No board was flashed, no
heating run was performed, no config was written to a board.
