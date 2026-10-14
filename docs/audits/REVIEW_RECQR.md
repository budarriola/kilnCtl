# Review: RECQR, recovery LCD Wi-Fi join QR: 0 HIGH, 0 MED, 5 LOW, 5 INFO (2026-10-13)

Adversarial review of the origin/dev commits ending at `fe3fa544d`:

- `4508672af` Recovery LCD: draw the Wi-Fi join QR next to the passphrase text
  (`recovery_wifi_qr.[ch]`, `recovery_lcd.c`, vendored `third_party/qrcodegen/`,
  `test_recovery_wifi_qr.c`, `check_recovery_wifi_qr.ps1`, `docs/RECOVERY_IMAGE_PLAN.md`).
- `95f11227f` lcd_ap_reader: decode the recovery screen's QR first, glyph fallback
  (`lcd_ap_reader.py`, `join_board_ap.ps1`, `test_lcd_ap_reader.py`, `pyproject.toml`, `uv.lock`).
- `d7cbc43a1`, `fe3fa544d` (GATE_NEGATIVE_TEST_EVIDENCE rows).

Reviewed in a worktree at origin/dev (`fbb68c838`). No board access. Method: source reading,
a diff of the vendored qrcodegen against the LVGL copy it came from, a synthetic webcam
readability sweep (the test file's own renderer at the bench homography, plus Gaussian blur and
JPEG), and `tools/negtest.ps1` (table at the end).

## Questions

| Question | Answer |
|---|---|
| Passphrase or QR payload logged anywhere? | No. `recovery_wifi_qr.c` has no logging; `recovery_lcd.c` logs only counts; `recovery_wifi.c` logs the SSID only. The reader's `ReadError` texts never interpolate the payload or the passphrase, the CLI's JSON carries SSID/counts/warnings only, the candidates file is 0600 and deleted by `join_board_ap.ps1` after reading. Tests assert the passphrase is absent from stdout and from fallback warnings |
| Left in RAM longer than needed? | The module matrix `s_qr` lives for the boot, as does `s_net_pass` (the text needs it for every redraw), so nothing new is retained. `s_qr_tmp` is wiped (static, the memset survives). The stack `payload[]` wipe is a plain `memset` of a dying local, which `-Os` may drop (LOW-1) |
| Weaker than the text? | No. The QR is the same secret in another encoding, on the same panel, shown under exactly the same condition (`show_qr` mirrors the text branch: no error head, AP not down, not none/storage-fail, `s_net_set`). A camera or eye that can see the panel can already read the 12 large characters. The owner decision (recovery image unsecured, passphrase LCD-only) is unchanged: the QR is not emitted on any other channel |
| Escaping | `\ ; , : "` are escaped in both SSID and passphrase, matching the ZXing/Android `WIFI:` convention. The recovery alphabet has none of them, so only an NVS-configured SSID can need escaping. The reader unescapes any `\x` to `x` and splits on unescaped `;`; round-trip tested for `;` in the SSID and `: ; ,` in a normal-mode password |
| Capacity | Byte mode (the `;` forces it). Version 4-L holds 78 bytes; payload = 30 + escaped-SSID bytes, so any SSID of up to 32 plain bytes fits (62 B max, version 4), and only an SSID with more than 16 escaped characters fails. The default `kilnctl-recovery` is 46 B, version 3 (29 modules). `RQR_PAYLOAD_MAX` 96 covers the 94 B worst case; the encoder's own byte-mode bound (`bufLen` 138) is above it |
| Does not fit | `rqr_encode` returns 0, `s_qr_size` 0, the area paints solid background, the text stays. Tested (`too long for version 4 -> 0`) |
| Draw bounds and overlap | Window x 356..479, y 0..123, inside the 480x320 panel. Text in rows 0..123 is at most 28 glyphs x 12 px = 336 px wide ("Boot guard count: unreadable"), title 234 px, so no glyph is overdrawn. Full-width red fault bands in those rows (`NVS UNAVAILABLE`, `BOOT GUARD RECORD INVALID`, `RELAY CTRL FAULT`) are cut at x=356 (LOW-3). `Y_JOIN` (126) starts below the area |
| Stack | Drawing adds nothing to the redraw frame (`rqr_scanline` is a leaf with a few ints). Encoding runs once, in `app_main` (`recovery_main.c` -> `recovery_wifi_start` -> `start_softap` -> `recovery_lcd_set_ap`): ~100 B `payload` plus qrcodegen frames (fixed, small, no recursion; `rsdiv[30]`, `runHistory[7]`), roughly 400 B, nested under `start_softap`'s `wifi_config_t`. `app_main` is not a created task, so `check_stack_margin_registration` does not apply; `recovery_main.c` logs its high-water mark at WARN. No bench boot of this image has been reported with that number (LOW-2) |
| Heap | None. Two static 138 B arrays (`.bss`, internal DRAM, 276 B) plus the existing DMA `s_line`. The 8 KB internal-RAM floor is an application-image rule; the recovery image has no such budget check, but 276 B is negligible |
| Task watchdog | 124 SPI transactions of 248 B at 20 MHz: about 12 ms of wire time per redraw, redraws are event-driven (not the 1 Hz tick). No WDT risk |
| Vendored qrcodegen | MIT `LICENSE.txt` present, copyright line kept in both files. Diff against `firmware/KilnFW/components/lvgl/src/libs/qrcode/` is only the include/guard swap (`LV_ASSERT` -> `assert`, `LV_STD*_INCLUDE` -> `<std*.h>`, `#if LV_USE_QRCODE` dropped). Nayuki's code is well-audited, no heap, no recursion; inputs here are always in range (version 1..4, Ecc_LOW, Mask_AUTO), so the asserts cannot fire (INFO-2) |
| Hidden on error / no network | Yes, `show_qr` is the text branch's condition, and the area is repainted with background when hidden, so a stale symbol never outlives its AP. Not host-tested (LOW-4) |
| Reader: QR first, recovery only, strict | Recovery: QR first; accepted only as a single distinct `WIFI:` payload whose `P` is exactly 12 recovery-alphabet characters and whose `S` is non-empty; anything else falls back to glyph segmentation with a warning. Normal: QR only, unchanged validation (any password) |
| A wrong QR joined? | A reflection of the screen's own QR decodes to the same payload (dedup by `set`). Two distinct `WIFI:` codes in the frame force the glyph fallback. A foreign `WIFI:` code (phone, printout) with a 12-char recovery-alphabet password and some other SSID would be accepted: the QR's SSID is not cross-checked against anything (LOW-5). The consequence is a join attempt to that SSID, not a leak of the board's passphrase |
| Webcam readability at 3 px/module | At the bench homography (round-4 corners) the QR region maps to 1.74 x 1.77 camera px per panel px, i.e. about 5.2 camera px per module, a 37-module (v3 + quiet) symbol about 195 px across. Sweep (zxing-cpp 3.1.1, synthetic render at the test geometry): decodes up to Gaussian blur sigma 2.0 px at JPEG q50 and 2.5 px at q90; 2 px/module fails from sigma 1.5; 4 px/module holds to 3.0. The SSID line is already read by glyph segmentation at the same 3 px feature size, so the camera resolves it. Likely decodable; keep 3 px (INFO-1) |

## LOW-1: the stack payload wipe can be optimised away and does not use the module's secure_zero

`rqr_encode()` ends with `memset(payload, 0, sizeof(payload))` on a local that is dead right
after. GCC at `-Os` (`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`) is allowed to remove that store.
`recovery_wifi.c` already uses a `secure_zero()` for the same reason. Impact is small (the same
passphrase stays in `s_net_pass` for the boot anyway), but the header's claim "the payload buffer
... wiped before rqr_encode() returns" is not guaranteed. The host test cannot see a stack local,
so no mutant can catch it (negtest `c_payload_wipe_dropped` MISSED, as expected). Fix: a volatile
wipe (share `secure_zero`) or note the limit in the header.

## LOW-2: app_main stack headroom with the encoder nested in not yet measured

The encoder (~400 B) runs nested in `app_main` under `start_softap()`. `CONFIG_ESP_MAIN_TASK_STACK_SIZE`
is the IDF default (not set in `sdkconfig.defaults`). The commit reports image size only, not the
`stack hwm: app_main min free` line `recovery_main.c` prints. Read that line on the next bench boot
of this image; raise the main stack if it is under ~1 KB.

## LOW-3: the QR area truncates full-width fault bands at x=356

`draw_qr_area()` paints its 124x124 square after the text, including when hidden (solid background).
The red `NVS UNAVAILABLE` / `BOOT GUARD RECORD INVALID` / `RELAY CTRL FAULT` bands in rows 50..121
lose their right 124 px. The text itself (<= 300 px) stays whole, so this is cosmetic, and the code
comment says so. If wanted: paint the hidden area in the band colour, or limit the area to the
rows that are not a fault band.

## LOW-4: show/hide logic has no test

`show_qr` is a hand copy of the text branch condition in `draw_status()`, which no host test covers;
`check_recovery_wifi_qr.ps1` builds only `recovery_wifi_qr.c` (negtest `lcd_show_ignores_ap_down`
MISSED). It matches today. Moving the predicate into `recovery_lcd_policy.c` (already host-tested)
and using it for both the text branch and the QR would keep them from drifting.

## LOW-5: recovery QR SSID trusted without a cross-check; reader test gaps; dependency floor

(a) `read_recovery_qr()` accepts any single `WIFI:` code whose password matches the recovery
alphabet, with whatever SSID it carries. The glyph reader already finds the SSID band; comparing
the QR's SSID to it (or at least to the panel position: the code must lie in the panel's top-right)
would refuse a foreign code. No test covers the multi-QR and empty-SSID branches
(negtest `py_multi_qr_dropped`, `py_empty_ssid_dropped` MISSED).
(b) the length check is only exercised from above: the short case is also off-alphabet, so a `>` mutant survives. Add an 11-char recovery-alphabet case.
(c) `pyproject.toml` declares `zxing-cpp>=2.2`, but the tests use `create_barcode` /
`write_barcode_to_image`, the 3.x writer API; uv.lock pins 3.1.1, so this only bites an install that
ignores the lock. Raise the floor to the version the tests need.

## INFO-1: module size

3 px is the largest that fits: 4 px makes a v3 symbol 148 px and a v4 symbol 164 px, which would
overlap the 336 px status text (480 - 336 = 144). If the bench camera fails to decode, the cheaper
levers are focus/`-Frames`, or drawing the quiet zone only on the panel side (the bezel is black,
so the top/right quiet zone must stay light).

## INFO-2: vendored asserts

`LV_ASSERT` is mapped to `assert`; with the recovery image's assertion level, a violated
precondition would abort. All call-site arguments are constants in range, so this is unreachable.

## INFO-3: non-ASCII SSIDs

Byte mode without an ECI segment: a UTF-8 SSID is decoded by zxing's charset guess. Fine for the
ASCII default; untested for others.

## INFO-4: built-in mutants are real

The 15 mutants in `check_recovery_wifi_qr.ps1` each compile (a `cl` failure throws and fails the
check rather than counting as caught) and the base run must print `RESULT pass=N fail=0` with
N >= 30 (38 today). Each targets an assertion that exists in `test_recovery_wifi_qr.c`
(escapes, grammar, bounds, wipes, ECC/boost via an independent reference encode, geometry, endian).
The negtest table below adds mutants outside that list.

## INFO-5: Windows WLAN profile keeps the key (pre-existing)

`join_board_ap.ps1` keeps a successful profile with `<protected>false</protected>` key material,
readable via `netsh wlan show profile key=clear` until `-Disconnect`. Not changed by these commits;
the passphrase rotates per boot.

## Negative tests (tools/negtest.ps1)

Both runs: baseline PASS, `real_tree_unchanged: true`, copies removed. Every CAUGHT below compiled
(C) or imported (Python) and failed on a test assertion; no CAUGHT is a build or import error.

C, `-Preset check -PresetArg tools/check_recovery_wifi_qr.ps1` (baseline 38 assertions, plus its
15 built-in mutants inside each run). Log dir `negtest_logs\20261013_171504_6j94`.

| Mutant | Edit | Verdict | Evidence |
|---|---|---|---|
| c_maxver_3 | `RQR_MAX_VERSION` 4 -> 3 | CAUGHT | `test_recovery_wifi_qr.c:107` "32-char SSID gives version 4" |
| c_empty_ssid_allowed | drop the empty-SSID refusal in `rqr_build_payload` | CAUGHT | test:41 and test:113 |
| c_module_px_4 | `RQR_MODULE_PX` 3 -> 4 | CAUGHT | 5 geometry assertions fail |
| c_payload_wipe_dropped | drop the stack `payload` memset | MISSED | expected: a dead local is not observable from a host test (LOW-1) |
| c_area_guard_dropped | drop `ext <= RQR_AREA_PX` | MISSED | equivalent for every size the encoder can return (v4 + quiet = 111..123 px) |
| lcd_show_ignores_ap_down | drop `s_ap_down` from `show_qr` | MISSED | `recovery_lcd.c` is never compiled by any host test (LOW-4) |

Python, `-Preset pytest -PresetArg tests/test_lcd_ap_reader.py`. Log dir `negtest_logs\20261013_171503_jgpj`.

| Mutant | Edit | Verdict | Evidence |
|---|---|---|---|
| py_alphabet_dropped | drop the alphabet check | CAUGHT | `test_recovery_bad_qr_passphrase_falls_back_to_glyphs` |
| py_len_loose | `!= PASS_LEN` -> `> PASS_LEN` | MISSED | the short case `"short"` is lowercase, so the alphabet check rejects it; an 11-char recovery-alphabet QR password is accepted untested (LOW-5b) |
| py_glyph_first | QR path never tried | CAUGHT | `prefers_the_qr`, `5_s_ambiguity`, `cli_..._never_prints_the_passphrase` |
| py_unescape_dropped | no backslash unescape | CAUGHT | `test_parse_wifi_qr`, `escaped_ssid`, `normal_mode_reads_the_qr_only` |
| py_no_warning | fallback warning not appended | CAUGHT | three fallback tests |
| py_cli_glyph_only | CLI uses glyph reader only | CAUGHT | `test_cli_recovery_qr_never_prints_the_passphrase` |
| py_multi_qr_dropped | drop the distinct-payload check | MISSED | no two-QR test (LOW-5a) |
| py_empty_ssid_dropped | drop the empty-SSID check | MISSED | no empty-SSID QR test (LOW-5a) |
| py_qr_leaks_pw_in_error | put the password in the ReadError text | CAUGHT | the fallback-warning secrecy assertion in `bad_qr_passphrase` |

Totals: 15 mutants, 9 CAUGHT, 6 MISSED (2 expected/equivalent, 4 real test gaps, all LOW).
