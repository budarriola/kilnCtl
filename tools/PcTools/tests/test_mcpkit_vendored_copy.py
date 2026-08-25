#!/usr/bin/env python3
"""Guards the vendored copy of ``mcpkit/registry.py`` against drift.

``mykicadMcp`` is a separate git submodule, published on its own and expected to
work outside this checkout, so it cannot import ``mcpkit`` from
``tools/PcTools/src``. It carries a byte-for-byte copy at
``mykicadMcp/mcpkit_registry.py`` instead.

Vendoring is a deliberate trade -- two copies of one file is a real cost -- and
it is only safe while the two stay identical. The failure this test exists to
prevent is quiet: someone fixes a search-ranking bug in one copy, the other
keeps the old behaviour, and the two servers start disagreeing about what
``find`` returns for the same query. Nothing crashes; the answers just get
worse in one place.

If this fails, copy the PcTools file over the vendored one (that direction, not
the other) and commit the submodule.

The submodule may be uninitialised in a fresh clone, so a missing vendored file
skips rather than fails.

Run with: python -m pytest tools/PcTools/tests/test_mcpkit_vendored_copy.py
"""
from __future__ import annotations

import hashlib
import os
import unittest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
_SOURCE = os.path.join(_REPO_ROOT, "tools", "PcTools", "src", "mcpkit", "registry.py")
_VENDORED = os.path.join(_REPO_ROOT, "mykicadMcp", "mcpkit_registry.py")


def _digest(path: str) -> str:
    """Content hash, line endings normalised.

    The two copies live in two repositories, and git is free to check each out
    with different line endings (this one warns about LF becoming CRLF on every
    add). Hashing raw bytes would turn that into a failure that says "drift"
    while nothing has actually drifted, and a guard that cries wolf gets
    deleted. Read as text and hash the logical content instead.
    """
    with open(path, "r", encoding="utf-8", newline=None) as handle:
        return hashlib.sha256(handle.read().encode("utf-8")).hexdigest()


class VendoredRegistryCopyTests(unittest.TestCase):
    def test_source_exists(self):
        self.assertTrue(os.path.isfile(_SOURCE), f"missing {_SOURCE}")

    def test_vendored_copy_matches_source(self):
        if not os.path.isfile(_VENDORED):
            self.skipTest("mykicadMcp submodule not checked out")
        self.assertEqual(
            _digest(_VENDORED),
            _digest(_SOURCE),
            "mykicadMcp/mcpkit_registry.py has drifted from "
            "tools/PcTools/src/mcpkit/registry.py -- re-vendor by copying the "
            "PcTools file over it, then commit the submodule.",
        )


if __name__ == "__main__":
    unittest.main()
