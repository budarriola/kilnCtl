# check_bench_test_registry.ps1 -- keeps docs/BENCH_TEST_SYSTEM_PLAN.md's case
# tables and tools/PcTools/src/kilnctrl/bench_test/registry.py's REGISTRY from
# silently drifting apart. Bidirectional: every case id mentioned in the plan
# doc's tables must exist in REGISTRY, and every id in REGISTRY must be
# mentioned somewhere in the plan doc.
#
# Exit 0 on agreement, 1 on any mismatch (named).
$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$venvPython = Join-Path $repoRoot "tools\PcTools\.venv\Scripts\python.exe"
$planDoc = Join-Path $repoRoot "docs\BENCH_TEST_SYSTEM_PLAN.md"

if (-not (Test-Path $venvPython)) {
    Write-Error "check_bench_test_registry.ps1: venv python not found at $venvPython"
    exit 1
}
if (-not (Test-Path $planDoc)) {
    Write-Error "check_bench_test_registry.ps1: plan doc not found at $planDoc"
    exit 1
}

$pyScript = @'
import re
import sys

plan_path = sys.argv[1]
repo_root = sys.argv[2]
sys.path.insert(0, repo_root + r"\tools\PcTools\src")

from kilnctrl.bench_test import registry

with open(plan_path, "r", encoding="utf-8") as f:
    text = f.read()

# Case ids look like ST-05, FL-01, OT-E01, AT-01, HP-01, SP-01, LCD-01,
# WEB-DASH-01 -- an all-caps area code (optionally two segments) followed by
# a two-digit number, with an optional single letter glued to the number
# (OT-E01's "E").
pattern = re.compile(r"\b[A-Z]{2,4}(?:-[A-Z]{2,5})?-[A-Z]?\d{2}\b")
doc_ids = set(pattern.findall(text))

# The plan doc spells out most areas' ids literally, but the WEB sub-areas
# are sometimes referenced bare (e.g. "WIFI-06" in the §3 summary table's
# WIFI column, rather than "WEB-WIFI-06"). Normalize a bare WEB-sub-area id
# to its full WEB-<AREA>-NN form so it isn't reported as a spurious mismatch.
_WEB_SUBAREAS = (
    "DASH", "PROF", "ZONE", "SAF", "STIM", "COMM", "RDY", "WIZ", "DIAG",
    "OTA", "WIFI", "SEC", "BAK", "KCFG", "SET", "DISP", "LOG", "X",
)
normalized_doc_ids = set()
for cid in doc_ids:
    area = cid.split("-", 1)[0]
    if area in _WEB_SUBAREAS and not cid.startswith("WEB-"):
        normalized_doc_ids.add("WEB-" + cid)
    else:
        normalized_doc_ids.add(cid)
doc_ids = normalized_doc_ids

registry_ids = set(registry.REGISTRY.keys())

# WEB-* and LCD-* are enumerated in the plan doc as counts per sub-area
# (§3's summary table: "WEB | 96 (DASH 13, PROF 11, ...)", "LCD | 21"),
# not spelled out one id at a time everywhere -- individual ids only show
# up where a specific case is called out (suite membership, a footnote).
# Reconcile those two areas by count instead of demanding every id appear
# literally in the doc text.
problems = []

web_ids = {c for c in registry_ids if c.startswith("WEB-")}
lcd_ids = {c for c in registry_ids if c.startswith("LCD-")}
other_registry_ids = registry_ids - web_ids - lcd_ids
other_doc_ids = {c for c in doc_ids if not c.startswith("WEB-") and not c.startswith("LCD-")}

only_in_doc = sorted(other_doc_ids - other_registry_ids)
only_in_registry = sorted(other_registry_ids - other_doc_ids)
if only_in_doc:
    problems.append("Case ids in the plan doc but missing from registry.py:\n" +
                     "\n".join(f"  {c}" for c in only_in_doc))
if only_in_registry:
    problems.append("Case ids in registry.py but not found in the plan doc:\n" +
                     "\n".join(f"  {c}" for c in only_in_registry))

# The doc's own WEB header figure (96) does not match the sum of its own
# parenthetical per-sub-area breakdown (119) -- a pre-existing typo in the
# doc, not something this check can fix. Validate against the itemized
# breakdown (the number actually used to derive registry.py's per-area
# lists), which is the number that matters for catching real drift.
web_breakdown_match = re.search(r"\|\s*WEB\s*\|\s*\d+\s*\(([^)]+)\)", text)
if not web_breakdown_match:
    problems.append("Could not find the WEB per-sub-area breakdown in the plan doc's §3 summary table")
else:
    breakdown_sum = sum(int(n) for n in re.findall(r"(\d+)", web_breakdown_match.group(1)))
    if breakdown_sum != len(web_ids):
        problems.append(
            f"WEB case count mismatch: plan doc's per-sub-area breakdown sums to "
            f"{breakdown_sum}, registry.py has {len(web_ids)}"
        )

lcd_total_match = re.search(r"\|\s*LCD\s*\|\s*(\d+)\s*\|", text)
if not lcd_total_match:
    problems.append("Could not find the LCD total count in the plan doc's §3 summary table")
elif int(lcd_total_match.group(1)) != len(lcd_ids):
    problems.append(
        f"LCD case count mismatch: plan doc says {lcd_total_match.group(1)}, "
        f"registry.py has {len(lcd_ids)}"
    )

if problems:
    print("\n\n".join(problems))
    sys.exit(1)

print(f"OK: {len(registry_ids)} case ids agree between the plan doc and registry.py "
      f"({len(other_registry_ids)} literal + {len(web_ids)} WEB by count + {len(lcd_ids)} LCD by count)")
sys.exit(0)
'@

$tmpScript = Join-Path $env:TEMP "check_bench_test_registry_$PID.py"
Set-Content -Path $tmpScript -Value $pyScript -Encoding utf8

try {
    & $venvPython $tmpScript $planDoc $repoRoot
    $exitCode = $LASTEXITCODE
}
finally {
    Remove-Item -Path $tmpScript -Force -ErrorAction SilentlyContinue
}

exit $exitCode
