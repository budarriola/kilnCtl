#!/usr/bin/env python3
"""Unit tests for flash_firmware()'s post-flash verification
(mcp_server_flash.py: _verify_flash_landed() and the verify= wiring).

History: flash_firmware() resolves its write target dynamically from
partitions.csv (the `app`/ota_0 slot as of the 2026-09-16 single-slot OTA
redesign, docs/OTA_SINGLE_SLOT_PLAN.md) and never touches `otadata`. If an
OTA ever pointed the boot target somewhere else, the bootloader keeps
booting that OLD image forever -- every later flash_firmware() reports
"flashed and verified OK" (OpenOCD's own byte-compare during the write)
while the board silently keeps running old code. This is the recurring "my
change vanished" debugging-session drain CLAUDE.md documents.
_verify_flash_landed() closes that gap by independently asking the RUNNING
firmware (over HTTP) what partition it's booting and what build it reports,
and comparing both against what was just flashed.

All against MOCKED partition_http_client.get_partitions,
capability_preflight.get_board_info, and a synthetic on-disk .bin (built the
same way test_esp_app_desc.py does) -- no real socket, no live board.

Run with: python -m pytest tools/PcTools/tests/test_flash_firmware_verify.py -q
"""
from __future__ import annotations

import os
import shutil
import struct
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_flash as mf  # noqa: E402
from kilnctrl import esp_app_desc  # noqa: E402
from kilnctrl import partition_http_client  # noqa: E402
from kilnctrl import capability_preflight  # noqa: E402
from kilnctrl import flash_provenance  # noqa: E402


def _pad(s: str, width: int) -> bytes:
    b = s.encode("utf-8")
    return b + b"\x00" * (width - len(b))


def _write_bin(time_s: str = "20:13:41", date_s: str = "Sep  3 2026") -> str:
    header = b"\xff" * esp_app_desc.APP_DESC_OFFSET
    desc = struct.pack("<I", esp_app_desc.ESP_APP_DESC_MAGIC_WORD)
    desc += struct.pack("<I", 1)
    desc += b"\x00" * 8
    desc += _pad("v1", 32)
    desc += _pad("KilnCtrl", 32)
    desc += _pad(time_s, 16)
    desc += _pad(date_s, 16)
    desc += b"\x00" * (32 + 32 + 80)
    f = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
    f.write(header + desc)
    f.close()
    return f.name


class VerifyFlashLandedTest(unittest.TestCase):
    def setUp(self):
        self.bin_path = _write_bin(time_s="20:13:41", date_s="Sep  3 2026")
        self.addCleanup(os.unlink, self.bin_path)

    def test_running_app_and_matching_build_succeeds_silently(self):
        with unittest.mock.patch.object(
            partition_http_client, "get_partitions",
            return_value={"running": "app", "partitions": []},
        ), unittest.mock.patch.object(
            capability_preflight, "get_board_info",
            return_value=capability_preflight.BoardInfo(reachable=True, fw_build="Sep  3 2026 20:13:41"),
        ):
            result = mf._verify_flash_landed("192.168.1.50", self.bin_path)
        self.assertEqual(result, "")

    def test_running_ota_0_fails_loud_and_names_it(self):
        """The core bug this whole change exists to catch: bootloader still
        booting a stale/blank-otadata boot target after an `app`-only flash."""
        with unittest.mock.patch.object(
            partition_http_client, "get_partitions",
            return_value={"running": "ota_0", "partitions": []},
        ):
            with self.assertRaises(RuntimeError) as ctx:
                mf._verify_flash_landed("192.168.1.50", self.bin_path)
        msg = str(ctx.exception)
        self.assertIn("ota_0", msg)
        # ota_rollback_esp() is named, but explicitly as NOT a fix for this
        # scenario (it reverts an already-booting board over HTTP; it has no
        # path to set an unset/blank otadata after a bare JTAG flash) -- see
        # the KNOWN GAP explanation in _verify_flash_landed()'s message.
        self.assertIn("ota_rollback_esp", msg)
        self.assertIn("does NOT fix this", msg)
        self.assertIn("KNOWN GAP", msg)

    def test_recovery_image_response_fails_loud_naming_recovery(self):
        """docs/audits/web_code_duplication_drift_2026-09-18.md section 2.3:
        a board that came up running the recovery image answers
        GET /api/partitions with partition_http_client.RecoveryImageResponse
        (raised, not returned as a normal dict). _verify_flash_landed() must
        catch it and fail loud naming the recovery image and next_update --
        not the generic "malformed response" PartitionHttpError this used to
        surface as, and not the every-poll-attempt retry loop a plain
        PartitionHttpError gets (the board answered; retrying won't change
        what image it's running)."""
        with unittest.mock.patch.object(
            partition_http_client, "get_partitions",
            side_effect=partition_http_client.RecoveryImageResponse(
                running="recovery", running_offset="0x009000", next_update="app",
            ),
        ):
            with self.assertRaises(RuntimeError) as ctx:
                mf._verify_flash_landed("192.168.1.50", self.bin_path)
        msg = str(ctx.exception)
        self.assertIn("RECOVERY", msg)
        self.assertIn("recovery", msg)
        self.assertIn("app", msg)  # next_update named

    def test_build_timestamp_mismatch_fails_loud_with_both_values(self):
        with unittest.mock.patch.object(
            partition_http_client, "get_partitions",
            return_value={"running": "app", "partitions": []},
        ), unittest.mock.patch.object(
            capability_preflight, "get_board_info",
            return_value=capability_preflight.BoardInfo(reachable=True, fw_build="Sep  1 2026 08:00:00"),
        ):
            with self.assertRaises(RuntimeError) as ctx:
                mf._verify_flash_landed("192.168.1.50", self.bin_path)
        msg = str(ctx.exception)
        self.assertIn("Sep  1 2026 08:00:00", msg)
        self.assertIn("Sep  3 2026 20:13:41", msg)

    def test_http_unreachable_warns_does_not_raise(self):
        """verify=True default's HTTP-unreachable case must WARN, never
        hard-fail -- distinct from an actual wrong-partition/build finding.
        Speeds through all poll attempts by mocking time.sleep."""
        with unittest.mock.patch.object(
            partition_http_client, "get_partitions",
            side_effect=partition_http_client.PartitionHttpError("unreachable: timed out"),
        ), unittest.mock.patch.object(mf.time, "sleep"):
            result = mf._verify_flash_landed("192.168.1.50", self.bin_path)
        self.assertIn("WARNING", result)
        self.assertIn("skipped", result)

    def test_status_unreachable_after_partition_ok_warns_not_raises(self):
        with unittest.mock.patch.object(
            partition_http_client, "get_partitions",
            return_value={"running": "app", "partitions": []},
        ), unittest.mock.patch.object(
            capability_preflight, "get_board_info",
            return_value=capability_preflight.BoardInfo(reachable=False, error="timed out"),
        ):
            result = mf._verify_flash_landed("192.168.1.50", self.bin_path)
        self.assertIn("WARNING", result)
        self.assertIn("app", result)

    def test_fw_build_none_warns_not_reported_as_stale_binary(self):
        """5c92141c's dashboard_status_http.c gate redacted fw_build to null
        for EVERY caller (including this tool, which holds no session) on a
        default, auth-off board -- build_timestamps_match(app_desc, None)
        always returns False, so every successful flash raised the "does not
        match the binary just flashed" RuntimeError, blaming a stale binary
        for what was actually a tooling blind spot. Corrected 2026-09-17:
        fw_build is now only null when web auth is genuinely ON and this
        caller isn't an admin -- a real "cannot check" case, not a mismatch.
        This must WARN, not raise, and must say so, not blame a stale
        binary."""
        with unittest.mock.patch.object(
            partition_http_client, "get_partitions",
            return_value={"running": "app", "partitions": []},
        ), unittest.mock.patch.object(
            capability_preflight, "get_board_info",
            return_value=capability_preflight.BoardInfo(reachable=True, fw_build=None),
        ):
            result = mf._verify_flash_landed("192.168.1.50", self.bin_path)
        self.assertIn("WARNING", result)
        self.assertNotIn("stale", result.lower())
        self.assertNotIn("does not match", result)

    def test_unparseable_bin_warns_does_not_raise(self):
        bad_bin = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
        bad_bin.write(b"not a real image")
        bad_bin.close()
        self.addCleanup(os.unlink, bad_bin.name)
        result = mf._verify_flash_landed("192.168.1.50", bad_bin.name)
        self.assertIn("WARNING", result)


class FlashFirmwareVerifyWiringTest(unittest.TestCase):
    """End-to-end through flash_firmware() itself: proves verify=True/False
    actually gate _verify_flash_landed, and that a verification failure
    surfaces as the tool's returned error string (flash_firmware() never
    raises out to its caller -- _tool()'s wrapper would mangle the message,
    so _post_flash catches RuntimeError itself)."""

    def setUp(self):
        self._openocd_patch = unittest.mock.patch.object(mf, "_find_openocd_exe", return_value="fake-openocd.exe")
        self._openocd_patch.start()
        self.addCleanup(self._openocd_patch.stop)

        self._kill_patch = unittest.mock.patch.object(mf, "kill_openocd_sessions", return_value="")
        self._kill_patch.start()
        self.addCleanup(self._kill_patch.stop)

        self._run_patch = unittest.mock.patch.object(mf, "_run_openocd", return_value=(True, "verified"))
        self._run_patch.start()
        self.addCleanup(self._run_patch.stop)

        stale_ok = unittest.mock.Mock(stale=False, reason="")
        self._stale_patch = unittest.mock.patch.object(mf.stale_check, "check_kilnfw_stale", return_value=stale_ok)
        self._stale_patch.start()
        self.addCleanup(self._stale_patch.stop)

        # This test class is about verify=True/False wiring, not the
        # sensitive-dirty-file guard -- pin the tree state to clean so the
        # actual (often dirty, shared) repo working tree can't flip these
        # tests. See test_flash_provenance.py for the guard's own tests.
        clean_state = flash_provenance.TreeState(timestamp=0.0, head="abc1234")
        self._provenance_patch = unittest.mock.patch.object(
            mf.flash_provenance, "capture_tree_state", return_value=clean_state
        )
        self._provenance_patch.start()
        self.addCleanup(self._provenance_patch.stop)
        self._provenance_write_patch = unittest.mock.patch.object(
            mf.flash_provenance, "write_provenance_json", return_value=None
        )
        self._provenance_write_patch.start()
        self.addCleanup(self._provenance_write_patch.stop)

        # M1 (2026-09-15 review): flash_provenance.json's path is now
        # elf_archive.kiln_provenance_path(), guarded like the ELF archive
        # dirs -- unpatched under pytest this raises. write_provenance_json
        # is mocked above anyway, so a fake path is harmless.
        self._provenance_path_patch = unittest.mock.patch.object(
            mf.elf_archive, "kiln_provenance_path", return_value="FAKE-flash_provenance.json"
        )
        self._provenance_path_patch.start()
        self.addCleanup(self._provenance_path_patch.stop)

        self._isfile_patch = unittest.mock.patch.object(mf.os.path, "isfile", return_value=True)
        self._isfile_patch.start()
        self.addCleanup(self._isfile_patch.stop)

        # These tests never actually create build/KilnCtrl.bin on disk
        # (_isfile_patch above fakes its presence), so os.path.getsize()
        # against it would raise FileNotFoundError -- fake a size comfortably
        # under the real `app` partition's 0x800000 B so the new pre-flight
        # size check (added alongside the dynamic-offset fix) is a no-op here.
        self._getsize_patch = unittest.mock.patch.object(mf.os.path, "getsize", return_value=1024)
        self._getsize_patch.start()
        self.addCleanup(self._getsize_patch.stop)

        # flash_firmware() now probes the board's HTTP address BEFORE
        # flashing (see _preflash_board_address). These tests are about the
        # verify= wiring, not that probe -- and an unpatched probe would try
        # a real UART/HTTP round trip from a unit test.
        self._preflash_patch = unittest.mock.patch.object(
            mf, "_preflash_board_address", return_value=None
        )
        self.preflash_mock = self._preflash_patch.start()
        self.addCleanup(self._preflash_patch.stop)

        # 2026-09-10: this class drives flash_firmware() end-to-end, which
        # calls _archive_flashed_elf() -> elf_archive.archive_kiln_elf()
        # against the CANONICAL (main-tree) archive -- this test class never
        # meant to exercise that, but nothing here stopped it from actually
        # doing so and overwriting a real manifest entry with a fabricated
        # commit (see elf_archive.py's own guard for the incident this
        # caused). Patch it out explicitly, in addition to that guard.
        self._archive_patch = unittest.mock.patch.object(mf.elf_archive, "archive_kiln_elf")
        self._archive_patch.start()
        self.addCleanup(self._archive_patch.stop)

        # Owner decision 2026-09-19 made the post-flash boot_guard reset
        # default-on whenever KILNCTL_WEB_USERNAME/PASSWORD are set -- clear
        # them here so this whole test class (most of which never mocks the
        # boot_guard HTTP calls) can't accidentally make a real network call
        # just because the machine running the suite happens to have web
        # auth configured. BootGuardResetWiringTest's own env tests set
        # these back explicitly where they need to.
        self._env_patch = unittest.mock.patch.dict(mf.os.environ, {}, clear=False)
        self._env_patch.start()
        self.addCleanup(self._env_patch.stop)
        mf.os.environ.pop(mf.http_auth.USERNAME_ENV, None)
        mf.os.environ.pop(mf.http_auth.PASSWORD_ENV, None)

    def test_verify_false_skips_verification_entirely(self):
        with unittest.mock.patch.object(mf, "_verify_flash_landed") as verify_mock:
            result = mf.flash_firmware(verify=False)
        verify_mock.assert_not_called()
        self.assertIn("flashed and verified OK", result)

    def test_verify_true_failure_surfaces_as_error_string(self):
        with unittest.mock.patch.object(
            mf, "_verify_flash_landed",
            side_effect=RuntimeError("running partition 'ota_0', not 'factory' -- call ota_rollback_esp()"),
        ):
            result = mf.flash_firmware(verify=True)
        self.assertTrue(result.startswith("error:"))
        self.assertIn("ota_0", result)
        self.assertIn("ota_rollback_esp", result)

    def test_verify_true_warning_is_appended_not_a_failure(self):
        with unittest.mock.patch.object(mf, "_verify_flash_landed", return_value="WARNING: board unreachable"):
            result = mf.flash_firmware(verify=True)
        self.assertFalse(result.startswith("error:"))
        self.assertIn("WARNING: board unreachable", result)

class DefaultHostResolutionTest(unittest.TestCase):
    """REGRESSION: every test above passes an EXPLICIT host, so none of them
    exercised the default (host=None) path -- the one every documented
    `flash_firmware()` call actually takes. That path used to resolve
    straight to PARTITION_AP_DEFAULT_HOST (192.168.4.1), an address a board
    on home Wi-Fi does not answer, so verification always timed out into the
    "could not reach the board" WARNING and never once ran on the failure it
    exists to catch. It must use the package-wide _ota_resolve_host() (STA IP
    first), the same resolution debug_check_partition_table() uses."""

    def setUp(self):
        self.bin_path = _write_bin()
        self.addCleanup(os.unlink, self.bin_path)

    def test_default_host_uses_sta_ip_not_the_ap_fallback(self):
        from kilnctrl import mcp_server_ota

        seen = []

        def fake_get_partitions(host, timeout=None):
            seen.append(host)
            return {"running": "app", "partitions": []}

        with unittest.mock.patch.object(
            mcp_server_ota, "_ota_resolve_host", return_value="192.168.1.77"
        ), unittest.mock.patch.object(
            partition_http_client, "get_partitions", side_effect=fake_get_partitions
        ), unittest.mock.patch.object(
            capability_preflight, "get_board_info",
            return_value=capability_preflight.BoardInfo(reachable=True, fw_build="Sep  3 2026 20:13:41"),
        ):
            result = mf._verify_flash_landed(None, self.bin_path)
        self.assertEqual(result, "")
        self.assertEqual(seen, ["192.168.1.77"])
        self.assertNotIn(partition_http_client.PARTITION_AP_DEFAULT_HOST, seen)


class WrongAddressRegressionTest(unittest.TestCase):
    """THE 2026-09-04 DEFECT. `flash_firmware()` (defaults) verified against
    192.168.4.1 -- the SoftAP fallback -- while the board sat on the LAN at
    192.168.1.156 answering fine. Cause: verification resolved its host ONCE,
    via `_ota_resolve_host()`, immediately after the OpenOCD reset, i.e. at
    the one moment the board provably has not re-associated with Wi-Fi yet;
    wifi_get_status() answered sta_connected=False and resolution fell to the
    AP address. The resulting timeout produced the deliberately-soft
    "could not reach the board" WARNING -- emitted identically whether the
    flash landed or not, so the check said nothing in exactly the case it
    exists for.

    Fix under test: an ordered candidate list (pre-flash-observed address
    first), re-resolved on each poll attempt, plus failure semantics keyed on
    whether the board was answering BEFORE the flash."""

    def setUp(self):
        self.bin_path = _write_bin()
        self.addCleanup(os.unlink, self.bin_path)

    def _patch_resolve(self, resolved):
        from kilnctrl import mcp_server_ota
        return unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value=resolved)

    def test_lan_address_tried_even_when_resolution_says_ap(self):
        """(a) Board reachable at the LAN address while post-reset
        wifi_get_status() still reports the AP fallback (the live failure).
        Verification must still find the board and confirm the build."""
        seen = []

        def fake_get_partitions(host, timeout=None):
            seen.append(host)
            if host != "192.168.1.156":
                raise partition_http_client.PartitionHttpError("unreachable: timed out")
            return {"running": "app", "partitions": []}

        with self._patch_resolve("192.168.4.1"), unittest.mock.patch.object(
            partition_http_client, "get_partitions", side_effect=fake_get_partitions
        ), unittest.mock.patch.object(
            capability_preflight, "get_board_info",
            return_value=capability_preflight.BoardInfo(reachable=True, fw_build="Sep  3 2026 20:13:41"),
        ) as board_mock:
            result = mf._verify_flash_landed(None, self.bin_path, pre_flash_host="192.168.1.156")

        self.assertEqual(result, "")
        self.assertEqual(seen[0], "192.168.1.156")
        self.assertEqual(board_mock.call_args[0][0], "192.168.1.156")

    def test_old_build_on_lan_address_fails_loud(self):
        """(b) Board reachable, running 'app', but reporting an OLD
        build -- the failure the whole feature exists for. Must RAISE and
        name both timestamps."""
        with self._patch_resolve("192.168.4.1"), unittest.mock.patch.object(
            partition_http_client, "get_partitions",
            return_value={"running": "app", "partitions": []},
        ), unittest.mock.patch.object(
            capability_preflight, "get_board_info",
            return_value=capability_preflight.BoardInfo(reachable=True, fw_build="Aug 12 2026 08:01:02"),
        ):
            with self.assertRaises(RuntimeError) as ctx:
                mf._verify_flash_landed(None, self.bin_path, pre_flash_host="192.168.1.156")
        msg = str(ctx.exception)
        self.assertIn("Aug 12 2026 08:01:02", msg)
        self.assertIn("Sep  3 2026 20:13:41", msg)

    def test_unreachable_after_being_reachable_before_is_a_hard_failure(self):
        """(c1) Nothing answers anywhere AND the board was answering before
        the flash -> hard failure, not a warning."""
        with self._patch_resolve("192.168.4.1"), unittest.mock.patch.object(
            partition_http_client, "get_partitions",
            side_effect=partition_http_client.PartitionHttpError("unreachable: timed out"),
        ), unittest.mock.patch.object(mf.time, "sleep", return_value=None):
            with self.assertRaises(RuntimeError) as ctx:
                mf._verify_flash_landed(None, self.bin_path, pre_flash_host="192.168.1.156")
        msg = str(ctx.exception)
        self.assertIn("192.168.1.156", msg)
        self.assertIn("192.168.4.1", msg)
        self.assertIn("verification FAILURE", msg)

    def test_unreachable_and_was_unreachable_before_stays_a_warning(self):
        """(c2) Same silence, but the board was NOT answering before the
        flash either -- genuine bring-up, still only a WARNING."""
        with self._patch_resolve("192.168.4.1"), unittest.mock.patch.object(
            partition_http_client, "get_partitions",
            side_effect=partition_http_client.PartitionHttpError("unreachable: timed out"),
        ), unittest.mock.patch.object(mf.time, "sleep", return_value=None):
            result = mf._verify_flash_landed(None, self.bin_path, pre_flash_host=None)
        self.assertTrue(result.startswith("WARNING:"))
        self.assertIn("192.168.4.1", result)
        self.assertIn("early bring-up", result)

    def test_explicit_host_is_the_only_address_probed(self):
        with self._patch_resolve("192.168.4.1"):
            self.assertEqual(
                mf._resolve_verify_hosts("10.0.0.9", pre_flash_host="192.168.1.156"),
                ["10.0.0.9"],
            )

    def test_candidate_order_is_preflash_then_resolved_then_ap(self):
        with self._patch_resolve("192.168.1.200"):
            self.assertEqual(
                mf._resolve_verify_hosts(None, pre_flash_host="192.168.1.156"),
                ["192.168.1.156", "192.168.1.200", "192.168.4.1"],
            )

    def test_preflash_probe_returns_the_answering_address(self):
        def fake_get_partitions(host, timeout=None):
            if host != "192.168.1.156":
                raise partition_http_client.PartitionHttpError("unreachable: timed out")
            return {"running": "app", "partitions": []}

        with self._patch_resolve("192.168.1.156"), unittest.mock.patch.object(
            partition_http_client, "get_partitions", side_effect=fake_get_partitions
        ):
            self.assertEqual(mf._preflash_board_address(None), "192.168.1.156")

    def test_preflash_probe_counts_a_recovery_running_board_as_observed_up(self):
        """A board answering with the recovery image's shape raises
        partition_http_client.RecoveryImageResponse -- that still means the
        board answered GET /api/partitions, just not with the main app's
        table. _preflash_board_address() must return that candidate rather
        than treating it as unreachable and falling through to the next
        one/None: downgrading a recovery-mode board to 'never observed up'
        would turn _verify_flash_landed()'s eventual hard failure into its
        soft bring-up WARNING instead."""

        def fake_get_partitions(host, timeout=None):
            if host != "192.168.1.156":
                raise partition_http_client.PartitionHttpError("unreachable: timed out")
            raise partition_http_client.RecoveryImageResponse(
                running="recovery", running_offset="0x009000", next_update="app",
            )

        with self._patch_resolve("192.168.1.156"), unittest.mock.patch.object(
            partition_http_client, "get_partitions", side_effect=fake_get_partitions
        ):
            self.assertEqual(mf._preflash_board_address(None), "192.168.1.156")

    def test_preflash_probe_returns_none_when_nothing_answers(self):
        with self._patch_resolve("192.168.4.1"), unittest.mock.patch.object(
            partition_http_client, "get_partitions",
            side_effect=partition_http_client.PartitionHttpError("unreachable: timed out"),
        ):
            self.assertIsNone(mf._preflash_board_address(None))


class PreFlashProbeWiringTest(FlashFirmwareVerifyWiringTest):
    """flash_firmware() must actually take the pre-flash observation and hand
    it to _verify_flash_landed -- otherwise the semantics above are dead code."""

    def test_preflash_address_is_passed_through_to_verification(self):
        self.preflash_mock.return_value = "192.168.1.156"
        with unittest.mock.patch.object(mf, "_verify_flash_landed", return_value="") as verify_mock:
            mf.flash_firmware(verify=True)
        self.assertEqual(verify_mock.call_args[0][2], "192.168.1.156")

    def test_preflash_probe_runs_even_when_verify_is_false(self):
        """d6747fc6 made _preflash_board_address() unconditional on purpose
        (see mcp_server_flash.py's comment right above where it's called,
        currently around line 950-955): the partition-offset write guard
        (_check_app_flash_offset_matches_chip()) consumes that probed
        address too, and that guard runs regardless of `verify` -- the
        write itself is not something verify=False should be able to skip
        confirming. This test used to assert the opposite
        (preflash_mock.assert_not_called()), which was already stale
        against that intentional change; it is rewritten here to assert
        the actual, intended behaviour instead of re-asserting the
        superseded one."""
        with unittest.mock.patch.object(mf, "_verify_flash_landed") as verify_mock:
            mf.flash_firmware(verify=False)
        verify_mock.assert_not_called()
        self.preflash_mock.assert_called()


class BootGuardResetWiringTest(FlashFirmwareVerifyWiringTest):
    """flash_firmware(ap_password=...) -- the tool-driven trigger for
    docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md's fix
    (ota_http_client.boot_guard_reset_esp(), calling
    App/drivers/http/ota_http_recovery.c's
    ota_boot_guard_reset_post_handler()).

    THE gate this class exists to prove: the call must happen ONLY after
    _verify_flash_landed() returns "" (full, unambiguous success) -- never
    on a raise (verification failure), a WARNING (unconfirmed/bring-up),
    or verify=False. A board just flashed with something broken, or whose
    landing was never actually confirmed, must still be free to walk into
    recovery mode on its own; see boot_guard_reset_counter()'s own header
    comment (firmware side) for why this is the one hard rule the whole
    fix depends on."""

    def test_called_after_full_verified_success(self):
        self.preflash_mock.return_value = "192.168.1.156"
        with unittest.mock.patch.object(mf, "_verify_flash_landed", return_value=""), \
             unittest.mock.patch.object(
                 mf.ota_http, "get_boot_guard_status",
                 return_value={"boot_count": 2, "recovery_mode": False}) as status_mock, \
             unittest.mock.patch.object(
                 mf.ota_http, "boot_guard_reset_esp",
                 return_value={"ok": True, "boot_count": 0}) as reset_mock:
            result = mf.flash_firmware(verify=True, ap_password="hunter2")
        status_mock.assert_called_once_with("192.168.1.156")
        reset_mock.assert_called_once_with("192.168.1.156", "hunter2")
        self.assertNotIn("error:", result)
        self.assertIn("boot_guard_reset", result)
        self.assertIn("cleared and verified", result)
        # Item 4 of the plan: report the count BEFORE the clear (from the
        # GET probe above) alongside the verified after-value, not just the
        # after-value alone -- a silent clear with no before/after context
        # is not acceptable per RELEASE_HARDENING_PLAN.md blocker 6.
        self.assertIn("before=2", result)
        self.assertIn("after=0", result)

    def test_before_count_unknown_when_get_status_fails(self):
        """The pre-reset GET /api/boot_guard probe is best-effort only -- its
        failure must never block or fail the reset call itself, and must be
        reported as "unknown" rather than silently omitted or raised."""
        self.preflash_mock.return_value = "192.168.1.156"
        with unittest.mock.patch.object(mf, "_verify_flash_landed", return_value=""), \
             unittest.mock.patch.object(
                 mf.ota_http, "get_boot_guard_status",
                 side_effect=mf.ota_http.OtaHttpError("unreachable")), \
             unittest.mock.patch.object(
                 mf.ota_http, "boot_guard_reset_esp",
                 return_value={"ok": True, "boot_count": 0}) as reset_mock:
            result = mf.flash_firmware(verify=True, ap_password="hunter2")
        reset_mock.assert_called_once_with("192.168.1.156", "hunter2")
        self.assertNotIn("error:", result)
        self.assertIn("before=unknown", result)
        self.assertIn("after=0", result)

    def _clear_web_auth_env(self):
        """Isolate these tests from whatever KILNCTL_WEB_USERNAME/PASSWORD
        happen to be set in the actual environment this suite runs in --
        owner decision 2026-09-19 made the reset default-on whenever those
        are present, so a real dev environment with them set would otherwise
        make the "no credentials" tests flaky/order-dependent."""
        patcher = unittest.mock.patch.dict(
            mf.os.environ,
            {mf.http_auth.USERNAME_ENV: "", mf.http_auth.PASSWORD_ENV: ""},
            clear=False,
        )
        patcher.start()
        self.addCleanup(patcher.stop)
        # patch.dict sets them to "" rather than removing them -- explicitly
        # pop so os.environ.get(...) sees None, matching a genuinely unset var.
        for key in (mf.http_auth.USERNAME_ENV, mf.http_auth.PASSWORD_ENV):
            mf.os.environ.pop(key, None)
        self.addCleanup(mf.os.environ.pop, mf.http_auth.USERNAME_ENV, None)
        self.addCleanup(mf.os.environ.pop, mf.http_auth.PASSWORD_ENV, None)

    def test_skipped_without_ap_password_or_env_credentials(self):
        """Owner decision 2026-09-19: with neither an explicit `ap_password`
        nor KILNCTL_WEB_USERNAME/PASSWORD set, the reset is a no-op (never
        calls the HTTP endpoints) but the result must say so explicitly --
        a caller must be able to tell a skip from a silent success."""
        self._clear_web_auth_env()
        self.preflash_mock.return_value = "192.168.1.156"
        with unittest.mock.patch.object(mf, "_verify_flash_landed", return_value=""), \
             unittest.mock.patch.object(mf.ota_http, "get_boot_guard_status") as status_mock, \
             unittest.mock.patch.object(mf.ota_http, "boot_guard_reset_esp") as reset_mock:
            result = mf.flash_firmware(verify=True)
        status_mock.assert_not_called()
        reset_mock.assert_not_called()
        self.assertIn("boot_guard_reset", result)
        self.assertIn("skipped", result)
        self.assertIn("no credential available", result)

    def test_env_credentials_used_when_ap_password_omitted(self):
        """The new default-on path: KILNCTL_WEB_USERNAME/PASSWORD alone
        (no explicit `ap_password`) must trigger the same reset call, using
        the env password."""
        self.preflash_mock.return_value = "192.168.1.156"
        env_patch = unittest.mock.patch.dict(
            mf.os.environ,
            {mf.http_auth.USERNAME_ENV: "admin", mf.http_auth.PASSWORD_ENV: "envpw123"},
        )
        env_patch.start()
        self.addCleanup(env_patch.stop)
        with unittest.mock.patch.object(mf, "_verify_flash_landed", return_value=""), \
             unittest.mock.patch.object(
                 mf.ota_http, "get_boot_guard_status",
                 return_value={"boot_count": 2, "recovery_mode": False}), \
             unittest.mock.patch.object(
                 mf.ota_http, "boot_guard_reset_esp",
                 return_value={"ok": True, "boot_count": 0}) as reset_mock:
            result = mf.flash_firmware(verify=True)
        reset_mock.assert_called_once_with("192.168.1.156", "envpw123")
        self.assertIn("cleared and verified", result)

    def test_explicit_ap_password_wins_over_env(self):
        self.preflash_mock.return_value = "192.168.1.156"
        env_patch = unittest.mock.patch.dict(
            mf.os.environ,
            {mf.http_auth.USERNAME_ENV: "admin", mf.http_auth.PASSWORD_ENV: "envpw123"},
        )
        env_patch.start()
        self.addCleanup(env_patch.stop)
        with unittest.mock.patch.object(mf, "_verify_flash_landed", return_value=""), \
             unittest.mock.patch.object(
                 mf.ota_http, "get_boot_guard_status",
                 return_value={"boot_count": 2, "recovery_mode": False}), \
             unittest.mock.patch.object(
                 mf.ota_http, "boot_guard_reset_esp",
                 return_value={"ok": True, "boot_count": 0}) as reset_mock:
            mf.flash_firmware(verify=True, ap_password="explicit-pw")
        reset_mock.assert_called_once_with("192.168.1.156", "explicit-pw")

    def test_reset_boot_guard_false_opts_out_even_with_credentials(self):
        """`reset_boot_guard=False` must override even a fully-populated
        credential set -- it is an unconditional opt-out."""
        self.preflash_mock.return_value = "192.168.1.156"
        with unittest.mock.patch.object(mf, "_verify_flash_landed", return_value=""), \
             unittest.mock.patch.object(mf.ota_http, "get_boot_guard_status") as status_mock, \
             unittest.mock.patch.object(mf.ota_http, "boot_guard_reset_esp") as reset_mock:
            result = mf.flash_firmware(verify=True, ap_password="hunter2", reset_boot_guard=False)
        status_mock.assert_not_called()
        reset_mock.assert_not_called()
        self.assertNotIn("boot_guard_reset", result)

    def test_not_called_on_warning(self):
        """A WARNING from _verify_flash_landed (board unreachable/could-not-
        confirm) is NOT full verified success -- must not trigger the
        reset. A resolvable board address is deliberately provided (not
        the setUp default of None) so this test cannot pass merely because
        _maybe_reset_boot_guard() had no address to call -- it must be the
        WARNING gate itself doing the work."""
        self.preflash_mock.return_value = "192.168.1.156"
        with unittest.mock.patch.object(mf, "_verify_flash_landed",
                                         return_value="WARNING: board unreachable"), \
             unittest.mock.patch.object(mf.ota_http, "get_boot_guard_status") as status_mock, \
             unittest.mock.patch.object(mf.ota_http, "boot_guard_reset_esp") as reset_mock:
            mf.flash_firmware(verify=True, ap_password="hunter2")
        status_mock.assert_not_called()
        reset_mock.assert_not_called()

    def test_not_called_on_verification_failure(self):
        """THE central negative case: a hard verification failure (wrong
        partition/build) must never clear the recovery-mode counter -- a
        board that just got a bad flash must still be free to enter
        recovery mode."""
        with unittest.mock.patch.object(
                mf, "_verify_flash_landed",
                side_effect=RuntimeError("board is running partition 'ota_0', not 'factory'")), \
             unittest.mock.patch.object(mf.ota_http, "boot_guard_reset_esp") as reset_mock:
            result = mf.flash_firmware(verify=True, ap_password="hunter2")
        reset_mock.assert_not_called()
        self.assertTrue(result.startswith("error:"))

    def test_not_called_when_verify_is_false(self):
        """verify=False means the flash's own landing was never even
        checked -- calling boot_guard_reset_esp here would be strictly
        worse than doing nothing, since it would clear the counter on the
        strength of NO evidence the new build is running at all."""
        with unittest.mock.patch.object(mf, "_verify_flash_landed") as verify_mock, \
             unittest.mock.patch.object(mf.ota_http, "boot_guard_reset_esp") as reset_mock:
            mf.flash_firmware(verify=False, ap_password="hunter2")
        verify_mock.assert_not_called()
        reset_mock.assert_not_called()

    def test_unverified_reset_reported_as_warning_not_error(self):
        """The board's own read-back could not confirm the clear (the
        lying-write class) -- flash_firmware() must say so plainly, but
        must NOT report the whole call as an error: the flash itself
        landed and verified fine, only the counter-clear is in doubt."""
        self.preflash_mock.return_value = "192.168.1.156"
        with unittest.mock.patch.object(mf, "_verify_flash_landed", return_value=""), \
             unittest.mock.patch.object(
                 mf.ota_http, "get_boot_guard_status",
                 return_value={"boot_count": 2, "recovery_mode": False}), \
             unittest.mock.patch.object(
                 mf.ota_http, "boot_guard_reset_esp",
                 return_value={"ok": False, "boot_count": 2}):
            result = mf.flash_firmware(verify=True, ap_password="hunter2")
        self.assertFalse(result.startswith("error:"))
        self.assertIn("WARNING", result)
        self.assertIn("NOT confirmed cleared", result)

    def test_unreachable_boot_guard_endpoint_reported_as_warning_not_error(self):
        """An OtaHttpError calling the endpoint (e.g. the board dropped off
        Wi-Fi in the instant between verification and this call) is also a
        WARNING, not a tool failure -- the flash already landed."""
        self.preflash_mock.return_value = "192.168.1.156"
        with unittest.mock.patch.object(mf, "_verify_flash_landed", return_value=""), \
             unittest.mock.patch.object(
                 mf.ota_http, "get_boot_guard_status",
                 return_value={"boot_count": 2, "recovery_mode": False}), \
             unittest.mock.patch.object(
                 mf.ota_http, "boot_guard_reset_esp",
                 side_effect=mf.ota_http.OtaHttpError("unreachable")):
            result = mf.flash_firmware(verify=True, ap_password="hunter2")
        self.assertFalse(result.startswith("error:"))
        self.assertIn("WARNING", result)
        self.assertIn("boot_guard_reset call failed", result)


class KilnFwRootOverrideTest(unittest.TestCase):
    """flash_firmware(kiln_fw_root=...) -- the worktree-build override.

    See tools/PcTools/src/kilnctrl/debug_probe.py's `_kiln_fw_root()`
    hardcoding the main tree, and flash_firmware()'s new `kiln_fw_root`
    parameter that lets a build produced in a separate git worktree (checked
    out clean at HEAD, used when the main tree carries another session's
    WIP) be flashed without that WIP riding along.
    """

    def setUp(self):
        self.tmp_root = tempfile.mkdtemp()
        self.worktree_root = os.path.join(self.tmp_root, "worktree")
        self.override_kiln_fw_root = os.path.join(self.worktree_root, "firmware", "KilnFW")
        self.build_dir = os.path.join(self.override_kiln_fw_root, "build")
        os.makedirs(os.path.join(self.build_dir, "bootloader"))
        os.makedirs(os.path.join(self.build_dir, "partition_table"))
        with open(os.path.join(self.build_dir, "bootloader", "bootloader.bin"), "wb") as f:
            f.write(b"\x00")
        with open(os.path.join(self.build_dir, "partition_table", "partition-table.bin"), "wb") as f:
            f.write(b"\x00")
        with open(os.path.join(self.build_dir, "KilnCtrl.bin"), "wb") as f:
            f.write(b"\x00")
        # _resolve_app_flash_target() reads partitions.csv from the tree
        # being flashed -- the override tree's own copy, matching the real
        # single-slot table, not the main tree's.
        with open(os.path.join(self.override_kiln_fw_root, "partitions.csv"), "w", encoding="utf-8") as f:
            f.write(
                "# Name,   Type, SubType, Offset,  Size\n"
                "otadata,  data, ota,     0x200000, 0x2000,\n"
                "app,      app,  ota_0,   0x210000, 0x800000,\n"
                "recovery, app,  factory, 0xA10000, 0x1E0000,\n"
                "coredump, data, coredump,0xBF0000, 0x100000,\n"
            )
        self.addCleanup(shutil.rmtree, self.tmp_root, ignore_errors=True)

        self._openocd_patch = unittest.mock.patch.object(mf, "_find_openocd_exe", return_value="fake-openocd.exe")
        self._openocd_patch.start()
        self.addCleanup(self._openocd_patch.stop)

        self._kill_patch = unittest.mock.patch.object(mf, "kill_openocd_sessions", return_value="")
        self._kill_patch.start()
        self.addCleanup(self._kill_patch.stop)

        self._run_patch = unittest.mock.patch.object(mf, "_run_openocd", return_value=(True, "verified"))
        self.run_mock = self._run_patch.start()
        self.addCleanup(self._run_patch.stop)

        stale_ok = unittest.mock.Mock(stale=False, reason="")
        self._stale_patch = unittest.mock.patch.object(mf.stale_check, "check_kilnfw_stale", return_value=stale_ok)
        self._stale_patch.start()
        self.addCleanup(self._stale_patch.stop)

        self._adapter_patch = unittest.mock.patch.object(mf, "_refuse_if_adapter_absent", return_value=None)
        self._adapter_patch.start()
        self.addCleanup(self._adapter_patch.stop)

        self._preflash_patch = unittest.mock.patch.object(mf, "_preflash_board_address", return_value=None)
        self._preflash_patch.start()
        self.addCleanup(self._preflash_patch.stop)

        # See FlashFirmwareVerifyWiringTest.setUp -- this class also drives
        # flash_firmware() end-to-end and must not touch the real archive.
        self._archive_patch = unittest.mock.patch.object(mf.elf_archive, "archive_kiln_elf")
        self._archive_patch.start()
        self.addCleanup(self._archive_patch.stop)

        # M1 (2026-09-15 review): flash_provenance.json now lives at
        # elf_archive.kiln_provenance_path() -- always the MAIN tree's, a
        # sibling of kiln_archive_dir(), regardless of kiln_fw_root. This
        # class exercises real capture_tree_state()/write_provenance_json(),
        # so point the path at a tmp file instead of the real main-tree
        # location (which the guard would refuse anyway under pytest).
        self.prov_path = os.path.join(self.tmp_root, "flash_provenance.json")
        self._provenance_path_patch = unittest.mock.patch.object(
            mf.elf_archive, "kiln_provenance_path", return_value=self.prov_path
        )
        self._provenance_path_patch.start()
        self.addCleanup(self._provenance_path_patch.stop)

    def test_missing_kiln_fw_root_path_is_refused(self):
        result = mf.flash_firmware(kiln_fw_root=os.path.join(self.tmp_root, "does-not-exist"), verify=False)
        self.assertTrue(result.startswith("error:"))
        self.assertIn("does not exist", result)

    def test_non_absolute_kiln_fw_root_is_refused(self):
        result = mf.flash_firmware(kiln_fw_root="relative/path/firmware/KilnFW", verify=False)
        self.assertTrue(result.startswith("error:"))
        self.assertIn("absolute", result)

    def test_kiln_fw_root_missing_binaries_is_refused(self):
        empty_root = os.path.join(self.tmp_root, "empty", "firmware", "KilnFW")
        os.makedirs(empty_root)
        result = mf.flash_firmware(kiln_fw_root=empty_root, verify=False)
        self.assertTrue(result.startswith("error:"))
        self.assertIn("missing build output", result)
        self.assertIn("override", result)

    def test_valid_override_flashes_from_that_tree(self):
        with unittest.mock.patch.object(
            mf.flash_provenance, "capture_tree_state", wraps=mf.flash_provenance.capture_tree_state
        ) as capture_mock:
            result = mf.flash_firmware(kiln_fw_root=self.override_kiln_fw_root, verify=False)
        self.assertIn("flashed and verified OK", result)
        # cwd for the OpenOCD invocation must be the OVERRIDE tree, not the
        # main tree's firmware/KilnFW.
        self.assertEqual(self.run_mock.call_args[1]["cwd"], self.override_kiln_fw_root)
        # provenance must have been captured against the override tree's own
        # root (two levels above kiln_fw_root), not the main repo root.
        expected_repo_root = os.path.normpath(os.path.join(self.override_kiln_fw_root, "..", ".."))
        capture_mock.assert_called_with(repo_root=expected_repo_root)

    def test_provenance_json_records_the_override_path(self):
        mf.flash_firmware(kiln_fw_root=self.override_kiln_fw_root, verify=False)
        prov = mf.flash_provenance.read_provenance_json(self.prov_path)
        self.assertIsNotNone(prov)
        self.assertEqual(prov["kiln_fw_root_override"], self.override_kiln_fw_root)
        self.assertEqual(prov["outcome"], mf.flash_provenance.OUTCOME_FLASHED_OK)

    def test_write_offset_comes_from_partitions_csv_not_hardcoded(self):
        """THE Defect 1 regression: the TCL program_esp offset for
        KilnCtrl.bin must be the `app` partition's offset as resolved from
        THIS tree's own partitions.csv (0x210000 in the fixture above), not
        any hardcoded constant. Sabotaging the resolution (see the negative
        test in the report) turns this red."""
        mf.flash_firmware(kiln_fw_root=self.override_kiln_fw_root, verify=False)
        tcl = self.run_mock.call_args.args[2]
        self.assertIn("build/KilnCtrl.bin 0x210000 verify", tcl)

    def test_oversized_binary_is_refused_before_openocd(self):
        """THE new hard pre-flight size check: a KilnCtrl.bin larger than
        the target partition (0x800000 B per the fixture partitions.csv)
        must be refused, naming both sizes, before OpenOCD is ever touched."""
        oversized = 0x800000 + 1
        with open(os.path.join(self.build_dir, "KilnCtrl.bin"), "wb") as f:
            f.truncate(oversized)
        result = mf.flash_firmware(kiln_fw_root=self.override_kiln_fw_root, verify=False)
        self.assertTrue(result.startswith("error:"))
        self.assertIn(str(oversized), result)
        self.assertIn(str(0x800000), result)
        self.run_mock.assert_not_called()

    def test_no_override_omits_it_from_provenance_json(self):
        # Default path (no override) must still write None -- proves the
        # field is not just always the main tree's path by accident.
        with unittest.mock.patch.object(mf, "_kiln_fw_root", return_value=self.override_kiln_fw_root):
            mf.flash_firmware(verify=False)
        prov = mf.flash_provenance.read_provenance_json(self.prov_path)
        self.assertIsNotNone(prov)
        self.assertIsNone(prov["kiln_fw_root_override"])


if __name__ == "__main__":
    unittest.main()
