#!/usr/bin/env python3
"""wire_protocol_fingerprint_check.py -- guards against the failure class
that bit this project twice, both found by accident:

  Instance 1 (a3dbad8): GET_STACK_MARGIN's reply grew two bytes BEFORE the
  first entry -- a breaking layout change -- with no UART_PROTOCOL_VERSION
  bump. An old pc_tools build would have silently misread the new bytes as
  the first entry's name_len/name instead of refusing.

  Instance 2 (5173539): SAFETY_CMD_ANNOUNCE_REBOOT (0x18) was added to the
  isolated safety link with no KILNLINK_PROTOCOL_VERSION bump.

Neither link's own tests caught these -- both sides of each link agreed
with EACH OTHER (they're built from the same headers), so nothing round-
tripped wrong in a same-build test. What was missing was a check that the
declared wire-protocol version actually MOVED when the wire-relevant
declarations did.

WHY A FINGERPRINT, NOT "did the header change" -- a plain file-hash/mtime
check is worthless here: uart_task_ids.h and the kilnlink/*.h headers carry
enormous doc comments (see kilnlink_version.h's own multi-KB version
history) that change constantly for reasons that have nothing to do with
the wire. A check keyed on "the file changed" would fire on every comment
edit and get ignored within a week -- exactly the false-positive trap
CLAUDE.md and this project's own history (source_path_drift_check.py's
docstring, frame_a_offset_drift_check.py's docstring) warn about.

Instead this fingerprints only the DECLARATIONS that are actually wire-
relevant: opcode/task-id/subcommand `#define NAME VALUE` lines, and frame
length/offset macros. Comments, reordering, and prose are already excluded
because the extraction regex only matches `#define IDENTIFIER <value>`
lines in the first place -- not because anything is stripped after the
fact. A hash of the file was explicitly rejected by this project once
already (uart_task_ids.h's own UART_PROTOCOL_VERSION doc comment: "not a
hash of this file... would force a version bump on changes that were never
actually incompatible"); this script applies that same reasoning one layer
down, by hashing the narrow slice of the file that actually matters instead
of the whole thing.

THE INVARIANT: a small checked-in manifest (wire_protocol_fingerprints.json,
next to this script) records, per link, the (version, fingerprint) pair
that was true the last time a human reviewed a wire-relevant change. This
check recomputes both live and compares:

  - live fingerprint == recorded fingerprint, live version == recorded
    version: nothing moved since the last reviewed snapshot. PASS.
  - live fingerprint != recorded fingerprint, live version == recorded
    version: wire-relevant declarations changed but the version constant
    that exists specifically to announce that did not move. FAIL, naming
    the added/removed/changed declarations and the version constant that
    was expected to move.
  - anything else (version moved, whether or not the fingerprint also did):
    the manifest is stale relative to a change that was apparently already
    reviewed (the version bump itself is evidence someone looked at this).
    FAIL, but with a distinct "run --update" message rather than an alarm
    -- this is the expected state immediately after a deliberate, correct
    version bump, until the manifest is refreshed and committed alongside
    it. Requiring the explicit `--update` step (rather than silently self-
    healing) keeps this check itself from becoming a check that can never
    fail: the manifest is a human-reviewed record, not a cache.

This mirrors check_uart_version_independence.ps1's own design note that
there is no automatic way to tell "does this change break the wire format"
-- so this script doesn't try to. It only proves that SOMETHING wire-shaped
moved and forces a conscious decision (bump the version, or bless the
change via --update) rather than letting either happen silently.

FAIL CLOSED: if extraction finds fewer than MIN_DEFS_PER_LINK declarations
for either link, or the version #define itself can't be found, that means
the regexes stopped matching reality (a header renamed, a macro reformatted
out of #define style) -- this is a failure, not a clean pass over nothing.

Usage: python wire_protocol_fingerprint_check.py [repo_root] [--update]
Exit 0: both links' live (version, fingerprint) match the manifest.
Exit 1: drift detected (with or without a version bump), extraction fell
        below the fail-closed floor, or a target file/define is missing.
"""
import hashlib
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _drivers_layout import DriverFileError, resolve_driver_file  # noqa: E402

MIN_DEFS_PER_LINK = 30

DEFINE_RE = re.compile(
    r'^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(.+?)\s*(?:/\*.*)?$'
)


def strip_comments(text: str) -> str:
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)
    text = re.sub(r'//[^\n]*', '', text)
    return text


def extract_defines(path: Path, name_pattern: re.Pattern) -> dict:
    """Return {name: value_text} for every #define NAME VALUE line in path
    whose NAME matches name_pattern. Only single-line #define statements are
    considered (continuation lines ending in backslash are folded onto one
    logical line first, comments stripped first) -- multi-line aggregate
    macros like KILNLINK_ANNOUNCE_MAX_LEN still get one deterministic value
    string out of this, they just are not line-broken.
    """
    raw = path.read_text(encoding="utf-8")
    raw = strip_comments(raw)
    # Fold backslash-continued lines into one logical line.
    raw = raw.replace("\\\r\n", " ").replace("\\\n", " ")
    out = {}
    for line in raw.splitlines():
        m = DEFINE_RE.match(line)
        if not m:
            continue
        name, value = m.group(1), m.group(2).strip()
        if name_pattern.match(name):
            out[name] = value
    return out


def fingerprint(defs: dict) -> str:
    lines = [f"{name}={value}" for name, value in sorted(defs.items())]
    blob = "\n".join(lines).encode("utf-8")
    return hashlib.sha256(blob).hexdigest()[:16], lines


def read_version(path: Path, macro: str) -> int:
    raw = strip_comments(path.read_text(encoding="utf-8"))
    m = re.search(
        r'^\s*#\s*define\s+' + re.escape(macro) + r'\s+(.+)$',
        raw, flags=re.MULTILINE,
    )
    if not m:
        return None
    val = m.group(1).strip()
    # Take the LAST bare integer literal on the line, not the first digit
    # sequence -- a cast like `((uint16_t)11)` would otherwise match the
    # "16" inside "uint16_t" before ever reaching the real value.
    nums = re.findall(r'(?<![A-Za-z_])(\d+)(?![A-Za-z_])', val)
    return int(nums[-1]) if nums else None


def diff_defs(old_lines, new_lines):
    old_set, new_set = set(old_lines), set(new_lines)
    added = sorted(new_set - old_set)
    removed = sorted(old_set - new_set)
    return added, removed


def build_link_specs(root: Path):
    kilnlink_dir = root / "firmware/CommonFW/include/kilnlink"
    # uart_task_ids.h and uart_protocol.h live somewhere under
    # firmware/KilnFW/App/drivers/ -- the layer subdirectory they sit in is
    # an implementation detail (uart_protocol.h in particular used to be
    # nested under a now-flattened espInterfaces/ subfolder), so resolve by
    # basename rather than a hand-built flat/nested path.
    uart_ids = resolve_driver_file(root, "uart_task_ids.h")
    safaty_link_frame = root / "firmware/SaftyFW/src/tasks/link_frame.h"
    max_payload_hdr = resolve_driver_file(root, "uart_protocol.h")

    return {
        "kilnlink": {
            "version_file": root / "firmware/CommonFW/include/kilnlink/kilnlink_version.h",
            "version_macro": "KILNLINK_PROTOCOL_VERSION",
            "sources": [
                # _LEN/_OFF cover fixed frame lengths/offsets directly; _NUM_/
                # _COUNT cover the multiplicand macros a _LEN macro's own
                # *text* can reference (e.g. KILNLINK_STACK_MARGIN_LEN is
                # defined as "... NUM_TASKS * ENTRY_LEN") without that
                # referenced macro's own name ending in _LEN/_OFF. Found
                # missing 2026-09-14: KILNLINK_STACK_MARGIN_NUM_TASKS changing
                # 9 -> 10 silently grows the wire frame 47 -> 52 bytes while
                # every captured string stays byte-identical, since the
                # fingerprint hashes the recorded #define TEXT, not its
                # expanded/computed value -- see this file's own module
                # docstring for why a text-level fingerprint was chosen over
                # a value-level one, and negative-test proof in
                # docs/audits/saftyfw_live_stack_reporting_impl_2026-09-14.md.
                (kilnlink_dir, "*.h", re.compile(r'^KILNLINK_.*(_LEN|_OFF|_NUM_|_COUNT)\w*$'),
                 lambda p: p.name != "kilnlink_version.h"),
                (uart_ids.parent, uart_ids.name, re.compile(r'^SAFETY_CMD_'), None),
                (safaty_link_frame.parent, safaty_link_frame.name,
                 re.compile(r'^LINK_FRAME_.*_CMD$'), None),
            ],
        },
        "uart": {
            "version_file": uart_ids,
            "version_macro": "UART_PROTOCOL_VERSION",
            "sources": [
                (uart_ids.parent, uart_ids.name,
                 re.compile(r'^UART_TASK_ID_|^SAFETY_CMD_|^[A-Z0-9]+_CMD_'), None),
                (max_payload_hdr.parent, max_payload_hdr.name,
                 re.compile(r'^UART_PROTO_MAX_PAYLOAD$'), None),
            ],
        },
    }


def collect_link_defs(spec):
    all_defs = {}
    missing = []
    for dir_path, glob_or_name, pat, file_filter in spec["sources"]:
        if any(ch in glob_or_name for ch in "*?"):
            files = sorted(dir_path.glob(glob_or_name))
        else:
            f = dir_path / glob_or_name
            files = [f] if f.is_file() else []
        if not files:
            missing.append(str(dir_path / glob_or_name))
            continue
        for f in files:
            if file_filter and not file_filter(f):
                continue
            if not f.is_file():
                missing.append(str(f))
                continue
            defs = extract_defines(f, pat)
            for k, v in defs.items():
                all_defs[f"{f.name}:{k}"] = v
    return all_defs, missing


def main():
    args = sys.argv[1:]
    do_update = "--update" in args
    args = [a for a in args if a != "--update"]
    root = Path(args[0]).resolve() if args else Path(__file__).resolve().parents[4]

    manifest_path = Path(__file__).resolve().parent / "wire_protocol_fingerprints.json"
    if not manifest_path.is_file():
        print(f"FAIL: manifest missing: {manifest_path}")
        return 1
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    try:
        specs = build_link_specs(root)
    except DriverFileError as exc:
        print(f"FAIL: {exc}")
        return 1
    failures = []
    updated = dict(manifest)

    for link_name, spec in specs.items():
        defs, missing = collect_link_defs(spec)
        if missing:
            failures.append(
                f"FAIL CLOSED [{link_name}]: could not locate expected source file(s): "
                f"{', '.join(missing)} -- has a header moved or been renamed? "
                f"Update wire_protocol_fingerprint_check.py's build_link_specs()."
            )
            continue
        if len(defs) < MIN_DEFS_PER_LINK:
            failures.append(
                f"FAIL CLOSED [{link_name}]: only found {len(defs)} wire-relevant "
                f"#define(s), expected at least {MIN_DEFS_PER_LINK} -- extraction "
                f"regex stopped matching reality (macro reformatted out of "
                f"'#define NAME VALUE' style?). Not treating this as a clean pass."
            )
            continue

        live_fp, live_lines = fingerprint(defs)
        live_version = read_version(spec["version_file"], spec["version_macro"])
        if live_version is None:
            failures.append(
                f"FAIL CLOSED [{link_name}]: could not find "
                f"'#define {spec['version_macro']}' in {spec['version_file']} -- "
                f"has it moved, been renamed, or changed format?"
            )
            continue

        rec = manifest.get(link_name, {})
        rec_fp = rec.get("fingerprint")
        rec_version = rec.get("version")
        rec_lines = rec.get("defs", [])

        if live_fp == rec_fp and live_version == rec_version:
            continue  # matches last reviewed snapshot

        added, removed = diff_defs(rec_lines, live_lines)
        changed = sorted(
            n for n in {l.split("=", 1)[0] for l in added} & {l.split("=", 1)[0] for l in removed}
        )
        added_only = [l for l in added if l.split("=", 1)[0] not in changed]
        removed_only = [l for l in removed if l.split("=", 1)[0] not in changed]

        detail_lines = []
        for name in changed:
            old_v = next(l.split("=", 1)[1] for l in rec_lines if l.startswith(name + "="))
            new_v = next(l.split("=", 1)[1] for l in live_lines if l.startswith(name + "="))
            detail_lines.append(f"    changed: {name}: {old_v!r} -> {new_v!r}")
        for l in added_only:
            detail_lines.append(f"    added:   {l}")
        for l in removed_only:
            detail_lines.append(f"    removed: {l}")
        detail = "\n".join(detail_lines) if detail_lines else "    (no line-level diff available -- manifest had no recorded defs)"

        if live_version == rec_version:
            failures.append(
                f"FAIL [{link_name}]: wire-relevant declarations changed but "
                f"{spec['version_macro']} did NOT move (still {live_version}, "
                f"manifest recorded {rec_version}). Bump {spec['version_macro']} in "
                f"{spec['version_file']}, then rerun this script with --update.\n"
                f"{detail}"
            )
        else:
            failures.append(
                f"STALE MANIFEST [{link_name}]: {spec['version_macro']} moved "
                f"{rec_version} -> {live_version} without the manifest being "
                f"refreshed. If this bump was deliberate and reviewed, rerun with "
                f"--update and commit the manifest alongside it.\n"
                f"{detail}"
            )

        updated[link_name] = {
            "version": live_version,
            "fingerprint": live_fp,
            "defs": live_lines,
        }

    if do_update:
        manifest_path.write_text(
            json.dumps(updated, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        print(f"Manifest updated: {manifest_path}")
        return 0

    if failures:
        print("WIRE PROTOCOL FINGERPRINT CHECK FAILED:")
        for f in failures:
            print(f)
            print()
        return 1

    print("Wire protocol fingerprint check passed: kilnlink and uart links both "
          "match their last reviewed (version, fingerprint) snapshot.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
