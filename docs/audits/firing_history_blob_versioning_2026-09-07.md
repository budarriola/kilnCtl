# Firing-history blob versioning (2026-09-07)

Follow-up to `docs/audits/iter_tune_decision_2026-09-07.md` (`3bf773af`) and
`08db04d7` (which surfaced `start_temp_c` live via `/api/profile_exec` but did
not add it to the persisted record). iter_tune needs per-firing history back
across a reboot, and today's persisted blob cannot safely grow a field.

## 1. Current shape, exactly

Types (`firmware/KilnFW/App/drivers/control/profile_executor.h`,
`profile_executor_state.h`, `profile_executor_internal.h`):

```
profile_exec_firing_stats_t   64 bytes   -- per-zone derived tracking figures
profile_firing_zone_record_t  80 bytes   -- { active, stats, kp, ki, kd }
profile_firing_run_record_t  272 bytes   -- { profile_id, profile_name[16],
                                              run_started_unix_s, duration_s,
                                              zone_mask, zones[3] }
profile_firing_history_blob_t 1364 bytes -- { count, runs[5] }
```

(`MAX31856_CHANNEL_COUNT` = 3, `PROFILE_NAME_MAX_LEN` = 15,
`PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH` = 5. Sizes confirmed with a
standalone MSVC layout replica, same discipline `zone_cfg_v21_t`'s frozen
snapshot uses in `zones_config_json.h` -- never guessed.)

Notably, `profile_exec_firing_stats_t` does **not** carry `start_temp_c`.
That field lives only on the live-status struct
(`profile_exec_zone_status_t::start_temp_c`, fed from
`zone_runtime_t::fs_start_temp_c` in `profile_executor_status.c:339`) and is
never copied into `profile_firing_zone_record_t` at finalize
(`firing_stats_build_record()` in `profile_executor_firing_stats.c`). So
today's persisted history already lacks the one field iter_tune's
`iter_tune.h:251` doc comment says it needs.

**NVS storage** (`profile_executor_firing_stats.c`):
- Partition `profiles_nvs` (384 KB), namespace `fire_stats` (deliberately
  separate from `kiln_nvs` and from `profiles_http.c`'s own `kiln_cfg`
  namespace in `profiles_nvs`, so a collision is structurally impossible).
- Key `"fs_<profile_id>"` (e.g. `"fs_3"`), one key per profile that has
  actually fired. Value is the raw `profile_firing_history_blob_t`, written
  via `hal_kv_set_blob()`/read via `hal_kv_get_blob()` -- no JSON, no
  wrapper struct, no separate version/CRC record.
- **Write site:** `firing_stats_persist()`, called once per run at the
  DONE/FAULTED tick transition or from `profile_executor_halt()` (never per
  tick). Read-modify-write: loads the existing blob, shifts `runs[]` down by
  one (dropping the oldest once at depth 5), writes the new record into
  `runs[0]`.
- **Read site:** `firing_stats_load()`, called by `firing_stats_persist()`
  itself (to get the existing ring before prepending) and by
  `profile_executor_get_firing_history()` (the public accessor for
  dashboard/API consumers).

**What happens today on a size/layout change:** `firing_stats_load()`
reads with `size_t len = sizeof(*out)` passed to `hal_kv_get_blob()`, then
checks `len != sizeof(*out)`. Since the blob was written with the OLD
firmware's (different) `sizeof`, this check fails immediately on the first
read after flashing new firmware with a changed layout:

```c
if (err != HAL_OK || len != sizeof(*out)) {
    ESP_LOGW(PE_TAG, "firing_stats_load(%u) failed: %s (len %u/%u)", ...);
    memset(out, 0, sizeof(*out));
    return false;
}
```

That is the entire defence: no CRC, no version byte, no reinterpretation of
the old bytes, no migration. The failure is **not loud** in the sense that
matters operationally:
- It logs one `ESP_LOGW` line, not a board-level warning surfaced to the
  operator UI (contrast `zones_config_store.c`'s equivalent rejection path,
  which is also just a log line but is at least paired with a real
  version/CRC/validate pipeline that can *reject and fall back to defaults*
  rather than *silently discard*).
- `firing_stats_persist()` calls `firing_stats_load()` and **discards the
  bool return** ("empty blob on any failure -- still safe to prepend into"),
  so the practical effect of any layout change is: every profile's entire
  5-run history ring for that profile is silently reset to empty (not
  migrated, not rejected-and-kept-old) the next time that profile fires.
  Nothing crashes, nothing refuses to boot, no operator-facing signal beyond
  a log line that most sessions will never read.

**Versioning found near it:** none. No version byte, no CRC32 (contrast
`zones_cfg_t::crc32` / `zones_config_json_compute_crc()`), no `_Static_assert`
pinning size (until this pass), no migration function analogous to
`zones_config_migrate.c`'s `migrate_zones_cfg_v1_to_current()`. The comment
block above `profile_firing_zone_record_t` in `profile_executor.h` (line
~299) documents the ring-of-5 semantics and the kp/ki/kd re-tune-detection
rationale, but says nothing about on-disk compatibility.

## 2. Options

**(a) Add a version+size header and a migration path, `zones_config_json.h` style.**
Store a leading version byte (or a small fixed header: version + count +
reserved) ahead of `runs[]`, keep frozen `profile_firing_run_record_vN_t`
snapshots for every layout that ever shipped, and write a
`migrate_firing_history_vN_to_current()` per gap, the same shape as
`zones_config_migrate.c`. On load: check the stored version, dispatch to the
matching frozen struct, convert forward.
- Cost: real, ongoing. Every future field addition needs a new frozen `_vN_t`
  struct, a conversion function, and a test, mirroring the discipline
  `zones_config_json.h` already pays for `zones_cfg_t` (which has done this
  22 times as of `ZONES_CFG_VERSION 22`). For a 5-entry ring of a
  low-stakes, non-safety diagnostic record, that is a heavier machine than
  the data currently justifies.
- Risk: low once built -- this is the codebase's own proven pattern, and
  `zones_config_migrate.c`'s test coverage shows the shape of the tests a
  reviewer would expect alongside it.

**(b) Tail-append with a `_Static_assert` on size, treat an old-size blob as version 1.**
Only ever ADD fields at the end of `profile_exec_firing_stats_t` /
`profile_firing_zone_record_t` / `profile_firing_run_record_t` (never
reorder, never widen/narrow an existing field, never remove one). Pin every
struct's size with `_Static_assert` (this pass does that for the current,
only-known layout). On a future addition, keep a record of the OLD size
(same "frozen `_vN_t` snapshot" idea as (a), but for exactly one prior
version rather than an open chain) and treat `len == old_size` on load as
"version 1, zero-fill the tail" rather than discarding.
- Cost: much lower than (a) for the common case (single-field additions,
  which is exactly today's ask -- `start_temp_c`), since there is normally
  only ever one prior layout to special-case, not an open chain. Breaks down
  if two size-changing edits land between flashes (the same hazard
  `zones_config_json.h`'s "on-disk version, newer than this firmware's"
  branch exists for) -- an old-size blob is ambiguous between "pre-field-A"
  and "pre-field-A-and-B" unless a real version number is added anyway, at
  which point this degrades into a lightweight version of (a).
- Risk: moderate. Nobody has to design a version chain up front, but nothing
  stops a second contributor from adding a field the same way and creating
  exactly the ambiguity above, silently -- there is no version byte to force
  the question. This is a narrower, cheaper version of (a), and it is only
  safe if disciplined tail-append is actually followed every time, which the
  `_Static_assert`s added in this pass only detect (loudly, as a build
  break), never enforce.

**(c) Leave persisted history alone; keep per-firing iter_tune inputs in RAM only.**
Add `start_temp_c` (and whatever else iter_tune needs) to the live
`s_exec`/`zone_runtime_t` RAM state and to `profile_exec_status_t` for
in-progress reads, but never to the persisted blob. iter_tune reads the
in-RAM record for the just-finished run before the next run overwrites it,
and loses that data across a reboot.
- Cost: lowest of the four -- zero persistence-format work, and matches how
  `start_temp_c` already got wired live in `08db04d7`.
- Risk: iter_tune's whole premise per
  `docs/audits/iter_tune_decision_2026-09-07.md` is comparing across
  firings, some of which are separated by a reboot (OTA, brownout,
  deliberate restart between test firings) -- losing exactly the runs that
  bracket a firmware update or a bench power cycle is losing the runs most
  worth comparing. This option quietly narrows iter_tune's usable dataset
  without saying so anywhere iter_tune's own code would surface it.

**(d) Other: CRC only, no version (partial option, not recommended alone).**
Add a CRC32 over the blob (matching `zones_cfg_t::crc32`) without a version
byte. This catches torn/corrupted writes but does **nothing** for the actual
problem here -- a layout change still changes `sizeof`, so the length check
already catches that case today; a CRC adds a second, redundant safety net
for a failure mode (corruption) that isn't the one in front of us
(intentional layout evolution). Worth doing eventually for its own sake, but
it is not a substitute for (a) or (b).

## Recommendation

**(b)**, with the asserts this pass adds as its foundation. The codebase's
own precedent (`zones_config_json.h`'s 22-version chain with full frozen
`_vN_t` snapshots and dedicated migration functions) is real and battle
tested, but it is calibrated for `zones_cfg_t`: a large, safety-adjacent,
frequently-evolving struct where getting migration wrong risks running on
wrong PID gains after a rollback (see `CLAUDE.md`'s `ota_rollback_esp()`
hazard). The firing-history blob is a much smaller, purely diagnostic,
5-entry ring behind a namespace that already tolerates "start empty, nothing
breaks" as its normal cold-start behavior (`firing_stats_load()`'s own
`HAL_NOT_FOUND` path). Paying (a)'s full versioned-migration-chain cost for
that is disproportionate. (b) gets iter_tune what it actually needs --
survive the *next* field addition (`start_temp_c`) without silently
discarding every profile's history -- at a fraction of the machinery, and
without foreclosing a later move to (a) if the schema starts changing often
enough that (b)'s "usually only one prior version" assumption stops holding.
(c) is the fallback if reboot-survival turns out not to matter enough to
justify even (b)'s modest cost, but it should be a deliberate choice, not an
accident of "persistence was too much work."

## 3. This pass's change (non-behavioural)

Added to `firmware/KilnFW/App/drivers/control/profile_executor_internal.h`
(next to the `profile_firing_history_blob_t` definition): `_Static_assert`s
pinning `sizeof()` of `profile_exec_firing_stats_t` (64),
`profile_firing_zone_record_t` (80), `profile_firing_run_record_t` (272) and
`profile_firing_history_blob_t` (1364), plus `offsetof()` for every member of
`profile_firing_run_record_t` and `profile_firing_history_blob_t`, and a
comment naming the versionless-discard hazard and pointing at this doc. No
layout change, no version bump, no behavioural change -- this only turns a
future accidental layout edit into a loud build break instead of a silent
on-flash format change discovered later as "history is empty for some
reason." Values were captured with a standalone MSVC layout replica (not
firmware headers, not guessed), the same method `zone_cfg_v21_t`'s own
frozen-snapshot asserts in `zones_config_json.h` use.

Host tests (`build_host_tests.ps1`) were run to prove the asserts compile
cleanly against the real header; `build_kilnfw` was intentionally NOT run
(another session had the ESP32 board attached for flashing at the time of
this pass).

## 4. Follow-up pass (2026-09-07): option (b) implemented

The recommendation above is now implemented, not just written down:

- **Loud discard.** `firing_stats_load()`
  (`profile_executor_firing_stats.c`) now probes the on-disk size with
  `hal_kv_get_blob(..., buf=NULL, ...)` before reading, so a real read
  failure, an unrecognized size, and the new migration case below are
  distinguishable. The unrecognized-size branch logs
  `ESP_LOGW(PE_TAG, "firing_stats_load(%u) failed: ... on-disk size %u
  matches neither current (%u) nor known prior (%u) layout -- discarding
  history", ...)` naming the module, both sizes, and that history is being
  discarded. Discard-on-mismatch behaviour is unchanged (per this doc's own
  reasoning: rejecting instead would strand the key, which is worse).
- **Tail-append migration hook.** `profile_executor_internal.h` adds
  `#define PROFILE_FIRING_HISTORY_BLOB_SIZE_V1 1364u`, documented as the one
  prior on-disk size `firing_stats_load()` knows how to migrate forward by
  zero-filling the tail. `firing_stats_load()` now has three branches:
  on-disk size == current `sizeof` (normal path, unchanged behaviour),
  on-disk size == `PROFILE_FIRING_HISTORY_BLOB_SIZE_V1` and smaller than
  current (migrate: read the old bytes into the zeroed buffer's front, log
  at WARN that a migration happened), else (unknown/garbage size: discard
  loud, as above). Since `PROFILE_FIRING_HISTORY_BLOB_SIZE_V1` equals
  today's only shipped size, the migration branch is unreachable today --
  by design, this pass is zero behavioural change. The NEXT field addition
  (e.g. `start_temp_c`) must leave `PROFILE_FIRING_HISTORY_BLOB_SIZE_V1` at
  1364 while `sizeof(profile_firing_history_blob_t)` grows, which is what
  makes the migration path go live instead of every profile's history being
  silently zeroed on the first load after that flash.
- **Tests** (`test_profile_executor_prestart.c`, host-run via
  `build_host_tests.ps1`, using the real `firing_stats_load()` rather than a
  hand-rolled copy): the existing exact-size round-trip test is unchanged;
  `test_firing_stats_load_migrates_known_old_size_blob` writes a
  `PROFILE_FIRING_HISTORY_BLOB_SIZE_V1`-byte blob directly via
  `hal_kv_set_blob()` and confirms `firing_stats_load()` returns it
  successfully with the tail zero-filled and the stored fields intact;
  `test_firing_stats_load_discards_unknown_size_blob` writes a blob at
  neither known size and confirms the load fails and the caller's buffer is
  zeroed, exercising the warning path.
- **Negative-tested.** `PROFILE_FIRING_HISTORY_BLOB_SIZE_V1` was
  deliberately set to `1360u` (wrong) and `build_host_tests.ps1` re-run: the
  migration test failed as expected --
  `FAIL test_profile_executor_prestart.c:6665: today there is only one known
  layout -- V1 must equal the current size until a field is actually added,
  per this constant's own doc comment` (4827/4828, 1 failure) -- then the
  edit was reversed by hand back to `1364u` and `git diff` on
  `profile_executor_internal.h` shows only this pass's intended addition, no
  stray leftover from the negative test. Host tests are green again
  (29/29 executables) after the revert.
- **Not done in this pass:** `build_kilnfw` (target build) was intentionally
  skipped per this task's own scope note -- flashing is pending on this
  board for other reasons; host tests are the verification for this change.
  Option (a)'s full versioned-migration-chain, and (c) (RAM-only, no
  persistence), remain unimplemented and are not needed unless a second
  size-changing edit lands before this one's migration path is exercised for
  real (see `PROFILE_FIRING_HISTORY_BLOB_SIZE_V1`'s own doc comment in
  `profile_executor_internal.h` for that escalation condition).
