# Update path robustness audit (2026-10-13)

Scope: the board-side GitHub release update path on origin/dev at `551722a76`,
device code only. The goal under audit is an update that runs unattended: the
board checks GitHub, downloads and stages the image, enters recovery, applies
it, and returns to the application, with no PC download and no human joining
the recovery AP. No board access was used (another agent owns the bench).

Files read: `firmware/KilnFW/App/drivers/update/update_fetch.c`,
`update_release.c`, `update_url.c`/`.h`, `update_stage.c`,
`update_stale_stage.c`, `firmware/KilnFW/App/drivers/http/ota_http_recovery.c`,
`firmware/KilnFW/App/drivers/persist/recovery_switch.c`,
`firmware/KilnFW_recovery/main/recovery_apply.c`, `sdkconfig.defaults`
(mbedTLS block), and the IDF mbedTLS port allocator.

The AUTOAPPLY design (`POST /api/update/install` plus a one-shot NVS record)
is not on origin/dev. Section 5 lists what it must handle.

Totals: 1 HIGH, 2 MED, 8 LOW.

## 1. Answers to the two specific questions

### Q1: does the redirect hop close and free the first TLS connection before opening the second?

**Yes.** In `http_get()`, a redirect response runs
`esp_http_client_close(c); esp_http_client_cleanup(c);` at
`update_fetch.c:656-657` before the loop goes round to the next hop's heap
admission and `esp_http_client_init()`. The open-failure path also cleans up,
at `update_fetch.c:574`. The two TLS sessions never coexist.

Evidence: the new host test `test_redirect_hop_frees_first_client`
(`firmware/KilnFW/App/test/test_update_fetch.c`). The fake HTTP client
(`stubs_update_fetch/fake_support.c`) now counts live client handles. The
test serves a 302 from the API asset URL to
`objects.githubusercontent.com` and asserts these things:
- the stage ends VERIFIED
- the second hop opened the Location URL
- at most one client handle was ever live
- no open happened while another handle was live
- every handle was cleaned up at the end

`build_host_tests.ps1 -Only update_fetch`: PASS. `test_update_fetch` ran 461
checks with 0 failed, and all 3 executables passed.

Negative test (`tools\negtest.ps1`, kilnfw-host ExpectPattern):

| Mutation | Edit | Verdict |
|---|---|---|
| M1_drop_hop_cleanup | remove `esp_http_client_cleanup(c);` at `update_fetch.c:657` | CAUGHT (compiled; assertions failed at `test_update_fetch.c:414-416`, RUN FAILURES) |
| M2_keep_redirect_client_live | run that cleanup only `if (!again)` | CAUGHT (compiled; assertions failed at `test_update_fetch.c:414-416`, RUN FAILURES) |

`real_tree_unchanged` was true for the run.

### Q2: would MFL (max fragment length) or smaller mbedTLS buffers cut peak internal heap?

**No.** `sdkconfig.defaults` turns on all four of these:
- `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`
- `CONFIG_MBEDTLS_DYNAMIC_BUFFER`
- `CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA`
- `CONFIG_MBEDTLS_DYNAMIC_FREE_CA_CERT`

`update_fetch.c:51-53` `#error`s if either of the first two is off. With
EXTERNAL_MEM_ALLOC, IDF's `esp_mem.c` sends every mbedTLS `calloc` to PSRAM,
which covers the record buffers, the SSL context and the certificate chain.
MFL or a smaller `IN/OUT_CONTENT_LEN` would therefore only shrink PSRAM use.

The internal-heap cost of a fetch comes from elsewhere:
- crypto DMA descriptors and bounce buffers (`esp_aes_dma_core.c`, `sha.c`)
- lwIP and Wi-Fi pbufs and the socket
- the 2048-byte HTTP client buffer (`FETCH_TX_BUF_BYTES`), which lands in
  internal heap because it is under `SPIRAM_MALLOC_ALWAYSINTERNAL` (8192)

Two further reasons not to pursue it:
- IDF `esp-tls` never negotiates MFL.
- Cutting `IN_CONTENT_LEN` below 16384 would break any server that ignores
  MFL, and the GitHub CDN does.

Recommendation: do not pursue. If internal heap must come down further, the
levers are the HTTP client buffer (move it to PSRAM via an explicit
`heap_caps_malloc` if the client API allows) and the UPDFX2/UPDFX3 admission
rules, not the TLS configuration.

## 2. Findings

### HIGH-1: the recovery image never applies the stage by itself (unattended goal not met)

- Where:
  - `ota_http_recovery.c:317` (`recovery_switch_select_boot()`)
  - `recovery_switch.c:44-75`
  - `firmware/KilnFW_recovery/main/recovery_apply.c`, which runs only on
    `POST /api/recovery/apply_staged`
- Scenario: the board stages a verified image and enters recovery with
  `recovery_enter`. `esp_ota_set_boot_partition(factory)` erases otadata, so
  the bootloader boots `recovery`. The recovery image is AP-only and serves
  unauthenticated HTTP on its own SoftAP; it has no station mode and no boot
  path that applies the stage. Nothing calls `apply_staged` unless a client
  joins the SoftAP, and its random passphrase is shown only on the LCD. A
  power cycle changes nothing, because otadata still points at factory.
- End state: **stuck in recovery** until a human joins the AP and calls
  apply_staged or recovery_exit. The board is not bricked: the app partition
  is untouched until an apply starts.
- Fix: AUTOAPPLY (section 5). This is the work the owner already asked for.

### MED-1: `recovery_boot` does not require an installable stage

- Where: `ota_http_recovery.c` around 198-330. The handler checks only that
  no update is in progress (:295) and that relays are off (:305) before
  `recovery_switch_select_boot()` (:317).
- Scenario: `recovery_enter` is called with the stage BLANK, mid-write, or
  holding an image that would fail recovery's checks (wrong project, or
  oversize for `app`). The board then sits in recovery with nothing to
  apply. Under AUTOAPPLY the same gap becomes a loop risk: enter, find
  nothing usable, and then do nothing or repeat.
- End state: **stuck in recovery** (today, needs a human). It is a harmless
  `recovery_exit` once someone is there.
- Fix: when the request is part of an install, refuse unless
  `update_stage_status()` reads VERIFIED with the expected size and sha.
  Keep the bare manual entry (an operator escape hatch) unconditional.

### MED-2: power loss during the recovery copy leaves `app` erased with otadata at factory

- Where: `recovery_apply.c:124-137` (`app_modified = true` from the first
  erase), `:173` (`set_boot`), `:176` (stage header erase).
- Scenario: power is lost, or `debug_reset` runs, after the first app erase
  and before `set_boot`. The app partition is partially written, and otadata
  still points at factory (as `recovery_enter` left it).
- End state: **recovery loop** (boots recovery every time; the app image is
  invalid) until the apply is triggered again. The stage is still VERIFIED,
  so a retry is safe and completes. Today that retry needs a human (see
  HIGH-1).
- Fix: AUTOAPPLY must be idempotent. On every recovery boot with a VERIFIED
  stage and a pending install record, re-run the apply, under a bounded
  attempt count (section 5).

Harmless neighbour: a power cut after `set_boot` and before the stage header
erase boots the new app with a stale VERIFIED stage that equals the running
image. `update_stale_stage.c` clears exactly this case after a confirmed
boot (its ELF sha and contents match the running app). End state: harmless.

### LOW-1: a new download destroys the old good stage as soon as its header passes

- Where: `update_stage.c:262-339` (`flush_head`); the header sector erase is
  at `:331`, after the image checks and the gate.
- Scenario: a VERIFIED stage exists, and a new download starts and passes
  `flush_head`. A mid-body failure follows: Wi-Fi drop, stall, cancel (`cancelled`),
  `sha256_mismatch`, `timeout`, or `low_heap_midrun`.
- End state: **harmless retry**. Nothing is staged, and the app is untouched.
  The previous good stage is gone, though, so an AUTOAPPLY flow must never
  start a re-download between "stage verified" and "enter recovery".

### LOW-2: GitHub 403, 429 and 5xx all collapse into `http_status`, with no backoff

- Where: `update_fetch.c:604`.
- Scenario: unauthenticated GitHub allows 60 requests per hour per IP. A
  scheduled checker, or several boards behind one NAT, gets 403 or 429. The
  board reports `http_status` and does not read `Retry-After`. An automatic
  retry loop would keep hitting the limit.
- End state: **harmless retry**, but a future automatic checker must back
  off (honour `Retry-After` / `X-RateLimit-Reset`, at least 1 h on 403/429).
- Fix: map 403/429 to `rate_limited` and 5xx to `server_error` so callers
  can tell them apart.

### LOW-3: a latest-only check with only prereleases on the repo reports `http_status`

- Where: `update_fetch.c:758` (the endpoint choice) and `:604`.
- Scenario: with `allow_prerelease` false, `/releases/latest` returns 404
  when every release is a prerelease. This repo's daily tags are
  `v1.0.0-pre.N`, so this is the normal state today.
- End state: **harmless**, but the text reads like a fault. There is no
  `no_stable_release` reason.
- Fix: map 404 on the latest endpoint to `no_stable_release`.

### LOW-4: header-phase timeouts and errors are labelled `bad_response`

- Where: `update_fetch.c:584-585`. `esp_http_client_fetch_headers()` returns
  0 (not negative) for a chunked body, so `clen < 0` covers only
  header-phase errors and timeouts.
- End state: **harmless retry**; the reason misleads anyone diagnosing a
  Wi-Fi drop. Suggest `header_timeout` / `header_failed`.

### LOW-5: buffers of a wedged writer job stay allocated until reboot

- Where: `update_fetch.c` writer-wedge path. This is deliberate: the wedging
  job's buffers are abandoned, and later jobs free their own.
- End state: **harmless**, at the cost of less PSRAM until the next reboot.

### LOW-6: expired certificate chains are accepted

- Where: `sdkconfig.defaults`, `CONFIG_MBEDTLS_HAVE_TIME_DATE` unset.
  `update_fetch.c:741` refuses with `clock_not_synced` until SNTP has synced.
- End state: **harmless** (documented). The image is still bound to the
  release's sha256 and to the four-host allowlist (`update_url.c:112-118`).

### LOW-7: `FETCH_LIST_PER_PAGE` 5 can miss a usable release

- Where: `update_fetch.c:59`, `update_release.c:576` (skip draft/unusable).
- Scenario: with `allow_prerelease`, more than 5 newer entries that are
  drafts or have no `KilnCtrl-<tag>.bin` asset hide the newest usable one.
- End state: **harmless**; it reports no update.

### LOW-8: cancel latency is up to about 10 s

- Where: `update_fetch.c:76-80` (`FETCH_HTTP_TIMEOUT_MS` 5000,
  `FETCH_EAGAIN_RETRIES` 1).
- End state: **harmless**. A caller waiting on cancel needs a deadline of
  more than 10 s.

### Checked and found sound

- **Heat interlock:** `heat_run_active` is checked before every hop
  (`update_fetch.c:544`) and every 8 body chunks. A firing or autotune that
  starts mid-download aborts the job. `recovery_boot` refuses with relays on
  (`ota_http_recovery.c:305`).
- **Concurrent jobs:** the atomic busy flag (`update_fetch.c:936`) gives a
  second job 409 `fetch_busy` (`:993`, `:1047`). `fetch_busy_probe` keeps OTA
  routes out while a fetch runs (`:1145-1167`).
- **Body bounds:**
  - JSON is capped at 256 KB in PSRAM (`FETCH_API_BODY_CAP`, `:58`)
  - the manifest is capped at 16384 (`:60`)
  - the image fails with `too_large` (`:671`, `:680`), `size_mismatch` at
    stage begin (`:690`), and `overrun` / `short_body` in the read loop
  - the job deadline is 20 min (`:75`)
- **Redirects:** at most 3 hops (`update_url.h:33`), each checked by
  `update_redirect_check` (`:594`) against the host allowlist.
- **Asset and release matching:**
  - only `KilnCtrl-<tag>.bin` matches (`update_release.c:524`, `:930`)
  - drafts are refused with `draft_release` (`:521`, `:961`)
- **Stage commit:** `upload_finish` writes the VERIFIED header last, with a
  read-back, and erases it on mismatch (`update_stage.c:463-517`). A power
  cut at any point during staging leaves BLANK or the old header erased,
  never a false VERIFIED.
- **Apply ordering:** `apply_staged` clears boot_guard before it erases app.
  `set_boot` follows the copy, and the stage header erase comes last
  (`recovery_apply.c:173-176`).

## 3. Bench robustness matrix

These are the expected end states on origin/dev at `551722a76`, derived from
code. They have not been run, because there was no board access. Run them
after AUTOAPPLY lands, then compare.

| Case | How | Expected end state today | Under AUTOAPPLY |
|---|---|---|---|
| Wi-Fi off mid-download | drop the AP while `update_fetch_status` shows `downloading` | job `failed` with `read_failed` or `timeout`; stage BLANK (LOW-1); app runs; retry is clean | same; no install record written |
| Reset mid-download | `debug_reset` during `downloading` | boots app; stage BLANK or a partial header-less image that reads BLANK; `update_status` shows nothing staged | same; the record must not exist before VERIFIED |
| Reset mid-apply | `debug_reset` while `recovery_apply_status` shows copying | boots recovery (MED-2); stage still VERIFIED; app invalid; needs a human to re-run apply | re-applies on the next recovery boot, attempt count +1, then boots app |
| Cancel | `update_fetch_cancel` mid-body | job `cancelled` within about 10 s (LOW-8); stage BLANK; claim balanced | same |
| Low heap | start a fetch with internal heap below the UPDFX2 admit floor | refused before any TLS open (`low_heap_free`, `low_heap_block` or `low_heap_dma` at admit); `low_heap_midrun` aborts mid-body; stage BLANK; app runs | install refused; no recovery entry |
| Rate limited | 61st unauthenticated check in an hour | `http_status` (LOW-2) | automatic checker must back off |
| Firing starts mid-download | start a profile during `downloading` | job aborts on the next 8-chunk heat check; stage BLANK | install refused while a run is active |

## 4. Tests added

- `firmware/KilnFW/App/test/stubs_update_fetch/fake_support.[ch]`: live
  client counting (`g_fr_live_clients`, `g_fr_max_live_clients`,
  `g_fr_open_while_live`).
- `firmware/KilnFW/App/test/test_update_fetch.c`:
  `test_redirect_hop_frees_first_client` (Q1).

## 5. What AUTOAPPLY must handle

1. **Enter only with a VERIFIED stage.** Check the stage header, size and
   sha immediately before `recovery_switch_select_boot()` (MED-1), and refuse
   while a fetch job is busy or a run is active.
2. **Idempotent apply on every recovery boot.** While a pending record and a
   VERIFIED stage exist, re-run the apply. This covers MED-2.
3. **Bounded attempts.** Store an attempt counter in the record and
   increment it **before** the app erase. After N attempts (for example 3),
   stop applying:
   - If `app_modified` was never set, `recovery_exit` back to the old app
     and record the failure.
   - Otherwise stay in recovery with the failure recorded. Booting the
     erased app is not an option.
4. **Clear the record before `set_boot`, or make it one-shot by attempt.**
   A record that survives a successful apply must not trigger a second
   apply of the same image. `update_stale_stage.c` already clears the
   matching stage; the record needs the same treatment.
5. **A failure reason the app can read** after `recovery_exit`, so that
   `update_status` can explain why the board is still on the old version.
6. **boot_guard interaction.** apply_staged already clears boot_guard before
   the erase. A recovery boot loop driven by MED-2 retries must not trip
   boot_guard's own threshold into a different state.
7. **NVS unavailable** (the record cannot be read in recovery): fail toward
   "stay in recovery, AP up". Never reboot into an app the copy may have
   erased.
8. **Pico busy or link down in recovery** (expected S6b during a recovery
   dwell): the apply must not depend on the safety link. Clearing the S6b
   that results is covered by the owner decision of 2026-10-02.
9. **No re-download between stage and install** (LOW-1).

## 6. Fix status (LOW batch)

Fixed in `980c50c59` (code and tests) and `574616774` (cancel tests for the
header and body waits). All host-test only; nothing run on hardware.

| Finding | Status | Change |
|---|---|---|
| LOW-1 | Kept, documented | The old verified stage is erased only in `flush_head` (`update_stage.c`), i.e. on the first flash write of the new image. A download that fails earlier (HTTP error, bad size, cancel, timeout) leaves the old stage staged. New test `old verified stage kept when the new download fails before writing`. |
| LOW-2 | Fixed | `rate_limited` (403/429) and `server_error` (5xx) are separate reasons. `Retry-After` (seconds) and `X-RateLimit-Reset` set a board-side backoff (default 900 s rate, 60 s server, clamped 5..3600 s). check/download POST answer `429 backoff` with `retry_after_s`; status JSON carries `retry_after_s`. RAM only. |
| LOW-3 | Skipped | Already fixed by UPDFX3 (`566a4861d`). |
| LOW-4 | Fixed | A header-read or body-read timeout is labelled `timeout`, not `bad_response`. |
| LOW-5 | Fixed | A wedged writer's job buffers (body, scratch, writer state) are handed to the writer and freed when its late operation returns. Test asserts they stay held while parked and are released afterwards. |
| LOW-6 | Skipped | Stays documented. |
| LOW-7 | Fixed | `per_page` raised to 15 (`update_url_build_list` allows 1..30), staying inside the heap budget; no internal RAM added (buffers in PSRAM). |
| LOW-8 | Fixed | Reads and the header wait are sliced (1 s, 5 stall slices); the settle-wait already polled cancel before this fix. Tests cancel a stalled header wait and a stalled body read and assert effect within 1.25 s. |

Negative tests (`tools\negtest.ps1`, kilnfw-host pattern, base `980c50c59`
for the first seven, base `574616774` for the two cancel mutants; every
CAUGHT mutant compiled and failed on a test assertion):

| Mutation | Result |
|---|---|
| L2_reason_generic | CAUGHT |
| L2_no_backoff_note | CAUGHT |
| L4_header_timeout_bad_response | CAUGHT |
| L4_body_stall_label | CAUGHT |
| L8_header_ignores_cancel | CAUGHT (`test_update_fetch.c:720`) |
| L8_slice_too_long | CAUGHT (`test_update_fetch.c:720`) |
| L5_no_handoff | CAUGHT |
| L7_per_page_5 | CAUGHT |
| L1_early_erase | CAUGHT |
