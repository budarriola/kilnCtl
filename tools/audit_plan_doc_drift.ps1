# audit_plan_doc_drift.ps1 -- flags likely drift between plan/status .md docs
# and the code they describe: a doc asserting a code artifact is missing/open
# when it demonstrably exists, or citing a file/endpoint/commit that does not
# exist. ADVISORY ONLY -- see "WHY ADVISORY, NOT A GATE" below. It is
# deliberately named audit_*, not check_*, so run_all_checks.ps1's discovery
# glob does not pick it up.
#
# WHY THIS EXISTS. Four confirmed instances in about a day, each costing a
# full agent survey pass to discover the doc was stale:
#   - DRAM_PSRAM_PLAN.md sec 4.1/4.2 listed as remaining work; commit a698dc0
#     had already landed both (fixed by 409d157).
#   - WEB_UI.md documented rules_http.c / POST /api/relay as live; both were
#     deleted 2026-08-27 (fixed by 2316ac9).
#   - PID_EXPANSION_PLAN.md sec 3.6 said a zones_http_client field-mapping bug
#     was "being fixed separately"; it had been fixed days earlier in
#     b1ea749d, with a named regression test (fixed by a6a25da).
#   - SaftyFW TODO.md listed S2/S6/S10 and tc_placement_mode/tc_source as
#     open; all were implemented with host tests (now shows as `- [x]`).
# The reverse also happened once and is worse: an audit claimed six KilnFW
# host-test files were orphaned when they are pulled in via #include from
# test_adaptive_tune.c. That shape (falsely claiming something is MISSING
# when it exists as a reference/include) is explicitly OUT OF SCOPE here --
# it needs understanding build graphs / #include chains, which is not a
# syntactic doc-vs-grep check. Do not read a clean run of this script as
# proof that direction is covered.
#
# WHAT THIS CAN AND CANNOT CATCH.
# This is a syntactic scanner, not a comprehension engine. It cannot decide
# "is this plan item done" in general. It only checks the narrow, genuinely
# checkable shape the four cases share: the doc text names a CONCRETE code
# artifact (a file path, an HTTP route, a commit hash, or a bare symbol name)
# and makes an assertion a grep can contradict.
#
#   A. Dead file reference   -- backtick-quoted path w/ a known code extension
#                                that does not resolve under any of the repo's
#                                known source roots, and the line has no
#                                deletion/rename acknowledgement language.
#   B. Dead endpoint          -- backtick-quoted `GET /x` / `POST /x` / bare
#                                `/api/...` that does not appear as a live
#                                `.uri = "..."` registration anywhere in
#                                firmware/**/*.c, again with no ack language.
#   C. Dangling commit cite   -- backtick hex token 7-40 chars that does not
#                                resolve to a real commit via `git cat-file`.
#   D. "Still open" but a matching symbol/file already exists -- a line
#                                carrying a small, deliberately narrow set of
#                                phrases that assert an ACTIVE blocker or
#                                current non-existence ("being fixed
#                                separately", "not yet implemented", "not
#                                implemented", "still open/blocked", "blocked
#                                by/briefly a ...") followed by a backtick-
#                                quoted identifier that turns out to have a
#                                real function definition or same-stem source
#                                file in the tree. Generic `- [ ]` open
#                                checkboxes are deliberately EXCLUDED from the
#                                trigger -- this repo's TODO/plan docs carry
#                                hundreds of legitimately-still-open items
#                                behind that syntax, and including it drowned
#                                the four real cases in noise on the first
#                                pass (30+ hits, nearly all noise).
#
# D is the highest false-positive-risk detector by a wide margin -- a doc can
# legitimately name a symbol that exists today while describing work still
# not done to it (a field can exist and be unused, a function can exist and
# be wrong). D does NOT prove the doc is stale. It proves only that the named
# artifact exists, which is exactly the fact a human needs to go re-check the
# claim quickly instead of running a full survey pass. Treat every D hit as
# "look here first", not "this doc is wrong".
#
# EVERYTHING THIS CANNOT CATCH: prose claims with no named artifact ("the
# hardware run hasn't happened" -- true or false only a human/board can say);
# a claim that is accurate about a symbol that changed MEANING without
# changing NAME; the reverse direction (doc says done, code quietly reverted
# it, symbol still exists); the orphaned-test-file class above; anything
# where the artifact name in the doc doesn't literally match the one in code
# (renames defeat every detector here).
#
# WHY ADVISORY, NOT tools/run_all_checks.ps1's GATE. Every other check_*.ps1
# in this tree proves a mechanical invariant (a cap that must stay ahead of a
# count, a naming convention, a macro that must appear). This script instead
# proves "an artifact with this name exists", which is evidence for a human
# judgment call, not a pass/fail fact -- two of the four detectors (A and D)
# depend on an "acknowledgement language" allow-list that will always be
# incomplete, and D's false-positive rate on a doc that legitimately discusses
# an existing-but-broken symbol is real. This project has a documented case of
# a sweep producing false reds from bugs in the sweep itself, after which its
# output was distrusted and ignored -- gating the build on this script risks
# exactly that outcome for every check_*.ps1 that runs alongside it. Findings
# are printed for human review; the exit code is 0 whenever the scan itself
# completed, whatever it found. Non-zero means the scan itself broke (bad
# path, git failure), which IS worth alerting on.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\audit_plan_doc_drift.ps1
#        [-DocPath <path-to-single-md-file>]   # for testing against one file
param()

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot

# ---------------------------------------------------------------------------
# Discover plan/status docs: every .md in the tree except build output,
# dependency dirs, and vendored/dotted directories -- same exclusion shape as
# run_all_checks.ps1, plus the two submodule tool trees (their own docs are
# not this repo's plan docs) and firmware build/ dirs (generated).
$docs = Get-ChildItem -Path $repoRoot -Filter "*.md" -Recurse -File |
    Where-Object {
        $_.FullName -notmatch '\\build\\' -and
        $_.FullName -notmatch '\\node_modules\\' -and
        $_.FullName -notmatch '\\managed_components\\' -and
        $_.FullName -notmatch '\\components\\lvgl\\' -and
        $_.FullName -notmatch '\\tools\\mykicadMcp\\' -and
        $_.FullName -notmatch '\\tools\\pdfMcp\\' -and
        # Vendor/third-party trees dropped into Datasheets/ (a copy of lvgl's
        # own docs+changelog ships inside a display module's demo archive) --
        # not this project's plan docs, and lvgl's own CHANGELOG.md cites
        # hundreds of upstream commit hashes this repo's history never had.
        $_.FullName -notmatch '\\Datasheets\\' -and
        $_.FullName -notmatch '\\\.[^\\]+\\'
    } |
    Sort-Object FullName

if ($docs.Count -eq 0) {
    Write-Host "FAILED: found 0 .md docs under $repoRoot -- glob is almost" -ForegroundColor Red
    Write-Host "        certainly broken (this repo has dozens). Not trusting a clean scan." -ForegroundColor Red
    exit 2
}

# Doc-cited paths are resolved by SUFFIX MATCH against every real file in the
# repo, not by guessing which base directory the doc author had in mind.
# Earlier version tried repo-root-relative plus a fixed list of firmware
# roots plus the doc's own directory; that missed genuine references like
# "SaftyFW/TODO.md" (two path segments short of any of those roots) and
# broke on legitimate "../.." doc-relative paths whose depth assumes a
# different base than the .md file's own location -- both produced false
# "dead file" positives on real, current paths. A suffix match against a
# full repo file index has neither failure mode: it only cares that SOME
# real file's path ends with the cited path's segments.
$allFiles = Get-ChildItem -Path $repoRoot -Recurse -File |
    Where-Object {
        $_.FullName -notmatch '\\build\\' -and
        $_.FullName -notmatch '\\node_modules\\' -and
        $_.FullName -notmatch '\\managed_components\\' -and
        $_.FullName -notmatch '\\\.[^\\]+\\'
    }
# Index by basename (lowercase) -> list of full relative paths (forward
# slashes), so a suffix check only has to scan same-named candidates.
$filesByBasename = @{}
foreach ($f in $allFiles) {
    $rel = $f.FullName.Substring($repoRoot.Length + 1).Replace('\', '/')
    $key = $f.Name.ToLowerInvariant()
    if (-not $filesByBasename.ContainsKey($key)) { $filesByBasename[$key] = New-Object System.Collections.Generic.List[string] }
    $filesByBasename[$key].Add($rel)
}

function Resolve-DocPath {
    param([string]$RelPath)
    # Normalize: forward slashes, drop leading "./" / "../" segments (suffix
    # matching makes the exact number of ".." hops irrelevant).
    $clean = $RelPath.Replace('\', '/') -replace '^(\.\./|\./)+', ''
    $baseName = ($clean -split '/')[-1].ToLowerInvariant()
    if (-not $filesByBasename.ContainsKey($baseName)) { return $false }
    foreach ($cand in $filesByBasename[$baseName]) {
        if ($cand -eq $clean -or $cand.EndsWith("/$clean")) { return $true }
    }
    return $false
}

# ---------------------------------------------------------------------------
# Build the live HTTP route table once: every `.uri = "..."` registration
# across the firmware trees. This is the same ground truth WEB_UI.md itself
# now points readers at ("grep -n '.uri = \"' App/drivers/*.c").
$routeFiles = Get-ChildItem -Path (Join-Path $repoRoot "firmware") -Filter "*.c" -Recurse -File |
    Where-Object { $_.FullName -notmatch '\\build\\' -and $_.FullName -notmatch '\\managed_components\\' }
$liveRoutes = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($f in $routeFiles) {
    $hits = Select-String -Path $f.FullName -Pattern '\.uri\s*=\s*"([^"]+)"' -AllMatches
    foreach ($m in $hits) {
        foreach ($g in $m.Matches) { [void]$liveRoutes.Add($g.Groups[1].Value) }
    }
}

# All C/H source under firmware, and all Python under tools, as one blob per
# file is too slow to re-grep per identifier across a whole tree for every
# doc line; instead build one combined Select-String pass per document at
# scan time is still too slow across ~1500 files x many identifiers, so
# instead pre-index every top-level function-looking definition and every
# source file's basename stem once.
$codeFiles = Get-ChildItem -Path (Join-Path $repoRoot "firmware") -Include *.c,*.h -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch '\\build\\' -and $_.FullName -notmatch '\\managed_components\\' -and $_.FullName -notmatch '\\components\\lvgl\\' }
$codeFiles += Get-ChildItem -Path (Join-Path $repoRoot "tools") -Include *.py -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch '\\mykicadMcp\\' -and $_.FullName -notmatch '\\pdfMcp\\' -and $_.FullName -notmatch '\\\.venv\\' }

$definedSymbols = New-Object 'System.Collections.Generic.HashSet[string]'
$fileStems = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($f in $codeFiles) {
    [void]$fileStems.Add($f.BaseName)
    # A def-shaped line: an identifier immediately followed by '(' at the
    # start of a line (C function definitions/prototypes; Python `def name(`)
    # -- cheap and deliberately permissive; this indexes call sites too, but
    # that only makes the check MORE conservative (more things count as
    # "exists"), which is the safe direction for an advisory tool.
    # Any word-shaped token >=4 chars, not just def-shaped ones followed by
    # "(" -- a plan doc's "not yet implemented" bullet just as often names a
    # macro, struct field, or JSON key (MALLOC_CAP_DMA, heap_internal) as a
    # callable function, and requiring "(" missed those in testing against
    # this repo's own real stale-doc history. Wider recall is the safe
    # direction for an advisory "go look here" signal.
    $hits = Select-String -Path $f.FullName -Pattern '\b([A-Za-z_][A-Za-z0-9_]{3,})\b' -AllMatches
    foreach ($m in $hits) {
        foreach ($g in $m.Matches) { [void]$definedSymbols.Add($g.Groups[1].Value) }
    }
}

Write-Host "Indexed $($codeFiles.Count) source files, $($definedSymbols.Count) candidate symbols, $($liveRoutes.Count) live HTTP routes."
Write-Host "Scanning $($docs.Count) markdown docs..."
Write-Host ""

# ---------------------------------------------------------------------------
# Deliberately broad -- these are all ways this repo's own docs correctly
# talk about something that ONCE existed, without using the exact words the
# first pass's narrower list required. A 5-line context window plus this list
# still cannot cover every phrasing; see the header comment's limits section.
$ackPattern = '(?i)\b(deleted|removed|no longer exist|was deleted|used to exist|don''t look for|renamed|moved|gone|disabled|not built|deprecated|went with it|old .*endpoint|mirroring|mirrors|is not a |predate|frozen where|has no UART mirror|old\b|is history|superseded|replaced by)\b'
# Deliberately narrower than "every open checkbox" -- this repo's TODO/plan
# docs use `- [ ]` for hundreds of genuinely-still-open future items, and a
# generic checkbox trigger buried the four real cases in noise on first run
# (30+ hits, nearly all legitimate open work, none of them the shape this
# audit is for). These phrases are the ones the four confirmed cases actually
# used to assert CURRENT non-existence or an active blocker, not merely "this
# is future work":
$openPattern = '(?i)(being fixed separately|not yet implemented|\bnot implemented\b|remaining work, in full|blocked (by|briefly) a|still (open|blocked)|- \[ \] \*\*Implement|commissioning field\*\*)'
$pathPattern = '`([A-Za-z0-9_./\\-]+\.(c|h|py|html|ps1|md|js|css))`'
$endpointPattern = '`((GET|POST|PUT|DELETE)\s+)?(/[A-Za-z0-9_/.\-]{3,})`'
$hashPattern = '`([0-9a-f]{7,40})`'
$identPattern = '`([A-Za-z_][A-Za-z0-9_]{3,})`'

$findings = New-Object System.Collections.Generic.List[object]

foreach ($doc in $docs) {
    $relDoc = $doc.FullName.Substring($repoRoot.Length + 1)
    $lines = Get-Content -LiteralPath $doc.FullName -ErrorAction SilentlyContinue
    if (-not $lines) { continue }

    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        $lineNo = $i + 1

        # Acknowledgement language ("...was deleted, don't look for it") is
        # often in the sentence BEFORE or AFTER the citation once markdown
        # wraps a paragraph across lines (WEB_UI.md does this routinely after
        # its own rules_http.c cleanup) -- checking only the exact line
        # produced false positives on prose that already correctly explains
        # a deletion. Widen to a 5-line window centered on the hit.
        $ctxLo = [Math]::Max(0, $i - 2)
        $ctxHi = [Math]::Min($lines.Count - 1, $i + 2)
        $context = ($lines[$ctxLo..$ctxHi] -join " ")

        # --- A: dead file reference ---
        foreach ($m in [regex]::Matches($line, $pathPattern)) {
            $relPath = $m.Groups[1].Value
            if ($relPath -notmatch '[\\/]') { continue }  # bare filename, too ambiguous
            if ($relPath.StartsWith('/')) { continue }     # URL path, not a file path -- detector B's job
            if ($context -match $ackPattern) { continue }
            # "name.c/.h" / "name.h/.c" is this codebase's own doc shorthand
            # for "both files exist" -- normalize to the first file named and
            # accept either extension resolving (DISPLAY_ST7796_PLAN.md,
            # ARCHITECTURE.md and others use this constantly for real,
            # current pairs; treating it literally made every one of them a
            # false "dead file").
            $pairMatch = [regex]::Match($relPath, '^(.+)\.(c|h)/\.(c|h)$')
            $resolved = $false
            if ($pairMatch.Success) {
                $stem = $pairMatch.Groups[1].Value
                $resolved = (Resolve-DocPath -RelPath "$stem.$($pairMatch.Groups[2].Value)") -or (Resolve-DocPath -RelPath "$stem.$($pairMatch.Groups[3].Value)")
            } else {
                $resolved = Resolve-DocPath -RelPath $relPath
            }
            if (-not $resolved) {
                $findings.Add([pscustomobject]@{
                    Kind = "A:dead-file"; Doc = $relDoc; Line = $lineNo
                    Detail = ("references ``{0}`` -- no file in the repo matches that path" -f $relPath)
                    Text = $line.Trim()
                })
            }
        }

        # --- B: dead endpoint reference ---
        foreach ($m in [regex]::Matches($line, $endpointPattern)) {
            $route = $m.Groups[3].Value
            if ($route -notmatch '^/(api|settings|diagnostics|readiness|safety|ota|profiles|wifi)\b') { continue }
            if ($context -match $ackPattern) { continue }
            if (-not $liveRoutes.Contains($route)) {
                $findings.Add([pscustomobject]@{
                    Kind = "B:dead-endpoint"; Doc = $relDoc; Line = $lineNo
                    Detail = ("references route ``{0}`` -- no live .uri registration found" -f $route)
                    Text = $line.Trim()
                })
            }
        }

        # --- C: dangling commit citation ---
        foreach ($m in [regex]::Matches($line, $hashPattern)) {
            $hash = $m.Groups[1].Value
            if ($hash.Length -lt 7) { continue }
            # An all-digit token (a Mouser/JLCPCB part number, say) is valid
            # hex but essentially never a real git short hash -- confirmed
            # false positive in testing (hardware/mainBoard/todo.md's MPN
            # `74269244182`). Require at least one a-f letter.
            if ($hash -notmatch '[a-f]') { continue }
            # Native stderr under $ErrorActionPreference = "Stop" is promoted
            # to a terminating error (same trap run_all_checks.ps1 documents
            # for its own child-process calls) -- a nonexistent hash is the
            # EXPECTED, common case here, not a script bug, so this call runs
            # under "Continue" and the exit code is read directly.
            $prevEAP = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            & git -C $repoRoot cat-file -e "$hash^{commit}" 2>$null
            $gitOk = ($LASTEXITCODE -eq 0)
            $ErrorActionPreference = $prevEAP
            if (-not $gitOk) {
                $findings.Add([pscustomobject]@{
                    Kind = "C:dangling-commit"; Doc = $relDoc; Line = $lineNo
                    Detail = "cites commit ``$hash`` -- not found in this repo's history"
                    Text = $line.Trim()
                })
            }
        }

        # --- D: "still open" language naming an artifact that already exists ---
        # The trigger phrase and the backtick-quoted artifact are frequently
        # NOT on the same line -- DRAM_PSRAM_PLAN.md's real stale text was
        # "Remaining work, in full:" followed by a bulleted list naming
        # `heap_internal`, `dashboard_http.c`, `MALLOC_CAP_DMA` two-plus lines
        # down. Scan a forward block from the trigger line to the next blank
        # line or markdown heading (or a hard cap of 10 lines), the same
        # "read the paragraph, not just the line" shape as the ack-language
        # context window above.
        if ($line -match $openPattern) {
            # A single blank line commonly separates the trigger sentence
            # from the bulleted evidence that follows it (a markdown
            # paragraph break before a list) -- stop only on a heading or two
            # blank lines in a row (an actual section boundary), not the
            # first blank line, or the DRAM_PSRAM_PLAN.md real-world shape
            # ("Remaining work, in full:" <blank line> "- `heap_internal`...")
            # never reaches its own evidence.
            $blockEnd = $i
            $prevBlank = $false
            for ($j = $i; $j -lt [Math]::Min($lines.Count, $i + 12); $j++) {
                if ($j -gt $i -and $lines[$j] -match '^#') { break }
                if ($j -gt $i -and $lines[$j].Trim() -eq '') {
                    if ($prevBlank) { break }
                    $prevBlank = $true
                } else {
                    $prevBlank = $false
                }
                $blockEnd = $j
            }
            # One finding per TRIGGER (not per identifier) -- a block commonly
            # names several existing identifiers for the same open item
            # (tc_placement_mode's line alone names both the field and its
            # two enum values), and a separate finding per identifier just
            # inflates the count a human has to read without adding
            # information. Collect them together instead.
            $seenInBlock = New-Object 'System.Collections.Generic.HashSet[string]'
            $blockHits = New-Object System.Collections.Generic.List[string]
            for ($j = $i; $j -le $blockEnd; $j++) {
                foreach ($m in [regex]::Matches($lines[$j], $identPattern)) {
                    $ident = $m.Groups[1].Value
                    if (-not $seenInBlock.Add($ident)) { continue }
                    if ($definedSymbols.Contains($ident) -or $fileStems.Contains($ident)) {
                        [void]$blockHits.Add($ident)
                    }
                }
            }
            if ($blockHits.Count -gt 0) {
                $findings.Add([pscustomobject]@{
                    Kind = "D:open-but-exists"; Doc = $relDoc; Line = $lineNo
                    Detail = ("marks work open/blocked and names {0}, each with a matching symbol or source file in the tree -- verify by hand, this does not prove the doc is stale" -f (($blockHits | ForEach-Object { "``$_``" }) -join ", "))
                    Text = $line.Trim()
                })
            }
        }
    }
}

Write-Host ""
if ($findings.Count -eq 0) {
    Write-Host "No drift signals found across $($docs.Count) docs." -ForegroundColor Green
    exit 0
}

Write-Host "$($findings.Count) signal(s) found -- REVIEW BY HAND, none of these are proven:" -ForegroundColor Yellow
Write-Host ""
foreach ($f in ($findings | Sort-Object Doc, Line)) {
    Write-Host "[$($f.Kind)] $($f.Doc):$($f.Line)" -ForegroundColor Yellow
    Write-Host "    $($f.Detail)"
    Write-Host "    > $($f.Text)"
    Write-Host ""
}

exit 0
