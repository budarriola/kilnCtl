#!/usr/bin/env python3
"""update_chain_lint.py -- source-shape guards for the GitHub update chain review fixes
(docs/audits/GITHUB_UPDATE_CHAIN_REVIEW_2026-10-09.md). update_http.c / update_fetch.c are target-only
(no host build), so these are static checks. Exit 0 = pass.
  LOW-1: claim_refuses() re-checks the mode gate AFTER taking the claim and releases it on refusal.
  LOW-2: no portMAX_DELAY wait on wr_done; wr_call uses FETCH_WR_TIMEOUT_MS.
  MED-1: the fetch scratch is MALLOC_CAP_INTERNAL, not a PSRAM array.
  MED-2: WR_BEGIN installs update_stage_manifest_gate.
"""
import pathlib, re, sys

root = pathlib.Path(__file__).resolve().parent.parent / "drivers" / "update"
http = (root / "update_http.c").read_text()
fetch = (root / "update_fetch.c").read_text()
bad = []

m = re.search(r"static bool claim_refuses\(.*?\n}\n", http, re.S)
if not m:
    bad.append("claim_refuses not found")
else:
    b = m.group(0)
    i, j, k = b.find("ota_http_update_try_begin"), b.find("mode_gate_refuses"), b.find("ota_http_update_end")
    if not (0 <= i < j < k):
        bad.append("LOW-1: claim_refuses must take the claim, then re-check the mode gate, then release on refusal")

if re.search(r"xSemaphoreTake\(s_c->wr_done,\s*portMAX_DELAY\)", fetch):
    bad.append("LOW-2: wr_done wait is unbounded")
if not re.search(r"xSemaphoreTake\(s_c->wr_done,\s*pdMS_TO_TICKS\(FETCH_WR_TIMEOUT_MS\)\)", fetch):
    bad.append("LOW-2: wr_call has no bounded wait")
if "uint8_t scratch[" in fetch or "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);" not in fetch.split("static void fetch_task")[1]:
    bad.append("MED-1: fetch scratch must be a MALLOC_CAP_INTERNAL allocation")
if "update_stage_set_gate(st, update_stage_manifest_gate" not in fetch:
    bad.append("MED-2: fetch does not install update_stage_manifest_gate")

for x in bad:
    print("FAIL:", x)
print("update_chain_lint:", "FAIL" if bad else "ok")
sys.exit(1 if bad else 0)
