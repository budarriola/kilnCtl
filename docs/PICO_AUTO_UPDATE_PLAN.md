# The ESP application updates the Pico automatically on boot

> **Status:** plan · **Opened:** 2026-09-16. Nothing here is implemented. No firmware was changed, no board was flashed, and no `.kicad_*` file was touched. Board access during drafting was read-only (`safety_get_status`, `safety_get_commissioning`).

Owner requirement, verbatim: *"The esp should always in the real fw check the pico version on boot and update it if needed automatically"*. Read precisely: **"the real fw" is the application image**, explicitly not the recovery image, which by the same day's decision carries no Pico firmware and no safety link at all (`docs/OTA_SINGLE_SLOT_PLAN.md:110`); **"always on boot"** is unconditional in the application boot path; **"automatically"** means no operator action.

## 0. The tradeoff, stated first

Today a Pico update is an operator action: somebody uploads a `SaftyFW` binary to `POST /api/ota/pico`, which stages it and starts the relay (`firmware/KilnFW/App/drivers/http/ota_http_pico.c:69`). After this change the ESP application carries the Pico image it expects, compares it against what the Pico reports at boot, and reflashes the Pico on its own when they differ.

What that buys: the two processors can never sit at mismatched builds without someone noticing, and nobody has to remember to update the safety processor after an ESP OTA.

What it costs, and this is the whole of the cost: **the ESP now reprograms, unattended, the one processor whose entire purpose is to be an independent second opinion.** Every mitigation below exists for that single sentence. Two consequences are accepted deliberately rather than engineered around:

1. **A boot that decides an update is needed is a boot that cannot fire.** The kiln is unavailable for the update window and, if the update fails, until somebody repairs it at the bench. That is the wanted direction — a Pico of unknown state with heaters available is never acceptable, an unfirable kiln always is.
2. **The ESP application image and the Pico image become one versioned unit.** Rolling the ESP application back rolls the Pico back with it. See §8 (Q7); this is chosen, not overlooked.

## 1. What already exists — verified against `origin/main` today

Most of this feature is built. The honest gap is small, and this plan does not re-plan any of the following.

| Already built | Where |
|---|---|
| Whole ESP→Pico transfer: `UPDATE_BEGIN`/`DATA`/`END`/`ABORT`/`STATUS`, broadcast + gap-report retransmit, CRC verified by read-back from flash, progress phases | `firmware/KilnFW/App/drivers/net/ota_pico_relay.h:117`, `:171`; `firmware/CommonFW/docs/UPDATE_PROTOCOL.md` §4 |
| The `pico_img` staging partition, looked up by name so no code tracks its offset | `firmware/KilnFW/partitions.csv:272`; `firmware/KilnFW/App/drivers/net/ota_pico_relay.h:115` |
| Interlock snapshot check (idle, no autotune, no heater commanded, per-zone temperature under a ceiling, link up) | `firmware/KilnFW/App/drivers/net/ota_interlock.h:240`, ceiling constant at `:65` |
| Single cross-processor update mutex, and the asymmetric ownership rule for the Pico path | `firmware/KilnFW/App/drivers/net/ota_pico_relay.h:13` |
| The Pico's build identity arriving at the ESP unsolicited at boot: `protocol_version`, `min_compatible`, `dirty`, `commit`, `datetime`, `boot_id`, `config_version`, `config_crc` | `firmware/CommonFW/include/kilnlink/kilnlink_fw_version.h:67` |
| Pico config store physically outside both application slots | `firmware/SaftyFW/bootloader/flash_layout.h:74`, `:77` |
| ESP-side cache of all 68 Pico commissioning params, CRC-driven refetch, and confirm-by-read-back write-back | `firmware/KilnFW/App/drivers/safety/safety_cfg_store.h:79`, `:222`; `firmware/KilnFW/App/drivers/safety/safety_cfg_write.h:60`, `:125` |
| Ceiling-sync ordering (raise the Pico first) and link-up reconcile | `firmware/KilnFW/App/drivers/safety/safety_ceiling_sync.h:60`, `:68`, `:114` |
| Config-divergence comparison, already consumed by four surfaces | `firmware/KilnFW/App/drivers/safety/config_divergence.c:134` |
| The persisted, read-back-verified, bounded-retry counter pattern this plan must copy | `firmware/KilnFW/App/drivers/persist/boot_guard.c:213`, `:313`, `:348` |

**The gap is three things and one prerequisite.**

- **G1. The ESP has no Pico image at boot.** `pico_img` is written by exactly one thing, the HTTP upload handler (`firmware/KilnFW/App/drivers/http/ota_http_pico.c:185`), and read by exactly one thing, the relay (`firmware/KilnFW/App/drivers/net/ota_pico_relay.h:115`). No `SaftyFW` image is embedded in the ESP application build — the only `EMBED_*` content is gzipped web assets. A freshly OTA'd ESP has whatever a human last uploaded, or nothing.
- **G2. The ESP has no notion of an *expected* Pico version.** It knows what the Pico *is* (`kilnlink_fw_version.h:67`). There is no constant, file or field anywhere in the tree saying what it *should* be, so "if needed" is currently undecidable. `UPDATE_BEGIN`'s `version` field is a fixed placeholder string, not a real version.
- **G3. There is no boot-time trigger and no persisted attempt bound.** Nothing in the boot path (`firmware/KilnFW/App/main_control_bringup.c:60` starts the link; `firmware/KilnFW/App/main_boot_early.c:415` reads recovery mode) consults the Pico's version for update purposes, and no counter bounds how many times a wrong decision may re-attempt across reboots.
- **P1, prerequisite, not new work.** This bench Pico still runs `SaftyFW.elf` flashed directly, not booted through the two-slot bootloader; the metadata sector reads as ordinary code rather than a `KLN1` record (`firmware/SaftyFW/TODO.md:692`). Until the bootloader plus a slot image are flashed by `debug_program(peer="pico")`, a perfectly relayed image lands in a slot the boot vector never consults, and the whole feature is untestable end to end.

## 2. The design

**Image source (closes G1): embed one `SaftyFW` slot image in the ESP application build.** The build embeds the matching `SaftyFW_slotA` binary plus its length, CRC32 and a version string. At boot, when and only when an update is needed, the ESP copies the embedded bytes into `pico_img` and starts the existing relay unchanged. The relay keeps its single reader contract and its partition lookup; `pico_img` gains a second writer, which is the entire new write path.

Rejected alternative: teach the relay to read straight out of embedded `.rodata`. It saves one 95 KB copy and a partition erase, and costs a change to the one module whose wire behaviour is exercised and recorded. Not worth it.

Cost: the ESP application grows by the Pico image, about 95 KB against today's 2.29 MB. Irrelevant under the 8 MiB slot of `docs/OTA_SINGLE_SLOT_PLAN.md:22`, and a real constraint worth naming if that plan does not land first.

**The decision (closes G2): exact match on an identity string, not an ordering.** The build stamps the expected Pico identity — the `SaftyFW` commit the embedded image was built from — into the application. At boot the ESP compares it against the `commit` the Pico already reports (`firmware/CommonFW/include/kilnlink/kilnlink_fw_version.h:67`). Equal, and `dirty == 0`: nothing happens, no flash is touched, no counter moves. Unequal: update.

Why an exact match and not "newer than": there is no monotonic version number on the wire to compare, only a commit string; inventing an ordering means inventing a version field on both sides and trusting it. Exact match makes the ESP application and the Pico image one tested pair, which is also the answer to Q7. A `dirty` Pico build always mismatches, which is correct — a hand-built Pico is not a known state.

**The trigger (closes G3): in the application boot path, after the link is up, before anything can fire.** Placed after `safety_link_start()` (`firmware/KilnFW/App/main_control_bringup.c:60`) so a version has actually arrived, and gated on `!ctx->recovery_mode` in the same style as the subsystem starts at `firmware/KilnFW/App/main_control_bringup.c:268`. In-application recovery mode is being deleted by the OTA plan; until then, not updating in it is the safe reading.

## 3. Q1 — the Pico is not guarding during the update

**Nothing is bypassed.** The link going quiet during a relay trips `SAFETY_FAULT_SRC_SAFETY_LINK`, and every relay-on path is blocked while it is asserted (`firmware/CommonFW/docs/UPDATE_PROTOCOL.md:105`). The Pico independently trips link-dead and holds K4 open. `safety_link_set_update_in_progress()` changes only which log line is emitted and does not touch the fault bit (`firmware/KilnFW/App/drivers/net/ota_pico_relay.h:68`). **This plan adds no update-mode carve-out to `relay_authority_on_blocked()` or anywhere else, and no relay is ever driven outside `kiln_io_owner`.**

So the heating block during the window is not new work — it is a property already there, and the work is to not break it. One thing is added: the boot-time updater calls the existing interlock check (`firmware/KilnFW/App/drivers/net/ota_interlock.h:240`) before starting, so it refuses rather than relying solely on the link-dead trip. Belt and braces, in that order.

Residual, inherited from `ota_interlock.h`'s own documented gap: a relay held on by a manual or rule-driven path with no profile running is not visible to the snapshot. At boot that state cannot exist yet, which is precisely why the boot path is the safest place this feature could live.

## 4. Q2 — never during a firing

An ESP reboot can land inside a live firing, so "we are in the boot path" is not by itself proof the kiln is idle.

**Rule: the updater runs before any firing may start or resume, and if a firing is running, paused, or resumable, it does not update at all.** Concretely: check for a running, paused or resumable profile *before* the profile executor is allowed to resume; if any exists, record "deferred: firing present" and return without touching flash. The next boot that comes up idle performs it.

**Refusing to fire is not the behaviour here.** A mismatched Pico is not by itself proof of an unsafe Pico, and blocking a firing on a version string would strand a kiln mid-schedule for a cosmetic difference. Deferring is the weaker and correct action. The one exception already exists and is not changed: a config-divergence or link fault blocks heat on its own merits.

**How the operator learns:** the deferred/attempted/failed state is exposed on the existing status surfaces the relay already feeds (`ota_pico_relay_get_status()`, `firmware/KilnFW/App/drivers/net/ota_pico_relay.h:183`), plus a boot log line. No new alarm channel is invented — there is no GUI fault-text surface in this codebase yet.

## 5. Q3 — `abs_max_temp_c` equality, and the Pico never unarmed

**The favourable finding first: a Pico firmware update does not wipe its configuration.** The config store sits outside both application slots, and `flash_layout.h` says so in those words (`firmware/SaftyFW/bootloader/flash_layout.h:74`, offset at `:77`, slot A at `:70`, metadata at `:64`). `abs_max_temp_c` and the arming state survive an update by construction. On the bench board this is observable: `config_crc` matches live and S1 reads `abs_max_temp_c=80C ARMED` today.

So there is no restore to perform in the normal case, and the work is verification, not restoration:

1. After the Pico reboots into the new image, wait for `FW_VERSION` and compare `config_crc` against the value cached before the update (`firmware/KilnFW/App/drivers/safety/safety_cfg_store.h:222` — the CRC is the authority, never the cache).
2. Run the existing link-up ceiling reconcile (`firmware/KilnFW/App/drivers/safety/safety_ceiling_sync.h:114`) and the existing divergence check (`firmware/KilnFW/App/drivers/safety/config_divergence.c:134`). A mismatch is a fault: alarm and heaters disabled, exactly as today. No new policy.
3. Only if the config genuinely did not survive, write it back from the 68-param cache with confirm-by-read-back (`firmware/KilnFW/App/drivers/safety/safety_cfg_write.h:125`), **raising the Pico's ceiling first and only then the ESP's** (`firmware/KilnFW/App/drivers/safety/safety_ceiling_sync.h:60`, `:68`). The Pico's ceiling is never left tighter than the ESP's.

**The one real residual risk, named rather than hidden:** a future `SaftyFW` that bumps `CONFIG_STORE_FORMAT_VERSION` migrates forward but deliberately forces `calibration_missing` true (`firmware/SaftyFW/src/config_store.h:68`), and a record newer than the firmware is refused outright (`:72`). An automatic update across such a bump therefore *can* de-arm the CT guards even though nothing was erased. **Mitigation: an image whose config format version differs from the running Pico's reported `config_version` (`firmware/CommonFW/include/kilnlink/kilnlink_fw_version.h:67`) is not eligible for automatic update.** It is deferred and reported, and a human does that one at the bench. Step 5 in §9 builds this gate; without it §6 is not answered.

## 6. Q4 — the Pico's CT normals

`i_normal_a[0..2]` are params `0x031A`–`0x031C` (decimal 794–796) in the Pico's own config store (`firmware/SaftyFW/src/config_params.c:128`, write path at `:496`). Because that store is outside both slots (§5), **an automatic update preserves them.** On this bench board they are unset and S14/S15 read DORMANT, so nothing is at stake here today; on a calibrated kiln a wipe would silently de-arm those guards and force the recalibration the owner has said they do not want.

Preservation here is structural, not procedural: the plan's update path never writes the config sector at all. The two ways they could still be lost are both covered — the format-version gate of §5, and the pre-existing single-copy erase window recorded in `firmware/SaftyFW/bootloader/flash_layout.h:74`'s neighbourhood, which this feature does not enter and does not fix. A separate pass is concurrently making the backup export round-trip these fields; nothing here touches those files.

## 7. Q5 and Q6 — a failed update, and a wrong decision

**Q5, failure.** The Pico's two-slot bootloader is why a failed relay is survivable: the image is written into the inactive slot and the metadata record is only flipped once the image verifies by read-back from flash. A transfer that dies mid-way leaves the previously-good slot still selected, so the Pico comes back on its old firmware and keeps guarding. A failure *after* the flip that will not boot leaves the Pico dead, the link dead, and K4 open — the kiln cannot heat. Both outcomes are acceptable; an unarmed Pico with heaters available is not reachable from either.

The ESP may retry, bounded as below. **The physical escape is `debug_program(peer="pico")` over the SWD probe**, which is a different path from this feature and is the recovery-of-last-resort already recorded in `firmware/CommonFW/docs/UPDATE_PROTOCOL.md` §5. This plan uses the ESP-driven link relay, never the host programmer; the host programmer is only how a human repairs a board that this feature broke.

**Q6, a wrong decision.** A version check that is wrong — a mis-stamped expected string, a Pico that reports a commit the ESP will never match — retries forever, and a RAM-only counter bounds nothing because it resets on the reboot the update itself causes. This exact shape bricked the board three times in the `boot_guard` area.

The bound, copying `boot_guard.c`'s hard-won mechanics rather than re-deriving them:

- A counter persisted in NVS, **keyed by the pair (expected identity, observed Pico identity)**, so a new mismatch gets its own fresh budget and a stuck one cannot ratchet.
- Every write read back before being reported as success, with one bounded erase-then-retry — `persist_count()` / `verify_persisted_count()` / `erase_then_persist_count()` (`firmware/KilnFW/App/drivers/persist/boot_guard.c:213`, `:313`, `:348`). **A bare `HAL_OK` from an NVS write is not evidence in this module's neighbourhood; 2026-09-08 proved the write can report success while the value never changed.**
- Budget: **3 attempts per pair.** On exhaustion the ESP stops attempting, reports "Pico update abandoned after 3 attempts", and leaves the Pico alone. It does not refuse to fire on that ground alone — the existing divergence and link faults already decide whether heat is allowed.
- The counter clears only on a verified success, i.e. the Pico reporting the expected identity.

## 8. Q7 — downgrade and rollback

If the ESP application is rolled back to an image carrying an older embedded Pico image, the exact-match rule of §2 means **it will push that older image to the Pico.** That is the intended behaviour and the reason exact match was chosen: the pair was built and tested together, and a rolled-back ESP running against a Pico from a newer, now-rejected build is a combination nobody has exercised. Rolling back the ESP should roll back the whole controller.

Two guards keep it from becoming a loop. The per-pair counter of §7 bounds a downgrade exactly as it bounds an upgrade. And the compatibility floor (frame ids 0x00–0x0F, `firmware/CommonFW/include/kilnlink/kilnlink_fw_version.h:20`) guarantees that even a mismatched pair can still exchange versions, so the ESP can always see what it is talking to; where both need updating, `UPDATE_PROTOCOL.md` §4's existing rule is to update the ESP first, which this ordering follows for free.

The case this plan does *not* cover: a downgrade across a `CONFIG_STORE_FORMAT_VERSION` bump, where the older `SaftyFW` refuses the newer config record outright (`firmware/SaftyFW/src/config_store.h:72`). The §5 gate blocks it from happening automatically, which is the right answer — that one needs a human.

## 9. Work plan, in order

Each step is independently verifiable. `(bench, human)` marks a step needing someone at the board.

0. **(bench, human) Close prerequisite P1.** Flash the two-slot bootloader plus a freshly built `SaftyFW_slotA` via `debug_program(peer="pico")`, so the Pico actually boots through the bootloader. Verify: the metadata sector reads a `KLN1` record, the link comes up, and `safety_get_commissioning` shows the same `config_crc` as before the flash. **Nothing past step 3 is testable until this is done** (`firmware/SaftyFW/TODO.md:692`).
1. **Stamp the expected identity.** Build-time constant for the `SaftyFW` commit, plus the embedded image's length and CRC32. Verify (host): a unit test compares the stamped identity against the embedded image's own build metadata and fails if they disagree. No board needed.
2. **Embed the image and add the `pico_img` writer.** A function that copies the embedded bytes into `pico_img`, erase-then-write, verifying by read-back CRC32 before reporting success. Verify (host + target build): image size accounted for in the map file; a target build passes. **Reconcile the CRC32 variant here** — the 2026-09-05/06 record shows the ESP-computed CRC32 and host zlib CRC32 disagreeing, so the embedded CRC must be produced by the same routine the reader uses, checked by a host test rather than assumed.
3. **The decision function, pure and host-testable.** Inputs: expected identity, the decoded `FW_VERSION`, profile state, interlock snapshot, persisted attempt count. Output: one of update / skip / defer-firing / defer-config-format / abandoned, each with a reason string. Verify (host): table-driven tests covering every branch, including `dirty`, `config_version` mismatch, count exhausted, and link never came up. **No flash, no board.** This is where the §5 format-version gate lives.
4. **The persisted counter.** Keyed by identity pair, read-back-verified, one erase-then-retry, per `boot_guard.c`'s helpers. Verify (host): a test that a failed write is never reported as success, and that a new pair gets a fresh budget.
5. **Wire it into the boot path.** After `safety_link_start()`, gated on `!recovery_mode`, using the existing mutex and the existing relay. Verify (target build + log): on a board already at the expected version, the boot log records "match, no action" and no flash write occurs.
6. **(bench, human) The real thing, once.** With the board idle and cold, stamp a deliberately different expected identity and let the boot path update the Pico unattended. Verify, in order: K4 open and no heat for the whole window; the relay reaches DONE; the Pico reboots reporting the expected identity; `config_crc` unchanged; `abs_max_temp_c` equal on both sides; S1 still ARMED; the attempt counter back to zero.
7. **(bench, human) The failure case, deliberately.** Interrupt a relay mid-transfer. Verify: the Pico comes back on its old slot still guarding, the counter incremented and persisted across the reboot, and a third failure ends in "abandoned" with no further attempts.
8. **(bench, human) The firing case.** Start a firing, reboot the ESP, and confirm the updater defers rather than updating, with the deferral visible on a status surface.

## 10. Owner decisions

Each carries one recommendation, not a menu.

**10.1 — Does an ESP rollback downgrade the Pico? Recommend yes**, as §8 describes. The ESP application and the Pico image are built and tested as one pair; a rolled-back ESP paired with a newer Pico is an untested combination, and the alternative needs a monotonic version field invented on both sides plus a policy for refusing it. The per-pair attempt bound is what makes it safe to say yes.

**10.2 — Attempt budget of 3. Recommend 3**, matching the existing 3-failure lockout convention in the OTA auth path so there is one number in the codebase rather than two. The value matters less than that it is persisted and read-back-verified; 1 would strand a board on a single transient link glitch, and anything above 3 buys nothing a human would not be needed for anyway.

**10.3 — A version-mismatched boot does not refuse to fire. Recommend deferring only**, per §4. A mismatched version string is not evidence of an unsafe Pico, and treating it as a firing block would strand a kiln on a cosmetic difference. Heat remains blocked by the checks that actually measure safety — divergence, link liveness, arming.

**10.4 — A config-format-version bump is excluded from automatic update. Recommend excluding it**, per §5. It is the one path where an update really can de-arm S14/S15, and the whole feature's premise is that the config is untouched. Excluding it keeps that premise true instead of adding a restore path that would have to be trusted unverified.

**10.5 — The ESP application grows by the embedded Pico image (~95 KB). Recommend accepting it.** It is the only way "on boot, no operator action" can work at all; a partition-only staging path means somebody has to have uploaded something. This does bind this feature loosely to the 8 MiB slot of `docs/OTA_SINGLE_SLOT_PLAN.md`; if that plan does not land, the headroom against the current 3 MB slot should be re-measured before step 2.
