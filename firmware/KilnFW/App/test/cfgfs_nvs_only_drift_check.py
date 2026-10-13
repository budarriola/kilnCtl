#!/usr/bin/env python3
"""cfgfs_nvs_only_drift_check.py -- catches the exact drift a 2026-09-08 audit
found live: cfg_fs_status.c's `/api/cfgfs` response hand-lists which
docs/FILESYSTEM_USER_DATA.md items are still NVS-only (`nvs_only`) vs.
permanently NVS-by-design (`nvs_permanent`), and nothing forced that list to
be revisited when a new `*_cfg_fs.c` bridge module landed and moved an item
off it (that is exactly how `prefs`/`profiles` went stale in the list while
`34927a77`/`530dc2f7` moved them to the filesystem).

What this checks, mechanically: every `persist/*_cfg_fs.c` bridge file's
module-name symbol prefix (the part before `_cfg_fs.c`) must appear in
EXPECTED_BRIDGE_MODULES below. A new bridge file with a prefix not in that
set means a MOVE item just gained file-backing and cfg_fs_status.c's
`nvs_only` array almost certainly needs an item removed from it -- so this
check fails loudly, naming the new file, rather than staying silently green
while `/api/cfgfs` keeps reporting a migrated item as NVS-only.

This is deliberately narrow (see CLAUDE.md's "reset one side of a pair" bug
class writeup for why a broad heuristic here would be wrong): it does not
try to parse cfg_fs_status.c's nvs_only/nvs_permanent arrays and cross-check
every string against real bridge coverage -- that mapping is many-to-one
(pref_cfg_fs.c alone backs four different MOVE items: display power, unit
pref, ramp assist, TZ) and not mechanically recoverable from file names
alone. What IS mechanically checkable, and what actually broke, is "a new
*_cfg_fs.c bridge appeared that nobody told this check about" -- so that is
exactly what is enforced. Adding a real new bridge is expected to require a
one-line update to EXPECTED_BRIDGE_MODULES *and* a matching edit to
cfg_fs_status.c's nvs_only/nvs_permanent arrays in the same commit.

Invalidated by: renaming a `persist/*_cfg_fs.c` file's basename prefix
without updating EXPECTED_BRIDGE_MODULES to match (a clean rename plus this
file's update is not drift -- it is the same bridge, new name). Also
invalidated if `persist/*_cfg_fs.c` bridge files are moved to a different
naming convention entirely (e.g. suffix instead of prefix) -- this check's
glob and split logic would need to move with them.

Usage: python cfgfs_nvs_only_drift_check.py <repo_root>
"""
import sys
from pathlib import Path

# One entry per persist/*_cfg_fs.c file that exists TODAY and is already
# accounted for by cfg_fs_status.c's dual_write/nvs_only/nvs_permanent
# handling. Adding a new bridge file is a deliberate migration step (per
# docs/FILESYSTEM_USER_DATA.md section 5) -- add its module prefix here
# in the SAME commit that updates cfg_fs_status.c, not before.
EXPECTED_BRIDGE_MODULES = {
    "kiln_cfg_store",  # item 8: named kiln config slots
    "pref",            # generic bridge backing items 10/11/12/14 (ramp assist, unit pref, display power, TZ)
    "profiles",        # items 5/6: user profiles + hidden-builtin mask
    "zones_config",    # items 1/2/3: zones config, zone normals, relay names
    "firing_stats",    # item 7: firing stats/history -- 2026-09-08. cfg_fs_status.c's
                       # nvs_only/nvs_permanent arrays still need a matching edit (owned
                       # by a concurrent pass on that file as of this commit -- see this
                       # task's report for what /api/cfgfs needs: move "firing_stats"
                       # out of nvs_only, plus items 4 (relay_cycles, backed by the
                       # generic pref_cfg_fs bridge inside relay_cycles.c -- no new
                       # *_cfg_fs.c file, so it does not trip THIS check) and 9
                       # (adaptive_tune, same generic-bridge situation) out of nvs_only
                       # too, since all three are now dual-write.
}


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: cfgfs_nvs_only_drift_check.py <repo_root>", file=sys.stderr)
        return 1

    repo_root = Path(sys.argv[1])
    persist_dir = repo_root / "firmware" / "KilnFW" / "App" / "drivers" / "persist"
    if not persist_dir.is_dir():
        print(f"CFGFS NVS_ONLY DRIFT CHECK: persist dir not found at {persist_dir}")
        print("  FAILED -- cannot verify without it (see check_source_path_drift lesson: never skipTest silently).")
        return 1

    found_modules = set()
    for path in sorted(persist_dir.glob("*_cfg_fs.c")):
        name = path.name[: -len("_cfg_fs.c")]
        found_modules.add(name)

    unexpected = sorted(found_modules - EXPECTED_BRIDGE_MODULES)
    missing = sorted(EXPECTED_BRIDGE_MODULES - found_modules)

    ok = True
    if unexpected:
        ok = False
        print("CFGFS NVS_ONLY DRIFT CHECK: FAILED")
        for m in unexpected:
            print(f"  New bridge file found: persist/{m}_cfg_fs.c")
        print("  This module is not in EXPECTED_BRIDGE_MODULES (cfgfs_nvs_only_drift_check.py).")
        print("  That almost certainly means a docs/FILESYSTEM_USER_DATA.md MOVE item just")
        print("  gained file-backing and cfg_fs_status.c's nvs_only array now lists an item that")
        print("  is actually file-backed (the exact 2026-09-08 audit finding). Fix in one commit:")
        print("    1. Update cfg_fs_status.c's nvs_only/nvs_permanent arrays to match reality.")
        print("    2. Add the new module prefix to EXPECTED_BRIDGE_MODULES here.")

    if missing:
        ok = False
        print("CFGFS NVS_ONLY DRIFT CHECK: FAILED")
        for m in missing:
            print(f"  Expected bridge file missing: persist/{m}_cfg_fs.c")
        print("  EXPECTED_BRIDGE_MODULES lists a module that no longer has a bridge file --")
        print("  either it was renamed/removed without updating this check, or a migration was")
        print("  reverted without updating cfg_fs_status.c back. Reconcile both.")

    if ok:
        print(f"CFGFS NVS_ONLY DRIFT CHECK: OK ({len(found_modules)} bridge module(s) accounted for)")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
