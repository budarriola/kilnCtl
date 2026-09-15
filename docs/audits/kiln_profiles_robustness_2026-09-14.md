# Hardening analysis: kiln profiles — "make the config sharing and changing robust"

2026-09-14. Companion to `docs/KILN_PROFILES_PLAN.md` (`c70ae799`), which owns
the feature design and its section 8 work items. **This document does not edit
that plan.** It is a failure-mode analysis producing *additional, numbered*
work items (H1…H17) the plan's implementer folds into section 8, plus an
explicit list of things I would **not** build.

Read the plan first. Everything here assumes its section 1a storage model (one
flash home on the ESP, both processors running from RAM), its section 3
divergence invariant, and its section 6 missing-field rule.

Scope note: items 1, 2 and 12 of the plan have already landed
(`KILN_CFG_STORE_VERSION 3`, `kiln_package.c/.h`, `kiln_package_compute_hash()`),
so several findings below are about code that **exists now**, not about code
that might be written. Those are marked **LIVE DEFECT**.

---

## 0. Summary — findings ranked by actual risk

| # | finding | risk | status |
|---|---|---|---|
| **H1** | `zones_config_export_blob()` is a raw struct `memcpy`, so `pkg_hash` hashes compiler padding. A JSON round trip cannot reproduce it. Download → upload of an unmodified package will mismatch its own hash. | **HIGH — ships the feature broken and kills the divergence check** | **FIXED 2026-09-14** — see `docs/audits/kiln_package_canonical_serializer_2026-09-14.md`. `zones_config_export_canonical()`/`_import_canonical()` (X-macro field tables + compile-time shadow-struct completeness proof) replace the raw blob as `kiln_package_compute_hash()`'s ESP-half input; round-trip byte-identity and the `0xA5`-poisoned negative test both pass. `zones_cfg_t::crc32` has the same defect and is NOT fixed by this pass — named follow-up in that doc. |
| **H2** | `kiln_package_compute_hash()` returns `0` on three failure paths, and `0` is the store's documented "hash never computed" sentinel. A failed hash is indistinguishable from an absent one. | **HIGH — an unverified config can be certified** | LIVE DEFECT |
| **H3** | A corrupt store silently `reset_to_defaults()` — all 10 slots gone, one `ESP_LOGW`, no operator-visible signal, and the next save overwrites the `cfg` mirror too. The only copy of a kiln's identity is destroyed without anyone being told. | **HIGH — this is the "worse than no feature" case** | LIVE DEFECT |
| **H4** | Auto-save's dirty-flag fan-in is the plan's largest new surface for the `137dea1a` defect class, and one live writer path (`zones_config_import_blob()` via apply) is itself a whole-struct write. Four paths enumerated in §10. | **HIGH** | plan item 13 |
| **H5** | `kiln_cfg_store_delete()` has **no interlock** and will delete the active config mid-firing, clearing `active_id` and, under the new model, destroying the recorded `(config_version, config_crc)` reference the divergence check compares against. | **HIGH** | LIVE DEFECT |
| **H6** | No serialisation anywhere in `kiln_cfg_store`. The plan adds a debounced auto-save **task** and an apply **worker task** to a module written for a single httpd worker. Two writers, one `s_store`, one `nvs_save_store()`. | **HIGH** | plan items 5, 13 |
| **H7** | Version skew: a *forward*-compatible package has no partial-apply path today, but the store's "treat a stale-version file as absent" discipline means an OTA rollback across `KILN_CFG_STORE_VERSION` silently empties the picker. | MEDIUM | LIVE DEFECT |
| **H8** | Swap/firing-start ordering is specified in one direction only. A firing requested during a swap is unspecified. | MEDIUM | plan item 5 |
| **H9** | `normalize_name()` validates length and emptiness but **not the character set** — control bytes, `"`, `\`, and invalid UTF-8 all reach flash, the JSON export, the filename, and the LCD. | MEDIUM | LIVE DEFECT |
| **H10** | The pending-swap marker has no integrity field of its own, so the plan's five-case recovery has an unhandled sixth case: a marker that is itself corrupt. | MEDIUM | plan item 8 |
| **H11** | Write-rate bound is stated but not *enforced*. Nothing fails if a future writer raises the dirty flag per tick. | LOW-MEDIUM | plan item 13 |
| **H12** | Store-full and `cfg`-full behave correctly, but there is no ceiling on *upload body size* before the heap allocation. | LOW-MEDIUM | plan item 4 |
| **H13** | Hardware provenance is recorded (`source_board`) but not the hardware *shape*. Compatibility is checked against the live controller only, so a refusal cannot say what the package was built for. | LOW-MEDIUM | plan item 14 |
| **H14** | `-0.0` in a `zones_cfg_t` float hashes differently from `+0.0` on the memcpy path, and the plan's flush-to-`+0.0` rule is unimplementable while H1 stands. Fixing H1 fixes this. | LOW | subsumed by H1 |
| **H15** | Rename does not rehash, and it should not — but nothing says so, and an implementer will "fix" it. | LOW | doc-only |
| **H16** | A Pico that reboots mid-swap is handled; a Pico that reboots *between* step 7's read-back and step 12's commit is not distinguished from one that never received the push. | LOW | plan item 5 |
| **H17** | `pico_populated == 0` slots (v2-migrated, never re-saved) are half-packages by construction. Applying one is a silent partial swap. | MEDIUM | LIVE DEFECT |

**Would not build (§12):** package signing; a store-repair wizard; a
multi-writer lock protocol richer than one mutex; CRDT/merge semantics for
concurrent edits; queued swaps; an LCD picker; automatic recovery of a corrupt
slot by guessing; per-half hashes; an epsilon float comparison; a partial
forward-compatible apply.

**Total additional work implied: ~1,300–1,700 lines including tests, across 17
items**, of which H1, H2, H3, H5, H9 and H17 are small fixes to code that is
already on `main` and should land before any further plan item.

---

## 1. Recovery from a corrupt store (H3) — the "worse than no feature" case

### The failure

`nvs_load_store()` (`firmware/KilnFW/App/drivers/persist/kiln_cfg_store.c:277`)
has five rejection branches — partition init failure, key absent, unreadable,
wrong size for any known version, a v1/v2-sized blob whose claimed version
disagrees. **Every one of them ends in `reset_to_defaults()` and a single
`ESP_LOGW`.** The store then presents itself as a legitimately empty store: ten
free slots, `active_id = KILN_CFG_NO_ACTIVE_ID`, `next_id = 1`.

Three things make this worse than it looks:

1. **The operator is never told.** Nothing in `GET /api/kiln_configs`
   distinguishes "you have never saved a kiln config" from "your ten kiln
   configs were unreadable this boot". The picker is simply empty.
2. **The evidence is then destroyed.** The next `nvs_save_store()` — which
   auto-save (plan item 13) will trigger within 5 seconds of any config edit —
   writes the empty store over the NVS key *and* bumps `s_kiln_cfg_rev`, so the
   `cfg` LittleFS mirror is resynced to the empty store too. Both copies gone,
   in one debounce window, with no operator action.
3. **The live config is unaffected**, which is the one piece of good news and
   the reason this is recoverable *if* the operator is told in time: the active
   `zones_cfg_t` in `kiln_nvs` and the Pico's RAM record are untouched. The
   kiln still fires correctly. What is lost is the ability to go back to any
   *other* kiln.

Under the plan's model the loss is strictly larger than today's, because the
slot is now the only home for the Pico's 68 params — nothing else on the board
holds another kiln's `abs_max_temp_c`, S8 rate guard, or CT calibration.

### What boot must do instead

The controller must never be bricked by this, and the plan's own instinct —
"keep whatever already loaded" — is right. The fix is not a recovery algorithm;
it is **making the loss loud and making the destruction stop**.

**H3 (work item). Corrupt-store quarantine.**

1. `nvs_load_store()` gains an out-parameter distinguishing three outcomes:
   `LOADED`, `NEVER_SAVED` (key absent — today's benign case), and
   `UNREADABLE` (every other rejection branch), carrying the reason string and
   the observed/claimed version and length.
2. On `UNREADABLE`, the module sets a **persisted** `store_quarantined` flag in
   a *separate* NVS key (not inside the blob — the blob is the thing that is
   broken) recording the reason, and **refuses every write to the blob key**
   until the operator explicitly clears it. `save`, `clone`, `rename`,
   `delete`, upload and auto-save all return
   `"kiln config store was unreadable at boot and is quarantined; download a
   backup or clear the quarantine before saving"`. This is the whole mitigation:
   a store that cannot be read is not overwritten.
3. `GET /api/kiln_configs` reports `store_state: "quarantined"` plus the reason.
   The web page shows a non-dismissable banner; the LCD shows
   `Kiln store error` on the existing alert mechanism (no new colour,
   `project_status_color_contrast_impossible`).
4. **One operator action clears it:** `POST /api/kiln_configs/quarantine_clear`
   with an explicit `confirm_discard=1`, which erases the blob key, clears the
   flag, and logs what was discarded. That is the recovery path: *the operator
   decides to lose the slots*, the firmware never decides for them.
5. The raw bytes are **not** salvaged, parsed heuristically, or slot-scavenged.
   See §12.2 — I would not build a repair wizard.
6. Divergence interaction: a quarantined store means the recorded
   `(config_version, config_crc)` reference may be gone. Per plan §3.4's
   fail-safe default this is a divergence until re-established — but
   re-establishing it does not need the slot, only the *live* config on both
   sides (plan §3.1.2's re-establish path). So a quarantined store must **not**
   permanently prevent firing: heaters are disabled until the link-up
   comparison against the *live* ESP config succeeds, then normal running
   resumes with `active_id = NO_ACTIVE_ID` and the UI saying "unsaved — no kiln
   package selected" (plan §2.4 rule 1 already specifies that state). A
   quarantined store that blocked firing forever would be a brick by another
   name.

*Tested by:* host tests staging (a) a truncated blob, (b) a blob one byte too
long, (c) a v2-sized blob claiming version 7, (d) a v3-sized blob with a
garbled `version` field, (e) key absent. Assert each produces the correct one of
the three outcomes, that (a)–(d) set the quarantine, and that a subsequent
`kiln_cfg_store_save_current()` is **refused and writes nothing** — asserted by
reading the NVS key's bytes back and comparing to the staged corrupt bytes.

*Negative test:* remove the quarantine check from `nvs_save_store()` by hand,
and assert the test observing the corrupt bytes surviving now **fails** (the
save overwrites them). Restore by hand — never `git checkout --`, this tree is
shared (`feedback_negative_test_restore_by_hand`).

---

## 2. The active profile deleted, or the store empty (H5, H17)

### 2.1 Deleting the profile you are running (H5) — LIVE DEFECT

`kiln_cfg_store_delete()` (`kiln_cfg_store.c:989`) is fourteen lines and does
**no** interlock check. Compare `kiln_cfg_store_apply()`, whose own header
comment (`kiln_cfg_store.h:34-53`) explains at length why the interlock was
deliberately moved *inside* the store so a caller cannot forget it. Delete never
got the same treatment, and it is reachable today from
`kiln_cfg_http.c:281` with a firing running.

Today the consequence is mild: `active_id` is cleared, the live `zones_cfg_t` is
untouched, the firing continues on it. Under the plan's model it is not mild:

- the recorded `(config_version, config_crc)` reference for the divergence
  check lived in that slot, so the standing invariant loses its reference
  mid-firing;
- auto-save now has no active package, so every subsequent edit is unsaved
  (plan §2.4 rule 1) — correct behaviour, but the operator caused it with a
  click labelled "Delete" on a *different-looking* list row;
- there is no longer any stored copy of the Pico half the Pico is running from
  RAM. A Pico reboot then falls back to whatever its flash fallback holds, and
  the ESP cannot re-push what it no longer has.

**H5 (work item). Delete is interlocked and cannot silently orphan the active
package.**

1. `kiln_cfg_store_delete()` gains a `reason_out`/`reason_cap` pair and calls
   `ota_http_check_interlocks()` **itself**, first, exactly as
   `kiln_cfg_store_apply()` does — the same backstop-not-convention reasoning,
   and the same function (not `heat_interlock.c`, which answers the opposite
   question).
2. **Deleting the active package is refused outright**, with
   `"'<name>' is the kiln config this controller is running; select another
   kiln config first, or use Save as to keep a copy"`. This is the simple
   refusal, and it is better than any clever recovery: the operator's real
   intent is either "switch kilns" (Apply) or "tidy up" (delete a non-active
   slot), and both remain available. I considered and rejected
   "delete-and-keep-running-unsaved": it produces exactly the state where the
   only copy of the running kiln's identity is in volatile-ish live config,
   which is the loss this whole feature exists to prevent.
3. `kiln_cfg_http.c`'s delete handler pre-checks for a better message, as the
   apply handler already does — redundant, not load-bearing.
4. The UI disables the Delete control for the active row and says why on hover.
   The store's refusal is the backstop; the disabled control is courtesy.

*Tested by:* a host test that saves two configs, applies one, and asserts
`kiln_cfg_store_delete(active_id)` returns false with the active-config reason
and that the entry is still `in_use`; plus the existing fake-interlock harness
(`test_kiln_cfg_store.c:415`'s pattern) asserting a delete of a *non*-active
slot is refused while a firing runs.

*Negative test:* remove the `ota_http_check_interlocks()` call from
`kiln_cfg_store_delete()` by hand and assert the firing-refusal test fails.
Restore by hand.

### 2.2 An empty store, and what the dropdown shows

The plan already settles the behaviour (§2.4 rule 1: no active package → no
auto-save, UI says "unsaved — no kiln package selected"). What is missing is
that this state is currently *indistinguishable* from three different causes,
and the operator's correct action differs for each:

| cause | correct operator action |
|---|---|
| never saved anything | Save as |
| active slot deleted | Save as, or Apply another |
| store quarantined (H3) | download a backup, then clear quarantine |
| active slot failed validation at boot (`kiln_cfg_store_init()` already logs and clears this) | Save as; the old slot is still listed but was rejected |

**H5b (work item).** `GET /api/kiln_configs` reports `active_state` as one of
`active` / `never_saved` / `deleted` / `quarantined` / `active_invalid`, and the
UI renders the matching one-sentence instruction. The `active_invalid` case
needs the existing boot-time clear (`kiln_cfg_store.c:713-731`) to record *which
id* was rejected and why, persisted, rather than only logging it.

*Tested by:* one host test per state asserting the reported value.
*Negative test:* n/a per state, but assert the four states are mutually
exclusive and that `active` is never reported while `active_id ==
KILN_CFG_NO_ACTIVE_ID`.

### 2.3 Half-packages (H17) — LIVE DEFECT

`kiln_cfg_entry_t::pico_populated == 0` is the v2-migrated slot: real ESP half,
**no Pico half**, `pkg_schema` and `pkg_hash` both 0. The internal header is
admirably explicit that `kiln_cfg_store_apply()` "must not claim a Pico half
exists" for such a slot — but as of today `kiln_cfg_store_apply()` does not look
at `pico_populated` at all, because the Pico push (plan item 5) is not written
yet. When it is written, the obvious implementation applies the ESP half and
pushes nothing, which is **exactly the silent partial swap** the plan exists to
prevent: the previous kiln's `abs_max_temp_c` stays on the Pico.

**H17 (work item).** Applying a `pico_populated == 0` slot is **refused**, not
best-efforted: `"'<name>' was saved before this firmware stored the safety
processor's settings. Select it, check the safety settings, then press Save to
complete it."` The UI marks such rows with a "needs completing" badge (the
existing `has_pico_half` field in plan §7.2 already carries the information).
A plain `Save` onto that slot captures the Pico half and completes it — that is
the migration path, performed knowingly by the operator, which is the only
honest one available since no firmware can retroactively know what the Pico held
when a v2 slot was saved.

*Tested by:* stage a v2 blob, migrate, assert `pico_populated == 0`, assert
apply is refused with that reason and **no Pico write is attempted** (assert
against a Pico-push spy in the host fake), then assert a re-save sets
`pico_populated = 1` and a non-zero `pkg_hash`, after which apply succeeds.
*Negative test:* make apply proceed on `pico_populated == 0` by hand and assert
a test detects the Pico retaining the old ceiling. Restore by hand.

---

## 3. Round-trip fidelity (H1, H2, H14) — the highest-risk finding

### 3.1 H1: the hash includes compiler padding — FIXED 2026-09-14, see `docs/audits/kiln_package_canonical_serializer_2026-09-14.md`

`kiln_package_compute_hash()` (`kiln_package.c:105`) does the Pico half
correctly: it packs `{param_id, type, flags, value_bits}` field by field, in
ascending `param_id` order, into a byte stream. Plan §3.1.3 rules 2 and 3,
honoured exactly.

The ESP half does not. It is `memcpy(buf + off, esp_blob, esp_blob_len)`, and
`esp_blob` comes from `zones_config_export_blob()`
(`zones_config_accessors.c:1675`), which is:

```c
memcpy(out, &s_zones.cfg, sizeof(s_zones.cfg));
```

A raw copy of `zones_cfg_t`. **The package hash therefore covers every padding
byte the compiler inserted into that struct**, in direct violation of plan
§3.1.3 rule 3 ("Padding is never hashed. A struct hash would make the CRC depend
on compiler padding — a silent divergence across a toolchain change").

Why this is the top finding, not a nitpick:

- **It breaks download → upload, which is the owner's feature.** An uploaded
  package is necessarily reconstructed field by field from JSON into a fresh
  `zones_cfg_t`. Its padding will hold whatever the allocation held — zero for
  a `calloc`, garbage for a stack or `malloc` buffer — and will **not** match
  the padding in the originating board's live struct. The recomputed hash
  differs from the file's stated `pkg_hash`, and plan §5.2 refuses the upload.
  **The feature's headline capability fails on a file the same board produced
  minutes earlier.**
- **The same defect exists inside `zones_cfg_t::crc32`** (`zones_config_json.h:2282`,
  computed by `zones_config_store.c:474-475` as a whole-struct CRC with the
  field zeroed). That CRC is checked by `zones_config_import_blob()`. So a
  JSON-reconstructed blob fails *two* integrity checks for the same reason.
  Today this is invisible because the struct is only ever memcpy'd between RAM
  and flash, never reconstructed — the JSON round trip is what exposes it.
- **It is a silent toolchain landmine.** Any IDF or MSVC change to padding
  changes every stored `pkg_hash` and every stored `zones_cfg_t::crc32` — on a
  board whose configuration did not change. Plan §9's named risk "a spuriously
  mismatching hash is the way this feature kills itself" arrives via this
  route.

**H1 (work item). Canonicalise the ESP half before hashing, and zero-fill on
every reconstruction.** Three parts, all required:

1. **A new `zones_config_export_canonical()`** that writes a packed,
   declaration-order, padding-free byte stream (explicit per-field little-endian
   emission, floats as IEEE-754 bit patterns with `-0.0` flushed to `+0.0`,
   `crc32` **excluded**), and its inverse. `kiln_package_compute_hash()` takes
   *that* stream for the ESP half, not the raw blob. This is the plan's own rule
   3 implemented; it is ~150 lines of mechanical emission plus a size assertion.
   The stored `blob[]` keeps being the raw struct — it is the store's private
   on-flash form and does not need to change.
2. **Every path that constructs a `zones_cfg_t` from anything other than a
   verbatim copy must `memset(0)` it first.** `zones_config_convert.c` already
   does this on all its migration paths (nine `memset(d, 0, sizeof(*d))` calls);
   the JSON upload path must too, and a comment must say why.
3. **A compile-time guard against the struct-hash pattern returning.** Add a
   `_Static_assert` that the canonical stream's declared length equals the sum
   of the emitted field sizes, and a `check_*` script assertion that
   `kiln_package_compute_hash()` does not `memcpy` a `zones_cfg_t`-sized buffer.

*Tested by:* the plan's item 12 acceptance, made actually falsifiable — build a
fully-populated config, export to JSON, parse back into a **deliberately
garbage-filled** destination buffer, and assert (a) the recomputed `pkg_hash`
equals the original and (b) `zones_cfg_t::crc32` validates. Today's test almost
certainly uses a zero-initialised destination and therefore passes vacuously.

*Negative test, two parts:* (a) fill the destination with `0xA5` before parsing
and assert the test **fails** if the canonical serializer is replaced by the raw
memcpy — this is the load-bearing proof; (b) restore `%.9g` after swapping in
`%.4f` per the plan's own item 12 negative test, which is correct and should be
kept. Restore both by hand.

**Fields that do not survive the round trip.** With H1 fixed, the answer is
"none, and the hash proves it" — but only because of the plan's §6 rule that a
missing field is a *rejection*, not a default. That rule is what turns "a field
silently dropped" into "the file will not load". Keep it. The one carve-out
(fields newer than the package's `pkg_schema`) is safe precisely because it is
*named* in the upload response, and the hash is recomputed after the defaulting
so the resulting slot's hash honestly describes what is in it.

### 3.2 H2: `0` is both a hash and a sentinel — LIVE DEFECT

`kiln_package_compute_hash()` returns `0` from three failure paths (null
`pico`, `needed > KILN_PKG_HASH_SCRATCH_CAP`, `malloc` failure) and sets
`*out_ok = false` in each. `kiln_cfg_store_internal.h`'s own comment
simultaneously documents `pkg_hash == 0` as meaning "never computed" and argues
that a real CRC of 0 is "an astronomically unlikely coincidence this module does
not attempt to special-case".

Those two facts collide. A caller that forgets `out_ok` — and it is an
out-parameter, which is precisely the shape this repo has been bitten by
(`project_safety_calls_logging_unchecked_success`: three instances of a
discarded result plus an unconditional success log) — stores `pkg_hash = 0` on a
slot with `pico_populated = 1`. The divergence check then compares against a
hash that was never computed, and `pico_populated` says it is valid.

**H2 (work item).** Two changes, both small:

1. Make the failure unambiguous at the type level: `kiln_package_compute_hash()`
   becomes `bool kiln_package_compute_hash(..., uint32_t *out_hash)` returning
   success, with the hash only written on success. A `bool` return cannot be
   silently discarded the way a `uint32_t` can, and the compiler can be asked to
   enforce it (`__attribute__((warn_unused_result))` on the ESP build).
2. Add the store-side invariant as an assertion, not a comment: a slot with
   `pico_populated != 0` and `pkg_hash == 0` is **treated as
   `pico_populated = 0`** at load (i.e. a half-package per H17), logged, and
   not applied. That removes the astronomical-coincidence argument entirely
   rather than relying on it.

*Tested by:* a host test forcing each failure path and asserting no hash is
written and the slot is not marked populated.
*Negative test:* revert to the `uint32_t`-returning signature and a caller that
ignores `out_ok`, and assert a test catches a `pico_populated = 1,
pkg_hash = 0` slot being applied. Restore by hand.

### 3.3 H14: `-0.0`

Plan §3.1.3 rule 4 requires `-0.0` to be flushed to `+0.0` before hashing. On
the raw-memcpy path that is impossible without rewriting the live struct, which
would be a config change disguised as a hash computation. With H1's canonical
serializer the flush happens in the emitter, where it belongs, and costs
nothing. No separate work item — it is an acceptance criterion on H1, asserted
by a test that sets a float to `-0.0f` and asserts an identical hash to `+0.0f`.

---

## 4. Version skew on upload (H7)

### What is accepted and what is refused

The plan settles most of this correctly in §5.1 and §5.2a; restating it as a
single table because an implementer needs one:

| skew | verdict |
|---|---|
| `pkg_schema` **higher** than this firmware knows | **REFUSE.** No partial apply, ever. |
| `pkg_schema` **lower** | Accept; fields introduced after that schema are defaulted, each **named** in the response; hash recomputed after defaulting. |
| `kind` mismatch | REFUSE, "not a kiln package file". |
| ESP half's `zones_cfg_t` version older than 26 | Accept — `zones_config_import_blob()` already runs the full migration chain and is the single source of truth for it. Do not add a second version gate. |
| ESP half's version **newer** than 26 | REFUSE. `zones_config_json_decode_blob()` already distinguishes newer-than-firmware from corrupt (zones_http.c's "FIX 1"); propagate that distinction into the upload message rather than collapsing it to "invalid". |
| unknown Pico `param_id` present | REFUSE (plan §5.2 rule 5). Correct and non-negotiable: an unknown id means the file knows something this firmware does not. |
| known `param_id` **absent** from the package | REFUSE unless it postdates the declared `pkg_schema`. Same rule as any other missing field. |
| Pico `config_version` differs from the attached Pico's | Not a refusal — plan §3.1.2's re-establish path. |

**The forward-compatible package must not be partially applied.** The plan says
so; the thing to add is *why it cannot happen structurally*, not just as a rule:
the upload parses into a single candidate structure and the whole structure is
either committed to a new slot or discarded (`backup_import.c`'s two-pass
shape). There is no code path that writes a field at a time. **H7c:** assert
that structurally — a host test that injects a parse failure at the last field
of the last zone and asserts no slot was allocated and `next_id` did not move.

### H7: the reverse direction is the one nobody plans for

`ZONES_CFG_VERSION` is 26 with a long migration history, and `CLAUDE.md`
documents the OTA-rollback hazard: older firmware refuses a newer-than-it-knows
blob and runs on firmware-default PID gains. The plan correctly refuses to bump
`ZONES_CFG_VERSION` (§7.5). But it bumped `KILN_CFG_STORE_VERSION` 2 → 3, and
the `cfg`-bridge discipline is "a file at any other version is treated as
absent, not migrated in place".

So: flash v3 firmware (slots migrate 2 → 3, blob rewritten at v3), then roll
back to v2 firmware. v2's `nvs_load_store()` sees a v3-sized blob, matches none
of its size branches, calls `reset_to_defaults()`, and — per H3 — the next save
overwrites it. **An OTA rollback silently destroys all ten kiln packages**, and
because `ota_rollback_esp()` is a documented, routine operation in this repo,
this will happen.

**H7 (work item).** Two mitigations, both cheap:

1. **H3's quarantine is the primary fix** — old firmware that cannot read the
   blob quarantines it instead of overwriting it, so a roll-forward recovers
   every slot. This is the single largest payoff from H3 and the reason it is
   ranked HIGH rather than MEDIUM. Note it requires the quarantine to land in
   firmware *before* the rollback, so it should ship as early as possible.
2. **Record the store version where old firmware can see it.** A second, tiny,
   never-reformatted NVS key `store_ver` holding one byte. Firmware that cannot
   read the blob but reads a `store_ver` higher than its own logs and reports
   `"kiln configs were saved by newer firmware (store v3); this firmware reads
   up to v2. Update the firmware to see them again."` — a message that names the
   actual fix, matching the plan's own §3.1.2 style.

*Tested by:* stage a v3 blob, run the v2-era load logic (kept in the host test
as a frozen copy — this is a legitimate frozen-layout test, not a mirror),
assert quarantine and the message.
*Negative test:* remove `store_ver` and assert the message degrades to the
generic one, proving the key is load-bearing. Restore by hand.

---

## 5. Hardware incompatibility (H13)

Plan §5.2a is close to complete, and its negative test (item 14: a merely
*different* but compatible package must be **accepted**) is the right guard
against the over-strictness that would kill the feature. Two gaps.

### H13: the package records provenance but not shape

`source_board` is "opaque id, informational only". So when a refusal happens,
the message can say what *this* controller has but not what the package
*expected* — it can only infer it from the config, which is exactly the
coercion-adjacent reasoning the plan forbids.

**H13 (work item). Add a `hardware` block to the envelope, recording the
shape of the controller that produced it:**

```json
"hardware": { "thermo_channels": 3, "relay_count": 4, "ct_installed": true,
              "ct_topology": 1, "safety_tc_installed": true,
              "kilnlink_protocol": 14, "pico_config_version": 3 }
```

It is **provenance, never authority**: every compatibility decision is still
made against the live controller and the attached Pico (the plan's caution about
two boards disagreeing stands). Its only job is to make the refusal message
name both sides — `"this package was built for a controller with 3 thermocouple
channels; this controller has 2"` — which is precisely what the task asks for
and what a generic "incompatible" cannot deliver. It is additive, so an older
package without the block is accepted and the message degrades to naming only
this controller's side.

**On `KILNLINK_PROTOCOL_VERSION`:** compare that constant only, never the
UART link's own version (`project_two_protocol_versions` — `get_fw_version`
reports 11, `/api/status` reports kilnlink's, both correct). The plan already
warns about this; H13 adds that the recorded value goes in the `hardware` block
so a refusal can say "built against protocol 12, this build speaks 14".

**Never coerced.** No truncation to available channels, no masking of relay bits
above `relay_count`, no dropping of a CT-dependent guard. Each is a refusal.
The one *deliberate* mutation the plan specifies — forcing `calibrated = false`
and unsetting `i_normal_a[]` when `source_board` differs (§5.3) — is not
coercion: it degrades to "uncalibrated", which makes guards dormant rather than
confidently wrong, and it invalidates **both** sides together
(`project_invalidating_one_side_inverted_arming`). Keep it exactly as written,
and keep it in the upload response's named-warning list.

*Tested by:* the plan's item 14 acceptance, plus one test per `hardware` field
asserting the refusal message contains both values.
*Negative test:* the plan's item 14 negative test (a compatible-but-different
package is accepted) is the important one and must not be weakened. Add: a
package with **no** `hardware` block is still accepted.

---

## 6. Concurrent edits (H6) — the serialisation

### The failure

`kiln_cfg_store.c` has no mutex, no critical section, and no ownership comment
claiming one. That is defensible today: the only callers are
`kiln_cfg_http.c`'s five handlers, on the httpd worker. The plan changes that
in two places at once:

- **plan item 13** adds "a single debounced task" that calls
  `kiln_package_autosave_active()`, which reads live config and writes
  `s_store` + NVS;
- **plan item 5** puts the apply transaction "on a worker task, not the httpd
  worker", which also writes `s_store` + NVS.

Three writers, one `s_store`, one `s_kiln_cfg_rev`, one NVS key. The interleaving
that matters is not exotic:

> Auto-save serializes the live config, is preempted; the apply worker commits
> package P's ESP half into live config and writes `s_store`; auto-save resumes
> and writes its *pre-swap* serialization into the slot, then — because its own
> read-back compares against what it *intended*, not against live config —
> **verifies successfully and stamps a fresh `pkg_hash`**. The slot now holds
> the old kiln's config under the new kiln's identity, certified.

That is the task's "a hash that certifies it" scenario, reachable in two steps.

### H6 (work item). One mutex, one rule, and no cleverness

1. **A single recursive-free `SemaphoreHandle_t s_store_lock`** created in
   `kiln_cfg_store_init()`, taken by **every** public entry point of
   `kiln_cfg_store` and by `kiln_package_autosave_active()`. Not a
   reader/writer lock, not per-slot, not lock-free: ten slots on a single-user
   bench controller does not justify anything more (§12.3).
2. **The lock is never held across a producer call.** This is the repo's own
   standing rule (`7a8594d`; `project_screen_idle_brick_real_cause`): a policy
   tick that held a module lock across `dashboard_get_status()` — three SPI
   reads, a 200 ms queue wait and four interrupts-disabled heap walks — bricked
   the board. The apply transaction's Pico round trip (68 params plus a full
   config page read-back over the UART link) is *far* worse than that. So:
   **snapshot under the lock, do the link work outside it, re-take the lock to
   commit**, and re-validate the generation counter (below) before committing.
3. **A generation counter on `s_store`**, bumped on every mutation. Auto-save
   captures it with its serialization and, at commit time, **refuses to write if
   it changed** — dropping the write and leaving the dirty flag set for the next
   debounce window. The apply worker does the same. This is the "compare the
   version you read" discipline the Artifact DB's `if_version` uses and that
   `boot_guard`'s verified-write fix (`0b5d9dad`) reaches for by hand; it is
   four lines and it closes the interleaving above without any lock being held
   across the link.
4. **Auto-save is suppressed while the pending-swap marker is set** — plan §2.4
   rule 5 already says so. The generation counter is the backstop for the window
   *around* the marker being set and cleared.
5. **Reads are locked too.** `GET /api/kiln_configs` walking `entries[]` while
   the apply worker `memset`s one is a torn list, not a crash, but it is the kind
   of "internally consistent, relationship broken" state
   `project_reset_one_side_bug_class` is about.
6. **The MCP is not a third writer** — it reaches the board over HTTP and the
   UART CONTROL wire, both of which land in the same handlers. Say so in the
   module comment so nobody adds a bypass.

*Tested by:* a host test with two pseudo-threads (the harness's existing
cooperative stepping, or an explicitly interleaved call sequence) that stages
exactly the scenario above and asserts the auto-save write is **dropped** and
the slot's `pkg_hash` still describes P.
*Negative test:* remove the generation-counter check from the auto-save commit
by hand and assert the test now observes the pre-swap config stored with a fresh
hash. Restore by hand. **This is the single most valuable negative test in this
document** — it is the only one that reproduces "a hash that certifies a state
neither user intended".

---

## 7. A swap racing a firing start (H8)

Plan §4.3 specifies one direction: a swap during a firing is refused, via
`ota_http_check_interlocks()` inside `kiln_cfg_store_apply()`. The other
direction is unspecified, and the plan's own item 5 acceptance ("heaters are
demonstrably disabled for the whole window") is about the old, deleted disarm
model — under §1a the Pico stays armed and heaters are *not* disabled during a
swap, so nothing currently stops a firing starting mid-transaction.

The concrete race: `POST /api/profiles/start` arrives between step 6 (Pico
pushed) and step 8 (ESP committed). The firing would then run with the **new**
kiln's guard thresholds on the Pico and the **old** kiln's PID gains, models
and per-zone ceilings on the ESP. Both sides internally consistent; the
relationship broken; no alarm, because the pending-swap marker is *suppressing*
the divergence check for exactly that window (plan §3.2).

### H8 (work item). The marker is the interlock, in both directions

1. **`capability_preflight` refuses a firing start whenever the pending-swap
   marker is not `NONE`**, with `"a kiln configuration swap is in progress"`.
   Same place the plan already puts the divergence refusal and the existing
   unacknowledged-crash refusal — one refusal set, not a parallel gate.
2. **The ordering is: marker first, everything else after.** Step 2 persists the
   marker *before* any config is touched, which the plan already specifies for
   crash-recovery reasons; H8 makes that same write the firing interlock. So the
   window in which a firing can start and a swap can begin never overlaps: a
   firing start that reads `marker == NONE` and a swap that writes
   `marker = STAGED` are both under H6's `s_store_lock`, and whichever takes it
   first wins.
3. **The reverse check is `ota_http_check_interlocks()` under the same lock.**
   Today the apply handler checks interlocks and then calls apply, which checks
   again — both outside any lock, so a firing can start between them. Taking the
   lock across "check interlocks, write marker" closes it. This is a genuine
   check-then-act bug in the existing code, not a new one, and it is cheap to
   fix now.
4. **A swap that completes as a firing starts** is then impossible in the
   dangerous direction: the marker clears at step 12 *after* both read-backs
   pass, so a firing that starts one tick later runs on a fully verified,
   consistent pair.
5. **Refuse, never queue** — the plan is right and this extends to the firing
   side: a firing request during a swap is refused with a message telling the
   operator to retry in a moment, not held and released. A swap takes seconds; a
   queued firing start landing at an unattended moment is the surprise this
   feature exists to remove.

*Tested by:* host tests asserting (a) a firing start is refused with
`marker = STAGED / PICO_OPEN / PICO_DONE / ESP_DONE`, (b) an apply is refused
while a firing runs, (c) an interleaved "check interlocks / start firing / write
marker" sequence cannot produce both.
*Negative test:* remove the marker check from `capability_preflight` by hand and
assert (a) fails. Restore by hand.

---

## 8. Wear and exhaustion (H11, H12)

`project_flash_endurance_is_not_the_problem` measured 1,720 years of margin on
more frequently written stores, and plan §2.3/§2.4 restate the arithmetic
correctly. **I agree wear is not a design constraint, and I would not build any
wear mitigation.** What is missing is *bounded behaviour* — a guarantee that the
stated rate is the actual rate.

### Expected write rate, restated as a bound to enforce

| driver | writes |
|---|---|
| auto-save, 5 s debounce / 60 s hard flush | ≤ 60/hour while actively editing; single digits on a normal day |
| a kiln swap | 3 (marker STAGED, marker updates, final commit + clear) |
| upload | 1 |
| delete / rename / save | 1 each |

Each write is one ~24 KB LittleFS copy-on-write commit plus one NVS blob write
of ~17.5 KB.

### H11 (work item). Make the bound observable and enforced

1. A rolling counter of `kiln_cfg_store` NVS writes in the last hour, exposed in
   `GET /api/kiln_configs` and in `get_heap_status`'s diagnostics.
2. A **hard rate cap** in `nvs_save_store()`: more than 120 writes in an hour
   logs `ESP_LOGE` once per hour naming the last caller and **coalesces**
   further auto-save writes to the 60 s hard flush only (explicit operator
   actions — save, delete, apply, upload — are never coalesced; they are
   bounded by the human pressing buttons). The cap is not there because flash is
   fragile; it is there so a future writer that raises the dirty flag per control
   tick produces a loud log line instead of a quiet 1 Hz write loop. The repo
   has shipped exactly this shape before (`project_harness_prints_verdict_exits_zero`:
   a bound stated in a comment and enforced by nothing).
3. **Partition-full behaviour is already correct and must stay asymmetric**
   (plan §2.3): a failed `cfg` write is a logged warning because NVS is
   authoritative; a failed **NVS** write is a hard failure because the package
   would not survive a reboot. Add the missing half: today
   `kiln_cfg_store_delete()`, `_rename()` and `_apply()` all `ESP_LOGE` an NVS
   failure and then **return true**. Under auto-save that means the hash is
   recomputed over a config that did not persist. **Make them return false with
   a reason**, and make auto-save treat an NVS failure as "do not update
   `pkg_hash`, raise `autosave_failed`" — which is plan §2.4 rule 3 applied to
   the write path as well as the read-back.

*Tested by:* a host test driving 200 dirty-flag raises in a simulated hour and
asserting ≤ 120 NVS writes and one `ESP_LOGE`; a test with the NVS fake failing
asserting `kiln_cfg_store_delete()` returns false and `pkg_hash` is unchanged.
*Negative test:* make the NVS fake fail and assert the *current* code's
`return true` is caught — i.e. write the test first, watch it fail against
today's code, then fix. Restore any hand-broken production code by hand.

### H12: bound the upload body before allocating

Plan §7.2 correctly puts the import body on the heap
(`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`). It does not state a ceiling. A package
is ~18 KB of JSON; a malicious or mistaken 8 MB body would be allocated in full
before parsing. **H12:** reject `Content-Length` above a stated
`KILN_PKG_IMPORT_MAX_BODY` (64 KB — ~3.5x the largest legitimate package) before
allocating, and cap the accumulated body at the same value for a chunked
request. One `if`, one constant, one test.

---

## 9. Name handling (H9) — LIVE DEFECT

`normalize_name()` (`kiln_cfg_store.c:623`) is genuinely good on the axes it
covers, and the existing behaviour should be documented rather than changed:

| case | current behaviour | verdict |
|---|---|---|
| empty / whitespace-only | rejected (trimmed length 0) | correct |
| leading/trailing whitespace | trimmed **before** both the length check and the duplicate check | correct, and the comment explains why |
| longer than 23 bytes | rejected, not truncated | correct — truncation would silently create duplicates |
| duplicate, case-insensitive | rejected via `names_equal_ci()` | correct; "Spare" and "spare" are the same name to an operator |
| rename to its own name / a case variant | allowed (`exclude_id`) | correct |

What is **not** validated, at all:

- **control bytes** (`\n`, `\t`, `\0`-adjacent, `0x01`–`0x1F`): `isspace()`
  trims `\n` and `\t` at the edges but not in the middle. A name containing a
  newline reaches flash, the JSON export, the `Content-Disposition` filename,
  and the LCD.
- **`"` and `\`**: `kiln_cfg_http.c` does escape JSON output (it has an escape
  helper), so the API survives. The `Content-Disposition:
  attachment; filename="<name>.kilnpkg.json"` header in plan §7.2 **does not** —
  a `"` in the name breaks the header, and a `\` or `/` produces a filename the
  browser may reject or path-traverse.
- **invalid UTF-8**: a truncated multi-byte sequence (easy at a 23-byte
  boundary, since the length limit is in *bytes*) renders as a replacement
  character in the browser and as garbage on the LCD, and makes two visually
  identical names non-duplicate.
- **leading `.` / `..`, or a name that is only punctuation**: harmless in NVS,
  hazardous in a filename.

### H9 (work item). Validate the character set once, in the store

1. `normalize_name()` additionally rejects any byte below `0x20`, `0x7F`, and
   any of `" \ /`. Everything else printable is allowed — this is an operator's
   kiln name ("Skutt KM-1027", "Bailey #2"), and an over-strict alphanumeric
   rule would be an annoyance with no safety content.
2. **UTF-8 validity is checked**, and a name whose final byte is mid-sequence is
   rejected with `"name is too long (23 bytes)"` rather than accepted truncated.
   The limit stays in bytes — it is a flash field — but the message says bytes so
   the operator understands why a 12-character name was refused.
3. **The refusal reason is specific.** Today `kiln_cfg_store_rename()` returns a
   bare `false` for every name problem, and `kiln_cfg_http.c` cannot tell the
   operator whether the name was empty, too long, a duplicate, or contained a
   control byte. Give `_rename()` and `_save_current()` the same
   `reason_out`/`reason_cap` treatment `_apply()` already has.
4. **The filename is derived, not copied.** `Content-Disposition` uses a
   sanitised slug (alphanumerics, `-`, `_`; everything else collapsed to `_`;
   empty → `kiln_package`), with the real name carried inside the JSON. Never
   put an operator string in an HTTP header unfiltered.
5. **Validated in exactly one place.** The store, because the store is the
   backstop every surface funnels through — the same reasoning
   `kiln_cfg_store.h:34-53` gives for the interlock. The web UI and LCD may
   pre-check for a better message; those checks are redundant, not load-bearing.

*Tested by:* a table-driven host test, one row per case above, asserting accept
or reject **and the reason string**.
*Negative test:* remove the control-byte rejection by hand and assert the
`Content-Disposition` test (a name containing `"`) produces a malformed header.
Restore by hand.

---

## 10. The failure this project actually had (H4) — every subset-writing path

`137dea1a` / `docs/audits/plant_model_loss_investigation_2026-09-14.md`: a
whole-object `POST /api/zones` that omitted the model keys **deleted** the plant
models, undetected for three days, and the loss was only found because
`coupling_diag_k_dc` (a *different* field) survived and made the absence
conspicuous. Two compounding lessons, both of which apply here: the write was
whole-object but the *payload* was a subset, and the subsequent audits laundered
a hedged inference into an assertion (`project_retraction_hid_the_stale_claim`).

Under this feature the same shape is worse, because auto-save would persist the
loss into the last remaining copy **and recompute `pkg_hash` so the result looks
consistent**. Enumerating every path in this feature that writes a subset of
config, and what stops each:

| # | path | what it writes | why it cannot silently drop a field | residual risk |
|---|---|---|---|---|
| **P1** | `zones_config_set_*()` accessors (PID gains, model, coupling, per-zone fields) — the existing live-config writers | one field or one group | They are *by design* single-field. They cannot drop a field because they never claim to write more than one. Auto-save's dirty flag is raised **after** the accessor returns true, so a failed set does not trigger a save. | **Low.** The hazard is not the accessor, it is what auto-save then serializes — see P3. |
| **P2** | `POST /api/zones` whole-object handler — **the original defect** | the whole `zones_cfg_t` from a JSON body | *Nothing in this feature.* The fix for this path belongs to `zones_http_post.c` and is out of scope here. **But this feature must not amplify it.** | **HIGH, and the reason H4 is ranked HIGH.** See H4 below. |
| **P3** | `kiln_package_autosave_active()` (plan item 13) | the whole active slot | Plan §2.4 rule 2 (complete, never partial — the same whole-struct builder as a manual save) plus rule 3 (read-back verify, hash follows the verification). The builder is `zones_config_export_blob()`, which is a `memcpy` of the **live struct** — it cannot omit a field, because it does not enumerate fields. | **Low by construction.** Ironically the raw memcpy that causes H1 is what makes this path safe. H1's canonical serializer must preserve that property: emit from the struct, never from a key list. |
| **P4** | `kiln_cfg_store_save_current()` / `_clone()` | one slot's ESP blob + Pico half | Same memcpy builder for the ESP half; `kiln_package_capture_pico_half()` walks `CONFIG_PARAM_TABLE` by index rather than a hand-written id list, so a newly minted param id cannot be missed. | **Low.** Keep the table walk; a field list here would rot. |
| **P5** | `kiln_cfg_store_apply()` → `zones_config_import_blob()` | the whole live `zones_cfg_t` | All-or-nothing, version-checked, CRC-checked, `zones_config_json_validate()`d. | **Low**, but see H17: a `pico_populated == 0` slot makes the *cross-processor* write a subset. |
| **P6** | upload → new slot | one slot | Plan §6: a missing field is a **rejection**. Two-pass validate-then-commit. Nothing is written on refusal. | **Low**, conditional on §6 actually being implemented as reject-not-default. Plan item 2's negative test covers it. |
| **P7** | the Pico push (plan item 5 step 6) | 68 params | `apply_pairs()` over a table walk, then a **full config-page read-back compared field by field** (step 7). A dropped param fails the read-back and rolls back. | **Low.** The read-back is the guard; it must not be weakened to a hash-only check. |
| **P8** | `safety_ceiling_sync_*()` (the other agent's work) | `abs_max_temp_c` only | Deliberately a single field, with its own ordering contract. Being a subset is the *point*. | **Low**, provided this feature calls it and does not write the ceiling by a second path. |
| **P9** | `migrate_store_v2_to_v3()` | every slot | Cannot invent a Pico half, and correctly says so by setting `pico_populated = 0`. | **Medium — this is H17.** Honest at the storage layer, dangerous if apply ignores the flag. |

### H4 (work item). Amplification guards on the auto-save path

The finding is not that auto-save drops fields — P3 shows it structurally
cannot. The finding is that auto-save **propagates and certifies** a loss that
happened upstream at P2, and the plan's item 13 negative test accepts outcome
(b) "persists the complete config including the models" as a pass — but if P2
already deleted the models, the "complete config" *is* the one without them, it
verifies perfectly, and the hash is recomputed. Plan §2.4's blast-radius
paragraph identifies this and then the test does not actually close it.

Three additions:

1. **A loss detector, not a completeness detector.** Auto-save compares the
   serialized config against **the slot's previous contents** and refuses to
   write — raising a visible `autosave_field_cleared` warning naming the
   fields — when a field transitions from a non-default value to its
   zero/sentinel default **and** the transition was not made by an explicit
   operator action on that field. Concretely: a set of "expensive to re-derive"
   fields is enumerated once (`model_k_dc`, `model_tau_s`,
   `model_dead_time_s`, `coupling_coeff[]`, `coupling_tau_s[]`,
   `coupling_dead_time_s[]`, `coupling_diag_k_dc`, `autotune_baseline_k_dc`,
   `pid_kp/ki/kd`, `cal_offset_c`, the CT calibration), and a non-zero → zero
   transition in any of them **blocks the auto-save** and requires an explicit
   `POST /api/kiln_configs/save?ack_cleared_fields=1`. This is a deliberate
   asymmetry: losing tuning is expensive and nearly always accidental; gaining
   it is always deliberate. It is the one piece of "clever" machinery in this
   document I would build, because it is the exact defect that cost three days.
2. **The warning is surfaced, not logged.** `GET /api/kiln_configs` reports
   `autosave_blocked` with the field list; the page shows it; the LCD shows
   `Config save blocked` on the existing alert mechanism. A logged-only warning
   is how the original defect went unnoticed for three days.
3. **`settings_source[]` is the corroborating signal** and travels in the
   package already (plan §1.2). A field whose `settings_source` says "autotuned"
   going to zero is a stronger signal than a hand-entered one doing so; use it
   to word the warning, not to decide (deciding on it would make the guard
   depend on a field that could itself be dropped).

*Tested by:* replay the `137dea1a` scenario — a whole-object write that omits
the model keys — and assert auto-save is **blocked**, `pkg_hash` is unchanged,
the slot still holds the models, and the warning names all three fields per
zone.
*Negative test:* clear the expensive-fields list and assert the test fails —
proving the list is load-bearing. Then separately make the read-back
verification always report success (the plan's own item 13 negative test) and
assert that too is caught. Restore both by hand.

**Ranking note:** I considered whether H4's loss detector is over-engineering.
It is the kind of heuristic guard that can annoy an operator who *intends* to
zero a model (e.g. before a re-autotune). Two things settle it for me: the
escape hatch is one explicit acknowledgement, and the alternative — the defect
recurring with the last copy destroyed and a hash certifying it — has already
happened once in this project at smaller blast radius. Build it.

---

## 11. Interrupted swap at boot (H10, H16)

Plan §4.4's five-case table is sound and its choice not to "finish the swap"
after a `PICO_DONE` crash is correct. Stressing it:

### 11.1 Power loss at each step

| lost after | marker on disk | plan's action | assessment |
|---|---|---|---|
| step 1 (validate) | `NONE` | nothing | Correct — nothing was written. |
| step 2 (marker persisted) | `STAGED` | discard, both sides on old package | Correct. |
| step 4 (ceiling raised, nothing else) | `STAGED` | discard | **GAP.** The Pico's ceiling was raised and the marker says nothing happened. The raise is in the *safe* direction only for the new package; against the old one it is a **looser** Pico ceiling than the ESP's, which `zones_http_post.c`'s existing rule forbids. See H10a. |
| step 6 (push in flight) | `PICO_OPEN` | latch, re-apply R, read back | Correct. |
| step 7 (read-back done) | `PICO_DONE` | latch, re-apply R | Correct, and correctly refuses to finish. |
| step 8 (ESP committed) | `ESP_DONE` | verify both, finish | Correct — this is the one case where finishing is right, because both halves already agree. |
| step 12 (committed, marker not yet cleared) | `ESP_DONE` | as above | Correct and idempotent. |
| step 13 (fallback persist) | `NONE` | nothing | Correct; the swap was complete at step 12. |

**H10a (work item). The ceiling raise needs its own marker transition.** Step 4
mutates the Pico before the marker advances past `STAGED`. Add
`marker = CEILING_RAISED` between steps 4 and 6, whose boot action is: re-assert
the ceiling from the *active* (old) package via
`safety_ceiling_sync_apply_lower()`, verify, then discard the record. Without
it, a power cut at exactly the wrong moment leaves a Pico ceiling looser than
the ESP's with nothing recording that it happened — and because a looser Pico
ceiling is not itself a trip, the only thing that would catch it is the
divergence check, which is correct but reports it as a mysterious mismatch
rather than a known recoverable state.

### 11.2 A Pico that reboots mid-swap

`boot_id` is the detector and the plan hooks re-push into the existing
`boot_id_changed` block — right call, and explicitly not a sixth instance of
`project_reset_one_side_bug_class`.

**H16 (work item).** One case is not distinguished: a `boot_id` change observed
**between step 7's read-back and step 12's commit**. At that moment the ESP has
recorded `(config_version, config_crc)` from a Pico that no longer exists; the
rebooted Pico is on its flash fallback (or unconfigured). The plan's re-push
logic would push the *active* package — which is still R, because `active_id` is
not updated until step 12 — silently reverting the swap while the transaction
believes it succeeded.

Mitigation: the transaction **captures the Pico's `boot_id` at step 6 and
re-checks it at steps 7, 10 and 12**. A change at any of them aborts the
transaction into ROLLBACK (not into the latch — the Pico rebooting is a
recoverable event, and R is what the re-push would install anyway, so the two
agree). `boot_id` is already parsed in `safety_link_frames.c`; this is reading an
existing field three more times.

*Tested by:* a host test that changes the fake Pico's `boot_id` at each of steps
7, 10 and 12 and asserts ROLLBACK with `active_id` unchanged and no latch.
*Negative test:* remove the step-12 `boot_id` re-check and assert a test observes
the swap reported as successful while the Pico runs R. Restore by hand.

### 11.3 A link that drops mid-swap

Covered by plan §1a.6 and by `confirm_commit_landed()`'s "never trust a bare
ACK". The one addition: the transaction needs a **bounded overall timeout** that
ends in ROLLBACK or the latch, never in an indefinitely-held marker. A marker
stuck at `PICO_OPEN` with the apply worker blocked on a dead link would, via H8,
refuse every firing start forever with no explanation. **H16b:** a transaction
deadline (30 s is generous for 68 params plus a page read-back) after which the
worker latches `CONFIG_DIVERGENCE` with `"kiln configuration swap did not
complete: safety processor did not respond"`, leaves the marker for boot
recovery, and exits. Bounded, loud, and it does not expire into "proceed" — the
distinction plan §1a.6 draws and item 16's negative test already guards.

### 11.4 The marker is itself corrupt (H10) — the unhandled sixth case

The plan's table handles five marker values. It does not handle a marker field
holding a sixth, impossible value, or a pending-swap record whose rollback
package R is torn. And R is ~1.6 KB written into the same NVS blob as everything
else — so a corrupt store (H3) takes the marker with it, and the recovery
mechanism is gone precisely when it is needed.

**H10 (work item).**

1. The pending-swap record carries **its own CRC-32** over `{marker, target_id,
   R}`, checked before the record is believed. This is independent of NVS's own
   per-entry CRC because the record can be logically inconsistent without being
   byte-corrupt (e.g. a partially written R from an interrupted
   `nvs_save_store()`).
2. **An unrecognised marker value, or a failed record CRC, is treated as the
   most pessimistic known case — `PICO_DONE`:** latch `CONFIG_DIVERGENCE`,
   disable heaters, and re-apply **the active package** (not R, which cannot be
   trusted) to the Pico, read back, clear on success. The active package's slot
   has its own `pkg_hash` and is independently verifiable, so it is a better
   recovery source than an unverifiable R. If the active package is also
   unavailable (H3's quarantine), the latch stays set and the message says
   `"kiln configuration state could not be recovered; select a kiln
   configuration to continue"` — a controller that refuses to fire and says why,
   which plan §4.2 already establishes as the acceptable outcome.
3. `marker` is stored as a **tagged value with a magic prefix**, not a bare
   enum, so an all-zero or all-`0xFF` region does not decode as a valid marker.
   (`NONE` must not be `0` for this reason — or if it is, the magic must be
   checked first.)

*Tested by:* host tests staging (a) marker = 0x5A, (b) a valid marker with a
corrupted R, (c) a valid marker with R's CRC deliberately recomputed over wrong
bytes, (d) an all-`0xFF` record. Assert each takes the pessimistic path and that
heating is impossible until the comparison passes.
*Negative test:* remove the record CRC check and assert (b) is believed and a
torn R is pushed to the Pico. Restore by hand.

---

## 12. What I would NOT build

Ranked by how likely someone is to propose it anyway.

1. **A store-repair / slot-scavenging wizard.** Walking a corrupt blob looking
   for plausible-looking `kiln_cfg_entry_t` structures, or reconstructing a slot
   from partially valid bytes. The failure mode is a *silently wrong* kiln
   configuration applied to a real kiln with a safety processor attached — worse
   than an empty picker by a wide margin. H3's quarantine (preserve the bytes,
   tell the operator, let them choose) is the whole of what should exist. If
   forensics are ever wanted, add a `GET /api/kiln_configs/raw` that dumps the
   quarantined bytes as hex for offline analysis — read-only, never a write
   path.

2. **Package signing.** Plan §9 item 9 already rejects it and is right. No key
   management story exists on this board; a signature would imply a guarantee it
   cannot make. A CRC honestly documented as integrity-not-authentication is
   correct, and the real protection is that upload never applies — an operator
   presses Apply on a package the UI has already shown them with warnings.

3. **Anything richer than one mutex for concurrency (H6).** No reader/writer
   lock, no per-slot locking, no lock-free ring, no priority inheritance
   analysis. Ten slots, one operator, a handful of writes per day. A single
   mutex plus a generation counter is the correct amount of machinery, and the
   lock-not-held-across-producer-calls rule matters far more than the lock's
   sophistication.

4. **Merge semantics for concurrent edits.** No CRDT, no three-way merge, no
   "last writer wins per field". Two humans editing one bench controller's
   commissioning pages simultaneously is not a real scenario; the generation
   counter's "drop the stale write and retry on the next debounce" is the right
   answer and costs four lines.

5. **Queued swaps.** Plan §4.3 is right: refused, never queued. Same for a
   firing requested during a swap (H8 rule 5). A deferred config change landing
   unattended is exactly the surprise this feature removes.

6. **An LCD kiln picker.** Plan §7.4 is right. The confirm dialog cannot be
   presented honestly on a 480x320 no-scroll page
   (`feedback_lcd_no_scrolling`). The LCD reports the active name and the
   divergence banner; selecting is a browser action.

7. **Per-half hashes.** One CRC over the whole package. A half-valid package is
   not a kiln package. Two hashes would invite a "the ESP half is fine, apply
   just that" path — the partial apply the plan forbids.

8. **Epsilon float comparison.** Plan §3.2 is right: bit-for-bit or nothing. An
   epsilon is a tolerance nobody can justify and would hide a mis-write of a
   nearby field.

9. **A partial apply of a forward-compatible package.** The task says it must
   not be, and structurally it is not: the candidate is committed whole or
   discarded. H7c asserts that rather than trusting it.

10. **Retry-forever or timeout-into-proceed anywhere.** Plan §1a.6 is emphatic
    and correct. H16b's deadline ends in a *latch*, never in proceeding.

11. **A `ZONES_CFG_VERSION` bump.** Plan §7.5 forbids it and documents the
    OTA-rollback hazard (`project_pending_zone_cfg_v17_flash`,
    `project_flash_firmware_ignores_otadata`). Nothing in this document needs
    one; H1's canonical serializer is a *new* function alongside the existing
    blob, deliberately so.

12. **Automatic clearing of the divergence latch by any operator control.**
    Plan §3.6, no dismiss. H3's quarantine clear is a different thing — it
    discards *slots*, not a safety latch, and it cannot enable heating on its
    own.

---

## 13. Total additional work

| item | size | depends on |
|---|---|---|
| H1 canonical ESP-half serializer + zero-fill + guard | ~250 lines incl. tests | — |
| H2 hash signature + store invariant | ~60 | — |
| H3 corrupt-store quarantine | ~200 | — |
| H4 auto-save loss detector | ~200 | plan item 13 |
| H5 / H5b delete interlock + active-state reporting | ~120 | — |
| H6 mutex + generation counter | ~150 | plan items 5, 13 |
| H7 / H7c store-version key + structural no-partial test | ~80 | H3 |
| H8 marker-as-firing-interlock + check-then-act fix | ~100 | plan item 5 |
| H9 name character set + reasons + filename slug | ~120 | — |
| H10 / H10a marker CRC, pessimistic default, ceiling-raise marker | ~150 | plan item 8 |
| H11 write-rate counter/cap + NVS-failure return values | ~120 | — |
| H12 upload body ceiling | ~30 | plan item 4 |
| H13 `hardware` provenance block | ~120 | plan item 14 |
| H16 / H16b `boot_id` re-checks + transaction deadline | ~90 | plan item 5 |
| H17 half-package refusal + badge | ~80 | — |
| **total** | **~1,750 lines incl. tests** | |

Against the plan's own ~6,000-line estimate for its 16 items, this is roughly a
30 % addition — concentrated, notably, in the items that protect the operator
from losing a kiln's identity rather than in the swap machinery itself.

**Landing order.** The six LIVE DEFECTS against code already on `main` should go
first and are independent of every unlanded plan item: **H2, H3, H5, H9, H17,
H1** — in that order, H1 last of the six only because it is the largest, and
H3 first because it is what makes an OTA rollback survivable (H7). Then H7,
H12, H13 with the upload items; H6 and H11 before auto-save (plan item 13)
lands; H4 with it; H8, H10, H16 with the transaction (plan items 5 and 8).
