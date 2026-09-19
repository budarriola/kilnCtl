# Live profile edit — improving a firing while it runs

2026-09-18. Owner request, verbatim:

> "I want a page that allows the user to modify the currently running profile
> so that a fireing can be improved while in progress. Add this as a page in
> the website only. Doing this should automatically create a new profile. And
> at the end of the fireing should ask the user weather or not they would like
> to name and save it or overwright the original. Can not overwright a factory
> profile, ask for confirmation when overwrighting."

Two further requirements, same day, both non-negotiable:

> The user must not be able to modify a running profile in a way that would
> exceed the kiln's maximum temperature — rejected server-side, not warned
> about in the browser.

> Both profile tools must be built on one implementation, so a change to one
> changes both.

Nothing here is built. This is a plan; the decisions in it are settled unless
they appear under "Owner decisions needed" at the end.

---

## 1. Shape of the feature

A firing is running against profile *P*. The operator opens a new web page,
changes a future segment's target, and submits. At that moment the firmware
**forks**: it copies *P* into a free user slot, applies the edit to the copy,
and hands the copy to the executor. *P* on disk is never touched while the
firing runs — this is the whole point, and it is what makes the feature safe
to abort at any moment.

When the firing ends, the web UI asks what to do with the working copy: name
and keep it as a new profile, overwrite the original it was forked from, or
discard it. A builtin schedule can never be the target of an overwrite.

---

## 2. What is editable, and what is not

The executor tracks `s_exec.segment_index` (`profile_executor.c`, the advance
sites at `:643` and `:765`). That index splits the schedule into three cases,
and they are genuinely different.

**Segments already executed (`index < segment_index`): frozen, refused.** An
edit there cannot change anything — the heat is already in the ware — but it
would make the saved profile a false record of what was actually fired, and
the firing-stats accumulators (`profile_executor_firing_stats.c`) were
computed against the old values. The working copy still *carries* those
segments verbatim, so the saved recipe is complete and re-fireable; it simply
cannot alter them. A request that differs at or before the running index is
refused with 409 naming the segment.

**The segment running right now (`index == segment_index`): partially
editable.** `target_c`, `ramp_c_per_hr` and `dwell_min` may change, because
each of those is consumed forward from the current tick. `seg_kind` may not:
`io_seg_start()`/`io_seg_finish()` bookkeeping is keyed by segment index, and
turning a running ZONE_RAMP into a RELAY_IO (or the reverse) would strand a
relay with nothing owning it — the exact hazard `io_leave_on_at_end`'s default
exists to prevent. `dwell_min` lowered below the elapsed time does not rewind:
the segment simply becomes complete on the next tick. It never yields negative
elapsed time.

**Future segments (`index > segment_index`): fully editable** — change,
insert, delete, reorder, change kind. Deleting every remaining segment is
legal and ends the firing cleanly at the end of the current one; that is a
legitimate "stop early but cool on schedule" action and is better than a halt.

**Not editable at all while running:** `zone_mask`. The run's active zone set
was validated against the profile at start, and `reload_zone_config()` already
documents why a zone must not be silently dropped from a running firing — the
dashboard and the physical kiln would disagree about what is being driven.
On/off rules (`profile_on_off_rules[]`) are editable only for rows whose
`segment_index` is in the future.

---

## 3. How the executor picks the change up

**Mechanism: a generation counter, polled by the control task — the same shape
as the existing mid-run zones-config reload, deliberately.**
`reload_config_if_changed()` (`profile_executor.c:260`) compares
`zones_config_generation()` against `s_exec.config_generation` once per tick
and re-reads on a change. The live editor gets its own counter,
`profiles_live_generation()`, bumped when a working copy is committed, polled
immediately alongside it, and serviced by `reload_live_profile()` in a new
`profile_executor_live_reload.c` — a sibling of
`profile_executor_config_reload.c`, which exists for exactly this reason.

**Why there is no discontinuity.** The ramp state is not in the profile. The
commanded setpoint, its baseline, the segment index and the segment's elapsed
time all live in `s_exec`, and the swap copies *only* `s_exec.profile`. The
next tick therefore continues from the setpoint it was already commanding,
toward the new `target_c`, at the new `ramp_c_per_hr`. The slope and the
destination change; the setpoint itself does not jump. PID state is
deliberately **not** reset — unlike a control-mode change, where
`reload_zone_config()` correctly restarts the PID cold because the output
means something different, a setpoint edit has a perfectly meaningful
handover, and resetting the integrator would produce the transient this
feature is supposed to avoid. `segment_index`, `segment_elapsed_s`,
`io_segs[]` and the firing-stats accumulators are untouched.

Because section 2 refuses any edit at or before the running index, the swapped
profile is always consistent with the executor's current position. The one
remaining case is a working copy whose `segment_count` no longer exceeds
`segment_index` (every future segment deleted): the run completes at the end
of the current segment. The index is clamped, never allowed to walk past the
array.

The swap happens under `s_exec.lock`, in the control task, from the tick — not
from the httpd task. The HTTP handler only writes storage and bumps the
counter.

---

## 4. Where the working copy lives

**Recommendation: in a real user profile slot, written through the existing
`profiles_http_save()` path.** It is an ordinary profile from the moment it is
forked.

This is the recommendation because everything it needs already exists on that
path and nothing has to be rebuilt: NVS persistence, the per-slot rev
counters, the `cfg` LittleFS dual-write (`profiles_cfg_fs.c`), the
`PROFILE_VERSION` migration chain, and export/import. **It survives an ESP
reboot mid-firing by construction**, which the owner's end-of-firing prompt
requires — the prompt can be raised hours later, after a brownout, and the
edit is still there.

A RAM-only scratch copy was considered and rejected twice over: it loses the
operator's work on a reboot, and it forks the persistence path, which the
"one implementation" requirement forbids.

The cost is a slot. `PROFILES_MAX_COUNT` is 8, and a fork consumes one.
**When no slot is free the fork is refused**, with an error telling the
operator to delete a profile first — refused at fork time, before any edit is
accepted, never half-way through one. See the owner decision on raising the
count.

Alongside it, one small NVS record holds the pending decision (key
`live_edit_v1`, 12 characters, inside the 15-character limit —
`check_nvs_key_length.ps1` enforces this):

```
{ version, origin_id, working_id, origin_is_builtin, pending, origin_name[16] }
```

It is written **at fork time**, not at firing end. That is what makes
section 5 work.

---

## 5. Abort, trip, or reboot before the prompt

Nothing is lost in any of those cases, because the working copy is already a
persisted profile and the pending record is already written.

**Who asks, and when:** the web UI. `GET /api/profile/live` reports
`pending_decision` whenever the record says pending and the executor is not
RUNNING. The page raises the prompt on that condition alone — it does not care
*how* the run ended. A DONE, a HALTED, a FAULTED trip and a reboot that
interrupted the firing all land in the same state and all raise the same
prompt. This is deliberate: making the prompt a consequence of a clean
completion would lose it in precisely the cases where the operator most wants
to keep the edit that was mid-flight.

Because this is web-only by requirement, a board whose operator never opens
the browser again keeps the working copy as an ordinary, named-by-derivation
profile forever. That is safe: nothing is lost and nothing is overwritten. The
LCD gets no page, no menu entry and no prompt.

The record is cleared only by an explicit decision — save-as, overwrite, or
discard. Discard deletes the working slot through the existing
`profiles_http_delete()`.

---

## 6. "Is this a factory profile", decided on the server

**It is structural, not a flag, and that is why it is trustworthy.** Builtin
schedules live in a `const` table in `.rodata` with their own id range
starting at `PROFILE_BUILTIN_ID_BASE` (128); user slots are 0..7. There is no
writable storage behind a builtin id at all, and `profiles_builtin.h` says so
explicitly — "you cannot erase a const table in flash."

The overwrite handler therefore refuses, with 403 and before touching any
storage, whenever `origin_id >= PROFILES_MAX_COUNT`, and independently
whenever the persisted record's `origin_is_builtin` is set. Both checks read
server-side state, never anything from the request body: the request names no
origin at all, it only says "overwrite", and the firmware resolves what that
means from its own record. A forged or replayed request cannot express an
origin to attack.

The browser hiding the button is cosmetic and is stated as such in the page.

Note the common case this produces: firing a builtin directly and then editing
it forks from a builtin origin, so **overwrite is never offered for that
firing** — only save-as. That is correct and is what the owner asked for.

---

## 7. Temperature and rate bounds

Enforced **server-side, at the moment the edit is accepted, before the working
copy is written and before the generation counter is bumped** — and then a
second time inside the executor before the swap is adopted.

At accept time, for every ZONE_RAMP segment against every zone in the running
`zone_mask`:

| bound | source | on violation |
|---|---|---|
| `target_c` <= zone `max_temp_c` | `zones_config_get_temp_limits()` | **400, refused**, naming segment, value and ceiling |
| `target_c` <= `PROFILE_TARGET_C_MAX` (2015 °C) | `profiles_http_internal.h:64` | 400, refused (input sanity, as today) |
| `ramp_c_per_hr` <= zone `max_ramp_c_per_hr` | `zones_config_get_max_ramp()` | **400, refused** |
| `ramp_c_per_hr` within 20% of that ceiling | `PROFILE_RAMP_WARN_FRACTION` | accepted with a warning, as today |
| zone `max_temp_c == 0` (uncommissioned) | — | refused: there is no ceiling to check against, and a missing ceiling must never read as an infinite one |

**The zone-ceiling rule is deliberately stricter here than at ordinary save
time.** `profile_exceeds_zone_ceiling()` treats exceeding the ceiling as
advisory when saving, and that is right: profiles are portable between kilns,
and only *starting* one enforces the ceiling
(`profile_executor_run.c:297-396`). A live edit is not portable. It edits a
profile that is firing *this* kiln, right now, and its result reaches the
elements within one tick — so the start-time rule applies at edit time
instead, as a hard refusal. This is the owner's requirement, and it is the
central asymmetry between the two editors.

**Relationship to the Pico's `abs_max_temp_c`.** This feature never writes it,
never reads it as its own limit, and never derives anything from it. It is an
independent backstop on the other processor, owned by `safety_ceiling_sync.c`,
and the standing mirroring invariant is that a zone's `max_temp_c` is never
above it. So an edit that passes the zone-ceiling check above cannot exceed
the Pico's ceiling, and **nothing in this feature can tighten it either** —
there is no code path from the live editor to any Pico parameter. That is a
requirement, and it is met by having no such path at all rather than by a
check.

**Second enforcement, at pickup.** The executor re-runs the same shared
validator against the *live* zone configuration, under `s_exec.lock`, before
adopting the swap. This closes the window in which a ceiling is lowered, or a
kiln configuration applied, between accept and pickup — and it makes the
feature robust even to a working copy that somehow reached storage without
passing validation. A copy that fails is **not adopted**; the run continues on
the last good profile, an ERROR is logged naming the failing segment, and the
page surfaces the refusal. The run is not aborted by a failed pickup: refusing
a change is not a reason to stop a firing.

**If the ceiling itself is lowered mid-firing below the running setpoint**,
this feature does not introduce that path and must not quietly "fix" it. The
pickup validator refuses to adopt and the page shows the conflict; what the
*firing* does about a ceiling that is now below its own setpoint remains with
guard 5 and the Pico, exactly as it is today. Whether that existing behaviour
should become an active abort is a real question, but it is not this feature's
question — see Owner decisions.

---

## 8. One implementation, shared with the existing profile editor

The requirement is that a change to one changes both. Concretely, here is what
is reused and what has to be factored out.

**Reused unchanged.** `profiles_http_save()`, `profile_encode_current_blob()`,
`profile_decode_blob()`, `nvs_save_slot()`, the per-slot rev array and the
whole `profiles_cfg_fs.c` dual-write. Because the working copy is an ordinary
slot, it inherits migration, dual-write and export with no new code.

**Must be factored out — this is the actual work of the requirement:**

1. `parse_profile_fields()` (`profiles_edit_http.c:29`) is `static`. Widen it
   non-`static` through `profiles_http_internal.h` and have the live-edit
   handler call it, so both editors decode the identical
   x-www-form-urlencoded shape. Two decoders would drift on the first new
   field. Per the CLAUDE.md rule on widening a `static`, prefix-rename it
   (`profiles_parse_profile_fields`) even if the grep is clean.
2. The validation body inside `profile_post_handler()` becomes
   `profiles_validate_candidate(const profile_t *, profile_validate_mode_t,
   warnings, err, cap)`, where the mode selects advisory-vs-hard for the
   zone-ceiling rule (section 7) and nothing else. **This single function is
   what the save handler, the live-edit handler and the executor's pickup
   check all call** — three call sites, one rule. It takes a `profile_t` and
   reads zone config; it touches no httpd state, so it host-tests directly.
3. The browser segment editor — `rampFieldsHtml()`, `ioTargetOptionsHtml()`,
   `ioFieldsHtml()`, `renderSegFields()`, `checkRamp()`, `updateZoneCeiling()`
   — is inline in `profiles_page.html`, a single `EMBED_TXTFILES` blob. To
   share it, it moves to a served `profile_editor.js` next to the existing
   `app.js`/`commissioning_shared.js`, included by both pages. This is the one
   genuine refactor cost, and it is worth paying: copying the segment editor
   into a second page is exactly the two-copies-of-one-fact drift this repo
   keeps getting bitten by.

**What genuinely cannot share a path, with the reason in each case:**

- *Commit semantics.* Creation writes the slot the user chose. A live edit
  writes the slot the firmware chose at fork time and bumps a generation
  counter. Same validation, same encoder, same storage call — different
  target resolution and one extra side effect. Sharing beyond the validator
  would mean passing a mode flag through the storage layer for no gain.
- *Frozen rendering.* Past and current segments render disabled, with the
  reason, and the current one renders partially disabled per section 2. There
  is no analogue at creation time, so this is new code in the shared editor
  module, driven by an `editable_from_segment` parameter that the creation
  page simply passes as 0.
- *The decision flow* (fork / decide / overwrite confirmation) has no
  counterpart in the creation page at all.

**Sequencing dependency — do not edit these out from under another session.**
`profiles_http.c`, `profiles_edit_http.c` and `profiles_page.html` are the
same files the kiln-profiles work (`docs/KILN_PROFILES_PLAN.md`) and the
in-flight favorites work touch. The factoring in items 1-3 above should land
*after* those settle, or be coordinated with them; it is a pure extraction
with no behaviour change, so it rebases cleanly, but it rewrites large
contiguous regions of two files and will conflict loudly if done concurrently.

---

## 9. Interaction with favorites and kiln configs

**Favorites.** There is no favorites code in the tree today — the work is
another agent's and is mid-flight. The touchpoint is slot identity: a fork
consumes a free slot and an overwrite replaces a slot's *content* in place,
never renumbering anything. So a favorite keyed by slot id keeps pointing at
the right recipe across an overwrite, which is the desired behaviour. Two
things the favorites work needs to know, stated here rather than edited into
their files: a live fork can consume the last free slot (so "create a
favorite" can fail for a reason unrelated to favorites), and a discarded
working copy deletes a slot (so a favorite must tolerate its target
disappearing).

**Kiln configs.** `kiln_cfg_store_apply()` already refuses during a firing
(`kiln_cfg_store.c:767`), so a configuration swap cannot race a live edit in
the obvious way. The remaining interaction is the ceiling: an applied
configuration changes `max_temp_c`/`max_ramp_c_per_hr`, and section 7's
pickup-time re-validation is what makes that safe rather than a
time-of-check/time-of-use hole. No file in that feature needs changing.

---

## 10. HTTP surface

All five routes are ADMIN. Each needs a row in
`firmware/KilnFW/App/drivers/http/route_tier_table.h` **in the same change**,
or `tools/check_route_tier_coverage.ps1` fails the build fail-closed.

| Route | Body | Notes |
|---|---|---|
| `GET /live_profile` | — | the page. ADMIN, matching `/profiles` |
| `GET /api/profile/live` | — | `{active, origin_id, origin_is_builtin, working_id, editable_from_segment, pending_decision, last_refusal}` |
| `POST /api/profile/live/fork` | none | origin is whatever is running; the request names nothing. Idempotent — a second call returns the existing working copy. Refuses if no slot is free, or if nothing is running |
| `POST /api/profile/live` | the same form `POST /api/profile` takes | validate → write working slot → bump generation. 400 on a bound violation naming segment/value/limit; 409 on a difference at or before the running segment |
| `POST /api/profile/live/decide` | `action=save_as&name=…` \| `action=overwrite&confirm=1` \| `action=discard` | overwrite: 403 on a builtin origin, 400 without `confirm=1` |

The end-of-firing prompt adds no route: the page polls `/api/profile/live`.

**Persistence format.** The working copy is a standard profile blob at the
current `PROFILE_VERSION` — no new format, no schema bump, and therefore no
new migration and no rollback hazard. The only new persisted object is the
fixed-size `live_edit_v1` record in section 4, versioned, discarded rather
than migrated on a version mismatch (the `run_state` precedent: losing a
pending-decision record costs one prompt, mis-parsing one would act on a
wrong origin id).

---

## 11. Test coverage

Host tests, each negative-tested, and each negative test ending in a **forced
full rebuild** rather than a hand-restore plus an empty `git diff` — the
poisoned-binary lesson from `ed854ac5`/`ba230bca`.

- `profiles_validate_candidate()` in hard mode: target above the zone ceiling
  refused; ramp above the zone ceiling refused; `max_temp_c == 0` refused;
  `PROFILE_TARGET_C_MAX` refused; the 20% warning band still only warns. Same
  inputs in advisory mode still save, proving the two editors share one
  function and differ only by mode.
- The edit-window rule: a difference at an index below `segment_index`
  refused; `seg_kind` changed on the running segment refused; `target_c`
  changed on the running segment accepted; a future segment deleted accepted.
- The pickup function, written pure (profile in, executor-shaped state in and
  out, no FreeRTOS) so it is directly callable: asserts `segment_index`,
  `segment_elapsed_s`, `io_segs[]` and the stats accumulators are unchanged
  across a swap, and that the commanded setpoint is continuous.
- Builtin-origin overwrite refusal asserted **at the decision layer, not only
  the handler**. `profiles_http.c` is one of the few handlers that links into
  the host suite, but the refusal must not depend on that — a rule that only
  exists inside a target-build-only handler is a rule no host test can
  defend.
- The `live_edit_v1` record through encode/decode, including a
  version-mismatch discard.

Also required, and easy to forget: `check_route_tier_coverage.ps1` rows,
`check_nvs_key_length.ps1` for the new key, and — since this adds no task —
no `check_stack_margin_registration.ps1` work, but the new httpd handler must
keep its locals off the 8 KB httpd stack (the standing big-locals-on-the-httpd-stack
hazard); the candidate `profile_t` goes on the heap, as the existing handlers
already do.

**Cannot be tested without a real firing**, and must be recorded as unverified
until it is: that the setpoint is genuinely continuous through a pickup on
hardware (host tests prove the arithmetic, not the elements); that a mid-run
swap does not disturb ramp-lock across more than one real zone; that the
end-of-firing prompt appears after a genuine DONE hours later; and that a
pickup refused by the ceiling re-check leaves a real firing running
undisturbed.

---

## 12. Owner decisions needed

1. **Slot budget.** A live fork consumes one of the 8 user profile slots. Is
   refusing the fork when all 8 are full acceptable, or should
   `PROFILES_MAX_COUNT` be raised? (Raising it touches the `used_bitmap`
   uint8_t, the UART protocol and the LCD id space, so it is not free.)
2. **A ceiling lowered mid-firing below the running setpoint.** This feature
   refuses to adopt an edit in that state and leaves the firing to guard 5 and
   the Pico, unchanged. Should that existing behaviour instead become an
   active response (abort the firing, or clamp the setpoint)? It is a
   pre-existing question this feature makes visible, not one it creates.
3. **Name collisions on save-as.** Profile names are not unique today. Refuse
   a duplicate name at save-as, or allow it?
4. **Editing while PAUSED and while FAULTED.** Recommendation: allow while
   PAUSED (the run is still live and the operator is most likely to want a
   change there), refuse while FAULTED (the run is over; the pending-decision
   prompt is the right surface, not the editor). Confirm.
5. **Web auth and the prompt.** If authentication is on and the firing ends
   with nobody logged in, the prompt simply waits until an admin logs in —
   the working copy is safe in the meantime. Confirm that waiting is
   acceptable rather than, say, auto-keeping the copy under a derived name.
