"""Driver for check_host_test_binary_freshness.ps1 -- see that file's header
for the exit-code contract and the incident this guards against
(docs/audits/review_sim_fuzzy_commits_2026-09-13.md).

Scans every *.exe already sitting in the KilnFW and SaftyFW host-test build
directories and, for each one, compares its mtime against every .c/.h file
under that build's own source roots. It does not rebuild or run anything --
a pure, cheap mtime scan, same mechanism as
tools/PcTools/src/kilnctrl/binary_provenance.py, applied repo-wide instead of
to one named exe.
"""

from __future__ import annotations

import glob
import os
import sys

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_THIS_DIR, "PcTools", "src"))

from kilnctrl.binary_provenance import check_binary_fresh  # noqa: E402


def main() -> int:
    repo_root = sys.argv[1] if len(sys.argv) > 1 else os.path.normpath(os.path.join(_THIS_DIR, ".."))

    # (build_dir, [source globs the build compiles from])
    targets = [
        (
            os.path.join(repo_root, "firmware", "KilnFW", "App", "test", "build"),
            [
                os.path.join(repo_root, "firmware", "KilnFW", "App", "test", "*.c"),
                os.path.join(repo_root, "firmware", "KilnFW", "App", "test", "*.h"),
                os.path.join(repo_root, "firmware", "KilnFW", "App", "drivers", "**", "*.c"),
                os.path.join(repo_root, "firmware", "KilnFW", "App", "drivers", "**", "*.h"),
                os.path.join(repo_root, "firmware", "CommonFW", "**", "*.c"),
                os.path.join(repo_root, "firmware", "CommonFW", "**", "*.h"),
            ],
        ),
        (
            os.path.join(repo_root, "firmware", "SaftyFW", "test", "build"),
            [
                os.path.join(repo_root, "firmware", "SaftyFW", "test", "*.c"),
                os.path.join(repo_root, "firmware", "SaftyFW", "test", "*.h"),
                os.path.join(repo_root, "firmware", "SaftyFW", "src", "**", "*.c"),
                os.path.join(repo_root, "firmware", "SaftyFW", "src", "**", "*.h"),
                os.path.join(repo_root, "firmware", "CommonFW", "**", "*.c"),
                os.path.join(repo_root, "firmware", "CommonFW", "**", "*.h"),
            ],
        ),
    ]

    any_build_dir_exists = False
    failures: list[str] = []
    checked = 0

    for build_dir, source_globs in targets:
        if not os.path.isdir(build_dir):
            continue
        any_build_dir_exists = True
        exes = sorted(glob.glob(os.path.join(build_dir, "*.exe")))
        for exe in exes:
            checked += 1
            result = check_binary_fresh(exe, source_globs)
            if result.stale:
                failures.append(f"{exe}: {result.reason}")

    if not any_build_dir_exists:
        print(
            "SKIP: neither firmware/KilnFW/App/test/build nor "
            "firmware/SaftyFW/test/build exists yet on this machine -- "
            "nothing has been built, so there is no host-test binary to "
            "check for staleness."
        )
        return 3

    if not failures and checked == 0:
        print(
            "check_host_test_binary_freshness: PASS (build dir(s) exist but "
            "hold no .exe yet -- nothing to check)"
        )
        return 0

    if failures:
        print(f"check_host_test_binary_freshness: FAIL ({len(failures)} of {checked} stale)")
        for f in failures:
            print(f"  {f}")
        return 1

    print(f"check_host_test_binary_freshness: PASS ({checked} binaries checked, all fresh)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
