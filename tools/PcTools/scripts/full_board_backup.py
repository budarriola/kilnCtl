"""full_board_backup.py -- complete pre-partition-change backup of live board state.

Context: docs/FILESYSTEM_PLAN.md / docs/FILESYSTEM_USER_DATA_PLAN.md stage a
partition-table change (a new `cfg` LittleFS partition, append-only into the
free tail). A partition-table change means an `otadata` erase + bootloader
reflash, and any partition whose OFFSET moves loses its data outright. The
most expensive data on this board to regenerate is the tuned PID gains and
coupling matrix (multi-hour bench runs) -- see the zones config inventory
item (#1) in FILESYSTEM_USER_DATA_PLAN.md.

The existing GET /api/backup/export (backup_http.c) covers zones config
(PID/model/coupling/guards/limits) and user fire profiles -- but NOT:
kiln config slots (/api/kiln_configs), firing/autotune history
(/api/firing_history), adaptive-tune state (/api/adaptive_tune), relay
cycle counters and relay_life (in /api/status), ramp-assist
(/api/ramp_assist), display power policy (/api/settings/display_power),
or the RP2040 safety commissioning mirror (/api/safety/commissioning).
This script pulls ALL of it into one timestamped JSON archive over the
existing HTTP API -- no new firmware required, no reflash needed, and it
runs from a PC that is about to survive the reflash the board itself is
not guaranteed to survive unscathed.

DELIBERATELY NOT CAPTURED: Wi-Fi credentials. There is no GET endpoint for
them (by design -- see backup_http.c's own header comment) and this script
does not try any other path to them. If a future endpoint ever exposes
them, do not add it here without the same explicit warning backup_http.c
carries.

ALSO NOT CAPTURED (documented, not silently dropped): /api/settings/tz has
POST only -- no GET handler exists in firmware, so the configured timezone
cannot be read back over the API at all. This is a genuine gap in the
firmware, not a gap in this script; see the "NOT COVERED" section this
script prints and the runbook in docs/FILESYSTEM_PLAN.md.

Output: tools/PcTools/board-backups/<UTC-timestamp>/board_backup.json plus
a manifest.txt (matches the *-backups/ gitignore pattern already in
.gitignore -- never commit board data).

Usage:
    uv run python tools/PcTools/scripts/full_board_backup.py --host 192.168.1.156
"""
from __future__ import annotations

import argparse
import base64
import datetime
import json
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

# Some endpoints (observed on /api/zones's safety_wiring.tc_temp_c) emit a
# bare lowercase `nan` for an unread thermocouple channel -- valid as a
# printf("%f", NAN) string but NOT valid JSON (Python's parser only accepts
# the capitalized NaN/Infinity/-Infinity literals). Rather than silently
# failing to parse an otherwise-good response, normalize the bareword before
# parsing. This is itself a firmware finding worth a one-line fix someday
# (emit `null` for an invalid float, matching the have_model/have_tc
# "omit rather than emit garbage" convention backup_export.c already uses
# elsewhere) -- noted, not fixed, here.
_BAREWORD_NAN_RE = re.compile(r"(?<=[:,\[])\s*nan\s*(?=[,}\]])")
_BAREWORD_INF_RE = re.compile(r"(?<=[:,\[])\s*(-?)inf\s*(?=[,}\]])")

# Endpoints that must exist for the backup to be considered complete for
# their inventory item. (endpoint, inventory_item, required)
GET_ENDPOINTS = [
    ("/api/backup/export", "zones config + profiles (native backup format)", True),
    ("/api/zones", "zones config detail incl. relay_names, zone normals (normal_current_a/measured)", True),
    ("/api/kiln_configs", "named kiln config slots", True),
    ("/api/adaptive_tune", "adaptive-tune Ki base + opt-in state", True),
    ("/api/status", "relay cycle counters, relay_life, unit preference (temp_unit), live channel snapshot", True),
    ("/api/ramp_assist", "ramp-assist enable flag", True),
    ("/api/settings/display_power", "display power / backlight policy", True),
    ("/api/safety/commissioning", "RP2040 safety commissioning mirror: tc_type, abs-max ceilings, CT cal, ct_installed", True),
    ("/api/profiles", "profile list (id/name/zone_mask/segment_count) -- cross-check against /api/backup/export", True),
    ("/api/profiles/builtin", "hidden built-in profiles mask", False),
    ("/api/crash_report", "unacknowledged crash report at backup time (diagnostic, not restorable)", False),
    ("/api/partitions", "running partition table snapshot at backup time (diagnostic)", False),
    # docs/FILESYSTEM_PLAN.md "Add filesystem coverage to the backup": the
    # `cfg` LittleFS partition's own file list -- this is the SAME listing
    # surface the firmware already exposes for observability
    # (cfg_fs_status.c / diagnostics_http.c's cfgfs_status_get_handler()), not
    # a parallel one invented here. Not required: an unformatted/unmounted
    # `cfg` partition (true on every board today, per docs/FILESYSTEM_PLAN.md)
    # answers mounted:false with an empty file list, which is expected, not a
    # failure -- see the cfgfs-specific handling in main() below, which
    # reports this plainly rather than folding it into ok/fail counts.
    ("/api/cfgfs", "cfg filesystem file listing + dual-write status (may be unmounted/empty)", False),
]

# /api/firing_history requires ?profile_id=N -- probe every user profile slot.
FIRING_HISTORY_PROFILE_IDS = range(8)  # PROFILES_MAX_COUNT


def _get_json(url: str, timeout: float):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            body = resp.read()
    except (urllib.error.URLError, OSError) as e:
        return None, f"request failed: {e}"
    try:
        return json.loads(body), None
    except json.JSONDecodeError:
        pass
    # Retry once with bareword nan/inf normalized -- see module comment.
    try:
        text = body.decode("utf-8")
    except UnicodeDecodeError as e:
        return None, f"non-UTF8 response: {e}"
    text = _BAREWORD_NAN_RE.sub("NaN", text)
    text = _BAREWORD_INF_RE.sub(lambda m: (m.group(1) or "") + "Infinity", text)
    try:
        return json.loads(text), None
    except json.JSONDecodeError as e:
        # backup/export is a streamed download but is still valid JSON;
        # a non-JSON body (e.g. an HTML error page) is a real failure.
        return None, f"non-JSON response even after nan/inf normalization ({e}): {body[:200]!r}"


def _get_bytes(url: str, timeout: float):
    """Like _get_json() but for a raw-bytes response (GET /api/cfgfs/file).

    Returns (bytes, None) on success or (None, error_str) on failure -- same
    two-tuple shape as _get_json() so callers share one error-handling
    pattern.
    """
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            return resp.read(), None
    except (urllib.error.URLError, OSError) as e:
        return None, f"request failed: {e}"


def _capture_cfgfs_files(host: str, timeout: float, cfgfs_status) -> tuple[dict, list]:
    """Fetches the raw bytes of every file GET /api/cfgfs listed, base64-
    encoded for the JSON archive (see backup_import.c's own "raw bytes on the
    wire, base64 only in the archive" convention this endpoint's firmware
    side follows -- diagnostics_http.c's cfgfs_file_get_handler()).

    Returns (files_dict, errors_list). files_dict maps filename ->
    {"size_bytes": N, "data_base64": "..."}. A file that fails to fetch is
    recorded in errors_list, not silently dropped -- same "report, don't
    hide" convention as every other endpoint in this script.
    """
    files: dict = {}
    errors: list = []
    if not cfgfs_status or not cfgfs_status.get("mounted"):
        # Not a failure -- see GET_ENDPOINTS's /api/cfgfs comment. The `cfg`
        # partition being unformatted/unmounted is the documented, expected
        # state on every board today.
        return files, errors
    for entry in cfgfs_status.get("files", []) or []:
        name = entry.get("name")
        if not name:
            continue
        url = f"http://{host}/api/cfgfs/file?name={urllib.parse.quote(name)}"
        data, err = _get_bytes(url, timeout)
        if err:
            errors.append({"endpoint": url, "item": f"cfg filesystem file '{name}'", "required": False, "error": err})
            continue
        files[name] = {"size_bytes": len(data), "data_base64": base64.b64encode(data).decode("ascii")}
    return files, errors


def restore_cfgfs_files(host: str, cfgfs_files: dict, timeout: float = 10.0, dry_run: bool = False):
    """Restores every captured cfg-filesystem file back onto a board via
    POST /api/cfgfs/file?name=<name>.

    VALIDATE EVERYTHING, THEN APPLY -- same two-pass discipline
    backup_import_apply() uses for zones/profiles (firmware side), applied
    here for the filesystem side: every entry's base64 is decoded and
    size-checked BEFORE any POST is sent, so a single corrupted/truncated
    field refuses the WHOLE restore rather than writing some files and not
    others. Returns (ok: bool, message: str, decoded: dict[name -> bytes]).
    """
    decoded: dict = {}
    for name, meta in cfgfs_files.items():
        if not isinstance(meta, dict) or "data_base64" not in meta or "size_bytes" not in meta:
            return False, f"cfg file '{name}': archive entry missing data_base64/size_bytes -- refusing whole restore", {}
        try:
            raw = base64.b64decode(meta["data_base64"], validate=True)
        except Exception as e:  # binascii.Error and friends
            return False, f"cfg file '{name}': data_base64 is not valid base64 ({e}) -- refusing whole restore", {}
        if len(raw) != meta["size_bytes"]:
            return False, (
                f"cfg file '{name}': decoded {len(raw)} bytes but archive says size_bytes={meta['size_bytes']} "
                "-- refusing whole restore"
            ), {}
        decoded[name] = raw

    if dry_run:
        return True, f"validated {len(decoded)} cfg file(s), dry run -- nothing written", decoded

    for name, raw in decoded.items():
        url = f"http://{host}/api/cfgfs/file?name={urllib.parse.quote(name)}"
        req = urllib.request.Request(url, data=raw, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                resp.read()
        except (urllib.error.URLError, OSError) as e:
            return False, f"cfg file '{name}': POST failed after {list(decoded).index(name)} prior file(s) already written: {e}", decoded
    return True, f"restored {len(decoded)} cfg file(s)", decoded


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="192.168.1.156", help="board IP or host:port")
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--out-dir", default=None, help="override output directory (default: tools/PcTools/board-backups/<timestamp>)")
    ap.add_argument("--restore-cfgfs-from", default=None, metavar="ARCHIVE_JSON",
                     help="restore ONLY the cfg-filesystem files captured in a previous board_backup.json onto "
                          "--host, validating every entry before writing any of them (see restore_cfgfs_files()). "
                          "Does not touch zones/profiles -- use POST /api/backup/import for those, unchanged.")
    ap.add_argument("--dry-run", action="store_true", help="with --restore-cfgfs-from, validate only, write nothing")
    args = ap.parse_args()

    if args.restore_cfgfs_from:
        archive_path = Path(args.restore_cfgfs_from)
        data = json.loads(archive_path.read_text(encoding="utf-8"))
        cfgfs_files = data.get("cfgfs_files", {})
        if not cfgfs_files:
            print("archive has no cfgfs_files -- nothing to restore")
            return 0
        ok, message, decoded = restore_cfgfs_files(args.host, cfgfs_files, args.timeout, dry_run=args.dry_run)
        print(message)
        if not ok:
            print(f"RESTORE REFUSED -- {len(decoded)} file(s) validated before the failing entry, nothing written")
            return 1
        return 0

    ts = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    repo_root = Path(__file__).resolve().parents[3]
    out_dir = Path(args.out_dir) if args.out_dir else repo_root / "tools" / "PcTools" / "board-backups" / ts
    out_dir.mkdir(parents=True, exist_ok=True)

    archive = {
        "kind": "kilnctl_full_board_backup",
        "script_version": 1,
        "captured_at_utc": ts,
        "host": args.host,
        "endpoints": {},
        "firing_history": {},
        "errors": [],
    }

    ok_count = 0
    fail_count = 0
    for path, item, required in GET_ENDPOINTS:
        url = f"http://{args.host}{path}"
        data, err = _get_json(url, args.timeout)
        if err:
            msg = f"{path} ({item}): {err}"
            archive["errors"].append({"endpoint": path, "item": item, "required": required, "error": err})
            print(("REQUIRED " if required else "optional ") + "FAILED: " + msg)
            fail_count += 1
            continue
        archive["endpoints"][path] = data
        print(f"OK: {path}  ({item})")
        ok_count += 1

    for pid in FIRING_HISTORY_PROFILE_IDS:
        url = f"http://{args.host}/api/firing_history?profile_id={pid}"
        data, err = _get_json(url, args.timeout)
        if err:
            # A profile slot with no history yet, or an unused slot, is not
            # a failure -- only a genuine transport/parse error is recorded.
            archive["firing_history"][str(pid)] = None
            archive["errors"].append({"endpoint": url, "item": f"firing history profile {pid}", "required": False, "error": err})
            continue
        archive["firing_history"][str(pid)] = data

    # docs/FILESYSTEM_PLAN.md "Add filesystem coverage to the backup": pull
    # every listed cfg-filesystem file's raw bytes so a restore can rebuild
    # filesystem state, not just NVS/HTTP-endpoint state. Reported separately
    # from ok_count/fail_count above (not required) -- an unformatted/
    # unmounted `cfg` partition is the expected, documented state on every
    # board today (see GET_ENDPOINTS's /api/cfgfs comment), not a failure.
    cfgfs_status = archive["endpoints"].get("/api/cfgfs")
    cfgfs_files, cfgfs_errors = _capture_cfgfs_files(args.host, args.timeout, cfgfs_status)
    archive["cfgfs_files"] = cfgfs_files
    archive["errors"].extend(cfgfs_errors)
    if cfgfs_status is None:
        print("cfg filesystem: /api/cfgfs did not answer -- filesystem section will be empty")
    elif not cfgfs_status.get("mounted"):
        reason = cfgfs_status.get("reason", "unknown")
        print(f"cfg filesystem: NOT mounted ({reason}) -- filesystem section is empty, this is expected today")
    else:
        print(f"cfg filesystem: mounted, captured {len(cfgfs_files)} file(s)"
              + (f", {len(cfgfs_errors)} file fetch error(s)" if cfgfs_errors else ""))

    # Wi-Fi credentials: deliberately never fetched. Record the fact, not the data.
    archive["wifi_credentials_captured"] = False
    archive["wifi_credentials_note"] = (
        "Not captured -- no GET endpoint exposes them (by design, see backup_http.c). "
        "Re-provision Wi-Fi manually via /wifi after any restore."
    )

    # Known, permanent gaps -- reported so a reader of the archive knows
    # what this backup does NOT prove, rather than assuming silence means coverage.
    archive["known_not_covered"] = [
        {"item": "Time-sync TZ (#14)", "reason": "/api/settings/tz is POST-only; firmware has no GET handler for it at all."},
        {"item": "Touch calibration (#13)", "reason": "read before mount/network in early boot; no HTTP endpoint exists (by design, per FILESYSTEM_USER_DATA_PLAN.md 'KEEP' list)."},
        {"item": "Boot-guard counter, watchdog panic-disable, OTA record, crash-report internals (#17-20)", "reason": "internal recovery-path state with no full-fidelity read endpoint; /api/crash_report captures only the current unacknowledged report, not history."},
        {"item": "Run-state breadcrumb (#16)", "reason": "transient mid-firing resume state; not meaningful to restore onto a different boot."},
        {"item": "RP2040 config store, full record (#22)", "reason": "only the fields safety_cfg_http.c mirrors/exposes via /api/safety/commissioning are captured; the RP2040's raw 4K CRC'd record itself has no export path from the ESP side."},
    ]

    out_file = out_dir / "board_backup.json"
    out_file.write_text(json.dumps(archive, indent=2, sort_keys=True), encoding="utf-8")

    manifest = out_dir / "manifest.txt"
    manifest.write_text(
        f"kilnCtl full board backup\n"
        f"captured_at_utc: {ts}\n"
        f"host: {args.host}\n"
        f"endpoints ok: {ok_count}\n"
        f"endpoints failed: {fail_count}\n"
        f"cfg filesystem files captured: {len(cfgfs_files)}\n"
        f"wifi credentials captured: NO (by design)\n"
        f"file: {out_file.name}\n",
        encoding="utf-8",
    )

    print(f"\nWrote {out_file}")
    print(f"{ok_count} endpoints ok, {fail_count} failed (see 'errors' in the JSON).")
    if fail_count and any(e for e in archive["errors"] if e["required"]):
        print("At least one REQUIRED endpoint failed -- this backup is INCOMPLETE.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
