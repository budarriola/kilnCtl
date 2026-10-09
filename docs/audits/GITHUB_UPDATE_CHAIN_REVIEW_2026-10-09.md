# GitHub release update chain review, 2026-10-09

Scope: the application side (`firmware/KilnFW/App/drivers/update/`: update_fetch, update_stage,
update_policy, update_url, update_http) and the recovery side
(`firmware/KilnFW_recovery/main/recovery_apply.c`, `recovery_apply_esp.c`, `recovery_http.c`),
reviewed against `docs/GITHUB_RELEASE_UPDATE_PLAN.md` on origin/dev after the stage-upload
hardening series F1-F7. Static review only: no build, no board. Release signing is out of scope
(owner decision: the correct-project check suffices).

## Findings

### MED-1: fetch-path verify borrows the largest free internal block about 500 times

`update_fetch.c:61,176` gives the stager a 16 KiB scratch buffer that lives in PSRAM (the work
struct is a PSRAM calloc in `fetch_task`). `update_stage_upload_finish` re-hashes the image from
flash through that scratch (`update_stage.c:329-359`, called at 379). `esp_partition_read` into a
non-DRAM buffer on SPI1 main flash cannot read directly
(`esp-idf/components/spi_flash/esp_flash_api.c:943-962`). It asks
`get_buffer_malloc` (`spi_flash_os_func_app.c:236-256`) for an internal bounce buffer of
`MIN(16384, largest free internal block)`. At the documented idle largest block of 9728 B
(`update_fetch_heap.h`, plan section 14), each of the roughly 500 reads of a 2 MB image
transiently takes the whole largest internal block. That draw is not in
`FETCH_HEAP_WORST_DRAW_BYTES`, and `heap_admit` does not run before WR_FINISH. While it is held,
any concurrent internal allocation bigger than the remaining fragments fails: httpd sessions,
the login KDF, and lwIP pbufs. The same class of hazard is already documented for the upload
path at `update_http.c:36-40`, which is why that path uses the internal static chunk buffer.
This is a plausible contributor to the unexplained min_free 8295 B dip (plan line 340 and the
"Limits" note in `update_fetch_heap.h`). It is not proven.

Fix: give the fetch stager an internal scratch of at most 4 KiB, either by reusing the
`ota_http_esp_chunk_buf` the claim already serialises or with a 4 KiB internal static, so reads
are direct. Alternatively, cap the stager read chunk at 4 KiB and count it in
`FETCH_HEAP_WORST_DRAW_BYTES`. Then log the current free and min_free around WR_FINISH, as WP8
gate (b) already asks.

### MED-2: fetched image not cross-checked against the manifest identity

The fetch path installs no stager gate (`update_stage.h:110-117`; `update_fetch.c:595` passes
NULL). As a result:

- The descriptor-version check runs only when `st->gate != NULL` (`update_stage.c:241-248`).
- The embedded identity record (`update_image_id_t`) is never read (`update_stage.c:260-267`).

The policy decision (`update_fetch.c:694-715`) uses only `release.json`'s `compat`
(`w->man.identity`). The asset's sha256 is checked against the manifest (`update_fetch.c:732-744`),
so the bytes are the ones the manifest names. However, nothing checks that the manifest's
zones_cfg/kilnlink/uart versions, semver and commit describe that image. A release-tooling error
(a stale or hand-edited `release.json`) would let a schema-lowering image pass the downgrade gate
as an upgrade. That is the zones_cfg rollback hazard the policy exists to stop. Plan section 5
(line 110) requires refusing an image whose descriptor project or commit differs from the
manifest. The project is checked (`update_stage.c:226-237`); the commit and version are not.

Fix: install a fetch gate (a static context; no allocation) that refuses `UPDATE_STAGE_ERR_POLICY`
unless:

- the embedded identity record is present and equal to `man.identity`'s three schema versions;
- the descriptor semver equals the manifest version (allowing a leading v).

Optionally, check the descriptor commit prefix against `man.identity.commit` where the build
embeds it. Host-test it with a deliberately mismatched manifest.

### LOW-1: stage upload checks the run gates before taking the claim

`update_http.c:309-315` checks the mode gate and the interlock, and only then takes the update
claim. Profile and autotune start test `ota_http_heat_blocked_by_update()` (`ota_http.c:568-627`),
which reads the claim. A run that starts in the window between the two checks is not seen by
either side. The upload then writes and erases about 2 MB of flash during a firing, which is the
thing `SYS_ACTION_STAGE_WRITE` (`system_mode_gate.c:142`) exists to stop. The upload never
re-checks the run while it receives. The fetch path is self-healing here, because
`heat_run_active` is re-checked per hop and every 8 chunks (`update_fetch.c:433-448,528-534`).

Fix: take the claim first, then run the mode-gate and interlock checks, and release the claim on
refusal. Alternatively, re-check `mode_gate_refuses` after the claim succeeds.

### LOW-2: the fetch job's writer calls have no timeout

`wr_call` waits on `wr_done` with `portMAX_DELAY` (`update_fetch.c:327-404`). A wedged flash
operation hangs the job, holds the update claim, and therefore blocks heating
(`fetch_busy_probe`, `update_fetch.c:1001`) until reboot. The 20 min job deadline is only checked
between hops.

Fix: use a bounded wait (for example, a few times the worst-case 64 KiB erase plus write time).
On timeout, mark the job failed and keep the claim held, reporting the stuck state rather than
racing the writer.

### LOW-3: a power cut during the first boot lands in recovery with the stage already gone

The recovery apply erases the stage header right after `set_boot` (`recovery_apply.c:173-176`).
Rollback is enabled (`KilnFW/sdkconfig.defaults:508-509`), so a reset before the new app marks
itself valid aborts it, and the board boots the factory slot (recovery). The new app is intact in
`app`, and `/api/recovery/exit` boots it, but `apply_status` will report "nothing is staged". This
is safe, but the operator path is undocumented.

Fix: add a docs-only note in plan section 4 that, after a first-boot cut, `recovery_exit` (not
re-apply) is the recovery action. Alternatively, erase the stage only from the application after
mark_valid. That is what the stale-stage clear already does (`update_http.c:588-643`), so the
erase at `recovery_apply.c:176` could simply be dropped.

### LOW-4: the TLS certificate validity dates are not checked

`CONFIG_MBEDTLS_HAVE_TIME_DATE` is unset (plan line 211), so expired or not-yet-valid
certificates in the chain are accepted. The fetch already requires a synced clock
(`update_fetch.c:216-222`). The owner has accepted this as a known item; it is listed only for
completeness.

No HIGH findings.

## Answers

### (a) Power cut at each step

| Step | Outcome | Evidence |
|---|---|---|
| Download, before the 320 B head passes its checks | The previous stage is intact. Nothing has been written, because the head is held back. | `update_stage.c:107-172` (begin does no flash access), `217-269` |
| Stage write | The header sector was erased once the checks passed and is rewritten last, so the stage reads blank. The app is untouched. The previous good stage is lost. | `update_stage.c:269`, `399-408` |
| Stage verify | Same as stage write. A verify failure erases the header explicitly. | `update_stage.c:374-385`, `399-408` |
| Recovery boot | Only reachable when no run is active and all relays are off. A cut before reboot changes nothing. | `system_mode_gate.c:117-140`, `ota_http_recovery.c:265` |
| Apply, before `set_boot` | `app` may be half-written, but otadata still points at the factory slot, so the board reboots into recovery. The stage is intact and verified, so the apply is retried. | `recovery_apply.c:124-169`; the stage erase happens only after `set_boot` (173-176) |
| Apply, between `set_boot` and the stage erase | The new app boots. The stale-stage auto-clear removes the leftover header after mark_valid. | `recovery_apply.c:173-176`, `update_http.c:588-643` |
| First boot | Pending-verify rollback, plus boot_guard (cleared in `apply_pre_boot` just before `set_boot`, `recovery_http.c:886-893`), sends a failing or interrupted first boot back to recovery. See LOW-3. | `recovery_apply_esp.c:128-136` |

### (b) Downgrade across a schema bump

Refused by default (`update_policy.c:136-151`, rule 6 in `update_policy.h`). It can be overridden
only with `allow_downgrade` plus a typed confirm equal to the tag or version
(`update_policy.c:260-270`; fetch `update_fetch.c:698-702`; upload `update_policy.c:301-358`).
`zones_cfg_lower` is surfaced to the caller (`update_fetch.c:713`, `update_http.c:287-289`).

For a hand upload, the schema versions come from the identity record embedded in the image. For
a fetch, they come from the manifest only (MED-2).

### (c) Allowlist on redirects

Enforced on every hop:

- Auto-redirect is disabled (`update_fetch.c:457`).
- Each `Location` goes through `update_redirect_check` (`update_fetch.c:489`). That applies the
  hop cap and the full `update_url_check` (`update_url.c` `update_redirect_check`).
- The URL must be lowercase `https://` only, with no userinfo, no IPv6 literal, and only port 443
  (`update_url_parse`).
- The host must be an exact, case-insensitive match against four fixed hosts
  (`update_host_allowed`), with no suffix or wildcard matching.
- A relative `Location` fails closed.
- The first URL is checked too (`update_fetch.c:424`).

### (d) Heap and PSRAM per step, and leaks

| Step | Internal RAM | PSRAM |
|---|---|---|
| Fetch admission | Requires 28672 B free and a largest block of at least 6144 B before each TLS session (`update_fetch_heap.h`). Aborts mid-body below 12288 B (`update_fetch.c:515`). | |
| Fetch internals | The `update_fetch_wr` stack (4096 B) plus a 2 KiB tx buffer, plus TLS handshake pieces (about 11 KB measured). | Task stack 12 KiB, the work struct with a 16 KiB scratch, and an API body of up to 256 KiB. |
| Fetch verify | The hidden bounce buffer (MED-1). | |
| Stage upload | The static 4 KiB chunk buffer (`update_http.c:124-127`). No heap. | |
| Stale-stage check | A 1 KiB buffer on the ota_confirm stack. | |
| Recovery apply | The 8192 B task stack, admitted only if free is at least 8192 + 8192 + 1024 (`recovery_apply_esp.c:200`). Context, progress and scratch are statics, and the reads are direct. | |

Leaks: none found.

- `fetch_task` error cleanup aborts PSA and the stager, stops the writer task, frees the work and
  body, and ends the claim (`update_fetch.c:754-793`).
- `wr_start` cleans up on its own failure path.
- The upload handler aborts the stager and ends the claim on a mid-body failure
  (`update_http.c:362-369`).
- Recovery apply allocates nothing after start.

### (e) Can a run be interrupted by stage or install?

Not by design.

- Stage upload, stage clear, and fetch download are refused 409 while a firing or autotune is
  running (`SYS_ACTION_STAGE_WRITE`, `update_http.c:156-168`; `update_fetch.c:888-933` via
  `update_http_gate_refuses`).
- `recovery_enter` additionally requires the relays off (`system_mode_gate.c:117-140`).
- Install happens only in the recovery image, which has no relay driver.
- In the other direction, a run cannot start while a stage or fetch holds the claim
  (`ota_http_heat_blocked_by_update`, which includes `fetch_busy`; `ota_http.c:561-627`;
  `heat_interlock.h`).
- The fetch also aborts itself if a run becomes active (`update_fetch.c:206-212,528-534`).
- The residual exposure is the narrow ordering window in LOW-1.

### (f) Is the hash re-checked before apply?

Yes. `recovery_apply_run` decodes the header and requires state VERIFIED
(`recovery_apply.c:78-91`). It then re-hashes the staged bytes from flash and compares them with
the header's sha256 before the first erase of `app` (99-109). It re-validates the chip and the
KilnCtrl project (111-122). After the copy, it hashes `app` against the same digest (157-166) and
runs `esp_image_verify` (167-169, `recovery_apply_esp.c:117-126`) before `set_boot`.
