# REVIEW_GH_FETCH_2026_10_18 (HIGH 0, MED 0, LOW 2, INFO 4)

Review-only, no board. Base: origin/dev at 207134862 (dev tip at review time). Scope: the KilnFW GitHub
update fetch: `firmware/KilnFW/App/drivers/update/update_fetch.c` (check/download job, `http_get`,
sinks, `run_job`, `fetch_task` cleanup), `update_url.c` (URL parse, host allow-list, redirect rule,
asset-URL pin), `update_release.c` (release list/API/manifest JSON), `update_policy.c` (prerelease,
downgrade typed confirm), and `update_stage.c` (begin/finish/abort as reached from the fetch).

## Findings

- **GF-01** LOW, `update_release.c:909`. The manifest `repo` field is optional: a `release.json` that
  omits it passes, and only a present-but-different value is refused (`UPDATE_REL_E_REPO_MISMATCH`).
  The correct-project check (owner 2026-10-07: no signing, correct-project check suffices) then rests
  only on the asset-URL pin (`update_asset_url_matches`, `github.com/<repo>/releases/download/<tag>/<name>`).
  `tools/release_manifest.py:219,255` always writes and validates `repo`, so requiring it costs nothing.
  Fix: refuse a manifest without `repo` (new error or reuse REPO_MISMATCH); host test with the field absent.
- **GF-02** LOW, `update_fetch.c:59,63,938`. The list read (`/releases?per_page=15`) goes into a fixed
  256 KiB body (`FETCH_API_BODY_CAP`). Each release object carries its `body` (GitHub allows about 125 000
  characters of notes), so 15 releases with long notes overflow it and the whole check fails `too_large`.
  The FA-01 fallback to `/releases/latest` only runs on `no_release`, not on `too_large`. Safe (fails
  closed, bounded), but a long changelog makes update checks fail for every board. Fix: lower per_page
  when the body would not fit, or fall back to `/releases/latest` (stable) on `too_large`; keep release
  notes short in the release tooling meanwhile.

## Checked and found clean (INFO)

- **GF-03** INFO. Redirects: `disable_auto_redirect`, at most `UPDATE_MAX_REDIRECTS` hops, each Location
  goes through `update_url_check` (lowercase `https://` only, no userinfo/IPv6/port other than 443, no
  control characters or backslash, length cap, host on a 4-entry GitHub allow-list). A relative or
  foreign-host Location fails closed. Location capture refuses truncation or duplicates. A hop may land
  on another path of an allowed GitHub host; integrity then rests on TLS plus the manifest SHA-256,
  which is acceptable because every allowed host is GitHub-operated.
- **GF-04** INFO. TLS: `esp_crt_bundle_attach` on every session (no pinning, by design), check and
  download refused until SNTP has synced (cert validity dates). Not a gap given the owner's no-signing decision.
- **GF-05** INFO. Bounds: memory sink refuses `clen > cap` and every append past `mem_cap`; known-length
  bodies are refused on overrun and on short body; chunked bodies are bounded by the sink. The stage sink
  refuses a Content-Length different from the API asset size and any byte past `app_size`; the manifest
  size must equal the API size. The JSON walker is length-bounded (never NUL-reliant), depth-capped,
  truncates strings with an overflow flag, maps `\u0000` and non-ASCII to `?`, and refuses duplicate keys.
  Header/body reads are sliced (1 s) with a 10 s stall limit and a 20 min job deadline; cancel and heat-run
  are re-checked per slice.
- **GF-06** INFO. Abort/partial download: no resume (every download restarts from byte 0, by design). The
  stage header is written last, only after the streamed SHA, a flash re-read SHA and `esp_image_verify`
  (OP-01) agree; any failure before that leaves no valid header, so the stage reads "not staged".
  `fetch_task` aborts the hash and issues `WR_ABORT` on every failure after begin; a wedged writer keeps
  the buffers alive until the late op returns. The asset SHA-256 is compared to the manifest before
  `WR_FINISH`. Semver pick (AUP-02) takes the highest eligible semver on the page, skips drafts, and drops
  tags with a prerelease part when prereleases are off even if GitHub marks them stable; the FA-01
  `/latest` fallback is covered by the policy's own `has_pre` refusal (`update_policy.c:112`). Downgrade and
  unknown-running-version `force` both need the typed confirm equal to the exact tag.

No duplicates in KNOWN_ISSUES (FA-01, AUP-02, OP-01, RX-11 cover adjacent, already-fixed items).
