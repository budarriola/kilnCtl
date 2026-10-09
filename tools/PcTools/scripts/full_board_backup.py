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

TIMEZONE, corrected 2026-09-20: an earlier version of this comment claimed
the configured POSIX TZ could not be read back over the API at all, because
/api/settings/tz is indeed POST-only. That was a stale half-truth: there is
no GET on that ROUTE, but the persisted string itself is carried as
"time_tz" in GET /api/status (dashboard_status_http.c's
`APPEND(",\"time_tz\":\"%s\"", ds->time_tz)`, fed from time_sync's own
persisted value), and POST /api/settings/tz takes exactly that string back
as its "tz" form field. TZ is therefore a fully round-trippable domain, and
restore_full() restores it -- it is NOT in IRREDUCIBLE_DOMAINS. A domain
parked as irreducible when a route exists is a hidden gap, which is what
this one was.

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

# PROFILES_MAX_COUNT mirrors the firmware constant (profiles_types.h) -- see
# kilnctrl.protocol's own copy for why the two must move together. sys.path
# is set up before the import (repo_root/tools/PcTools/src holds the
# kilnctrl package) so a bare `uv run python
# tools/PcTools/scripts/full_board_backup.py` still works from any cwd.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from kilnctrl.protocol import PROFILES_MAX_COUNT  # noqa: E402
from kilnctrl import http_auth  # noqa: E402

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

# /api/firing_history requires ?profile_id=N. Default probes every user
# profile slot 0..PROFILES_MAX_COUNT-1 (100 as of docs/PROFILE_SLOTS_100_PLAN.md
# section 7 task 6) -- main() below narrows this to only the ids actually
# present in /api/profiles when that endpoint answered, so a 100-slot board
# with a handful of profiles saved does not cost 100 requests every backup.
FIRING_HISTORY_PROFILE_IDS = range(PROFILES_MAX_COUNT)


def _is_auth_failure(err) -> bool:
    """True when a recorded error string is an authentication failure: either
    http_auth.HttpAuthError (missing/refused credential, reported by the
    helpers below as "authentication failed: ...") or a 401 that persisted
    after http_auth's one login-and-retry (urllib's str(HTTPError) is
    "HTTP Error 401: ..."). main() fails the whole backup run on any of
    these, required endpoint or not -- an archive missing content because
    the board refused this session is never a clean backup."""
    text = str(err or "")
    return "authentication failed" in text or "HTTP Error 401" in text


def _get_json(url: str, timeout: float):
    try:
        with http_auth.urlopen(url, timeout=timeout) as resp:
            body = resp.read()
    except http_auth.HttpAuthError as e:
        # A 401 that persists after http_auth's own one-shot login attempt
        # (missing credential, or a login the board itself refused) is
        # reported as a failure like any other -- never silently swallowed,
        # and never treated as a successful read. The caller (GET_ENDPOINTS
        # loop / _capture_cfgfs_files) records this in archive["errors"] and
        # never sets archive["endpoints"][path], so an auth failure can never
        # end up saved as backup content.
        return None, f"authentication failed: {e}"
    except (urllib.error.URLError, OSError) as e:
        return None, f"request failed: {e}"
    try:
        return json.loads(body), None
    except (json.JSONDecodeError, UnicodeDecodeError):
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
        with http_auth.urlopen(url, timeout=timeout) as resp:
            return resp.read(), None
    except http_auth.HttpAuthError as e:
        return None, f"authentication failed: {e}"
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
            with http_auth.urlopen(req, timeout=timeout) as resp:
                resp.read()
        except http_auth.HttpAuthError as e:
            return False, f"cfg file '{name}': authentication failed after {list(decoded).index(name)} prior file(s) already written: {e}", decoded
        except (urllib.error.URLError, OSError) as e:
            return False, f"cfg file '{name}': POST failed after {list(decoded).index(name)} prior file(s) already written: {e}", decoded
    return True, f"restored {len(decoded)} cfg file(s)", decoded


# ---------------------------------------------------------------------------
# Round-trip gap closure (docs/... this task): the backup above captures far
# more than the old restore path (restore_cfgfs_files() alone) could put
# back. IRREDUCIBLE_DOMAINS below is the single source of truth for what
# this tool can never restore, printed at BOTH backup time (main()'s
# "known_not_covered", built from this list) and restore time
# (restore_full()'s own report) -- one list, not two that can drift apart.
# Every entry names the reason a human can act on, not just "unsupported".
IRREDUCIBLE_DOMAINS = [
    {
        "item": "Wi-Fi credentials",
        "reason": "No GET endpoint exposes them, by design (see backup_http.c). "
                  "Re-provision Wi-Fi manually via /wifi after any restore.",
    },
    {
        "item": "Touch calibration",
        "reason": "read before mount/network in early boot; no HTTP endpoint exists by design.",
    },
    {
        "item": "Boot-guard counter, watchdog panic-disable, OTA record, crash-report internals",
        "reason": "internal recovery-path state with no full-fidelity read endpoint; "
                  "/api/crash_report captures only the current unacknowledged report, not history. "
                  "Corrected 2026-09-20: GET /api/boot_guard (ota_http.c, b09294fb) DOES expose "
                  "{boot_count, recovery_mode}, and POST /api/ota/esp/boot_guard_reset can clear the "
                  "counter -- but neither is a restore path and this tool deliberately never writes "
                  "them. A boot-guard count describes the boot the board is living through RIGHT NOW; "
                  "replaying an archived one would either mask a genuinely reset-looping board or "
                  "walk a healthy one toward recovery mode (see CLAUDE.md's boot_guard section). "
                  "Irreducible here means \"must not be restored\", not \"cannot be read\".",
    },
    {
        "item": "Run-state breadcrumb",
        "reason": "transient mid-firing resume state; not meaningful to restore onto a different boot.",
    },
    {
        "item": "RP2040 config store, full raw record",
        "reason": "only the fields safety_cfg_http.c mirrors via /api/safety/commissioning are captured; "
                  "the RP2040's raw 4K CRC'd record itself has no export path from the ESP side.",
    },
    {
        "item": "RP2040 safety commissioning (tc_type, abs-max ceilings, CT cal, ct_installed)",
        "reason": "captured (read-only) for the record, but DELIBERATELY not auto-restored by this tool: "
                  "POST /api/safety/commissioning takes a staged id=<decimal>&value=<v> protocol per field, "
                  "and this is exactly the state that must never be loosened (abs_max_temp_c must never end "
                  "up lower on the Pico than the ESP's own copy; CT noise-floor thresholds must never be "
                  "lowered) -- getting the staged-commit sequence and monotonic-safety checks wrong here has "
                  "a worse failure mode than refusing. Re-enter these by hand via the /safety/commissioning "
                  "page after a restore, then verify readback against this archive's captured values.",
    },
    {
        "item": "Adaptive-tune Ki-diagnosis baseline (ki_baseline/ki_baseline_valid)",
        "reason": "internal derived state with no GET endpoint at all (only the opt-in enable flag and the "
                  "already-applied gains are exposed, both of which travel with the zones config restore "
                  "below); never captured by this script, so never restorable. Harmless to lose: it "
                  "re-latches from a fresh dwell once adaptive tuning runs again.",
    },
    {
        "item": "Hidden built-in profiles mask",
        "reason": "GET /api/profiles/builtin has no matching POST/set route in firmware today.",
    },
]


# The profile executor's REAL state vocabulary, not a fabricated one. Sourced
# from firmware's exec_state_name() (dashboard_exec_http.c) and mirrored in
# kilnctrl.devices_profiles.ProfileExecStatus.STATE_NAMES, which the test for
# this module asserts against rather than re-typing the strings. Only "idle"
# is quiescent; "done" and "faulted" are NOT accepted here, because the
# executor can sit in either while an operator has not yet acknowledged the
# run and the relays have not necessarily been observed de-energized -- the
# relay check below is what actually settles that, and it is cheaper to be
# strict here than to reason about it.
PROFILE_EXEC_QUIESCENT_STATES = {"idle"}

# autotune_state_name() (dashboard_json.c). "settling"/"stepping"/
# "relay_approach"/"relay_cycling" all drive real heat WHILE the profile
# executor reports "idle" -- autotune does not run through the executor, it
# feeds its own setpoint (CLAUDE.md, "Autotune feeds a fake setpoint"). This
# is the gap that made a profile_exec-only check unsafe.
AUTOTUNE_QUIESCENT_STATES = {"idle", "done", "aborted"}

# KILN_CFG_MAX_COUNT, kiln_cfg_store.h:101. Mirrored here only to make the
# "not enough free slots" refusal below say a number an operator can act on;
# the authoritative check is firmware's own find_free_slot(), which refuses
# with "kiln config store is full".
KILN_CFG_MAX_COUNT = 10


def check_board_quiescent(host: str, timeout: float = 10.0):
    """Confirms -- positively, never by assumption -- that this board is not
    heating and is not in a state where a config write could take effect
    under a live heat output. Returns (refusal_reason_or_None, live_status).

    Why this is not just "GET /api/profile_exec == idle", which is what the
    first version of this function checked:

      * ARMED is a LATCH on the RP2040's relay_owner state machine, not a
        profile-executor state (CLAUDE.md; firmware/SaftyFW). /api/status's
        "safety_heating_enabled" is deliberately NOT used as the gate here:
        dashboard_http.h states outright that it is true on any healthy,
        past-grace-period Pico regardless of whether anyone requested heat,
        so gating on it would refuse every restore forever. The honest
        signal is "safety_relay_energized" -- K4, the relay that actually
        gates heat -- plus the four ESP-owned relays.
      * Autotune drives heat with the profile executor reporting "idle"
        (AUTOTUNE_QUIESCENT_STATES above).
      * POST /api/diagnostics/danger/relay can force any ESP relay on with
        neither the executor nor autotune running at all. That shows up in
        /api/status's "relays" array and nowhere else.

    Every one of those reads is a HARD refusal if it fails or is absent: an
    unreachable board, an io_ready:false board, or a board whose relay read
    failed (io_read_failed) is NOT quiescent, it is UNKNOWN, and unknown is
    refused. A restore that overwrites zones/PID config under a live heat
    output is the failure mode this exists to prevent.
    """
    exec_data, exec_err = _get_json(f"http://{host}/api/profile_exec", timeout)
    if exec_err:
        return (f"could not confirm the board is idle (GET /api/profile_exec failed: {exec_err}) "
                "-- refusing the whole restore"), None
    state = exec_data.get("state") if isinstance(exec_data, dict) else None
    if state not in PROFILE_EXEC_QUIESCENT_STATES:
        return (f"the profile executor is not idle (GET /api/profile_exec state={state!r}) "
                "-- refusing the whole restore"), None

    at_data, at_err = _get_json(f"http://{host}/api/autotune", timeout)
    if at_err:
        return (f"could not confirm autotune is not running (GET /api/autotune failed: {at_err}) "
                "-- refusing the whole restore"), None
    at_state = at_data.get("state") if isinstance(at_data, dict) else None
    if at_state not in AUTOTUNE_QUIESCENT_STATES:
        return (f"an autotune run is active (GET /api/autotune state={at_state!r}) -- autotune drives "
                "real heat while /api/profile_exec still reports \"idle\"; refusing the whole restore"), None

    status, status_err = _get_json(f"http://{host}/api/status", timeout)
    if status_err or not isinstance(status, dict):
        return (f"could not confirm the relays are de-energized (GET /api/status failed: "
                f"{status_err or 'response was not an object'}) -- refusing the whole restore"), None
    if not status.get("io_ready"):
        return ('the board reports io_ready:false, so its relay states cannot be read at all '
                '-- unknown is not quiescent; refusing the whole restore'), None
    if status.get("io_read_failed"):
        return ('the board reports io_read_failed, so the relay states in /api/status are not '
                'trustworthy -- unknown is not quiescent; refusing the whole restore'), None
    relays = status.get("relays")
    if not isinstance(relays, list) or not relays:
        return ('GET /api/status carried no "relays" array -- cannot confirm the relays are '
                'de-energized; refusing the whole restore'), None
    on_relays = [r.get("relay") for r in relays if isinstance(r, dict) and r.get("on")]
    if on_relays:
        return (f"relay(s) {on_relays} are energized right now (GET /api/status relays[]) -- something "
                "is driving heat (a manual/diagnostics relay force, a hold, or a run this check did "
                "not otherwise see); refusing the whole restore"), None

    # K4, the safety processor's own relay -- the one that actually gates
    # heat. null (not false) whenever the safety link has never answered,
    # which is exactly the "unknown" case that must not read as safe.
    k4 = status.get("safety_relay_energized")
    if k4 is None:
        return ("the safety processor's own relay state is unknown (GET /api/status "
                "safety_relay_energized is null -- the safety link has not answered) "
                "-- refusing the whole restore"), None
    if k4:
        return ("the safety processor's heat-gating relay K4 is ENERGIZED (GET /api/status "
                "safety_relay_energized) -- the heat path is live; refusing the whole restore"), None

    return None, status


def restore_full(host: str, archive: dict, timeout: float = 10.0, dry_run: bool = False):
    """Restores every domain this backup CAN put back, in one call, and
    reports -- by name -- every domain it cannot. This is the fix for the
    "silent partial restore" defect: a caller of this function gets a
    structured result that names every restored domain, every attempted
    domain that failed, and every domain this tool can never restore
    (IRREDUCIBLE_DOMAINS), rather than silently doing only zones/profiles
    and leaving the rest to be discovered missing later.

    Refuses the WHOLE restore up front (no partial writes at all) unless
    check_board_quiescent() below can positively confirm that nothing on
    this board is heating or able to heat -- see that function for why
    GET /api/profile_exec alone is NOT sufficient.

    Returns a dict: {"refused": str|None, "restored": [...], "failed": [...],
    "not_restorable": list(IRREDUCIBLE_DOMAINS)}. Each restored/failed entry is
    {"domain": str, "detail": str}.
    """
    result = {"refused": None, "restored": [], "failed": [], "not_restorable": list(IRREDUCIBLE_DOMAINS)}

    refusal, live_status = check_board_quiescent(host, timeout)
    if refusal:
        result["refused"] = refusal
        return result

    endpoints = archive.get("endpoints", {})

    def _post(path: str, data: bytes, content_type: str | None = None):
        """One POST, returning (ok, detail). On an HTTP error status the
        RESPONSE BODY is folded into the detail, not just str(e): firmware
        puts the only human-actionable reason there ("kiln config store is
        full", "a saved kiln config already has that name", "restore refused
        or persist failed"), and urllib's own str(HTTPError) is just
        "HTTP Error 400: Bad Request" -- which is exactly the silent-loss
        shape this whole pass exists to remove."""
        req = urllib.request.Request(f"http://{host}{path}", data=data, method="POST")
        if content_type:
            req.add_header("Content-Type", content_type)
        try:
            with http_auth.urlopen(req, timeout=timeout) as resp:
                return True, resp.read().decode("utf-8", errors="replace")
        except http_auth.HttpAuthError as e:
            return False, f"authentication failed: {e}"
        except urllib.error.HTTPError as e:
            try:
                body = e.read().decode("utf-8", errors="replace").strip()
            except Exception:  # noqa: BLE001 -- a body we cannot read must not mask the status
                body = ""
            return False, f"{e} -- board said: {body}" if body else str(e)
        except (urllib.error.URLError, OSError) as e:
            return False, str(e)

    def _post_form(path: str, fields: dict):
        return _post(path, urllib.parse.urlencode(fields).encode("ascii"))

    def _post_json_text(path: str, text: str):
        return _post(path, text.encode("utf-8"), "application/json")

    def _record(domain: str, ok: bool, detail: str):
        (result["restored"] if ok else result["failed"]).append({"domain": domain, "detail": detail})

    # zones config (PID/model/coupling/guards/limits), adaptive-tune opt-in
    # state, and user fire profiles -- one native-format blob, POSTed back
    # verbatim to the same endpoint that produced it.
    zones_profiles = endpoints.get("/api/backup/export")
    if zones_profiles is None:
        _record("zones_config_and_profiles", False, "archive has no /api/backup/export section -- was the backup incomplete?")
    elif dry_run:
        _record("zones_config_and_profiles", True, "dry run -- validated presence only, nothing written")
    else:
        ok, detail = _post_json_text("/api/backup/import", json.dumps(zones_profiles))
        _record("zones_config_and_profiles", ok, detail)

    # Named kiln config slots. GET /api/kiln_configs is metadata only
    # (id/name/is_active) -- the actual content lives behind GET
    # /api/kiln_configs/export?id=N, one call per slot. Import always
    # creates a NEW slot (kiln_cfg_http.c's own doc comment); this restore
    # therefore does not reproduce the original slot ids or which one was
    # active -- reported explicitly rather than silently assumed.
    kiln_cfg_exports = archive.get("kiln_config_exports", {})
    if not kiln_cfg_exports:
        _record("kiln_config_slots", True, "no kiln config slots in archive (or archive predates this capability) -- nothing to restore")
    else:
        # SLOT CAPACITY, 2026-09-20 review finding. kiln_cfg_http.c's import
        # ALWAYS creates a new slot -- it never overwrites the id it came
        # from and never re-selects the active one. Importing N slots onto a
        # board that already holds M therefore needs M+N <= KILN_CFG_MAX_COUNT,
        # and without this check the loop below would import slots one at a
        # time until firmware started answering "kiln config store is full",
        # leaving a half-restored store. Checked up front against the LIVE
        # list so the operator is told the number before anything is written.
        live_list, live_list_err = _get_json(f"http://{host}/api/kiln_configs", timeout)
        used = None
        if isinstance(live_list, dict) and isinstance(live_list.get("configs"), list):
            used = len(live_list["configs"])
        if used is None:
            _record("kiln_config_slots", False,
                    f"could not read the board's current kiln config slots (GET /api/kiln_configs: "
                    f"{live_list_err or 'unexpected response shape'}) -- refusing to import "
                    f"{len(kiln_cfg_exports)} slot(s) blind, since import always ALLOCATES a new slot "
                    f"and could silently exhaust the {KILN_CFG_MAX_COUNT}-slot store")
            kiln_cfg_exports = {}
        elif used + len(kiln_cfg_exports) > KILN_CFG_MAX_COUNT:
            _record("kiln_config_slots", False,
                    f"NOT restored: the board already holds {used} kiln config slot(s) and this archive "
                    f"has {len(kiln_cfg_exports)}, which exceeds the {KILN_CFG_MAX_COUNT}-slot store "
                    f"({KILN_CFG_MAX_COUNT - used} free). Import always creates a NEW slot and never "
                    "overwrites, so nothing was imported rather than filling the store partially and "
                    "failing partway. Delete unneeded slots on the board and re-run")
            kiln_cfg_exports = {}
    if kiln_cfg_exports:
        restored_ids = []
        failed_names = []
        for old_id, pkg_json in kiln_cfg_exports.items():
            if dry_run:
                restored_ids.append(old_id)
                continue
            ok, detail = _post_json_text("/api/kiln_configs/import", pkg_json)
            if ok:
                restored_ids.append(old_id)
            else:
                failed_names.append(f"{old_id}: {detail}")
        if failed_names:
            _record("kiln_config_slots", False,
                    f"restored {len(restored_ids)}/{len(kiln_cfg_exports)} slot(s) as NEW slots (original ids/active "
                    f"slot NOT preserved); failures: {'; '.join(failed_names)}")
        else:
            _record("kiln_config_slots", True,
                    f"restored {len(restored_ids)} slot(s) as NEW slots -- original ids and which slot was active are "
                    "NOT preserved (kiln_cfg_http.c's import always creates a new slot); re-select the active config by hand")

    # Relay cycle counters (RELAY_LIFE_BUDGET.md) -- the matching restore
    # route (POST /api/relay_cycles/restore) existed in firmware with zero
    # callers before this pass; this is the wiring task item 1 asked for.
    status = endpoints.get("/api/status")
    relay_life = status.get("relay_life") if isinstance(status, dict) else None
    if not relay_life:
        _record("relay_cycle_counters", False, "archive has no /api/status relay_life array -- was the backup incomplete?")
    else:
        try:
            archived = {int(entry["relay"]): int(entry["cycles"]) for entry in relay_life}
        except (KeyError, TypeError, ValueError) as e:
            _record("relay_cycle_counters", False, f"archive's relay_life entries are malformed: {e}")
        else:
            # MONOTONIC GUARD, PC side, 2026-09-20 review finding, UPDATED
            # same day once relay_cycles_restore_all() (relay_cycles.c) grew
            # its own firmware-side clamp: firmware now also refuses to move
            # a relay's live count downward (it clamps a lower request back
            # up to the live count it sees AT THE TIME OF THE POST, and names
            # every clamped relay -- both numbers -- in the response body).
            # This client-side clamp is kept anyway, as defence in depth, not
            # redundant trust: it is the ONLY guard that runs if this script
            # ever talks to an older board image that predates the firmware
            # fix, and it also catches the case this comment used to miss --
            # this tool cannot see the board's live count as of the moment
            # the POST actually lands, only as of the earlier /api/status
            # snapshot `live_status` below, so relying on the firmware guard
            # alone would leave a window where a value this tool itself
            # thought was safe still needed firmware's later, more current
            # clamp. Each relay is restored to max(live count, archived
            # count) here; the response is inspected below for firmware's
            # OWN "clamped" list so a clamp applied only by firmware (because
            # the board's live count moved between this script's snapshot and
            # the POST) is also reported, distinctly, rather than silently
            # assumed to be the same event as this script's own clamp.
            live = {}
            live_relay_life = live_status.get("relay_life") if isinstance(live_status, dict) else None
            if isinstance(live_relay_life, list):
                for entry in live_relay_life:
                    if isinstance(entry, dict) and isinstance(entry.get("relay"), int)                             and isinstance(entry.get("cycles"), int):
                        live[entry["relay"]] = entry["cycles"]
            if len(live) != len(archived):
                _record("relay_cycle_counters", False,
                        f"refusing to restore relay cycle counters: the board reported {len(live)} live "
                        f"relay_life entr(ies) but the archive has {len(archived)} -- without a live "
                        "reading for every relay this tool cannot guarantee it is not LOWERING a wear "
                        "counter (firmware's relay_cycles_restore_all() enforces only an upper ceiling, "
                        "never monotonicity)")
            else:
                clamped = {r: max(archived[r], live.get(r, 0)) for r in archived}
                lowered = {r: (archived[r], live[r]) for r in archived if archived[r] < live[r]}
                fields = {f"c{r}": str(clamped[r]) for r in sorted(clamped)}
                note = ""
                if lowered:
                    note = ("; STALE ARCHIVE: " + ", ".join(
                        f"relay {r} archived {a} < live {liv}, kept live {liv}" for r, (a, liv) in sorted(lowered.items())
                    ) + " -- a wear counter is never moved downward by this tool")
                if dry_run:
                    _record("relay_cycle_counters", True, f"dry run -- would restore {fields}{note}")
                else:
                    ok, detail = _post_form("/api/relay_cycles/restore", fields)
                    _record("relay_cycle_counters", ok, detail + note)

    # Ramp-assist enable flag.
    ramp_assist = endpoints.get("/api/ramp_assist")
    if not isinstance(ramp_assist, dict) or "enabled" not in ramp_assist:
        _record("ramp_assist", False, "archive has no /api/ramp_assist section -- was the backup incomplete?")
    elif dry_run:
        _record("ramp_assist", True, f"dry run -- would set enabled={ramp_assist['enabled']}")
    else:
        ok, detail = _post_form("/api/ramp_assist", {"enabled": "1" if ramp_assist["enabled"] else "0"})
        _record("ramp_assist", ok, detail)

    # Display power / backlight policy.
    display_power = endpoints.get("/api/settings/display_power")
    if not isinstance(display_power, dict) or "brightness_percent" not in display_power:
        _record("display_power", False, "archive has no /api/settings/display_power section -- was the backup incomplete?")
    elif dry_run:
        _record("display_power", True, f"dry run -- would restore {display_power}")
    else:
        ok, detail = _post_form("/api/settings/display_power", {
            "brightness": str(int(display_power["brightness_percent"])),
            "timeout": str(int(display_power["timeout_setting"])),
            "keep_on_while_firing": "1" if display_power.get("keep_on_while_firing") else "0",
            "display_on_error": "1" if display_power.get("display_on_error") else "0",
        })
        _record("display_power", ok, detail)

    # Unit preference (temp_unit), carried in /api/status.
    temp_unit = status.get("temp_unit") if isinstance(status, dict) else None
    if temp_unit not in ("C", "F"):
        _record("unit_preference", False, f"archive's /api/status has no usable temp_unit field (got {temp_unit!r})")
    elif dry_run:
        _record("unit_preference", True, f"dry run -- would set unit={temp_unit}")
    else:
        ok, detail = _post_form("/api/unit_pref", {"unit": temp_unit})
        _record("unit_preference", ok, detail)

    # Time-sync POSIX TZ string. Captured as /api/status's "time_tz"
    # (dashboard_status_http.c) and written back through POST
    # /api/settings/tz's "tz" form field -- the route has no GET, but the
    # VALUE is readable, which is why TZ is a restorable domain here and
    # NOT in IRREDUCIBLE_DOMAINS (see this module's docstring).
    time_tz = status.get("time_tz") if isinstance(status, dict) else None
    if not isinstance(time_tz, str) or not time_tz:
        _record("timezone", False, f"archive's /api/status has no usable time_tz field (got {time_tz!r})")
    elif dry_run:
        _record("timezone", True, f"dry run -- would set tz={time_tz!r}")
    else:
        ok, detail = _post_form("/api/settings/tz", {"tz": time_tz})
        _record("timezone", ok, detail)

    # cfg-filesystem files (existing capability, unchanged).
    cfgfs_files = archive.get("cfgfs_files", {})
    if cfgfs_files:
        ok, detail, _decoded = restore_cfgfs_files(host, cfgfs_files, timeout, dry_run=dry_run)
        _record("cfg_filesystem_files", ok, detail)
    else:
        _record("cfg_filesystem_files", True, "no cfg filesystem files in archive (unmounted at backup time) -- nothing to restore")

    return result


def print_restore_report(result: dict) -> int:
    """Prints restore_full()'s result in the "name what did and did not come
    back" shape this task requires, and returns a process exit code."""
    if result["refused"]:
        print(f"RESTORE REFUSED -- {result['refused']}")
        print("Nothing was written.")
        return 1
    print("Restored domains:")
    for entry in result["restored"]:
        print(f"  OK   {entry['domain']}: {entry['detail']}")
    if result["failed"]:
        print("FAILED domains (see detail -- board state for these is now UNKNOWN, verify by hand):")
        for entry in result["failed"]:
            print(f"  FAIL {entry['domain']}: {entry['detail']}")
    print("\nNever restorable by this tool (see reason for what to do instead):")
    for entry in result["not_restorable"]:
        print(f"  NOT RESTORED: {entry['item']} -- {entry['reason']}")
    return 1 if result["failed"] else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="192.168.1.156", help="board IP or host:port")
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--out-dir", default=None, help="override output directory (default: tools/PcTools/board-backups/<timestamp>)")
    ap.add_argument("--restore-cfgfs-from", default=None, metavar="ARCHIVE_JSON",
                     help="restore ONLY the cfg-filesystem files captured in a previous board_backup.json onto "
                          "--host, validating every entry before writing any of them (see restore_cfgfs_files()). "
                          "Does not touch zones/profiles -- use POST /api/backup/import for those, unchanged.")
    ap.add_argument("--restore-from", default=None, metavar="ARCHIVE_JSON",
                     help="full round-trip restore: every domain a previous board_backup.json can put back "
                          "(zones config, profiles, kiln config slots, relay cycle counters, ramp-assist, "
                          "display power, unit preference, cfg filesystem files) onto --host, refusing outright "
                          "if a firing is active/paused, and naming every domain it did not restore -- see "
                          "restore_full()/IRREDUCIBLE_DOMAINS.")
    ap.add_argument("--dry-run", action="store_true", help="with --restore-from or --restore-cfgfs-from, validate only, write nothing")
    args = ap.parse_args()

    if args.restore_from:
        archive_path = Path(args.restore_from)
        data = json.loads(archive_path.read_text(encoding="utf-8"))
        result = restore_full(args.host, data, args.timeout, dry_run=args.dry_run)
        return print_restore_report(result)

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
            if decoded and not args.dry_run:
                # Every entry validated, so the failure was a POST: files
                # before the failing one may already be on the board.
                print("RESTORE FAILED PARTWAY -- see the message above for how many cfg file(s) were "
                      "already written; board cfg filesystem state is now UNKNOWN, verify by hand")
            else:
                print("RESTORE REFUSED -- archive validation failed, nothing written")
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

    # Narrow the firing-history probe to ids actually present in
    # /api/profiles when that endpoint answered -- a 100-slot board (docs/
    # PROFILE_SLOTS_100_PLAN.md section 7 task 6) with only a handful of
    # profiles saved should not cost 100 requests every backup. Falls back
    # to the full PROFILES_MAX_COUNT range if /api/profiles failed or came
    # back in an unexpected shape, matching this script's existing
    # best-effort convention elsewhere.
    profiles_data = archive["endpoints"].get("/api/profiles")
    firing_history_ids = FIRING_HISTORY_PROFILE_IDS
    if isinstance(profiles_data, list):
        present_ids = sorted({
            entry["id"] for entry in profiles_data
            if isinstance(entry, dict) and not entry.get("builtin", False) and isinstance(entry.get("id"), int)
        })
        if present_ids:
            firing_history_ids = present_ids

    for pid in firing_history_ids:
        url = f"http://{args.host}/api/firing_history?profile_id={pid}"
        data, err = _get_json(url, args.timeout)
        if err:
            # A profile slot with no history yet, or an unused slot, is not
            # a failure -- only a genuine transport/parse error is recorded.
            archive["firing_history"][str(pid)] = None
            archive["errors"].append({"endpoint": url, "item": f"firing history profile {pid}", "required": False, "error": err})
            continue
        archive["firing_history"][str(pid)] = data

    # Named kiln config slots -- full content, not just the id/name metadata
    # GET /api/kiln_configs (above) returns. Without this, the old backup
    # silently could not restore kiln_configs at all: the list endpoint has
    # never carried the PID/thermocouple/guard payload, only id/name/
    # is_active. One GET /api/kiln_configs/export?id=N per slot, same
    # native package-JSON format POST /api/kiln_configs/import expects back.
    archive["kiln_config_exports"] = {}
    kiln_configs_list = archive["endpoints"].get("/api/kiln_configs")
    slot_ids = []
    if isinstance(kiln_configs_list, dict):
        slot_ids = [
            entry["id"] for entry in kiln_configs_list.get("configs", [])
            if isinstance(entry, dict) and isinstance(entry.get("id"), int)
        ]
    kiln_cfg_export_errors = 0
    for slot_id in slot_ids:
        url = f"http://{args.host}/api/kiln_configs/export?id={slot_id}"
        try:
            with http_auth.urlopen(url, timeout=args.timeout) as resp:
                text = resp.read().decode("utf-8")
        except http_auth.HttpAuthError as e:
            archive["errors"].append({"endpoint": url, "item": f"kiln config slot {slot_id} export", "required": False, "error": f"authentication failed: {e}"})
            kiln_cfg_export_errors += 1
            continue
        except (urllib.error.URLError, OSError) as e:
            archive["errors"].append({"endpoint": url, "item": f"kiln config slot {slot_id} export", "required": False, "error": str(e)})
            kiln_cfg_export_errors += 1
            continue
        archive["kiln_config_exports"][str(slot_id)] = text
    if slot_ids:
        print(f"kiln config slots: exported {len(archive['kiln_config_exports'])}/{len(slot_ids)}"
              + (f", {kiln_cfg_export_errors} error(s)" if kiln_cfg_export_errors else ""))

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
    # what this backup does NOT prove, rather than assuming silence means
    # coverage. Sourced from IRREDUCIBLE_DOMAINS, the SAME list restore_full()
    # reports at restore time -- one list, named at both ends, never two
    # that can silently drift apart.
    archive["known_not_covered"] = list(IRREDUCIBLE_DOMAINS)
    print("\nDomains this tool can never restore (see the archive's known_not_covered for reasons):")
    for entry in IRREDUCIBLE_DOMAINS:
        print(f"  NOT RESTORABLE: {entry['item']} -- {entry['reason']}")

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
    rc = 0
    if fail_count and any(e for e in archive["errors"] if e["required"]):
        print("At least one REQUIRED endpoint failed -- this backup is INCOMPLETE.")
        rc = 1
    auth_failures = [e for e in archive["errors"] if _is_auth_failure(e.get("error"))]
    if auth_failures:
        print(f"{len(auth_failures)} request(s) failed web authentication (401) -- this backup is INCOMPLETE. "
              f"Check {http_auth.USERNAME_ENV}/{http_auth.PASSWORD_ENV}.")
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
