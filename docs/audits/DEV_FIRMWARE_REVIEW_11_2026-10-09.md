# Dev firmware review 11 (2026-10-09)

This is a read-only Opus review of four origin/dev commits. No code was changed and the board was not touched.

| Commit | Subject |
|---|---|
| `ab210508` | zones.json.bad preservation, load-fault latch "cfg file: ...", UNTRUSTWORTHY boot log |
| `5e03dbc0` | captive 302 helper `http_captive_location_for_request` |
| `ea9aea5a` | profiles slot-generation test hook |
| `0aec8e7c` | web: host refusal text, zones blank-guard refusal, wizard step 11 read-back, OTA 403 mapping |

The reviewed tree is origin/dev `05d4ea58`. Paths are relative to `firmware/KilnFW/App/drivers/` unless they say otherwise.

## Findings

### MED-1 (pre-existing; ab210508 limits the damage): a rejected zones.json is still overwritten at boot by an older legacy candidate

**FIXED in SHAPLACE: a newer zones.json is never rewritten from NVS or the legacy copy and no stale copy is adopted (latch stays, firing refused); a rejected file with no .bad copy is not overwritten.**

**Where**
- `persist/zones_config_cfg_fs.c:356-371`. This is the `!file_valid` branch of `resolve_with_file_buf`. When the NVS candidate is valid, it calls `zones_config_cfg_fs_save(nvs_cfg, nvs_rev)` without checking why the file was rejected, including when the file was rejected as a NEWER schema version.
- `persist/zones_config_store.c:424-470`. `migrate_from_default_partition` adopts the default-partition copy and calls `nvs_save()`, which dual-writes zones.json. `http/zones_http.c:743-766` runs this migration when `kiln_nvs` has no blob.

**What goes wrong**
- A file written by newer firmware is replaced by the stale NVS or legacy config. This happens after a rollback, or when the zones.json CRC or decode fails.
- After the replacement, `s_zones_config_valid` is true and firings are allowed on the stale config.
- `zones.json.bad`, new in ab210508, is now the only surviving copy of the newer data. Before ab210508 the data was lost outright, so the new commit limits the damage but does not remove the overwrite.

**Test gap**
- `test/test_zones_config_cfg_fs.c` `test_rejected_file_is_preserved_and_latches_fault` covers only the no-NVS case.
- Its case-3 assertion, "newer-version zones.json is never rewritten", holds only because there is no NVS candidate.

**Recommendation**
- In `resolve_with_file_buf`, do not rewrite zones.json when the reject kind is NEWER, and probably also when the file was rejected for any reason.
- Apply the same rule in the migration path.
- Add a test with a valid NVS blob plus a NEWER file and a corrupt file. It should assert that zones.json is unchanged.

### MED-2 (ab210508): the load-fault latch fires even when a valid candidate was adopted, so banners claim firing is refused when it is not

**FIXED in SHAPLACE: latch only when no trustworthy copy was adopted; web and LCD banners worded by rejection reason; boot log no longer claims a zeroed config when a copy was adopted.**

**Where**
- `persist/zones_config_store.c:613-618`. The latch is set whenever a file rejection is recorded, regardless of what happened next. The NVS or legacy candidate may have been adopted and `zones_config_is_valid()` may be true.

**What the user sees in that case**
- Web, `http/main_page.html:2069-2093`: "ZONE CONFIG NOT UNDERSTOOD -- FIRING REFUSED ... running on blank/default settings". The firing gate in `profile_executor_run.c` checks `zones_config_is_valid()` first and only uses the fault to choose the message, so firing is actually allowed.
- LCD, `ui/ui_page_home_refresh.c:291-303`: "CONFIG QUARANTINED -- v%u unreadable, reflash matching fw".
  - This is wrong for a CRC or too-short reject: it prints "v0", and reflashing does not help.
  - This banner ranks above the kiln_cfg_swap boot-fault banner, so it can hide that banner.
- Boot log, `persist/zones_config_store.c:619-632`: the UNTRUSTWORTHY line says "running on a zeroed config". That is false when the default-partition migration adopts a copy later in the same boot (`zones_http.c:743-766`).
- The web remedy text, "cannot migrate it forward", only fits the NEWER reject kind.

**Recommendation**
- Latch only when no trustworthy candidate was adopted.
- Otherwise record a separate, informational "file rejected, fell back to NVS" state with its own wording.
- Word the banners by reject kind: NEWER, CRC, too-short or decode.

### MED-3 (pre-existing, adjacent to 0aec8e7c): a blank xzone on the zones page silently disables guard 8

**FIXED in SHAPLACE: page refuses a blank xzone; firmware keeps the stored value for a blank or missing guard/xzone field.**

**Where**
- `http/zones_page.html:2665`. `ZONE_OPTIONAL_KEY_RE` includes `xzone`, so `omitBlankOptionalParams` drops a blank `z<N>_xzone` from the POST.
- The new `ZONE_GUARD_BLANK_RE` at `:2660` lists only the 8 timing and rate guards, not xzone.
- In firmware (`http/zones_http_post_parse.c:707-726`), a blank xzone keeps the stored value, but an omitted xzone leaves `cross_zone_max_delta_c` at the zero that `zones_post_apply_body` memset it to. Zero means guard 8 is disabled.

**Why it matters**
- Clearing the field on the web page turns off the cross-zone guard with no warning. That conflicts with the owner decision that there should be no thermal-guard disable path (2026-09-28).
- The new comment at `:2657` says an omitted field means "keep the stored value". That is wrong for xzone and for every other field, because `zones_post_apply_body` starts from a memset tmp.

**Recommendation**
- Either add `xzone` to `ZONE_GUARD_BLANK_RE` so a blank is refused, or send a blank xzone as-is so firmware keeps the stored value.
- Correct the comment.

### LOW-1 (ab210508): read-only paths now write `.bad` outside any save lock, with a factory-reset fence race

**PARTLY FIXED in SHAPLACE: a failed .bad write is reported (file_rejected/bad_copy_failed) and blocks overwrite; the .bad write is still not moved under the save lock.**

- `load_raw` callers that now reach `preserve_rejected_file` (`persist/zones_config_cfg_fs.c:63-88`) at runtime:
  - GET /api/cfgfs (`diagnostics_http.c:1569`)
  - `zones_config_persisted_equals_ram` (`persist/zones_config_store.c:819`), used by aux convert (`zone_aux_convert_http.c:112`)
  - the save read-back (`persist/zones_config_store.c:523`)
- `preserve_rejected_file` checks `pref_cfg_fs_reset_refuses_write()` before it queues the write to the flash worker.
- A factory reset can pass its delete phase between that check and the worker running the write. The `.bad` file would then survive the reset.
- The window is small, and the file is only a diagnostic copy.
- Recommendation: run the preserve under the zones save lock, or have the flash worker re-check the fence.

### LOW-2 (ab210508): partition-full behaviour and visibility of `.bad`

**FIXED in SHAPLACE: bad_copy_failed is in the fault record and the web banner.**

- On a full `cfg` partition the roughly 1 KB `.bad` write fails and is only logged.
- If the write succeeds, the file persists until factory reset and is not listed anywhere a user would look. Its space can later make a zones.json atomic write fail.
- Recommendation: include `.bad` presence and size in the cfgfs status, and give the user or tooling a way to delete it.

### LOW-3 (ab210508, by design): the latch persists until reboot after a heal

**FIXED in SHAPLACE: a successful POST /api/zones (and a legacy-copy adoption) clears the latch.**

- After a successful POST /api/zones, `s_zones_config_valid` is set (`http/zones_http_post.c:759`) and firing is allowed. The banner stays up until the next boot, which then finds a valid file and does not latch.
- Recovery without a factory reset therefore works. Only the banner is stale for the rest of that boot.

### INFO

- **5e03dbc0.** `wifi_prov_request_arrived_on_ap` (`net/wifi_prov_link.c:967-982`) compares the local IP string with "192.168.4.1".
  - This means `on_ap` already implies the AP address is 192.168.4.1. The netif read in `http/http_origin_check.h:294-307` is redundant, and its warning branch is practically unreachable. Harmless.
  - Pre-existing edge case: a station whose LAN address is 192.168.4.1 is misclassified as AP.
- **ea9aea5a.** The "Ordering: both are seq_cst RMWs..." comment in `http/profiles_http.c:94-124` now sits above the hook variable instead of above `gen_begin`.
- **0aec8e7c.** The 30 s AbortController on the zones save stops only the client. The server still completes the save, so a timeout message does not mean the save failed.
- **0aec8e7c (pre-existing).** Wizard step 11's inner fetch chain is not returned. A network rejection of `postStepState(11)` becomes an unhandled promise rejection with no user message.

## Checked, no defect

- **ab210508 reason buffer.** `note_reject` writes "cfg file: %s" into `reason[96]` with snprintf, so long text is truncated safely.
- **ab210508 escaping.**
  - The dashboard copies are escaped into buffers sized at least 2x+1 (`dashboard_http.c:471-476`, `dashboard_status_http.c:493-496`).
  - The web banner uses textContent.
- **ab210508 rewrite.** A NEWER file is rewritten only in the candidate-present case (MED-1). With no candidate it is never rewritten.
- **ab210508 identical skip.** An existing `.bad` with the same content is not rewritten.
- **ab210508 OOM.** OOM is excluded from preservation, so a transient allocation failure never produces a `.bad`.
- **ab210508 factory reset.** `zones.json.bad` is in `kiln_scope_cfg_files.c`, so factory reset deletes it (`factory_reset.c:406`), and `check_kiln_scope_cfg_mirrors.ps1` passes with 16 entries.
- **ab210508 firing gate.** The latch does not block a POST /api/zones heal. The firing gate keys on validity, not on the latch.
- **ab210508 test hook.** `zones_config_load_fault_reset_for_test` (`persist/zones_config_store.c:96`) is not compiled out but has no production caller.
- **5e03dbc0 captive helper.**
  - Off-AP requests get a relative "/". On-AP requests get an absolute URL, falling back to 0x0104a8c0, which is 192.168.4.1 and correct.
  - Buffer handling is bounded.
  - Tests are in `test/test_http_auth_enforce.c:748-750`.
- **ea9aea5a.**
  - `s_slot_gen_hook` is a static NULL whose setter has no production caller, so the hook is inert.
  - It is called after `atomic_fetch_add`, with no lock or ordering change.
  - It is not compiled out; the cost is one pointer test.
- **0aec8e7c XSS.**
  - `kcHostRefusalFromText` wraps `JSON.parse` in try/catch (app.js 1154-1158), and `kcHostRefusalText` tolerates null and non-objects.
  - Every sink is textContent or `renderErrList` via `kcEscapeHtml`.
  - The OTA messages (`net/ota_page.html` 534-851) use textContent or `kcConfirm`.
  - The login page guards with `window.kcHostRefusalFromText &&`.
- **0aec8e7c blank-guard refusal.** Refusing a blank in any of the 8 guards is stricter than firmware, which keeps the stored value on a blank (`zones_http_post_parse.c:607-700`). The two never conflict.
- **0aec8e7c wizard.**
  - Steps 1, 2, 4, 5 and 6 go through `postStepStateOrThrow`. Steps 3 and 7 check `stepSaveFailed`.
  - Step 11 now requires a matching `web_enabled` read-back.
- **0aec8e7c OTA.** The 403 refusal mapping is correct.
