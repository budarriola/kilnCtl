# Review: Host allow-list rule (f8dffd8d) and linkcov LOW-1..4 (7452e63b), 2026-10-09

Reviewer: opus, read-only review on `origin/dev` at 289ea39c. No code changed.

Targeted host tests run in a clean worktree:
`build_host_tests.ps1 -Only "test_profiles_http$|test_profile_executor_store_link|test_http_auth_enforce|test_safety_link_compile"`
selected `main`, `profile_executor_store_link`, `safety_link`: "all 3 host test executables built and passed".

No HIGH or MED findings. Both commits do what they claim.

## f8dffd8d -- `http_origin_host_name_allowed` suffix allow-list (F4)

Checked by tracing the code; none of these get past the gate:

- Empty labels: `kilnctl..lan` gives suffix `.lan`, `kilnctl.lan..` loses one dot and then gives suffix `lan.`, `.lan` is shorter than the name. All refused.
- Prefix and label tricks: `kilnctlx.lan` (`host[n]` is not `.` or NUL), `kiln` (shorter than the name), `evil.kilnctl.lan`, `kilnctl.lan.evil.com`, `kilnctl.local.evil.example`. All refused. A suffix match uses exact `strcmp`, so a listed suffix followed by more labels never passes.
- Case: `parse_authority_` lower-cases the Host, and the compare lower-cases `mdns_name` too.
- Port: `:abc`, a bare `:`, `:80:80`, more than 6 digits, or a value over 65535 makes the parse fail, so the request is refused. `[::1]:abc` and `[::1]x` are refused the same way.
- Percent, whitespace and NUL: none of these is a stop character, so the bytes stay in the host and the compare fails (fail closed). httpd's NUL-terminated header copy truncates at a NUL. A browser cannot produce any of these in Host anyway.
- Buffer bounds: the glue refuses a Host whose length is >= 96, and so does `parse_authority_` (`n >= sizeof(host)`). The trailing-dot strip checks `hl > 0`. `hp.host + n + 1` is read only when `host[n] == '.'`, so `n + 1 <= hl`. The longest legal case, a 63-char name plus `.localdomain`, is 75 bytes and fits.
- Hostname source: `mdns_hostname_set("kilnctl")` is the only setter (`main_boot_early.c:349`). No runtime path changes it. The name is read live under the mdns lock into a local buffer. If the mdns service is down or returns an empty name, the code falls back to the same `"kilnctl"`. An empty `mdns_name` skips the name branch, so only IP literals and localhost pass.
- Recovery image: `origin_guard` passes `mdns_name = NULL`, so it never reaches the new branch. A `static const` local array inside a `static inline` function is valid C. Behavior there is unchanged.
- Captive portal: the `Location` header is `http://<AP IPv4>/`, and the IPv4-literal rule accepts it.

### LOW-1 Router-DNS suffixes are mostly unreachable: DHCP hostname is never set
`http_origin_check.h:231`. Nothing in the firmware calls `esp_netif_set_hostname()`, and `sdkconfig.defaults` does not set `CONFIG_LWIP_LOCAL_HOSTNAME`. The DHCP client therefore sends ESP-IDF's default name, `espressif`, so a router registers `espressif.lan`/`.home`, not `kilnctl.lan`. The `lan`/`home`/`localdomain`/... entries only help when the operator adds a manual DNS entry. The audit text (`WEB_UI_XSS_AUDIT_2026-10-09.md`, F4 follow-up) says "for router DNS" as if this works out of the box. This is a functional and documentation gap, not a security one.

### LOW-2 Trailing dot is stripped only in the name branch
`http_origin_check.h:213,219`. `localhost.` and `192.168.1.50.` are refused, while `kilnctl.` and `kilnctl.lan.` pass. This fails closed and is harmless, but it is inconsistent and has no test.

### LOW-3 Bracketed branch accepts any `[...]` content
`http_origin_check.h:210`. `hp.host[0] == '['` passes without checking IPv6 syntax, so `[kilnctl.attacker.com]` is accepted. A browser only sends brackets for an IPv6-literal URL, so DNS rebinding cannot use this. It is still looser than the comment ("bracketed IPv6 literal") says.

### LOW-4 Test gaps in `test_host_allowlist` (`test_http_auth_enforce.c:675`)
There are no cases for: `kilnctl..lan`, `kilnctl.lan..`, `kilnctl.` (bare name with a dot), `kilnctl.lan:abc`, `kilnctl.lan:`, `.lan`, an empty `mdns_name` (`""`), a mixed-case `mdns_name` argument (`"KilnCtl"`), or a 95/96-byte Host at the buffer boundary. Each behaves correctly when traced by hand. Without tests, a later refactor of the strip or prefix logic would go unnoticed.

## 7452e63b -- linkcov LOW-1..4

The memory ordering is correct:

- Writer: `profiles_slot_gen_begin` and `_end` are seq_cst `atomic_fetch_add` calls. The RMW's acquire side keeps slot writes from moving above `begin`, and its release side keeps them from moving below `end`.
- Reader: `profiles_http_slot_rev()` is a seq_cst load, which has acquire semantics, so the unlocked `profiles_http_get()` copy cannot move above the capture. The new `atomic_thread_fence(memory_order_acquire)` in `profiles_http_slot_runnable_rev` (`profiles_http.c:1898`) orders that copy before the generation re-read. This is the standard seqlock reader. The `s_exec.lock` take between the copy and the recheck already acts as a barrier, so the fence is redundant but correct.
- Nesting: the boot-load bracket wraps `profiles_boot_load_body()`, which does not call `gen_begin`/`gen_end` itself. The only other bracket sites are save, edit, delete and retarget. So the nesting cannot flip the counter's parity, and the test confirms the generation is even afterwards.

Start paths: every firing start goes through `profile_executor_run()`, which checks `profiles_http_slot_runnable_rev()` under `s_exec.lock` (`profile_executor_run.c:460`). That covers the HTTP route (`dashboard_exec_http.c:804`), the LCD (`ui_page_home_actions.c:127`, `ui_page_profile_detail.c:488`) and the UART bridge (`uart_bridge_ext_control.c:567`). Builtins hit the gate too, because the flag check comes before `profiles_builtin_id_valid()`. Autotune does not use profiles. There is no auto-resume (`run_state.c:408`).

Boot ordering: the flag cannot stay unset.
- `profiles_http_start()` sets `s_profiles_loaded` right after `profiles_boot_load()`, whatever the load result, and before the `server == NULL` early return.
- `main_network_http_bringup()` calls `profiles_http_start()` unconditionally, with no early return before line 423.
- `app_main()` calls the bringup unconditionally, including in recovery mode. In recovery mode the system-mode gate refuses starts anyway.
- A failed NVS load still sets the flag: the load falls back to the cfg files only. `test_profile_executor_store_link` calls `profiles_http_start()`, so the default-false flag does not break it.

### LOW-5 The fixed stale bound now fakes a Pico reboot under a slow poll period
`safety_link.c:308`, with the consequences in `safety_link_frames.c:331` and `:270-293`.

When `poll_period_ms >= 750` (settable through `safety_link_set_poll_period`), the "up" window is `3*P >= 2250` ms. One missed reply makes `age > 1500` at the post-poll check (`safety_link_poll.c:653`). The function then clears `pico_boot_id_known` although the link is still up. The next FW_VERSION reads `boot_id_changed = true` (`!pico_boot_id_known`), and `safety_note_pico_reboot_locked()` runs for a Pico that never rebooted. It:
- resets the trip dedup, so the TRIPPED log is duplicated;
- sets `diag_trip_seq_known = false`, so a bound trip clear is unbound and refused by a protocol-17 Pico until the next 31-byte DIAG;
- forgets `safety_relay_state`;
- queues a reannounce burst.

At the default 500 ms period the two bounds are identical (3*500 = 1500), so nothing changes there. Every effect is fail-safe, so this is LOW. If the intent is "the link is stale", keep the stale bound for heat gating and leave the reboot-evidence clear on `!safety_link_up_locked()`. Or clear only `peer_version_known`, which forces a re-request, and keep `pico_boot_id_known`, so that re-learning the same boot_id is not read as a reboot.

### LOW-6 The refusal before boot load finishes names the wrong cause
`profile_executor_run.c:460-462`. Before `s_profiles_loaded` is set, a start is refused with "profile is being deleted, was deleted or was re-saved -- not started". The LCD (LVGL starts in `main_boot_early`) can reach Start before `main_network_http_bringup()` runs `profiles_http_start()`, so an operator can see this misleading text. A distinct message, such as "profiles still loading", would be accurate.

### LOW-7 The test hook is an unconditional production symbol
`profiles_store.h:83`, `profiles_http.c:89`. `profiles_http_test_set_loaded()` is built into the firmware and declared in a shared header. Any caller could clear the flag and silently disable every profile start until reboot. Guard it with the host-test define the repo already uses for test hooks, or move it to a test-only header.

### LOW-8 Indentation regression
`profiles_http.c:1423`. The `ESP_LOGE(... "legacy NVS erase failed, file kept")` line in `nvs_erase_slot_locked` gained four extra spaces when the `atomic_store` above it was deleted. Cosmetic only.

### LOW-9 (existing before this commit, next to its claim) Saves are not gated on the loaded flag
`profiles_http.c:2005-2042` and `:1753`. `profiles_boot_load_body()` writes `s_profiles`/`s_profile_rev` without `profiles_save_lock()`, and on the NVS-failure fallback it does `memset(&s_profiles, 0, ...)`. The new flag gates only starts. A save that reaches `profiles_http_save()` while boot load runs, for example the LCD profile builder (LVGL is already up), races the unlocked load writes and can be overwritten or clobbered. The window is narrow, because the HTTP and UART save routes register later. The commit comment ("Boot load/migration writes RAM slots ... while /api/profile_exec/start is already registered") covers starts only. A save-side refusal until loaded, or taking the save lock across boot load, would close it.
