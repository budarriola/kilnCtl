# H1 fix: canonical (padding-free) ESP-half serializer for kiln packages

2026-09-14. Closes H1 from `docs/audits/kiln_profiles_robustness_2026-09-14.md`
(`5182c3a4`) against the implementation landed in
`docs/audits/kiln_profiles_implementation_2026-09-14.md` (`beb2c84e`). Read
both first; this document assumes their vocabulary (pkg_hash, pkg_schema,
`kiln_cfg_entry_t`, the plan's section 3.1.3 canonicalization rules).

## The defect, restated

`kiln_package_compute_hash()` (`firmware/KilnFW/App/drivers/persist/kiln_package.c`)
packs the Pico half field-by-field, but its ESP half used to be whatever
`zones_config_export_blob()` handed it -- a raw `memcpy(out, &s_zones.cfg,
sizeof(s_zones.cfg))`, compiler padding included. A package rebuilt
field-by-field from JSON (the future upload path) cannot reproduce another
board's padding bytes, so a package downloaded and immediately re-uploaded
unmodified would recompute a different `pkg_hash` and be refused.

## The fix

Two new functions, `zones_config_export_canonical()` /
`zones_config_import_canonical()`
(`firmware/KilnFW/App/drivers/persist/zones_config_accessors.{h,c}`), plus one
call-site change in `kiln_cfg_store.c`.

### The construction chosen, and why

The task's own constraint is the load-bearing one: the builder must be
**un-forgettable** the same way `kiln_package_capture_pico_half()`'s
CONFIG_PARAM_TABLE walk already is -- a hand-written field list that a future
field can silently bypass is exactly the `137dea1a` defect shape one level up
(see `project_reset_one_side_bug_class`/`project_split_module_missing_name_class`
in memory for two more instances of the same family).

Chosen: **option 1 from the task's own list** -- a single X-macro field-
descriptor table per struct (`ZONE_TIMING_PROFILE_FIELDS` / `ZONE_CFG_FIELDS`
/ `ZONES_CFG_FIELDS`, `zones_config_accessors.c`), each entry
`F(KIND, NAME, COUNT)`, expanded four times:

1. **`ZCFG_SHADOW_DECL`** -- declares a "shadow" struct with the identical
   field list, in identical order, as an ordinary (non-packed) C struct, so
   the compiler lays it out with the real struct's own alignment rules.
2. **A `_Static_assert` block** -- proves, per field (`offsetof` equality
   against the shadow struct) and for total size, that the shadow struct is
   byte-for-byte identical to the real one (`zone_timing_profile_t` /
   `zone_cfg_t` / `zones_cfg_t`).
3. **`ZCFG_EMIT`** -- the encoder, walking the table against the real struct.
4. **`ZCFG_PARSE_KIND`** -- the decoder, symmetric.

**Why option 3 (deriving from an existing runtime table) was rejected for
this struct specifically:** `zone_cfg_t`/`zones_cfg_t` have no runtime
enumeration mechanism analogous to `safety_cfg_store_param_count()`/
`_get_by_index()` -- they are plain C structs with no reflection. Building
one from scratch (a parallel `ZONE_CFG_PARAM_TABLE` walked at runtime) would
have been strictly more code than the X-macro for no additional safety
margin, since C structs cannot be introspected at runtime without exactly
this kind of hand-maintained table underneath anyway.

**Why option 2 (`_Static_assert(sizeof(...) == sum-of-field-sizes)`) alone
was rejected:** a bare size-sum assertion cannot distinguish "every field is
accounted for" from "some fields are missing but padding coincidentally
absorbed the difference," and more importantly it says nothing about
*order* -- a field moved to the wrong position would still sum correctly.
The shadow-struct comparison (this fix's actual mechanism) is strictly
stronger: it reconstructs the real struct's layout field-by-field from the
same table the encoder uses, so an error in the table shows up as a genuine
struct-layout mismatch, not merely an arithmetic coincidence.

### How forgetting a field fails, concretely

* **A field appended after the last table entry** (the codebase's own stated
  convention -- every `zone_cfg_t` growth comment in `zones_config_json.h`
  says "appended at the true tail"): `sizeof(zone_cfg_t)` grows;
  `sizeof(zone_cfg_shadow_t)` does not. The `_Static_assert(sizeof(zone_cfg_t)
  == sizeof(zone_cfg_shadow_t), ...)` fails, naming
  `zones_config_accessors.c` and `ZONE_CFG_FIELDS`.
* **A field inserted in the middle**: every already-tabled field after the
  insertion point shifts in the real struct but not in the shadow struct
  (which the compiler still lays out from the table's own, now-stale, field
  list). The first `_Static_assert(offsetof(zone_cfg_t, X) ==
  offsetof(zone_cfg_shadow_t, X), ...)` for any X after the insertion point
  fails.
* **Negative test performed**: a field (`NEGATIVE_TEST_UNREGISTERED_FIELD`,
  a `float`) was appended to the real `zone_cfg_t` in
  `zones_config_json.h` and the file was compiled stand-alone (MSVC, the
  same toolchain the host tests use). Result:

  ```
  zones_config_accessors.c(1665): error C2338: static assertion failed:
    'zones_cfg_t grew past ZONES_CONFIG_BLOB_MAX_SIZE -- widen the macro...'
  zones_config_accessors.c(1984): error C2338: static assertion failed:
    'zone_cfg_t's total size does not match ZONE_CFG_FIELDS -- a field was
    added or removed without updating that table (zones_config_accessors.c)'
  ```

  The **build fails**, not a test. The field was then restored by hand (never
  `git checkout --`, per this repo's standing rule) and a full rebuild from a
  clean private `-OutDir` (`C:\wt\h1build_final`) reconfirmed a clean build
  and all host tests green (see "Test results" below).

### The one gap this construction does not close

Documented in-line in `zones_config_accessors.c`'s own header comment on the
X-macro block, restated here per the task's "say plainly how yours fails
loudly" requirement: a field inserted whose size, in isolation, exactly fills
an existing alignment-padding gap **and** which sits as the struct's
textually **last** field (nothing after it to shift) would not be caught,
because the shadow struct would still match byte-for-byte. This does not
arise for any field type this codebase actually declares in these structs --
every gap is 0-3 bytes (the largest alignment requirement here is 4, for
`uint32_t`/`float`), and a gap can only exist *between* two fields, not after
the last one (trailing alignment is a property of the struct's own final
size, which the tail assert already covers) -- so the "last field, matching
gap size" combination is structurally impossible, not merely unlikely.

### Byte format

Declaration-order, no padding, little-endian: `uint8_t`/`uint16_t`/`uint32_t`
as their natural width; `float` as the IEEE-754 bit pattern with `-0.0f`
flushed to `+0.0f` first (`v == 0.0f` is true for both signs of zero, so
`if (v == 0.0f) v = 0.0f;` normalizes without any epsilon or numeric change);
`char[N]` fields copied verbatim (fixed length, no length prefix needed).
`zones_cfg_t::crc32` is excluded (see below).

`zones_config_import_canonical()` **zero-fills its destination first**,
unconditionally, before decoding a single field -- the task's explicit
"zero-fill on reconstruction" requirement -- so a caller's poisoned or
garbage-filled destination can never leak anything into the result. It also
refuses (destination left fully zeroed, nothing partially decoded) any `len`
that does not exactly match this build's own encoder output length for a
fully-populated struct, computed once via a throwaway scratch encode rather
than a second, hand-maintained length constant that could itself drift from
the field tables.

### Call-site change

`kiln_cfg_store.c`'s `populate_pico_half_and_hash()` now builds
`zones_config_export_canonical()`'s output and feeds *that* to
`kiln_package_compute_hash()` as the ESP half, instead of the raw exported
blob. The raw blob (`e->blob`) is completely unchanged -- it is still the
struct's on-flash form, still what `kiln_cfg_store_apply()` hands to
`zones_config_import_blob()`, which does its own independent CRC/version
checking. Only the **hash input** changed.

## `0xA5` negative test result

`test_canonical_negative_MEMCPY_INSTEAD_OF_CANONICAL_breaks_the_round_trip()`
(`firmware/KilnFW/App/test/test_zones_http.c`) builds two `zones_cfg_t`
instances holding the **identical logical config**, but poisoned with two
*different* byte patterns (`0xA5` and `0x5A`) before any field is set --
modeling two independent JSON reconstructions landing in unrelated,
non-zeroed heap allocations. Result:

* Hashing the **raw struct** (the pre-H1 approach) over the two instances
  gives **different** CRCs -- confirmed to actually reproduce the old defect
  (`old_style_crc_a != old_style_crc_b` passes), which is what makes this a
  non-vacuous negative test rather than one that would pass on a zeroed
  buffer regardless.
* Encoding both via `zones_config_export_canonical()` gives **byte-identical**
  output (`canon_a == canon_b`) -- the fix.

`test_canonical_import_zero_fills_even_when_buf_shorter_than_a_poisoned_struct()`
separately confirms a destination poisoned with `0xA5` is left **fully
zeroed** by a refused import (wrong length, or a NULL buffer) -- never
partially decoded, never left holding poison.

## Round-trip byte-identity result

`test_canonical_round_trip_byte_identical_and_hash_stable()` builds a fully-
populated `zones_cfg_t` (every field, both zone/timing-profile arrays,
including the deliberately awkward decimal float `123456.789f`, which
`%.4f` would truncate but binary IEEE-754 encoding — this format's actual
wire representation, never printf — carries exactly), serializes it,
deserializes into a struct pre-filled with `0xA5`, and serializes again.
Result: **byte-identical** (`memcmp() == 0`) across the full canonical
length, a stable CRC-32 over that byte stream across the round trip, and
spot-checks confirming the awkward float, a non-zero-index array element, and
a char-array field all survive exactly.

## `zones_cfg_t::crc32` — named follow-up, not fixed here

`zones_cfg_t::crc32` is itself a whole-struct CRC computed by
`zones_config_store.c` over a raw memcpy'd struct (field zeroed first) --
the same padding-exposure defect, one field over. **Not fixed by this pass**,
scope-limited deliberately:

* Today nothing reconstructs a `zones_cfg_t` from JSON and then checks that
  particular CRC. `zones_config_import_blob()` only ever sees a blob that
  was itself produced by a real board's own `memcpy` (never JSON-assembled),
  so its padding is internally consistent by construction.
* It becomes a live defect the moment upload/download (plan items 3/4/9,
  not yet implemented) reconstructs a `zones_cfg_t` and then validates
  `crc32` against a value computed on a *different* board with different
  padding, or persists a JSON-reconstructed struct straight to NVS without
  recomputing the CRC through the existing `memcpy`-based path.
* **Recommended fix, when that work starts**: compute `zones_cfg_t::crc32`
  over `zones_config_export_canonical()`'s output (this pass's new function)
  instead of the raw struct, the same substitution this pass made for
  `pkg_hash`. The canonical encoder already excludes `crc32` itself, so no
  self-reference problem. Filed here as a named item rather than left
  implicit, per the task's explicit requirement.

## Existing stored `pkg_hash` values

**Every stored `pkg_hash` predating this fix is now stale** -- it was
computed over the raw-blob ESP half, and this fix changes the hash input to
the canonical encoding. This is **not a data-loss event**: `kiln_cfg_entry_t`
itself (name, `blob[]`, `pico`) is completely unchanged; only the identity
hash recorded alongside it no longer matches what a fresh recompute would
produce.

Consequence, concretely: no download/upload feature exists yet (plan items
3/4/9), so nothing in the current codebase re-verifies a stored `pkg_hash`
against a fresh computation -- `kiln_cfg_store_apply()` reads
`pico_populated`/`pkg_hash` purely as a stored identity value, never
recomputing and comparing it against `kiln_package_compute_hash()` on load.
**No currently-shipped behavior changes for an existing saved slot.** The
hash becomes load-bearing only once the divergence check (plan item 7,
H6/H8 in the robustness doc) or upload validation starts recomputing and
comparing it -- and by the time that code exists, every slot on a board
running this firmware will have been saved (or re-saved) under the new
definition already, since `kiln_cfg_store_save_current()`/`_clone()`/`_apply()`
all funnel through `populate_pico_half_and_hash()`. A board that saved a
kiln config under old firmware and never re-saves it, then upgrades straight
to firmware carrying BOTH this fix AND a future hash-comparing consumer,
would see that one stale-hashed slot's `pkg_hash` read as merely "some
value" -- comparably harmless to a divergence check re-establishing its
baseline from the live config (plan section 3.1.2's re-establish path
already handles "no trustworthy prior hash" for exactly this kind of
transition) rather than a hard failure. No migration step is required or
added; this is called out explicitly rather than left to be discovered.

`KILN_CFG_STORE_VERSION` is **not bumped** (still 3) -- `kiln_cfg_entry_t`'s
on-flash layout is unchanged; only how one of its fields (`pkg_hash`) is
computed at save time changed, which is a code-behavior change, not a
schema change. `ZONES_CFG_VERSION` is also unchanged (still 26) -- nothing
about `zones_cfg_t`'s own layout or migration chain changed.

## Test results

* `test_zones_http.c` (`kilnctl_host_tests_zones.exe`): **1922/1922 passed**,
  including five new tests: round-trip byte-identity + stable CRC, zero-fill-
  on-refusal (three sub-cases: short, long, NULL), the `0xA5`-poisoned
  negative test proving both the old defect and the new fix, `-0.0f`/`+0.0f`
  identical encoding, and the canonical-size upper bound.
* Main host-test executable (`kilnctl_host_tests.exe`, includes
  `kiln_cfg_store.c`/`kiln_package.c` via `test_kiln_cfg_store.c`):
  **7618/7618 passed** -- confirms `populate_pico_half_and_hash()`'s new
  canonical-hash call site did not regress any existing `kiln_cfg_store`
  behavior (a new `zones_config_export_canonical()` stub was added to that
  test file's existing zones-config stub surface, matching its established
  convention for `zones_config_export_blob()`).
* Full `build_host_tests.ps1` run (private `-OutDir`, clean rebuild after the
  negative test's hand-restore): **every executable's own reported tally
  passed** (94 checks total across `tools/run_all_checks.ps1`, see below;
  every individual host-test executable's own PASS/FAIL tally in the build
  log shows 0 failures). The script's own final line reported a pre-existing,
  unrelated harness count mismatch ("44 executables built but 42 expected")
  that predates this change (confirmed via `git status` showing
  `build_host_tests.ps1` untouched by this pass) -- attributed here, not
  fixed, since it is outside this task's owned files and looks like fallout
  from a concurrent session adding an executable without updating the
  expected count.
* `tools/run_all_checks.ps1`: **94 passed, 0 skipped, 0 failed**, including
  `tools/check_doc_hash_citations.ps1`.
* ESP-IDF target build (`idf.py -C firmware/KilnFW build`) was attempted per
  this task's own build-verification requirement but could not run: the
  configured Python venv (`C:\Espressif\tools\python\v6.0.2\venv`) does not
  match the one the existing `build/` directory was configured with
  (`C:\Espressif\python_env\idf6.0_py3.14_env`), a pre-existing environment
  mismatch unrelated to any file this pass touched (confirmed no
  ESP-IDF/build-config files are among this pass's changes). Not resolved
  here to avoid an `idf.py fullclean` on a `build/` directory a concurrent
  session may depend on, per this repo's shared-tree caution
  (`project_concurrent_sessions_git_race`). The host-test suite (which
  compiles and links every touched `.c` file, including
  `zones_config_accessors.c`/`kiln_cfg_store.c`/`kiln_package.c`, under a
  real C11 toolchain) is the verification of record for this change;
  `build_kilnfw`/a clean-venv ESP-IDF build remains open for whoever next
  has a working `build/` directory.

## Files changed

* `firmware/KilnFW/App/drivers/persist/zones_config_accessors.h` -- new
  `zones_config_canonical_max_size()`/`_export_canonical()`/`_import_canonical()`
  declarations (type-erased `void *` for `zones_cfg_t`, matching this
  header's existing `zones_config_export_blob()` convention, since the
  header deliberately does not `#include` `zones_config_json.h`).
* `firmware/KilnFW/App/drivers/persist/zones_config_accessors.c` -- the
  X-macro field tables, shadow structs, completeness `_Static_assert`s,
  byte-level primitives, encoders/decoders, and the three public functions.
* `firmware/KilnFW/App/drivers/persist/kiln_cfg_store.c` -- `#include
  "zones_config_json.h"` (needed for `zones_cfg_t` at this call site only);
  `populate_pico_half_and_hash()` takes a `const zones_cfg_t *` and builds
  the canonical encoding before calling `kiln_package_compute_hash()`;
  `kiln_cfg_store_save_current()` reconstructs a real `zones_cfg_t` from the
  raw-exported scratch bytes (a properly-aligned struct copy, never a cast
  of the `uint8_t[]` scratch buffer) to pass in.
* `firmware/KilnFW/App/drivers/persist/kiln_package.h` -- updated the
  `KNOWN LIMITATION` comment on `kiln_package_compute_hash()` to record that
  H1 is fixed and where the new canonical caller lives; the function's own
  behavior (hash whatever bytes it is handed) is unchanged.
* `firmware/KilnFW/App/test/test_zones_http.c` -- five new tests (see "Test
  results" above).
* `firmware/KilnFW/App/test/test_kiln_cfg_store.c` -- a
  `zones_config_export_canonical()`/`_canonical_max_size()` stub pair,
  matching the file's existing `zones_config_export_blob()` stub convention.
