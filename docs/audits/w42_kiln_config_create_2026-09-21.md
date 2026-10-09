# W42 (`/settings/kiln_configs` create-then-delete) live FAIL, root cause — 2026-09-21

## Verdict

**(c) — runner-side test-fixture defect.** The runner's generated throwaway
name overflows the firmware's kiln-config name length limit; the firmware
correctly rejects the `save` POST with 400 and the page correctly surfaces
that rejection in its message area, but the runner's success check
(`_run_kiln_config_create_delete()`) never inspects the POST's HTTP status —
it only checks the CDP subprocess's exit code, then polls `GET
/api/kiln_configs` for a config with the generated name. Since the create
never landed, the name is (correctly) absent, and the runner reports the
generic "write did not land" failure. This is not a firmware defect and not
(b) (no signing is involved on this route) and not (a) (the click landed on
the right elements and the POST did fire).

## Evidence

- **The generated name overflows the limit.** `_run_kiln_config_create_delete()`
  builds `unique_name = f"__kc_web_commission_test_{int(time.time())}__"`
  (`tools/PcTools/src/kilnctrl/web_commission_row.py:628`). With a 10-digit
  Unix timestamp (true today and for the entirety of this decade) that is
  `len("__kc_web_commission_test_") + 10 + len("__")` = `25 + 10 + 2` = **37
  characters**. The driver's own decisive failure line from the bench run
  names the exact string: `'__kc_web_commission_test_1790032582__'` — counted
  directly, that literal is 37 characters long.
- **The firmware's limit is 23.** `#define KILN_CFG_NAME_MAX_LEN 23`
  (`firmware/KilnFW/App/drivers/persist/kiln_cfg_store.h:87`). The `save`
  handler declares `char name[KILN_CFG_NAME_MAX_LEN + 1];` and calls
  `parse_required_name(body, name, sizeof(name))`
  (`firmware/KilnFW/App/drivers/http/kiln_cfg_http.c:190-194`); on failure —
  which a name over 23 characters causes, per the function's own comment at
  `kiln_cfg_http.c:125-126` ("Does NOT itself enforce KILN_CFG_NAME_MAX_LEN"
  — the enforcement is the bounded buffer plus the store's own trim/length
  check at `kiln_cfg_store.c:814`: `if (trimmed_len == 0 || trimmed_len >
  KILN_CFG_NAME_MAX_LEN || trimmed_len >= out_cap)`) — the handler sends
  `HTTPD_400_BAD_REQUEST, "name missing or too long"`
  (`kiln_cfg_http.c:192`). 37 > 23, so this POST is refused.
- **The page surfaces the refusal correctly; it does not swallow it.**
  `kcPost()` always resolves `{httpOk: r.ok, body: parsed-json-or-{}}`
  (`firmware/KilnFW/App/drivers/http/kiln_configs_page.html:258-263`).
  `kcHandleResult()` treats `!result.httpOk` as failure and writes the
  error into the visible `#kcConfigMsg` element
  (`kiln_configs_page.html:265-273`), and the `kcSaveNewBtn` click handler
  calls exactly this path (`kiln_configs_page.html:417-426`). A human
  clicking the same button with the same name would see "name missing or
  too long" in the page's message area and the input would not be cleared
  (the `if (ok) input.value = '';` guard at line 423 does not fire on
  failure). The page is not the defect.
- **The runner's success check never looks at the POST status.**
  `_run_kiln_config_create_delete()` calls `_run_cdp(..., fills=create_fills,
  ...)` and only checks `proc.returncode != 0`
  (`web_commission_row.py:636-638`); the CDP driver
  (`tools/PcTools/scripts/_web_commission_cdp.mjs`, `--expect-post` mode)
  waits for the POST to *complete*, not for it to return 2xx, so a clean
  400 still exits 0. The only place the runner would have learned the real
  reason is the subsequent `GET /api/kiln_configs` read-back
  (`web_commission_row.py:640-648`), which correctly reports the name
  absent but produces the generic "write did not land" message rather than
  surfacing the 400 body — that generic message is what the bench run
  actually printed, and it is consistent with a clean, correctly-enforced
  rejection, not a landed-then-lost write.
- **The list route and client cache are not implicated.** `GET
  /api/kiln_configs` is the right endpoint (same one `kiln_configs_page.html`
  itself polls via `loadKilnConfigs()`, `kiln_configs_page.html:207-213`; same
  one W41's read-only row already exercises, `web_commission_row.py:292-296`)
  and nothing here caches it client-side beyond the in-page
  `kilnConfigsCache` variable, which the runner's own independent `GET` never
  touches — so a stale-cache explanation is ruled out.
- **W22/W38 passing rules out a broader auth/session/CDP regression.** Those
  rows exercise the same session, same CDP driver, same board build
  (33124aa8) and passed the same run — the CDP mechanics and session are
  fine; this failure is specific to the name this one row happens to
  generate.

## Smallest fix

Runner-side, in `web_commission_row.py`'s
`_run_kiln_config_create_delete()`: shorten `unique_name` to fit
`KILN_CFG_NAME_MAX_LEN` (23), e.g. a short fixed prefix plus a truncated
timestamp/counter (`f"kc_test_{int(time.time()) % 100000}"` is 15
characters, well under 23, and still unique enough within one bench session
to avoid colliding with a real config). This is the preferred fix — the page
and firmware both already behave correctly for a human operator entering a
compliant name; only the test fixture's generated name is out of spec. A
secondary, optional hardening: have `_run_kiln_config_create_delete()` check
`create` POST status via a small addition to the CDP driver's `--expect-post`
result (or a plain follow-up `POST` status check) so a future case of this
class reports "create POST returned 400: <body>" instead of the generic
"write did not land," which would have pointed straight at the length limit
without needing this investigation.

No firmware change is needed or recommended.
