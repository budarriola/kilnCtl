# Review 2026-10-18: GitHub OTA fetch path (GP)

Scope: read-only adversarial review of origin/main 293985c24 (`update_fetch.c` fetch_task / update_fetch_wr,
`update_release.c` list/latest/manifest parsing, `update_policy.c` refuse_downgrade gate, `update_semver.c`,
GF-02 `/releases/latest` fallback, `update_http.c` io_image_verify). No board writes, no builds.

Counts: HIGH 0, MED 0, LOW 2, INFO 5.

## Static stack estimate for update_fetch_wr (BX-03, RY-02)

`check_all_task_stack_budgets.py` has no `update_fetch_wr` row (`check_stack_task_table_consistency.ps1`:
"no static ceiling yet"). The same library (`stack_budget_lib.parse/deepest`, `drop_worker_only_edges`,
UNMODELED_OVERHEAD_BYTES 300) was run against the existing ELF
`C:\wt\checkbuild_origin_kilnfw\firmware\KilnFW\build\KilnCtrl.elf` (179cb0058, one promote before 293985c24;
the delta in `update_fetch.c` is only the WR_EXIT retain-exit path, `update_http.c` adds the timing hook).

| Root | Deepest (B) | Note |
|---|---|---|
| wr_task (resolved calls only) | 1488 | wr_task 64 + update_stage_upload_finish 848 + stage_header_encode chain |
| update_stage_upload_begin | 1120 | |
| update_stage_upload_write | 1168 | io_write callback adds 512 |
| io_image_verify | 736 | io_image_verify 64 + stage_image_verify 32 + stage_image_verify_opt 640 |
| iv_sha_finish (callback) | 1728 | deepest io callback (psa -> esp_sha DMA -> gdma alloc -> log -> assert) |
| iv_read / iv_sha_update | 1584 / 1616 | |

Worst composed path, adding the function-pointer callbacks by hand: wr_task 64 + upload_finish 848 +
io_image_verify path 736 + iv_sha_finish 1728 = **3376 B**, plus 300 B unmodelled overhead = **3676 B of 4096 B,
about 420 B honest free**. The callee chains are pessimistic (log + assert tails), but IDF-internal indirect
dispatch (esp_flash chip driver ops, PSA driver dispatch) is still unresolved, so this is an estimate, not a bound.
io_image_verify's 64 B frame confirms the RY-02 static `iv_sha_t` is off the stack.

- **RY-02 can close on static grounds**: the fix is a source fact, confirmed in the ELF frame size.
- **BX-03 cannot close on static grounds alone**: it fits with ~420 B, less than the 880 B accepted for
  info_uart_bridge, and the unresolved dispatch is not counted. See GP-01.

## Findings

- **GP-01** LOW, `update_fetch.c:98` FETCH_WR_STACK_BYTES 4096. No static ceiling row; estimate above leaves
  ~420 B. Fix: add an `update_fetch_wr` row to `check_all_task_stack_budgets.py` with the io callbacks
  (iv_*/io_*) as extra roots, ceiling ~3400 B; consider 4096 -> 5120 B if the row shows less than 512 B (1 KB internal
  RAM, counted in the admission draw).
- **GP-02** LOW, `update_fetch.c:584-616`. On a stable-channel list `too_large` (GF-02) the fallback asks
  `/releases/latest`, which is the most recently created stable release, not the highest semver (the AUP-02 reason
  the list is read at all). A newer release with long notes can be masked by a later-created lower one: the job
  reports refuse_downgrade / up_to_date instead of offering the upgrade. Fails closed, no bypass. Fix: on overflow,
  re-read the list with a smaller per_page before falling back.
- **GP-03** INFO. Prerelease channel list overflow is a hard `too_large` failure (GF-02 is stable-only by design).
- **GP-04** INFO. Downgrade gate: no bypass by crafted tag. Manifest tag must equal the API tag; identity.version is
  the tag; the stage manifest gate cross-checks the image descriptor version and schema record; prerelease
  identifiers compare numerically (pre.10 > pre.9); a dev build maps describe to pre.N.commits, and a truncated
  describe is unknown, which needs force plus the typed confirm. Schema-lower is refused without the typed override.
- **GP-05** INFO. Partial download cannot leave a VERIFIED stage: WR_FINISH runs only after recvd == app_size and the
  SHA-256 matches the manifest; every error path aborts the owned upload; an abandoned late finish clears the stage.
  Content-Length mismatch, overrun and short body are all refused.
- **GP-06** INFO. Claim/lock release: the update claim is released on task-create failure, at the end of every job,
  and in the BX-04 busy branch; check jobs take no claim; writer semaphores are deleted on every wr_start failure and
  in wr_stop; a wedged writer keeps its buffers by design (LOW-5).
- **GP-07** INFO. Internal RAM: rx/tx client buffers 2048 B each, scratch 1024 B and the 4096 B writer stack are all
  internal and inside the admission draw model (FETCH_HEAP_*); the RX-11 verify adds no internal allocation (static
  op). The draw table rows predate RX-11 on this task, so the BENCH-PENDING re-measure in update_fetch_heap.h still
  stands. No floor breach found beyond the documented sampled-backstop limits.
