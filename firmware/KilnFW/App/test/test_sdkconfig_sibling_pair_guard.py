#!/usr/bin/env python3
"""Regression test for the "reset one side of a pair" sdkconfig guard.

Context (2026-09-23): check_00_kilnfw_target_build.ps1 publishes an isolated
checkbuild's sdkconfig into the invoking tree's build/ next to the ELF it
also publishes there. A later plain `idf.py build` in that same build/
directory replaces KilnCtrl.elf but leaves the published build/sdkconfig
sibling in place, so use_sdkconfig_for_elf() could silently grade a newer
ELF against an older, disagreeing config from a different build (observed
on the bench: build/sdkconfig with CONFIG_KILNCTL_GPIO_PROBE=y, ELF actually
linked from a worktree sdkconfig with it off, spurious FAIL "could not
resolve root symbol 'gpio_probe_task'").

An mtime-based guard was considered and rejected: in the main tree, an
`idf.py build` from an UNCHANGED tree can legitimately leave build/sdkconfig
older than a freshly linked ELF while the two are byte-identical in CONFIG_
content -- an mtime-only rule would false-positive there. The guard instead
compares the two files' `CONFIG_*=value` lines directly and only fails when
they actually disagree.

Run standalone: `python test_sdkconfig_sibling_pair_guard.py`. No ELF, no
board, no network -- everything is built from a temp directory.
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_all_task_stack_budgets as sdk  # noqa: E402


SDKCONFIG_A = """\
#
# Some header comment
#
CONFIG_IDF_TARGET="esp32s3"
CONFIG_KILNCTL_GPIO_PROBE=y
CONFIG_KILNCTL_MAIN_TASK_STACK_SIZE=8192
"""

SDKCONFIG_A_IDENTICAL = """\
# a differently-worded header comment, content is what matters
CONFIG_IDF_TARGET="esp32s3"
CONFIG_KILNCTL_GPIO_PROBE=y
CONFIG_KILNCTL_MAIN_TASK_STACK_SIZE=8192
"""

SDKCONFIG_B_DIFFERS = """\
CONFIG_IDF_TARGET="esp32s3"
# CONFIG_KILNCTL_GPIO_PROBE is not set
CONFIG_KILNCTL_MAIN_TASK_STACK_SIZE=8192
"""


class _TempTreeCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name
        self.build_dir = os.path.join(self.root, "build")
        os.makedirs(self.build_dir)

    def tearDown(self):
        self._tmp.cleanup()
        sdk._SDKCONFIG_CACHE.clear()

    def _write(self, path, content):
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)

    def _elf_path(self):
        elf = os.path.join(self.build_dir, "KilnCtrl.elf")
        # The resolver only needs the ELF's path/directory, never its bytes.
        self._write(elf, "not a real elf\n")
        return elf


class IdenticalPairPasses(_TempTreeCase):
    def test_identical_content_resolves_to_sibling(self):
        self._write(os.path.join(self.build_dir, "sdkconfig"), SDKCONFIG_A)
        self._write(os.path.join(self.root, "sdkconfig"), SDKCONFIG_A_IDENTICAL)
        path, origin = sdk.use_sdkconfig_for_elf(self._elf_path())
        self.assertIsNotNone(path)
        self.assertEqual(os.path.abspath(path),
                          os.path.abspath(os.path.join(self.build_dir, "sdkconfig")))
        self.assertNotIn("UNRESOLVED", origin)


class OnlySiblingPresentPasses(_TempTreeCase):
    def test_no_parent_config_is_not_a_disagreement(self):
        self._write(os.path.join(self.build_dir, "sdkconfig"), SDKCONFIG_A)
        # No firmware/KilnFW/sdkconfig at all in this temp tree.
        path, origin = sdk.use_sdkconfig_for_elf(self._elf_path())
        self.assertIsNotNone(path)
        self.assertNotIn("UNRESOLVED", origin)


class DivergingPairFails(_TempTreeCase):
    def test_disagreeing_pair_fails_loudly_naming_both_paths_and_symbol(self):
        sibling = os.path.join(self.build_dir, "sdkconfig")
        parent = os.path.join(self.root, "sdkconfig")
        self._write(sibling, SDKCONFIG_A)
        self._write(parent, SDKCONFIG_B_DIFFERS)
        path, origin = sdk.use_sdkconfig_for_elf(self._elf_path())
        self.assertIsNone(path)
        self.assertIn("UNRESOLVED", origin)
        self.assertIn("CONFIG_KILNCTL_GPIO_PROBE", origin)
        self.assertIn(os.path.abspath(sibling), origin)
        self.assertIn(os.path.abspath(parent), origin)

    def test_negative_making_pair_identical_again_clears_the_failure(self):
        """Negative-test companion: prove the guard is load-bearing by first
        reproducing the failure, then restoring agreement and confirming it
        clears -- not just asserting the happy path in isolation."""
        sibling = os.path.join(self.build_dir, "sdkconfig")
        parent = os.path.join(self.root, "sdkconfig")
        self._write(sibling, SDKCONFIG_A)
        self._write(parent, SDKCONFIG_B_DIFFERS)
        elf = self._elf_path()
        path, origin = sdk.use_sdkconfig_for_elf(elf)
        self.assertIsNone(path)
        self.assertIn("UNRESOLVED", origin)

        # Restore agreement (equivalent to build_kilnfw refreshing the
        # sibling, or deleting it) and confirm the guard clears.
        self._write(sibling, SDKCONFIG_B_DIFFERS)
        path, origin = sdk.use_sdkconfig_for_elf(elf)
        self.assertIsNotNone(path)
        self.assertNotIn("UNRESOLVED", origin)


class ExplicitOverrideBypassesGuard(_TempTreeCase):
    def test_explicit_sdkconfig_skips_the_pair_check_entirely(self):
        sibling = os.path.join(self.build_dir, "sdkconfig")
        parent = os.path.join(self.root, "sdkconfig")
        explicit = os.path.join(self.root, "explicit.sdkconfig")
        self._write(sibling, SDKCONFIG_A)
        self._write(parent, SDKCONFIG_B_DIFFERS)
        self._write(explicit, SDKCONFIG_A)
        path, origin = sdk.use_sdkconfig_for_elf(self._elf_path(), explicit=explicit)
        self.assertEqual(os.path.abspath(path), os.path.abspath(explicit))
        self.assertIn("supplied explicitly", origin)


class ArchivedElfNeverComparesAgainstParent(_TempTreeCase):
    def test_archive_dir_sibling_pair_check_is_skipped(self):
        archive_dir = os.path.join(self.root, "elf_archive")
        os.makedirs(archive_dir)
        elf = os.path.join(archive_dir, "KilnCtrl-deadbeef.elf")
        self._write(elf, "not a real elf\n")
        # A bare "sdkconfig" sitting in an archive dir is not the normal
        # shape (archived ELFs get "<stem>.sdkconfig"), but even if one
        # existed alongside a disagreeing parent, the archive-dir rule
        # must win over the pair-agreement guard, not race it.
        self._write(os.path.join(archive_dir, "sdkconfig"), SDKCONFIG_A)
        self._write(os.path.join(self.root, "sdkconfig"), SDKCONFIG_B_DIFFERS)
        path, origin = sdk.use_sdkconfig_for_elf(elf)
        self.assertIsNotNone(path)
        self.assertNotIn("DISAGREE", origin)


if __name__ == "__main__":
    unittest.main()
