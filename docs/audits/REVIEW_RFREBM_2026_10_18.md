# Review 2026-10-18: RF / RE / BM fixes (read-only)

Base: origin/dev at 179cb0058 content. Read-only review, no board, no target builds.
Scope: RF-01..RF-03 (4a73d145d, 8fc0f16b7), RE-01/02/03/05 (7542aa3fc), BM-01/02 (054d8dbea).

Counts: HIGH 0, MED 0, LOW 2, INFO 4.

## Findings

- **RB-01 LOW** `ui_worker.c`, `ui_lcd_keypad.c` try_submit(). All LCD UI jobs share one FIFO worker,
  so a PIN verify waits behind queued Wi-Fi jobs. Each of scan/connect/forget/mode/AP-identity can block
  up to WIFI_OWNER_WAIT_MS (12 s) in the owner task. Worst case: "Checking PIN..." for tens of seconds
  before an admin can unlock the LCD (for example, for a safety clear). Nothing is lost and the
  result is still correct. Only the delay is a problem. Fix option: run the PIN job at the head
  of the queue (`xQueueSendToFront`) or give the PIN verify its own static worker.
- **RB-02 LOW** `update_semver_from_describe()` takes `esp_app_desc_t::version`, which is 32 bytes
  (31 characters). A longer describe string gets truncated. Example: a two-digit prerelease, a 4-digit
  commit count, a 9-char hash and `-dirty` (34 chars) truncates to `...-dirt`. The base does not
  parse, so the running version is unknown again. This fails safe, back to the pre-BM-02
  force-plus-confirm path. Fix option: set PROJECT_VER from `git describe --abbrev=7` and drop `-dirty`
  at build time, or add a host test for the 31-char truncation.
- **RB-03 INFO** RE-03 passphrase: `rnd[i] % 26` has modulo bias (256 = 9*26 + 22). 22 symbols get
  probability 10/256 and 4 get 9/256. Entropy is about 4.698 bits per char, about 61.0 bits over 13
  chars, so it still meets the >= 60-bit claim. No action needed.
- **RB-04 INFO** ui_worker queue length is 6. There are exactly six job kinds, and each is single-flight
  (busy flag or `s_worker_running`), so the queue cannot overflow today. A full queue fails closed:
  callers clear their busy flag and show "Busy, try again" / "Could not start". A 7th job kind or a
  non-single-flight caller could hit "queue full". A comment or a static count next to
  UI_WORKER_QUEUE_LEN would guard this. Job stacks fit: the largest frame is the scan's 20 records
  (about 1 KB), well under 6144 B. No job calls vTaskDelete or takes the LVGL lock. Results are
  posted via flags or lv_async_call.
- **RB-05 INFO** RE-01 fence: `s_restart_pending` is set before `xTaskCreate` and never cleared.
  If task creation fails, `restart_soon` restarts inline, so the fence cannot latch without a restart
  following. The apply/install tasks re-check it before claiming. No leak found.
- **RB-06 INFO** BM-02 mapping over a release base (`1.2.3-40-g...` maps to `1.2.3`). A dev build
  that is newer than tag 1.2.3 compares equal to a 1.2.3 candidate. Installing that candidate is
  really a downgrade, but it is treated as a same-version reinstall that needs force. This matches
  the documented design. BM-01 (stack slot passed by value) checked, sound. RF-03 probe measures
  internal largest block, matching where `xTaskCreate` puts the stack.
